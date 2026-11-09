#include <Arduino.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <ir_Midea.h>
#include <IRutils.h>
#include <DHT.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "cloud_config.h"

constexpr uint16_t IR_RECEIVE_PIN = 0;
constexpr uint16_t IR_SEND_PIN = 1;
constexpr uint16_t DHT_PIN = 2;
constexpr uint16_t CAPTURE_BUFFER_SIZE = 1024;
constexpr uint8_t CAPTURE_TIMEOUT_MS = 50;
// Defaults can be changed remotely with threshold#<on C>#<off C>.
float coolOnThresholdC = 28.0f;
float acOffThresholdC = 24.0f;
// Between the thresholds, keep the last commanded A/C state to prevent rapid
// on/off cycling.
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 2000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
constexpr uint16_t BEMFA_MQTT_PORT = 9501;
constexpr char BEMFA_MQTT_HOST[] = "bemfa.com";

// Select the sensor model wired to GPIO2: use DHT11 for a DHT11 module.
#define DHT_TYPE DHT22

enum class AcCommandedState : uint8_t { Unknown, Off, On };

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
DHT dht(DHT_PIN, DHT_TYPE);
Preferences preferences;
IRrecv irReceiver(
    IR_RECEIVE_PIN, CAPTURE_BUFFER_SIZE, CAPTURE_TIMEOUT_MS, true);
decode_results irResults;
IRMideaAC mideaAc(IR_SEND_PIN);

unsigned long lastSensorReadMs = 0;
unsigned long lastMqttAttemptMs = 0;
AcCommandedState acCommandedState = AcCommandedState::Unknown;
bool thermostatAutoEnabled = true;
uint8_t commandedMode = kMideaACCool;
uint8_t commandedTempC = 26;
uint8_t commandedFan = kMideaACFanAuto;

String bemfaSetTopic;
String bemfaUpTopic;
String bemfaThresholdSetTopic;
String bemfaThresholdUpTopic;

bool cloudCredentialsConfigured() {
  return strlen(WIFI_SSID) > 0 && strlen(BEMFA_PRIVATE_KEY) > 0 &&
         strlen(BEMFA_AC_TOPIC) > 0 && strlen(BEMFA_THRESHOLD_TOPIC) > 0;
}

uint8_t cloudModeToMidea(const int cloudMode) {
  switch (cloudMode) {
    case 1: return kMideaACAuto;
    case 2: return kMideaACCool;
    case 3: return kMideaACHeat;
    case 4: return kMideaACFan;
    case 5: return kMideaACDry;
    default: return kMideaACCool;
  }
}

int mideaModeToCloud(const uint8_t mode) {
  switch (mode) {
    case kMideaACAuto: return 1;
    case kMideaACCool: return 2;
    case kMideaACHeat: return 3;
    case kMideaACFan: return 4;
    case kMideaACDry: return 5;
    default: return 2;
  }
}

uint8_t cloudFanToMidea(const int cloudFan) {
  if (cloudFan <= 0) return kMideaACFanAuto;
  if (cloudFan == 1) return kMideaACFanLow;
  if (cloudFan == 2) return kMideaACFanMed;
  return kMideaACFanHigh;
}

int mideaFanToCloud(const uint8_t fan) {
  switch (fan) {
    case kMideaACFanLow: return 1;
    case kMideaACFanMed: return 2;
    case kMideaACFanHigh: return 3;
    default: return 0;
  }
}

void publishAcStatus() {
  if (!mqttClient.connected()) return;

  String status;
  if (acCommandedState == AcCommandedState::Off) {
    status = "off";
  } else if (acCommandedState == AcCommandedState::On) {
    status = "on#" + String(mideaModeToCloud(commandedMode)) + "#" +
             String(commandedTempC) + "#" + String(mideaFanToCloud(commandedFan));
  } else {
    status = "unknown";
  }

  if (!mqttClient.publish(bemfaUpTopic.c_str(), status.c_str(), true)) {
    Serial.println("Failed to publish A/C status to Bemfa");
  } else {
    Serial.println("Bemfa A/C status: " + status);
  }
}

void publishThresholds() {
  if (!mqttClient.connected()) return;

  char status[48];
  snprintf(status, sizeof(status), "threshold#%.1f#%.1f",
           coolOnThresholdC, acOffThresholdC);
  mqttClient.publish(bemfaThresholdUpTopic.c_str(), status, true);
  Serial.println("Bemfa thermostat thresholds: " + String(status));
}

