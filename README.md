# ESP32-2432S028 Solar / Air Monitor + MQTT

PlatformIO project for the ESP32-2432S028 ("Cheap Yellow Display") with:

- ACS712 solar current measurement
- SCD40 CO2
- SCD40 temperature
- SCD40 humidity
- Built-in ILI9341 display
- WiFiManager captive portal
- MQTT setup fields in the configuration portal
- MQTT settings saved in ESP32 NVS Preferences
- Automatic MQTT reconnect
- Retained individual MQTT telemetry topics
- Retained JSON state topic
- Normal web setup portal while connected

## First boot / Wi-Fi setup

If the ESP32 has no usable saved Wi-Fi credentials, it creates:

`Solar-Air-Setup`

Connect to that network from a phone, tablet, or computer.

The captive portal should open automatically. If it does not, browse to:

`192.168.4.1`

Select the Wi-Fi network and enter its password.

The setup page also has fields for:

- MQTT broker hostname or IP
- MQTT port
- MQTT username
- MQTT password
- MQTT base topic
- Device name

The MQTT settings are saved in ESP32 NVS flash and survive a restart.

## Setup after Wi-Fi is connected

While connected to the LAN, WiFiManager's web setup portal remains available at:

`http://<ESP32-IP>/`

The ESP32 display shows its IP address along the bottom.

This allows MQTT parameters to be changed later without recompiling the firmware.

## MQTT defaults

Default port: `1883`

Default base topic: `solarair`

Default device name is generated from the ESP32 chip ID, for example `solar-air-A1B2C3`.

## MQTT topic layout

```text
solarair/solar-air-A1B2C3/status
solarair/solar-air-A1B2C3/ip
solarair/solar-air-A1B2C3/solar/current_a
solarair/solar-air-A1B2C3/solar/power_w
solarair/solar-air-A1B2C3/air/co2_ppm
solarair/solar-air-A1B2C3/air/temperature_c
solarair/solar-air-A1B2C3/air/humidity_pct
solarair/solar-air-A1B2C3/wifi/rssi_dbm
solarair/solar-air-A1B2C3/state
```

Telemetry is published every 5 seconds.

## Wiring

### SCD40

| SCD40 | ESP32-2432S028 |
|---|---|
| VDD | 3.3 V |
| GND | GND |
| SDA | GPIO27 |
| SCL | GPIO22 |

SCD40 I2C address: `0x62`.

### ACS712

| ACS712 | ESP32-2432S028 |
|---|---|
| VCC | 5 V |
| GND | GND |
| OUT | resistor divider -> GPIO35 |

Do not connect a possible 0-5 V ACS712 output directly to GPIO35.

Suggested divider:

```text
ACS712 OUT
    |
   10k
    |
    +---------- GPIO35
    |
   20k
    |
   GND
```

## ACS712 selection

In `src/main.cpp`, set `ACS_SENSITIVITY_V_PER_A` to 0.185 for the 5A version, 0.100 for the 20A version, or 0.066 for the 30A version.

## Solar watts

The ACS712 measures current only. Current firmware estimates solar watts from current times the configured `SOLAR_BUS_VOLTS` value. For true instantaneous solar watts, add a protected voltage-divider input so the ESP32 measures actual solar voltage.
