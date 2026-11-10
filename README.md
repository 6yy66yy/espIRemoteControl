# ESP32-C3 Midea A/C controller

This ESP32-C3 sketch prints received IR codes as hexadecimal on GPIO0, reads a
DHT temperature/humidity sensor on GPIO2, and sends Midea A/C commands through
an IR emitter on GPIO1. It supports cloud commands for ON, OFF, cooling,
heating, and set temperature. A local web page provides manual controls and up
to ten temperature rules with configurable trigger/reset windows. Existing
ON/OFF thresholds migrate to cooling at 28 C and OFF at 24 C by default.

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
`DHT_TYPE = 22` to `DHT_TYPE = 11` in the sketch.

## Arduino IDE

1. Install the Espressif ESP32 board package (tested with **3.3.12**; its
   WebServer raw-body callbacks are required).
2. Install **IRremoteESP8266**, **DHTNEW** (tested with 0.5.5), **ArduinoJson 7**, and
   **NimBLE-Arduino** from Library Manager.
3. Open `ir_receive_demo/ir_receive_demo.ino`.
4. Select the matching ESP32-C3 board (for a generic board, use **ESP32C3 Dev Module**).
5. Select **No OTA (2MB APP/2MB SPIFFS)** as the partition scheme.
6. Upload, then open Serial Monitor at **115200 baud**.

Received IR messages are logged as hex, bit count, and protocol number only.
Temperature and humidity are printed every two seconds. The migrated cooling
rule sets 26 C and automatic fan. Cloud set temperatures are clamped to
17-30 C; cooling and heating use automatic fan.
Sensor pulse waits have timeouts; failed readings are logged and do not change
the commanded A/C state.

### A/C commands over USB serial

Send one command per line at **115200 baud**, with **Newline** selected.
These commands also work as Bemfa message payloads.

| Command | Action |
| --- | --- |
| `COOL 26` | Turn on cooling at 26 C |
| `HEAT 22` | Turn on heating at 22 C |
| `TEMP 24` | Change the setpoint to 24 C; retain the current mode and power |
| `ON` | Turn on with the most recently selected mode and setpoint |
| `OFF` | Turn off |
| `AUTO` | Resume the configured temperature rules |
| `STATUS` | Print commanded power, mode, setpoint, and AUTO/MANUAL |
| `HELP` | Print serial commands |

`COOL` and `HEAT` without a temperature retain the last setpoint. Valid integer
temperatures are clamped to 17-30 C. A manual power, mode, or temperature command
pauses the automatic thermostat until `AUTO` is sent. `TEMP` while off only
updates the next setpoint; it does not turn on the A/C. Invalid commands leave
the settings and automatic thermostat unchanged.

After a reboot, automatic rules are enabled; power starts as unknown until
this device sends an IR command. `AUTO` resets rule trigger states and evaluates
the rules at the next valid sensor reading. It does not change the saved rules.

## Web Controls and Temperature Rules

Connect a phone or computer to the same Wi-Fi as the ESP32. The serial log
prints `Web control: http://<device-IP>/` after connection. Open that address
to view temperature, humidity, last commanded A/C state, cloud connectivity,
and automatic/manual mode. The Chinese page works without internet access.
It provides ON/OFF, cooling/heating, 17-30 C adjustment, and resume AUTO.
Manual commands pause rules until AUTO is selected again. Temperature-only
adjustment does not power on an A/C that is off.

The rule editor supports up to ten rules, enable/disable, deletion, reordering,
and explicit saving. Unsaved edits do not affect the controller. Saving commits
the whole validated configuration to Preferences before activating it. Failed
saves retain the previous configuration. Empty rule lists are allowed.

Each rule has a stable ID, direction, trigger temperature, reset window,
action (OFF, Cool, Heat), and target temperature. Trigger temperatures are
-10 to 50 C, reset windows 0.1 to 10 C, both in 0.1 C increments. Targets are
integers from 17 to 30 C. The default window is 2 C.

| Rule | Fires once at | Rearms at |
| --- | --- | --- |
| Falling to 22 C, window 2 C | Temperature <= 22 C | Temperature >= 24 C |
| Rising to 28 C, window 2 C | Temperature >= 28 C | Temperature <= 26 C |

Rearming sends no IR. An OFF action requires its own rule. At boot, after
saving/enabling rules, and after AUTO, the first valid reading can trigger an
already satisfied condition. Trigger states live only in RAM. Sensor failures
neither fire nor rearm rules. Rules run without Wi-Fi or Bemfa connectivity.
When several rules fire on one reading, the first rule in the list wins and
all simultaneous matches enter the waiting-for-reset state. Commands identical
to the last commanded state skip IR transmission but still consume the trigger.
The page shows waiting/armed/paused states and the last rule event.

### Configuration migration and cloud compatibility

On first upgrade, the saved `on`/`off` thresholds migrate to rule ID 1 (rising,
Cool, 26 C) and ID 2 (falling, OFF), each with a 2 C reset window. Their previous
values are retained; 28/24 C are used if no valid thresholds were saved.
The original keys are kept as a recovery fallback. Invalid saved rule JSON is
not overwritten at boot; the page reports the fallback and an explicit save
replaces the invalid value.

`threshold#28#24` remains supported only while both migrated rules are enabled
and retain those directions/actions (and the 26 C cooling target). It changes
their thresholds, retaining order, windows, and other rules. If either rule was
deleted or repurposed, the command is refused with a serial message directing
you to the web page. Reusing IDs 1/2 for unrelated rules is discouraged.

### HTTP API

All responses are JSON except `/`. Configuration and control requests require
`Content-Type: application/json`. Body allocation is capped at 4096 bytes while
receiving. Invalid input returns an error without partially applying settings.

