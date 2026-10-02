#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <MQTT.h>
#include <TFT_eSPI.h>
#include <SparkFun_SCD4x_Arduino_Library.h>

static constexpr int PIN_SDA=27, PIN_SCL=22, PIN_ACS712=35, PIN_TFT_BL=21;
static constexpr float ACS_SENSITIVITY_V_PER_A=0.100f;
static float acsZeroVoltage=2.500f;
static constexpr float ACS_DIVIDER_RATIO=20.0f/30.0f, CURRENT_DIRECTION=1.0f, CURRENT_DEADBAND_A=0.08f, SOLAR_BUS_VOLTS=24.0f;
static constexpr int ADC_SAMPLES=64;
static constexpr uint32_t DISPLAY_INTERVAL_MS=1000, SENSOR_POLL_INTERVAL_MS=1000, MQTT_PUBLISH_INTERVAL_MS=5000, MQTT_RETRY_INTERVAL_MS=5000;\nstatic constexpr float CURRENT_FILTER_ALPHA=0.12f;  // lower = steadier display
static const char *CONFIG_AP_NAME="Solar-Air-Setup";
static char mqttHost[64]="", mqttPortText[7]="1883", mqttUser[48]="", mqttPassword[64]="", mqttBaseTopic[64]="solarair", deviceName[40]="";
static const char MQTT_PASSWORD_ATTR[]="type='password'";

WiFiManagerParameter pMqttHost("mqtt_host","MQTT broker / IP","",sizeof(mqttHost));
WiFiManagerParameter pMqttPort("mqtt_port","MQTT port","1883",sizeof(mqttPortText));
WiFiManagerParameter pMqttUser("mqtt_user","MQTT username","",sizeof(mqttUser));
WiFiManagerParameter pMqttPassword("mqtt_pass","MQTT password","",sizeof(mqttPassword),MQTT_PASSWORD_ATTR);
WiFiManagerParameter pMqttBase("mqtt_topic","MQTT base topic","solarair",sizeof(mqttBaseTopic));
WiFiManagerParameter pDeviceName("device_name","Device name","",sizeof(deviceName));

TFT_eSPI tft=TFT_eSPI();
SCD4x scd40(SCD4x_SENSOR_SCD40);
WiFiManager wm;
Preferences prefs;
WiFiClient networkClient;
MQTTClient mqtt(512);

float solarCurrentA=0,solarPowerW=0,temperatureC=NAN,humidityRH=NAN;\nfloat filteredCurrentA=NAN;
uint16_t co2ppm=0;
bool scd40Online=false,mqttConfigChanged=false,webPortalRunning=false;
uint32_t lastDisplay=0,lastSensorPoll=0,lastMqttPublish=0,lastMqttRetry=0;

// Display state: static screen is drawn once, only values are updated afterward.
bool dashboardDrawn=false;
String lastCurrent="",lastPower="",lastCO2="",lastTemp="",lastRH="",lastWiFi="",lastMQTT="";

static void cp(char*d,size_t n,const char*s){if(n)strlcpy(d,s?s:"",n);}
static uint16_t mqttPort(){long p=strtol(mqttPortText,nullptr,10);return(p<1||p>65535)?1883:(uint16_t)p;}
static String base(){String s(mqttBaseTopic);s.trim();while(s.startsWith("/"))s.remove(0,1);while(s.endsWith("/"))s.remove(s.length()-1);return s.length()?s:"solarair";}
static String topic(const char*l){return base()+"/"+deviceName+"/"+l;}
static void defaultName(){snprintf(deviceName,sizeof(deviceName),"solar-air-%06lX",(unsigned long)(ESP.getEfuseMac()&0xFFFFFFULL));}

