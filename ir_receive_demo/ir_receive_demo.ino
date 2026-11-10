#include <Arduino.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <ir_Midea.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include "cloud_config.h"

constexpr uint16_t IR_RECEIVE_PIN = 0;
constexpr uint16_t IR_SEND_PIN = 1;
constexpr uint16_t DHT_PIN = 2;
constexpr uint16_t CAPTURE_BUFFER_SIZE = 512;
constexpr uint8_t CAPTURE_TIMEOUT_MS = 50;
// Defaults can be changed remotely with threshold#<on C>#<off C>.
float coolOnThresholdC = 28.0f;
float acOffThresholdC = 24.0f;
// Between the thresholds, keep the last commanded A/C state to prevent rapid
// on/off cycling.
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 2000;
constexpr unsigned long TCP_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long TCP_HEARTBEAT_INTERVAL_MS = 60000;
constexpr unsigned long WIFI_PROVISION_TIMEOUT_MS = 90000;
constexpr uint16_t BEMFA_TCP_PORT = 8344;
constexpr char BEMFA_TCP_HOST[] = "bemfa.com";
constexpr char BEMFA_SERVICE_UUID[] = "BEFA2000-BE35-4A5A-9C4A-BE0E02000000";
constexpr char BEMFA_RX_UUID[] = "BEFA2001-BE35-4A5A-9C4A-BE0E02000000";
constexpr char BEMFA_TX_UUID[] = "BEFA2002-BE35-4A5A-9C4A-BE0E02000000";

// Select the sensor model wired to GPIO2: use DHT11 for a DHT11 module.
#define DHT_TYPE DHT22

enum class AcCommandedState : uint8_t { Unknown, Off, On };

WiFiClient bemfaClient;
DHT dht(DHT_PIN, DHT_TYPE);
Preferences preferences;
IRrecv irReceiver(
    IR_RECEIVE_PIN, CAPTURE_BUFFER_SIZE, CAPTURE_TIMEOUT_MS, true);
decode_results irResults;
IRMideaAC mideaAc(IR_SEND_PIN);

void sendProvisionStatus(int seq, const char* stage, int progress,
                         int code = 0, const char* reason = nullptr);

unsigned long lastSensorReadMs = 0;
unsigned long lastTcpAttemptMs = 0;
unsigned long lastTcpHeartbeatMs = 0;
unsigned long lastWifiAttemptMs = 0;
AcCommandedState acCommandedState = AcCommandedState::Unknown;
bool thermostatAutoEnabled = true;
bool bleConnected = false;
bool bleTxSubscribed = false;
bool wifiProvisioningPending = false;
bool wifiProvisionReady = false;
bool bleHelloDone = false;
bool firstAppWifiConfig = false;
bool provisionRegistrationRequired = false;
bool provisionDoneSent = false;
bool provisionFinished = false;
bool topicRegistrationPending = false;
bool topicRegistrationSucceeded = false;
bool cloudConnectingStatusPending = false;
unsigned long wifiProvisionStartedMs = 0;
unsigned long wifiStartDueMs = 0;
unsigned long cloudStatusDueMs = 0;
int wifiProvisionSeq = 0;
int pendingHelloSeq = -1;
uint8_t commandedMode = kMideaACCool;
uint8_t commandedTempC = 26;

String bemfaPrivateKey;
String bemfaTopic;
String pendingWifiSsid;
String pendingWifiPassword;
String pendingCloudToken;
String tcpInput;
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleTxCharacteristic = nullptr;
NimBLECharacteristic* bleRxCharacteristic = nullptr;

bool cloudCredentialsConfigured() {
  return bemfaPrivateKey.length() > 0 && bemfaTopic.length() > 0;
}

uint8_t cloudModeToMidea(const int cloudMode) {
  return cloudMode == 3 ? kMideaACHeat : kMideaACCool;
}

int mideaModeToCloud(const uint8_t mode) {
  return mode == kMideaACHeat ? 3 : 2;
}

