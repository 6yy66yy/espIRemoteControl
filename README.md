# ESP32-C3 Midea A/C controller

This ESP32-C3 sketch prints received IR codes as hexadecimal on GPIO0, reads a
DHT temperature/humidity sensor on GPIO2, and sends Midea A/C commands through
an IR emitter on GPIO1. It supports cloud commands for ON, OFF, cooling,
heating, and set temperature. Local automatic control cools above 28 C and
turns the A/C off below 24 C; both thresholds can be changed from Bemfa Cloud.

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

1. Install the Espressif ESP32 board package.
2. Install **IRremoteESP8266**, **DHT sensor library**, **ArduinoJson**, and
   **NimBLE-Arduino** from Library Manager.
3. Open `ir_receive_demo/ir_receive_demo.ino`.
4. Select the matching ESP32-C3 board (for a generic board, use **ESP32C3 Dev Module**).
5. Select **No OTA (2MB APP/2MB SPIFFS)** as the partition scheme.
6. Upload, then open Serial Monitor at **115200 baud**.

Received IR messages are logged as hex, bit count, and protocol number only.
Temperature and humidity are printed every two seconds. The automatic cooling
command sets 26 C and automatic fan. Cloud set temperatures are clamped to
17-30 C; cooling and heating use automatic fan.

## Bemfa Cloud App Provisioning

Install **ArduinoJson** and **NimBLE-Arduino** from Library Manager. The ignored
`ir_receive_demo/cloud_config.local.h` holds the private key and A/C topic; never
commit that local file. On first boot, the controller advertises a BLE device
named `Bemfa_XXXX`. Use the Bemfa Cloud App's Bluetooth provisioning flow to
send the Wi-Fi credentials and account token. The device saves them locally,
registers the configured topic with Bemfa, and connects to the TCP device cloud.
The provided A/C topic must be an existing Bemfa TCP topic ending in `005`.

### Configure Wi-Fi over USB serial

1. Open Serial Monitor for the ESP32-C3 at **115200 baud** and select **Newline**
   as the line ending.
2. Send `W`.
3. Enter the Wi-Fi SSID and password on separate lines. Leave the password empty
   for an open network.
4. The device connects and saves credentials to ESP32 Preferences. The Bemfa
   private key is read from the ignored local config or the previously saved
   preferences; it is not requested over serial.

The SSID must be 1-32 characters and the password at most 64 characters. Use a
2.4 GHz Wi-Fi network. Serial commands are `L` to scan nearby Wi-Fi networks,
`W` for Wi-Fi setup, and `P` for Bemfa BLE provisioning. Scanning disconnects
the current Wi-Fi attempt and prints each SSID, signal strength, and whether
the network is open or secured. The scan does not show or store passwords.

On connection failures, the serial log includes the ESP32 disconnect reason and
a hint. Authentication failures suggest checking the password and router
security; an AP-not-found result suggests checking the SSID, 2.4 GHz band,
signal, and password. Handshake timeouts can be caused by either a wrong
password or a weak signal. The sketch lets the Wi-Fi driver handle reconnects
instead of repeatedly reapplying configuration while a connection is in flight.

- The device connects to `bemfa.com:8344` using the TCP protocol. Send these
payloads to the A/C topic's `/set`:
- `on#2#26#0`: cooling on, 26 C, automatic fan.
- `on#3#22#0`: heating on, 22 C, automatic fan.
- `off`: turn the A/C off.
- `auto`: return to sensor-based automatic control.
- `on#<mode>#<temperature>#0`: mode `2` is Cool, mode `3` is Heat; fan is
  always automatic.

Send `threshold#28#24` to the A/C topic's `/set` to set the ON threshold to
28 C and the OFF threshold to 24 C. Values must be between -10 C and 50 C, with
ON greater than OFF. The ESP32 saves them across reboots. If a separate
thermostat topic is configured, active thresholds are reported to its `/up`.

The A/C `/up` status represents the last command sent by this device. Since an
IR emitter has no confirmation channel, this cannot prove that the physical A/C
received or acted on a command.

If the board uploads but no serial output appears over its native USB port,
enable **USB CDC On Boot** in the board options, then upload again.