static void loadSettings(){
  defaultName();
  prefs.begin("solarair",true);
  String s=prefs.getString("mqhost",""); cp(mqttHost,sizeof(mqttHost),s.c_str());
  s=prefs.getString("mqport","1883"); cp(mqttPortText,sizeof(mqttPortText),s.c_str());
  s=prefs.getString("mquser",""); cp(mqttUser,sizeof(mqttUser),s.c_str());
  s=prefs.getString("mqpass",""); cp(mqttPassword,sizeof(mqttPassword),s.c_str());
  s=prefs.getString("mqtopic","solarair"); cp(mqttBaseTopic,sizeof(mqttBaseTopic),s.c_str());
  s=prefs.getString("devname",deviceName); cp(deviceName,sizeof(deviceName),s.c_str());
  prefs.end();
  pMqttHost.setValue(mqttHost,sizeof(mqttHost));
  pMqttPort.setValue(mqttPortText,sizeof(mqttPortText));
  pMqttUser.setValue(mqttUser,sizeof(mqttUser));
  pMqttPassword.setValue(mqttPassword,sizeof(mqttPassword));
  pMqttBase.setValue(mqttBaseTopic,sizeof(mqttBaseTopic));
  pDeviceName.setValue(deviceName,sizeof(deviceName));
}

static void saveSettings(){
  cp(mqttHost,sizeof(mqttHost),pMqttHost.getValue());
  cp(mqttPortText,sizeof(mqttPortText),pMqttPort.getValue());
  cp(mqttUser,sizeof(mqttUser),pMqttUser.getValue());
  cp(mqttPassword,sizeof(mqttPassword),pMqttPassword.getValue());
  cp(mqttBaseTopic,sizeof(mqttBaseTopic),pMqttBase.getValue());
  cp(deviceName,sizeof(deviceName),pDeviceName.getValue());
  if(!*mqttPortText)cp(mqttPortText,sizeof(mqttPortText),"1883");
  if(!*mqttBaseTopic)cp(mqttBaseTopic,sizeof(mqttBaseTopic),"solarair");
  if(!*deviceName)defaultName();
  prefs.begin("solarair",false);
  prefs.putString("mqhost",mqttHost); prefs.putString("mqport",mqttPortText);
  prefs.putString("mquser",mqttUser); prefs.putString("mqpass",mqttPassword);
  prefs.putString("mqtopic",mqttBaseTopic); prefs.putString("devname",deviceName);
  prefs.end();
  mqttConfigChanged=true;
}

static void portal(WiFiManager*m){
  dashboardDrawn=false;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW,TFT_BLACK); tft.setTextFont(4); tft.drawString("SETUP MODE",160,35);
  tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setTextFont(2);
  tft.drawString("Connect to Solar-Air-Setup",160,90);
  tft.drawString("Portal: 192.168.4.1",160,125);
  Serial.printf("[WIFI] AP %s IP %s\n",m->getConfigPortalSSID().c_str(),WiFi.softAPIP().toString().c_str());
}

static void wifiSetup(){
  loadSettings();
  wm.setConfigPortalTimeout(300); wm.setConnectTimeout(20); wm.setAPCallback(portal); wm.setSaveParamsCallback(saveSettings);
  wm.addParameter(&pMqttHost); wm.addParameter(&pMqttPort); wm.addParameter(&pMqttUser); wm.addParameter(&pMqttPassword); wm.addParameter(&pMqttBase); wm.addParameter(&pDeviceName);
  std::vector<const char*> menu={"wifi","param","info","sep","restart","exit"}; wm.setMenu(menu);
  wm.autoConnect(CONFIG_AP_NAME); WiFi.setAutoReconnect(true);
  if(WiFi.status()==WL_CONNECTED){wm.setConfigPortalBlocking(false);wm.startWebPortal();webPortalRunning=true;Serial.printf("[WIFI] %s IP %s\n",WiFi.SSID().c_str(),WiFi.localIP().toString().c_str());}
}