void publishAcStatus() {
  if (!bemfaClient.connected()) return;

  String status;
  if (acCommandedState == AcCommandedState::Off) {
    status = "off";
  } else if (acCommandedState == AcCommandedState::On) {
    status = "on#" + String(mideaModeToCloud(commandedMode)) + "#" +
             String(commandedTempC) + "#0";
  } else {
    status = "unknown";
  }

  const String message = "cmd=2&uid=" + bemfaPrivateKey + "&topic=" +
                         bemfaTopic + "/up&msg=" + status + "\r\n";
  bemfaClient.print(message);
  Serial.println("Bemfa A/C status sent: " + status);
}

void publishThresholds() {
  if (!bemfaClient.connected() || strlen(BEMFA_THRESHOLD_TOPIC) == 0) return;

  char status[48];
  snprintf(status, sizeof(status), "threshold#%.1f#%.1f",
           coolOnThresholdC, acOffThresholdC);
  bemfaClient.printf("cmd=2&uid=%s&topic=%s/up&msg=%s\r\n",
                     bemfaPrivateKey.c_str(), BEMFA_THRESHOLD_TOPIC, status);
  Serial.println("Bemfa thermostat thresholds sent");
}

void registerBemfaTopic() {
  if (!topicRegistrationPending || WiFi.status() != WL_CONNECTED) return;
  topicRegistrationPending = false;

  HTTPClient http;
  if (!http.begin("http://pro.bemfa.com/vs/web/v1/deviceAddTopic")) {
    Serial.println("Bemfa device registration could not start");
    return;
  }
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> request;
  request["uid"] = bemfaPrivateKey;
  request["name"] = "ESP32 Midea AC";
  request["topic"] = bemfaTopic;
  request["type"] = 3;
  request["adminID"] = 0;
  request["wifiConfig"] = 2;
  request["unCreate"] = 1;
  String body;
  serializeJson(request, body);

  const int httpCode = http.POST(body);
  bool registered = false;
  if (httpCode > 0) {
    StaticJsonDocument<256> response;
    const DeserializationError error = deserializeJson(response, http.getString());
    const int resultCode =
        response["data"]["code"] | response["code"] | -1;
    registered = !error && (resultCode == 0 || resultCode == 40006);
  }
  http.end();
  topicRegistrationSucceeded = registered;
  Serial.println(registered ? "Bemfa app device registration confirmed"
                            : "Bemfa app device registration failed");
  if (!registered && bleConnected) {
    sendProvisionStatus(wifiProvisionSeq++, "cloud_failed", 100, 2001,
                        "cloud_failed");
  }
}

void sendCoolingCommand() {
  mideaAc.setUseCelsius(true);
  mideaAc.setMode(commandedMode);
  mideaAc.setTemp(commandedTempC, true);
  mideaAc.setFan(kMideaACFanAuto);
  mideaAc.on();
  mideaAc.send();

  Serial.println("Sent Midea A/C ON command");
  acCommandedState = AcCommandedState::On;
  publishAcStatus();
}