void sendCoolingCommand() {
  mideaAc.setUseCelsius(true);
  mideaAc.setMode(commandedMode);
  mideaAc.setTemp(commandedTempC, true);
  mideaAc.setFan(commandedFan);
  mideaAc.on();
  mideaAc.send();

  Serial.println("Sent Midea A/C ON command");
  Serial.println(mideaAc.toString());
  acCommandedState = AcCommandedState::On;
  publishAcStatus();
}

void sendOffCommand() {
  mideaAc.off();
  mideaAc.send();

  Serial.println("Sent Midea A/C command: OFF");
  Serial.println(mideaAc.toString());
  acCommandedState = AcCommandedState::Off;
  publishAcStatus();
}

void handleBemfaCommand(const char* payload) {
  String command(payload);
  command.trim();
  command.toLowerCase();

  if (command == "auto") {
    thermostatAutoEnabled = true;
    Serial.println("Thermostat automatic mode enabled");
    publishAcStatus();
    return;
  }

  thermostatAutoEnabled = false;
  if (command == "off") {
    sendOffCommand();
    return;
  }

  if (!command.startsWith("on")) {
    Serial.println("Unsupported Bemfa command: " + command);
    return;
  }

  commandedMode = kMideaACCool;
  commandedTempC = 26;
  commandedFan = kMideaACFanAuto;

  int fieldStart = command.indexOf('#');
  if (fieldStart >= 0) {
    String fields[6];
    uint8_t fieldCount = 0;
    while (fieldStart >= 0 && fieldCount < 6) {
      const int next = command.indexOf('#', fieldStart + 1);
      fields[fieldCount++] = command.substring(
          fieldStart + 1, next < 0 ? command.length() : next);
      fieldStart = next;
    }

    if (fieldCount > 0 && fields[0].length() > 0) {
      commandedMode = cloudModeToMidea(fields[0].toInt());
    }
    if (fieldCount > 1 && fields[1].length() > 0) {
      commandedTempC = static_cast<uint8_t>(
          constrain(fields[1].toInt(), 17, 30));
    }
    if (fieldCount > 2 && fields[2].length() > 0) {
      commandedFan = cloudFanToMidea(fields[2].toInt());
    }
  }

  sendCoolingCommand();
}

void handleThresholdCommand(const char* payload) {
  String command(payload);
  command.trim();

  const int first = command.indexOf('#');
  const int second = first < 0 ? -1 : command.indexOf('#', first + 1);
  if (!command.startsWith("threshold#") || first < 0 || second < 0 ||
      command.indexOf('#', second + 1) >= 0) {
    Serial.println("Expected threshold#<on C>#<off C>");
    return;
  }

  const float onThreshold = command.substring(first + 1, second).toFloat();
  const float offThreshold = command.substring(second + 1).toFloat();
  if (onThreshold < -10.0f || onThreshold > 50.0f ||
      offThreshold < -10.0f || offThreshold > 50.0f ||
      onThreshold <= offThreshold) {
    Serial.println("Invalid thresholds: require -10..50 C and ON > OFF");
    return;
  }

  coolOnThresholdC = onThreshold;
  acOffThresholdC = offThreshold;
  preferences.putFloat("on", coolOnThresholdC);
  preferences.putFloat("off", acOffThresholdC);
  Serial.printf("Updated thresholds: ON above %.1f C, OFF below %.1f C\n",
                coolOnThresholdC, acOffThresholdC);
  publishThresholds();
}

void bemfaCallback(char* topic, byte* payload, unsigned int length) {
  char command[96];
  const unsigned int copyLength = min(length, sizeof(command) - 1);
  memcpy(command, payload, copyLength);
  command[copyLength] = '\0';
  Serial.println("Bemfa command: " + String(command));

  const String incomingTopic(topic);
  if (incomingTopic == String(BEMFA_AC_TOPIC)) {
    handleBemfaCommand(command);
  } else if (incomingTopic == String(BEMFA_THRESHOLD_TOPIC)) {
    handleThresholdCommand(command);
  }
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED || strlen(WIFI_SSID) == 0) return;

  Serial.print("Connecting to Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("Wi-Fi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi connection failed; will retry");
  }
}