static void mqttConfig(){if(mqtt.connected())mqtt.disconnect();if(*mqttHost){mqtt.begin(mqttHost,mqttPort(),networkClient);mqtt.setKeepAlive(30);mqtt.setTimeout(2000);}}
static bool mqttConnect(){if(WiFi.status()!=WL_CONNECTED||!*mqttHost)return false;if(mqtt.connected())return true;bool ok=*mqttUser?mqtt.connect(deviceName,mqttUser,mqttPassword):mqtt.connect(deviceName);if(ok){mqtt.publish(topic("status"),"online",true,0);mqtt.publish(topic("ip"),WiFi.localIP().toString(),true,0);}return ok;}
static void mqttService(){if(mqttConfigChanged){mqttConfigChanged=false;mqttConfig();lastMqttRetry=0;}if(WiFi.status()!=WL_CONNECTED)return;mqtt.loop();if(!mqtt.connected()&&millis()-lastMqttRetry>=MQTT_RETRY_INTERVAL_MS){lastMqttRetry=millis();mqttConnect();}}
static void mqttPublish(){if(!mqtt.connected())return;mqtt.publish(topic("solar/current_a"),String(solarCurrentA,3),true,0);mqtt.publish(topic("solar/power_w"),String(solarPowerW,1),true,0);if(scd40Online&&!isnan(temperatureC)){mqtt.publish(topic("air/co2_ppm"),String(co2ppm),true,0);mqtt.publish(topic("air/temperature_c"),String(temperatureC,2),true,0);mqtt.publish(topic("air/humidity_pct"),String(humidityRH,1),true,0);}mqtt.publish(topic("wifi/rssi_dbm"),String(WiFi.RSSI()),true,0);String j="{\"current_a\":"+String(solarCurrentA,3)+",\"power_w\":"+String(solarPowerW,1);if(scd40Online&&!isnan(temperatureC))j+=",\"co2_ppm\":"+String(co2ppm)+",\"temperature_c\":"+String(temperatureC,2)+",\"humidity_pct\":"+String(humidityRH,1);j+=",\"rssi_dbm\":"+String(WiFi.RSSI())+",\"uptime_s\":"+String(millis()/1000UL)+"}";mqtt.publish(topic("state"),j,true,0);}

static float current(){uint32_t mv=0;for(int i=0;i<ADC_SAMPLES;i++){mv+=analogReadMilliVolts(PIN_ACS712);delayMicroseconds(250);}float v=(mv/(float)ADC_SAMPLES)/1000.0f/ACS_DIVIDER_RATIO;float a=(v-acsZeroVoltage)/ACS_SENSITIVITY_V_PER_A*CURRENT_DIRECTION;return fabsf(a)<CURRENT_DEADBAND_A?0:a;}
static uint16_t co2Color(){return !scd40Online?TFT_DARKGREY:co2ppm<800?TFT_GREEN:co2ppm<1200?TFT_YELLOW:TFT_RED;}

static void drawStaticDashboard(){
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0,0,320,32,TFT_DARKGREEN);
  tft.setTextDatum(MC_DATUM); tft.setTextFont(4); tft.setTextColor(TFT_WHITE,TFT_DARKGREEN);
  tft.drawString("SOLAR / AIR MONITOR",160,16);

  const int ys[]={36,68,100,132,164};
  const char* labels[]={"Solar Current","Solar Power*","CO2","Temperature","Humidity"};
  tft.setTextDatum(ML_DATUM); tft.setTextFont(2); tft.setTextColor(TFT_LIGHTGREY,TFT_BLACK);
  for(int i=0;i<5;i++){
    tft.drawString(labels[i],11,ys[i]+15);
    tft.drawFastHLine(11,ys[i]+30,298,TFT_DARKGREY);
  }

  tft.drawFastHLine(0,198,320,TFT_DARKGREY);
  dashboardDrawn=true;
  lastCurrent=lastPower=lastCO2=lastTemp=lastRH=lastWiFi=lastMQTT="";
}