void sendOffCommand() {
  mideaAc.off();
  mideaAc.send();

  Serial.println("Sent Midea A/C command: OFF");
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

void notifyProvisionJson(JsonDocument& doc) {
  if (!bleTxCharacteristic || !bleConnected || !bleTxSubscribed) return;
  String payload;
  serializeJson(doc, payload);
  bleTxCharacteristic->setValue(payload.c_str());
  bleTxCharacteristic->notify();
}

void sendHello(const int seq) {
  StaticJsonDocument<192> doc;
  doc["cmd"] = "hello";
  doc["seq"] = seq;
  doc["code"] = 0;
  doc["ver"] = 2;
  doc["device_id"] = BEMFA_AC_TOPIC;
  doc["device_type"] = 5;
  doc["name"] = "ESP32 Midea AC";
  JsonArray cap = doc["cap"].to<JsonArray>();
  cap.add("wifi");
  cap.add("cloud");
  notifyProvisionJson(doc);
}

void sendProvisionStatus(const int seq, const char* stage, const int progress,
                         const int code, const char* reason) {
  StaticJsonDocument<192> doc;
  doc["cmd"] = "status";
  doc["seq"] = seq;
  doc["code"] = code;
  doc["stage"] = stage;
  doc["progress"] = progress;
  if (reason) doc["reason"] = reason;
  if (strcmp(stage, "wifi_connected") == 0) {
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
  }
  notifyProvisionJson(doc);
}

void sendProvisionError(const int seq, const int code, const char* reason) {
  StaticJsonDocument<128> doc;
  doc["cmd"] = "error";
  doc["seq"] = seq;
  doc["code"] = code;
  doc["reason"] = reason;
  notifyProvisionJson(doc);
}

void startWifiFromProvisioning() {
  wifiProvisionReady = true;
  wifiProvisionStartedMs = millis();
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
  sendProvisionStatus(++wifiProvisionSeq, "wifi_connecting", 40);
}

class BemfaBleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    bleConnected = true;
    bleTxSubscribed = false;
    bleHelloDone = false;
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    bleConnected = false;
    bleTxSubscribed = false;
    bleHelloDone = false;
    if (!provisionFinished) NimBLEDevice::startAdvertising();
  }
};

class BemfaBleTxCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&,
                   uint16_t value) override {
    bleTxSubscribed = value != 0;
    if (bleTxSubscribed && pendingHelloSeq >= 0) {
      sendHello(pendingHelloSeq);
      pendingHelloSeq = -1;
    }
  }
};

class BemfaBleRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic,
               NimBLEConnInfo& connInfo) override {
    const std::string raw = characteristic->getValue();
    StaticJsonDocument<512> doc;
    const DeserializationError error = deserializeJson(doc, raw);
    const int seq = doc["seq"] | 0;
    if (error) {
      sendProvisionError(seq, 3002, "invalid_json");
      return;
    }

    const char* command = doc["cmd"] | "";
    if (strcmp(command, "hello") == 0) {
      bleHelloDone = true;
      pendingHelloSeq = seq;
      if (bleTxSubscribed) {
        sendHello(seq);
        pendingHelloSeq = -1;
      }
      return;
    }

    if (strcmp(command, "info") == 0) {
      StaticJsonDocument<192> response;
      response["cmd"] = "info";
      response["seq"] = seq;
      response["code"] = 0;
      response["device_id"] = BEMFA_AC_TOPIC;
      response["product_id"] = BEMFA_AC_TOPIC;
      response["fw"] = "3.1";
      response["hw"] = "ESP32-C3";
      response["mac"] = String(static_cast<uint32_t>(ESP.getEfuseMac() >> 16), HEX);
      notifyProvisionJson(response);
      return;
    }

    if (strcmp(command, "wifi") == 0) {
      if (!bleHelloDone) {
        sendProvisionError(seq, 3001, "invalid_state");
        return;
      }
      const char* ssid = doc["ssid"] | "";
      const char* password = doc["password"] | "";
      const char* token = doc["token"] | "";
      if (strlen(ssid) == 0 || strlen(token) == 0) {
        sendProvisionError(seq, 3002, "invalid_json");
        return;
      }

      pendingWifiSsid = ssid;
      pendingWifiPassword = password;
      pendingCloudToken = token;
      wifiProvisionSeq = seq;
      wifiProvisioningPending = true;
      wifiProvisionReady = false;
      wifiStartDueMs = millis() + 250;
      firstAppWifiConfig = true;

      StaticJsonDocument<128> response;
      response["cmd"] = "wifi";
      response["seq"] = seq;
      response["code"] = 0;
      response["accepted"] = true;
      notifyProvisionJson(response);
      return;
    }

    if (strcmp(command, "finish") == 0) {
      StaticJsonDocument<128> response;
      response["cmd"] = "finish";
      response["seq"] = seq;
      response["code"] = 0;
      response["stage"] = "done";
      notifyProvisionJson(response);
      delay(200);
      provisionFinished = true;
      NimBLEDevice::getAdvertising()->stop();
      if (bleConnected) {
        NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      }
      return;
    }

    sendProvisionError(seq, 3003, "unsupported_cmd");
  }
};

