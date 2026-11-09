# ESP32-C3 IR receive demo

This ESP32-C3 sketch reads an IR receiver on GPIO0, a DHT temperature/humidity
sensor on GPIO2, and sends Midea A/C commands through an IR emitter on GPIO1.
When a valid temperature reading rises above 28 C, it sends an ON / COOL / 26 C /
AUTO fan command. Below 24 C, it sends an OFF command. Between the two thresholds,
it keeps the last commanded state to prevent rapid switching. Both thresholds can
be changed remotely through a separate Bemfa Cloud topic.

## Wiring

- IR receiver `VCC` / `+` -> ESP32-C3 `3V3`
- IR receiver `GND` / `-` -> ESP32-C3 `GND`
- IR receiver `OUT` / `S` -> ESP32-C3 `GPIO0`
- DHT sensor `VCC` -> the voltage specified for the sensor module
- DHT sensor `GND` -> ESP32-C3 `GND`
- DHT sensor `DATA` / `S` -> ESP32-C3 `GPIO2`
- IR emitter module signal input -> ESP32-C3 `GPIO1`
- IR emitter module ground -> ESP32-C3 `GND`

Check the labels on your receiver module; pin order varies between modules.
Use an IR transmitter module with a driver stage, or a transistor driver for a
bare IR LED. Do not drive a bare IR LED directly from the ESP32-C3 GPIO.
All modules must share a common ground.

The sketch defaults to DHT22. If your sensor is DHT11, change
`#define DHT_TYPE DHT22` to `#define DHT_TYPE DHT11` in the sketch.

## Arduino IDE

1. Install the Espressif ESP32 board package, version 2.0.2 or newer.
2. Install **IRremoteESP8266** by crankyoldgit and **DHT sensor library** by
   Adafruit from Library Manager.
3. Open `ir_receive_demo/ir_receive_demo.ino`.
4. Select the matching ESP32-C3 board (for a generic board, use **ESP32C3 Dev Module**).
5. Upload, then open Serial Monitor at **115200 baud**.

Open Serial Monitor at **115200 baud**. Temperature and humidity are printed
every two seconds. Default thresholds are 28 C to turn cooling on and 24 C to
turn the A/C off. The on command always sets cooling mode to 26 C with automatic
fan speed.

The receiver continues to print decoded A/C state and source-code
representations for signals captured on GPIO0.

## Bemfa Cloud MQTT

Install **PubSubClient** by Nick O'Leary, then fill in the Wi-Fi and Bemfa values
in `ir_receive_demo/cloud_config.h`. Keep the private key out of public source
control. Create two distinct Bemfa topics: an A/C topic ending in `005` (for
example `aircon005`) and a thermostat topic (for example `climatecfg010`).
The device connects to `bemfa.com:9501` and subscribes to each base topic. Cloud
push commands are sent to the topic's `/set`; the device publishes retained
status to `/up`.

Send these payloads to the A/C topic's `/set`:

- `on#2#26#0`: cooling on, 26 C, automatic fan.
- `off`: turn the A/C off.
- `auto`: return to sensor-based automatic control.
- `on#<mode>#<temperature>#<fan>`: mode values are 1 Auto, 2 Cool, 3 Heat,
  4 Fan, 5 Dry; fan values are 0 Auto, 1 Low, 2 Medium, 3 High.

Send `threshold#28#24` to the configuration topic's `/set` to set the ON
threshold to 28 C and the OFF threshold to 24 C. The device accepts values from
-10 C to 50 C only when ON is greater than OFF, then reports the active values
as `threshold#28.0#24.0` to the configuration topic's `/up`. The ESP32 saves
the values so they survive reboot.

The A/C `/up` status represents the last command sent by this device or a Midea
state decoded from the IR receiver. Since an IR emitter has no confirmation
channel, it cannot prove that the physical A/C received or acted on a command.

If the board uploads but no serial output appears over its native USB port,
enable **USB CDC On Boot** in the board options, then upload again.