static void updateValue(int y,const String &value,uint16_t color,String &cache){
  if(value==cache)return;
  cache=value;
  tft.setTextDatum(MR_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(color,TFT_BLACK);
  tft.setTextPadding(150);
  tft.drawString(value,308,y+15);
  tft.setTextPadding(0);
}

static void updateStatus(int x,int y,const String &value,uint16_t color,bool right,String &cache,int padding){
  if(value==cache)return;
  cache=value;
  tft.setTextDatum(right?MR_DATUM:ML_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(color,TFT_BLACK);
  tft.setTextPadding(padding);
  tft.drawString(value,x,y);
  tft.setTextPadding(0);
}

static void display(){
  if(!dashboardDrawn)drawStaticDashboard();

  String sCurrent=String(solarCurrentA,2)+" A";
  String sPower=String(solarPowerW,1)+" W";
  String sCO2=scd40Online?String(co2ppm)+" ppm":"-- ppm";
  String sTemp=!isnan(temperatureC)?String(temperatureC,1)+" C":"-- C";
  String sRH=!isnan(humidityRH)?String(humidityRH,1)+" %RH":"-- %RH";
  String sWiFi=WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():"WiFi OFFLINE";
  String sMQTT=mqtt.connected()?"MQTT ONLINE":"MQTT OFFLINE";

  updateValue(36,sCurrent,solarCurrentA>=0?TFT_CYAN:TFT_ORANGE,lastCurrent);
  updateValue(68,sPower,TFT_YELLOW,lastPower);
  updateValue(100,sCO2,co2Color(),lastCO2);
  updateValue(132,sTemp,TFT_SKYBLUE,lastTemp);
  updateValue(164,sRH,TFT_GREEN,lastRH);

  updateStatus(7,210,sWiFi,WiFi.status()==WL_CONNECTED?TFT_GREEN:TFT_RED,false,lastWiFi,145);
  updateStatus(313,210,sMQTT,mqtt.connected()?TFT_GREEN:TFT_ORANGE,true,lastMQTT,145);
}

void setup(){
  Serial.begin(115200);
  pinMode(PIN_TFT_BL,OUTPUT); digitalWrite(PIN_TFT_BL,HIGH);
  tft.init(); tft.setRotation(1);
  analogReadResolution(12); analogSetPinAttenuation(PIN_ACS712,ADC_11db);
  Wire.begin(PIN_SDA,PIN_SCL,100000);
  scd40Online=scd40.begin(Wire,true,true,false,true);
  wifiSetup(); mqttConfig();
  const float rawCurrentA=current();\n  if(isnan(filteredCurrentA)) filteredCurrentA=rawCurrentA;\n  else filteredCurrentA += CURRENT_FILTER_ALPHA*(rawCurrentA-filteredCurrentA);\n  if(fabsf(filteredCurrentA)<CURRENT_DEADBAND_A) filteredCurrentA=0.0f;\n  solarCurrentA=filteredCurrentA;\n  solarPowerW=solarCurrentA*SOLAR_BUS_VOLTS;
  display(); mqttConnect();
}

void loop(){
  uint32_t now=millis();
  if(webPortalRunning)wm.process();
  mqttService();
  solarCurrentA=current(); solarPowerW=solarCurrentA*SOLAR_BUS_VOLTS;

  if(now-lastSensorPoll>=SENSOR_POLL_INTERVAL_MS){
    lastSensorPoll=now;
    if(scd40Online&&scd40.getDataReadyStatus()&&scd40.readMeasurement()){
      co2ppm=scd40.getCO2(); temperatureC=scd40.getTemperature(); humidityRH=scd40.getHumidity();
    }
  }

  if(now-lastMqttPublish>=MQTT_PUBLISH_INTERVAL_MS){lastMqttPublish=now;mqttPublish();}
  if(now-lastDisplay>=DISPLAY_INTERVAL_MS){lastDisplay=now;display();}
  delay(10);
}