| Method / path | Request or response |
| --- | --- |
| `GET /api/status` | Sensor validity/age, commanded A/C state, AUTO, rule runtime, last event, network and heap metrics |
| `GET /api/rules` | `{ "version": 1, "rules": [...] }` |
| `PUT /api/rules` | Replace the validated rule list; returns the saved configuration |
| `POST /api/control` | `{ "command": "HEAT 22" }`; accepts existing serial/cloud A/C commands |

Example rule document:

```json
{
  "version": 1,
  "rules": [
    { "id": 3, "enabled": true, "direction": "falling", "thresholdC": 22,
      "windowC": 2, "action": "heat", "targetC": 24 }
  ]
}
```

IDs must be unique integers from 1 to 65535. Rule priority is array order.
An OFF rule still carries a valid `targetC`, which is ignored for transmission.
The HTTP page has no account system and is intended for the local Wi-Fi;
remote access continues through Bemfa Cloud.

### Reduced IR build

`ir_receive_demo/build_opt.h` applies library compiler flags through the ESP32
Arduino core. It disables other IR protocols and retains only **Midea 48-bit
sending/decoding** and **unknown-code hash reception**. Cooling, heating,
17-30 C adjustment, and explicit ON/OFF remain available. Other remotes are
printed as unknown hexadecimal hashes, not their original full IR data.
Do not remove this file when copying the sketch. No changes to the globally
installed IR library are needed. Wi-Fi, Bemfa BLE provisioning, and the DHT
sensor remain enabled.

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
the current Wi-Fi attempt and prints each SSID, signal strength, channel, and
security mode. The scan does not show or store passwords.

On connection failures, the serial log includes the ESP32 disconnect reason and
a hint. Reason 2 means authentication timed out or the AP reported AUTH_EXPIRE;
it does not establish that the password is wrong. An AP-not-found result suggests
checking the SSID, 2.4 GHz band, signal, and password. Handshake timeouts can be
caused by either a wrong
password or a weak signal. The sketch lets the Wi-Fi driver handle reconnects
instead of repeatedly reapplying configuration while a connection is in flight.

The tested ESP32-C3 SuperMini repeatedly failed authentication at its default
20 dBm transmit power and connected at 8.5 dBm with the same credentials.
`WIFI_TX_POWER` sets this limit after each Wi-Fi restart, for both saved
credentials and serial/BLE provisioning. Adjust it for a different board only
after testing connection stability; lower power can reduce range. Successful
connections print the IP address and signal strength.

- The device connects to `bemfa.com:8344` using the TCP protocol. Send these
payloads to the A/C topic's `/set`:
- `on#2#26#0`: cooling on, 26 C, automatic fan.
- `on#3#22#0`: heating on, 22 C, automatic fan.
- `off`: turn the A/C off.
- `on`: turn on with the last selected mode and setpoint.
- `auto`: return to sensor-based automatic control.
- `on#<mode>#<temperature>#0`: mode `2` is Cool, mode `3` is Heat; fan is
  always automatic. Empty mode or temperature fields retain their last values.
  Optional Bemfa fan/swing fields are accepted but do not change the IR settings.

While the migrated rule pair is retained, send `threshold#28#24` to the A/C
topic's `/set` to set the ON threshold to
28 C and the OFF threshold to 24 C. Values must be between -10 C and 50 C, with
ON greater than OFF. The ESP32 saves them across reboots. If a separate
thermostat topic is configured, active thresholds are reported to its `/up`.

The A/C `/up` status represents the last command sent by this device. Since an
IR emitter has no confirmation channel, this cannot prove that the physical A/C
received or acted on a command.

If the board uploads but no serial output appears over its native USB port,
enable **USB CDC On Boot** in the board options, then upload again.

## Native Regression Tests

Run `.\tests\run.ps1` with MinGW g++ and IRremoteESP8266 installed. Optional
`-Compiler` and `-IrLibrary` parameters select other installation paths.
The tests cover serial/cloud parsing, state preservation, invalid commands,
temperature limits, rule hysteresis and priority, migration and JSON validation,
all 17-30 C cooling/heating ON/OFF frames decoded from simulated IR pulses, and
unknown-code hash reception. They do not transmit IR.

`node tests/web_test.cjs` runs Playwright browser tests with a local mock device.
Install Playwright or set `NODE_PATH` to an existing installation; the default
browser is Microsoft Edge (`PLAYWRIGHT_CHANNEL` can override it). Tests cover
desktop/mobile layouts, editing/saving/errors, the ten-rule limit, manual/AUTO
controls, and offline state. Screenshots are written under the system temporary
directory, not committed to Git.

`tests/device_test.ps1 -BaseUrl http://<device-IP>` checks a flashed device's API,
invalid requests, the body limit, ten-rule persistence, and a 32-second status
poll. It sends OFF, temporarily saves disabled rules, restores the original
rules, and restores AUTO if it was initially enabled. It does not turn the A/C on.

`node tests/web_live_test.cjs http://<device-IP>` checks the actual device page
at desktop and mobile sizes, including sensor readings and cloud connectivity.
It takes screenshots without changing device settings.

Hardware verification on November 10, 2026: the firmware used 1,457,341 bytes
of the 2,097,152-byte application partition. During the 32-second API poll with
ten saved rules, free heap stayed at 151,996-152,060 bytes and the slowest HTTP
response was 48 ms. Ten saved rules survived a hardware reboot; Wi-Fi, Bemfa,
and sensor readings recovered. Live Bemfa OFF/TEMP/AUTO and legacy threshold
updates passed, and the legacy command did not overwrite a repurposed rule.
All 56 Midea frame tests and the native rule and browser regression tests passed.