void startBemfaBleProvisioning() {
  if (bleServer) return;

  const uint64_t chipId = ESP.getEfuseMac();
  const uint16_t shortCode = static_cast<uint16_t>(chipId & 0xFFFF);
  char deviceName[20];
  snprintf(deviceName, sizeof(deviceName), "Bemfa_%04X", shortCode);

  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(185);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new BemfaBleServerCallbacks());
  NimBLEService* service = bleServer->createService(BEMFA_SERVICE_UUID);
  bleTxCharacteristic = service->createCharacteristic(
      BEMFA_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  bleTxCharacteristic->setCallbacks(new BemfaBleTxCallbacks());
  bleRxCharacteristic = service->createCharacteristic(
      BEMFA_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  bleRxCharacteristic->setCallbacks(new BemfaBleRxCallbacks());
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  NimBLEAdvertisementData scanResponseData;
  advData.setFlags(0x06);
  const uint8_t manufacturerData[] = {
      10, 0xFF, 0xFA, 0xBE, 0x02, 0x00, 0x05, 0x01, 0x03,
      static_cast<uint8_t>(shortCode & 0xFF),
      static_cast<uint8_t>((shortCode >> 8) & 0xFF)};
  advData.addData(manufacturerData, sizeof(manufacturerData));
  scanResponseData.setName(deviceName);
  scanResponseData.setCompleteServices(NimBLEUUID(BEMFA_SERVICE_UUID));
  advertising->setAdvertisementData(advData);
  advertising->setScanResponseData(scanResponseData);
  advertising->start();
  Serial.println(String("Bemfa BLE provisioning ready: ") + deviceName);
}

void beginWifiProvisioning() {
  pendingWifiSsid = preferences.getString("ssid", WIFI_SSID);
  pendingWifiPassword = preferences.getString("pass", WIFI_PASSWORD);
  pendingCloudToken = preferences.getString("token", BEMFA_PRIVATE_KEY);
  if (pendingWifiSsid.isEmpty()) {
    startBemfaBleProvisioning();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
}

void processProvisioning() {
  if (!wifiProvisioningPending) return;

  if (!wifiProvisionReady) {
    if (millis() < wifiStartDueMs) return;
    wifiProvisionReady = true;
    startWifiFromProvisioning();
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiProvisioningPending = false;
    pendingWifiSsid.trim();
    preferences.putString("ssid", pendingWifiSsid);
    preferences.putString("pass", pendingWifiPassword);
    preferences.putString("token", pendingCloudToken);
    bemfaPrivateKey = pendingCloudToken;
    Serial.println("Wi-Fi provisioning succeeded");
    sendProvisionStatus(++wifiProvisionSeq, "wifi_connected", 100);
    cloudConnectingStatusPending = true;
    cloudStatusDueMs = millis() + 250;
    if (firstAppWifiConfig) {
      topicRegistrationPending = true;
      provisionRegistrationRequired = true;
      firstAppWifiConfig = false;
    }
    return;
  }

  if (millis() - wifiProvisionStartedMs > WIFI_PROVISION_TIMEOUT_MS) {
    wifiProvisioningPending = false;
    wifiProvisionReady = false;
    sendProvisionStatus(++wifiProvisionSeq, "wifi_failed", 100, 1003,
                        "wifi_timeout");
  }
}

void connectBemfaTcp() {
  if (!cloudCredentialsConfigured() || WiFi.status() != WL_CONNECTED ||
      bemfaClient.connected()) {
    return;
  }

  if (!bemfaClient.connect(BEMFA_TCP_HOST, BEMFA_TCP_PORT)) {
    Serial.println("Bemfa TCP connection failed");
    if (bleConnected) {
      sendProvisionStatus(wifiProvisionSeq, "cloud_failed", 100, 2001,
                          "cloud_connect_failed");
    }
    return;
  }

  bemfaClient.printf("cmd=1&uid=%s&topic=%s\r\n",
                     bemfaPrivateKey.c_str(), bemfaTopic.c_str());
  lastTcpHeartbeatMs = millis();
  Serial.println("Connected to Bemfa TCP cloud");
  if (acCommandedState != AcCommandedState::Unknown) publishAcStatus();
  publishThresholds();
}

void processBemfaTcp() {
  if (!bemfaClient.connected()) return;

  while (bemfaClient.available()) {
    const char c = static_cast<char>(bemfaClient.read());
    if (c == '\n') {
      tcpInput.trim();
      const int msgAt = tcpInput.indexOf("&msg=");
      if (msgAt >= 0) {
        String command = tcpInput.substring(msgAt + 5);
        Serial.println("Bemfa command received");
        if (command.startsWith("threshold#")) {
          handleThresholdCommand(command.c_str());
        } else {
          handleBemfaCommand(command.c_str());
        }
      }
      tcpInput = "";
    } else if (tcpInput.length() < 512) {
      tcpInput += c;
    } else {
      tcpInput = "";
      Serial.println("Discarded oversized Bemfa TCP frame");
    }
  }

  if (millis() - lastTcpHeartbeatMs >= TCP_HEARTBEAT_INTERVAL_MS) {
    bemfaClient.print("ping\r\n");
    lastTcpHeartbeatMs = millis();
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
  bemfaTopic = BEMFA_AC_TOPIC;
  bemfaPrivateKey = preferences.getString("token", BEMFA_PRIVATE_KEY);
  if (bemfaPrivateKey.isEmpty()) bemfaPrivateKey = BEMFA_PRIVATE_KEY;

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

  dht.begin();
  mideaAc.begin();
  irReceiver.enableIRIn();

  beginWifiProvisioning();
  if (!cloudCredentialsConfigured()) {
    Serial.println("Bemfa disabled: configure the private key and topic");
  }
}

void loop() {
  processProvisioning();
  if (cloudConnectingStatusPending && millis() >= cloudStatusDueMs) {
    cloudConnectingStatusPending = false;
    sendProvisionStatus(++wifiProvisionSeq, "cloud_connecting", 100);
  }
  processBemfaTcp();
  registerBemfaTopic();
  if (bleConnected && !provisionDoneSent && !wifiProvisioningPending &&
      !cloudConnectingStatusPending && bemfaClient.connected() &&
      (!provisionRegistrationRequired || topicRegistrationSucceeded)) {
    sendProvisionStatus(++wifiProvisionSeq, "done", 100);
    provisionDoneSent = true;
  }

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && !wifiProvisioningPending &&
      !pendingWifiSsid.isEmpty() &&
      now - lastWifiAttemptMs >= 15000) {
    lastWifiAttemptMs = now;
    WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
  }
  if (!bemfaClient.connected() && cloudCredentialsConfigured() &&
      WiFi.status() == WL_CONNECTED &&
      now - lastTcpAttemptMs >= TCP_RETRY_INTERVAL_MS) {
    lastTcpAttemptMs = now;
    connectBemfaTcp();
  }

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
          sendCoolingCommand();
        } else if (temperature < acOffThresholdC &&
                   acCommandedState != AcCommandedState::Off) {
          sendOffCommand();
        }
      }
    }
  }

  if (irReceiver.decode(&irResults)) {
    Serial.printf("IR: 0x%llX, bits=%u, protocol=%d\n",
                  static_cast<unsigned long long>(irResults.value),
                  irResults.bits, irResults.decode_type);
    irReceiver.resume();
  }

  if (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    if (command == 'p' || command == 'P') {
      Serial.println("Starting Bemfa app provisioning mode");
      provisionFinished = false;
      provisionDoneSent = false;
      provisionRegistrationRequired = false;
      wifiProvisioningPending = false;
      WiFi.disconnect();
      startBemfaBleProvisioning();
    }
  }
}