void connectBemfaMqtt() {
  if (!cloudCredentialsConfigured() || WiFi.status() != WL_CONNECTED ||
      mqttClient.connected()) {
    return;
  }

  const String clientId = String(BEMFA_PRIVATE_KEY);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("Connected to Bemfa MQTT");
    mqttClient.subscribe(bemfaSetTopic.c_str());
    mqttClient.subscribe(bemfaThresholdSetTopic.c_str());
    publishAcStatus();
    publishThresholds();
  } else {
    Serial.printf("Bemfa MQTT connect failed, state=%d\n", mqttClient.state());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("ESP32-C3 IR A/C temperature controller ready");
  Serial.println("IR receive: GPIO0, IR transmit: GPIO1, DHT: GPIO2");
  Serial.println("DHT type: DHT22; ON above 28 C, OFF below 24 C");

  preferences.begin("irac", false);
  const float savedOnThreshold = preferences.getFloat("on", coolOnThresholdC);
  const float savedOffThreshold = preferences.getFloat("off", acOffThresholdC);
  if (savedOnThreshold > savedOffThreshold &&
      savedOnThreshold >= -10.0f && savedOnThreshold <= 50.0f &&
      savedOffThreshold >= -10.0f && savedOffThreshold <= 50.0f) {
    coolOnThresholdC = savedOnThreshold;
    acOffThresholdC = savedOffThreshold;
  }
  Serial.printf("Active thresholds: ON above %.1f C, OFF below %.1f C\n",
                coolOnThresholdC, acOffThresholdC);

  bemfaSetTopic = String(BEMFA_AC_TOPIC);
  bemfaUpTopic = String(BEMFA_AC_TOPIC) + "/up";
  bemfaThresholdSetTopic = String(BEMFA_THRESHOLD_TOPIC);
  bemfaThresholdUpTopic = String(BEMFA_THRESHOLD_TOPIC) + "/up";
  mqttClient.setServer(BEMFA_MQTT_HOST, BEMFA_MQTT_PORT);
  mqttClient.setCallback(bemfaCallback);
  mqttClient.setBufferSize(256);

  dht.begin();
  mideaAc.begin();
  irReceiver.enableIRIn();

  if (!cloudCredentialsConfigured()) {
    Serial.println("Bemfa disabled: fill in cloud_config.h first");
  } else {
    connectWifi();
    connectBemfaMqtt();
  }
}

void loop() {
  mqttClient.loop();

  if (cloudCredentialsConfigured() && !mqttClient.connected() &&
      millis() - lastMqttAttemptMs >= MQTT_RETRY_INTERVAL_MS) {
    lastMqttAttemptMs = millis();
    connectWifi();
    connectBemfaMqtt();
  }

  const unsigned long now = millis();
  if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = now;
    const float humidity = dht.readHumidity();
    const float temperature = dht.readTemperature();

    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("DHT read failed; check sensor type and wiring");
    } else {
      Serial.printf("Temperature: %.1f C, humidity: %.1f%%\n",
                    temperature, humidity);

      if (thermostatAutoEnabled) {
        if (temperature > coolOnThresholdC &&
            acCommandedState != AcCommandedState::On) {
          commandedMode = kMideaACCool;
          commandedTempC = 26;
          commandedFan = kMideaACFanAuto;
          sendCoolingCommand();
        } else if (temperature < acOffThresholdC &&
                   acCommandedState != AcCommandedState::Off) {
          sendOffCommand();
        }
      }
    }
  }

  if (irReceiver.decode(&irResults)) {
    if (irResults.overflow) {
      Serial.println("Warning: capture buffer overflow");
    }

    Serial.print(resultToHumanReadableBasic(&irResults));

    const String acDescription = IRAcUtils::resultAcToString(&irResults);
    if (acDescription.length() > 0) {
      Serial.println("A/C state: " + acDescription);
    }

    Serial.println(resultToSourceCode(&irResults));
    Serial.println();

    if (irResults.decode_type == MIDEA) {
      IRMideaAC receivedAc(IR_SEND_PIN);
      receivedAc.setRaw(irResults.value);
      if (receivedAc.getPower()) {
        acCommandedState = AcCommandedState::On;
        commandedMode = receivedAc.getMode();
        commandedTempC = receivedAc.getTemp(true);
        commandedFan = receivedAc.getFan();
      } else {
        acCommandedState = AcCommandedState::Off;
      }
      publishAcStatus();
    }

    irReceiver.resume();
  }
}
