#include <Arduino.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <ir_Midea.h>
#include <ArduinoJson.h>
#include <dhtnew.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include "ac_control.h"
#include "cloud_config.h"
#include "rules_config.h"
#include "web_page.h"

constexpr uint16_t IR_RECEIVE_PIN = 0;
constexpr uint16_t IR_SEND_PIN = 1;
constexpr uint16_t DHT_PIN = 2;
constexpr uint16_t CAPTURE_BUFFER_SIZE = 512;
constexpr uint8_t CAPTURE_TIMEOUT_MS = 50;
constexpr unsigned long SENSOR_READ_INTERVAL_MS = 2000;
constexpr unsigned long TCP_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long TCP_HEARTBEAT_INTERVAL_MS = 60000;
constexpr unsigned long WIFI_PROVISION_TIMEOUT_MS = 90000;
// This SuperMini repeatedly failed authentication at 20 dBm and connected at
// 8.5 dBm. Keep this configurable for boards with different radio hardware.
constexpr wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;
constexpr uint16_t BEMFA_TCP_PORT = 8344;
constexpr char BEMFA_TCP_HOST[] = "bemfa.com";
constexpr char BEMFA_SERVICE_UUID[] = "BEFA2000-BE35-4A5A-9C4A-BE0E02000000";
constexpr char BEMFA_RX_UUID[] = "BEFA2001-BE35-4A5A-9C4A-BE0E02000000";
constexpr char BEMFA_TX_UUID[] = "BEFA2002-BE35-4A5A-9C4A-BE0E02000000";

// Select the sensor model wired to GPIO2: 22 = DHT22, 11 = DHT11.
constexpr uint8_t DHT_TYPE = 22;

enum class SerialProvisionStep : uint8_t { Command, Ssid, Password };

WiFiClient bemfaClient;
DHTNEW dht(DHT_PIN);
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
AcState acState;
RuleEngine ruleEngine;
WebServer webServer(80);
bool webStarted = false;
bool sensorValid = false;
bool sensorHasReading = false;
float sensorTemperatureC = NAN;
float sensorHumidity = NAN;
int sensorError = -1;
unsigned long lastValidSensorMs = 0;
bool hasLastRule = false;
TemperatureRule lastRule;
unsigned long lastRuleMs = 0;
bool lastRuleSent = false;
String rulesWarning;
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
SerialProvisionStep serialProvisionStep = SerialProvisionStep::Command;
bool serialInputOverflow = false;
unsigned long wifiProvisionStartedMs = 0;
unsigned long wifiStartDueMs = 0;
unsigned long cloudStatusDueMs = 0;
int wifiProvisionSeq = 0;
int pendingHelloSeq = -1;

String bemfaPrivateKey;
String bemfaTopic;
String pendingWifiSsid;
String pendingWifiPassword;
String pendingCloudToken;
String tcpInput;
String serialInput;
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* bleTxCharacteristic = nullptr;
NimBLECharacteristic* bleRxCharacteristic = nullptr;

bool persistRules(const RuleSet& candidate, String& error);

const char* wifiAuthName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "OTHER";
  }
}

void configureWifiStation() {
  WiFi.mode(WIFI_STA);
  // Apply after every Wi-Fi restart, including serial and BLE provisioning.
  if (!WiFi.setTxPower(WIFI_TX_POWER)) {
    Serial.println("Could not apply Wi-Fi TX power limit");
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.printf("Wi-Fi associated: channel=%u, security=%s\n",
                  info.wifi_sta_connected.channel,
                  wifiAuthName(info.wifi_sta_connected.authmode));
    return;
  }
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("Wi-Fi connected: IP=%s, RSSI=%d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return;
  }
  if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;

  const uint8_t reason = info.wifi_sta_disconnected.reason;
  if (reason == 8) return;
  const char* hint = "Check the router, Wi-Fi band, and signal";
  switch (reason) {
    case 2:
      hint = "Authentication timed out or AP sent AUTH_EXPIRE";
      break;
    case 15:
    case 204:
      hint = "Handshake timed out; password may be wrong or signal weak";
      break;
    case 200:
      hint = "Beacon timeout; signal weak or access point unavailable";
      break;
    case 201:
      hint = "AP not found; check SSID, 2.4 GHz, signal, and password";
      break;
    case 202:
      hint = "Authentication failed; check password and router security";
      break;
    case 210:
    case 211:
      hint = "No compatible access point; check router security settings";
      break;
    case 212:
      hint = "Access point signal is below the minimum threshold";
      break;
  }

  Serial.printf("Wi-Fi disconnect reason=%u: %s\n", reason, hint);
}

void scanWifiNetworks() {
  wifiProvisioningPending = false;
  wifiProvisionReady = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, false);
  delay(100);
  configureWifiStation();

  Serial.println("Scanning nearby 2.4 GHz Wi-Fi networks...");
  const int count = WiFi.scanNetworks(false, true);
  if (count < 0) {
    Serial.printf("Wi-Fi scan failed (%d)\n", count);
  } else if (count == 0) {
    Serial.println("No Wi-Fi networks found");
  } else {
    for (int i = 0; i < count; ++i) {
      const String ssid = WiFi.SSID(i);
      Serial.printf("%2d. %s | RSSI %d dBm | channel %d | %s\n", i + 1,
                    ssid.c_str(), WiFi.RSSI(i),
                    WiFi.channel(i), wifiAuthName(WiFi.encryptionType(i)));
    }
  }
  WiFi.scanDelete();
  Serial.println("Send W to enter Wi-Fi credentials");
}

bool cloudCredentialsConfigured() {
  return bemfaPrivateKey.length() > 0 && bemfaTopic.length() > 0;
}

const char* acModeName() {
  return acState.mode == AcMode::Heat ? "HEAT" : "COOL";
}

void printAcStatus() {
  const char* power = acState.power == AcPower::On ? "ON" :
                      acState.power == AcPower::Off ? "OFF" : "UNKNOWN";
  Serial.printf("A/C commanded state: %s, %s, %u C, control=%s\n",
                power, acModeName(), acState.temperatureC,
                acState.automatic ? "AUTO" : "MANUAL");
}

void publishAcStatus() {
  if (!bemfaClient.connected()) return;

  String status;
  if (acState.power == AcPower::Off) {
    status = "off";
  } else if (acState.power == AcPower::On) {
    status = "on#" + String(static_cast<uint8_t>(acState.mode)) + "#" +
             String(acState.temperatureC) + "#0";
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
  int on, off;
  if (!legacyRuleIndexes(ruleEngine.rules, on, off)) return;

  char status[48];
  snprintf(status, sizeof(status), "threshold#%.1f#%.1f",
           ruleEngine.rules.items[on].threshold10 / 10.0,
           ruleEngine.rules.items[off].threshold10 / 10.0);
  bemfaClient.printf("cmd=2&uid=%s&topic=%s/up&msg=%s\r\n",
                     bemfaPrivateKey.c_str(), BEMFA_THRESHOLD_TOPIC, status);
  Serial.println("Bemfa thermostat thresholds sent");
}

void registerBemfaTopic() {
  if (!topicRegistrationPending || WiFi.status() != WL_CONNECTED) return;
  topicRegistrationPending = false;

  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
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

void sendOnCommand() {
  mideaAc.setUseCelsius(true);
  mideaAc.setMode(acState.mode == AcMode::Heat ? kMideaACHeat : kMideaACCool);
  mideaAc.setTemp(acState.temperatureC, true);
  mideaAc.setFan(kMideaACFanAuto);
  mideaAc.on();
  // Ignore our own emitter while transmitting; resume external reception after.
  irReceiver.pause();
  mideaAc.send();
  irReceiver.resume();

  Serial.printf("Sent Midea A/C: ON, %s, %u C, fan=AUTO\n",
                acModeName(), acState.temperatureC);
  acState.power = AcPower::On;
  publishAcStatus();
}

void sendOffCommand() {
  mideaAc.off();
  irReceiver.pause();
  mideaAc.send();
  irReceiver.resume();

  Serial.println("Sent Midea A/C command: OFF");
  acState.power = AcPower::Off;
  publishAcStatus();
}

bool handleAcCommand(const char* payload) {
  AcCommand command;
  if (!parseAcCommand(payload, command)) {
    Serial.println(
        "Invalid A/C command; use ON, OFF, COOL 26, HEAT 22, TEMP 24, AUTO, STATUS");
    return false;
  }

  const AcTransmission transmission = applyAcCommand(command, acState);
  if (command.action == AcAction::Automatic) ruleEngine.reset();
  if (transmission == AcTransmission::On) {
    sendOnCommand();
  } else if (transmission == AcTransmission::Off) {
    sendOffCommand();
  } else {
    publishAcStatus();
  }
  printAcStatus();
  return true;
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

  const String onText = command.substring(first + 1, second);
  const String offText = command.substring(second + 1);
  char* onEnd;
  char* offEnd;
  const double onThreshold = strtod(onText.c_str(), &onEnd);
  const double offThreshold = strtod(offText.c_str(), &offEnd);
  if (onText.isEmpty() || offText.isEmpty() || *onEnd || *offEnd ||
      !isfinite(onThreshold) || !isfinite(offThreshold) ||
      onThreshold < -10 || onThreshold > 50 || offThreshold < -10 ||
      offThreshold > 50 || onThreshold <= offThreshold ||
      fabs(onThreshold * 10 - round(onThreshold * 10)) > 0.0001 ||
      fabs(offThreshold * 10 - round(offThreshold * 10)) > 0.0001) {
    Serial.println("Invalid thresholds: require -10..50 C and ON > OFF");
    return;
  }

  int on, off;
  if (!legacyRuleIndexes(ruleEngine.rules, on, off)) {
    Serial.println("Legacy threshold rules changed or removed; configure rules on the web page");
    return;
  }
  RuleSet candidate = ruleEngine.rules;
  candidate.items[on].threshold10 = lround(onThreshold * 10);
  candidate.items[off].threshold10 = lround(offThreshold * 10);
  String error;
  if (!persistRules(candidate, error)) {
    Serial.println("Threshold save failed: " + error);
    return;
  }
  Serial.printf("Updated threshold rules: ON at %.1f C, OFF at %.1f C\n",
                onThreshold, offThreshold);
  publishThresholds();
}

#include "web_api.h"

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
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, false);
  delay(100);
  configureWifiStation();
  WiFi.setAutoReconnect(true);
  WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPassword.c_str());
  sendProvisionStatus(++wifiProvisionSeq, "wifi_connecting", 40);
}

void processSerialLine(const String& line) {
  if (serialInputOverflow) {
    Serial.println("Input too long; command ignored, serial Wi-Fi setup cancelled");
    serialProvisionStep = SerialProvisionStep::Command;
    return;
  }

  if (serialProvisionStep == SerialProvisionStep::Command) {
    if (line.equalsIgnoreCase("w")) {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(true, false);
      wifiProvisioningPending = false;
      serialProvisionStep = SerialProvisionStep::Ssid;
      Serial.println("Enter Wi-Fi SSID, then press Enter:");
    } else if (line.equalsIgnoreCase("l")) {
      scanWifiNetworks();
    } else if (line.equalsIgnoreCase("p")) {
      Serial.println("Starting Bemfa app provisioning mode");
      provisionFinished = false;
      provisionDoneSent = false;
      provisionRegistrationRequired = false;
      wifiProvisioningPending = false;
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(true, false);
      startBemfaBleProvisioning();
    } else if (line.equalsIgnoreCase("help") || line == "?") {
      Serial.println(
          "Wi-Fi: L = scan, W = setup, P = Bemfa BLE provisioning");
      Serial.println(
          "A/C: ON, OFF, COOL [17-30], HEAT [17-30], TEMP <17-30>, AUTO, STATUS");
    } else if (!line.isEmpty()) {
      handleAcCommand(line.c_str());
    }
    return;
  }

  if (serialProvisionStep == SerialProvisionStep::Ssid) {
    if (line.isEmpty() || line.length() > 32) {
      Serial.println("SSID must be 1-32 characters; enter W to try again");
      serialProvisionStep = SerialProvisionStep::Command;
      return;
    }
    pendingWifiSsid = line;
    serialProvisionStep = SerialProvisionStep::Password;
    Serial.println("Enter Wi-Fi password, then press Enter (blank for open Wi-Fi):");
    return;
  }

  if (line.length() > 64) {
    Serial.println("Wi-Fi password must be 64 characters or fewer; enter W to retry");
    serialProvisionStep = SerialProvisionStep::Command;
    return;
  }

  pendingWifiPassword = line;
  pendingCloudToken = preferences.getString("token", BEMFA_PRIVATE_KEY);
  if (pendingCloudToken.isEmpty()) pendingCloudToken = BEMFA_PRIVATE_KEY;
  wifiProvisioningPending = true;
  wifiProvisionReady = false;
  wifiStartDueMs = millis();
  firstAppWifiConfig = false;
  serialProvisionStep = SerialProvisionStep::Command;
  Serial.println("Connecting to Wi-Fi; credentials are not printed");
}

void processSerialInput() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;

    if (c == '\n') {
      processSerialLine(serialInput);
      serialInput = "";
      serialInputOverflow = false;
    } else if (serialInput.length() < 64) {
      serialInput += c;
    } else {
      serialInputOverflow = true;
    }
  }
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

  configureWifiStation();
  WiFi.setAutoReconnect(true);
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
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true, false);
    Serial.println(
        "Wi-Fi connection timed out; check the disconnect reason above and "
        "enter W to retry");
    sendProvisionStatus(++wifiProvisionSeq, "wifi_failed", 100, 1003,
                        "wifi_timeout");
  }
}

void connectBemfaTcp() {
  if (!cloudCredentialsConfigured() || WiFi.status() != WL_CONNECTED ||
      bemfaClient.connected()) {
    return;
  }

  if (!bemfaClient.connect(BEMFA_TCP_HOST, BEMFA_TCP_PORT, 1000)) {
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
  if (acState.power != AcPower::Unknown) publishAcStatus();
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
          handleAcCommand(command.c_str());
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
  Serial.printf("DHT type: DHT%u\n", DHT_TYPE);
  Serial.println(
      "Serial commands: L = scan Wi-Fi, W = Wi-Fi setup, P = Bemfa BLE provisioning");
  Serial.println(
      "A/C: ON, OFF, COOL 26, HEAT 22, TEMP 24, AUTO, STATUS; HELP for commands");

  preferences.begin("irac", false);
  bemfaTopic = BEMFA_AC_TOPIC;
  bemfaPrivateKey = preferences.getString("token", BEMFA_PRIVATE_KEY);
  if (bemfaPrivateKey.isEmpty()) bemfaPrivateKey = BEMFA_PRIVATE_KEY;

  loadRules();
  setupWebServer();

  // DHTNEW bounds pulse waits in microseconds; a missing pulse must not hold
  // interrupts off long enough to trigger the ESP32-C3 interrupt watchdog.
  dht.setType(DHT_TYPE);
  mideaAc.begin();
  irReceiver.enableIRIn();

  WiFi.onEvent(onWifiEvent);
  beginWifiProvisioning();
  if (!cloudCredentialsConfigured()) {
    Serial.println("Bemfa disabled: configure the private key and topic");
  }
}

void loop() {
  if (!webStarted && WiFi.status() == WL_CONNECTED) {
    webServer.begin();
    webStarted = true;
    Serial.printf("Web control: http://%s/\n", WiFi.localIP().toString().c_str());
  }
  if (webStarted) webServer.handleClient();
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
  if (!bemfaClient.connected() && cloudCredentialsConfigured() &&
      WiFi.status() == WL_CONNECTED &&
      now - lastTcpAttemptMs >= TCP_RETRY_INTERVAL_MS) {
    lastTcpAttemptMs = now;
    connectBemfaTcp();
  }

  if (now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    const int sensorStatus = dht.read();
    lastSensorReadMs = millis();
    const float humidity = dht.getHumidity();
    const float temperature = dht.getTemperature();

    sensorError = sensorStatus;
    sensorValid = sensorStatus == DHTLIB_OK && isfinite(temperature) &&
                  isfinite(humidity);
    if (!sensorValid) {
      Serial.printf("DHT read failed (%d); check sensor type and wiring\n",
                    sensorStatus);
    } else {
      sensorHasReading = true;
      lastValidSensorMs = millis();
      sensorTemperatureC = temperature;
      sensorHumidity = humidity;
      Serial.printf("Temperature: %.1f C, humidity: %.1f%%\n",
                    temperature, humidity);

      const int selected = ruleEngine.evaluate(lroundf(temperature * 10),
                                               true, acState.automatic);
      if (selected >= 0) {
        lastRule = ruleEngine.rules.items[selected];
        lastRuleMs = millis();
        hasLastRule = true;
        const AcTransmission transmission = applyRuleAction(lastRule, acState);
        lastRuleSent = transmission != AcTransmission::None;
        Serial.printf("Rule #%u triggered; IR %s\n", lastRule.id,
                      lastRuleSent ? "sending" : "unchanged, skipped");
        if (transmission == AcTransmission::On) sendOnCommand();
        else if (transmission == AcTransmission::Off) sendOffCommand();
      }
    }
  }

  if (irReceiver.decode(&irResults)) {
    Serial.printf("IR: 0x%llX, bits=%u, protocol=%d\n",
                  static_cast<unsigned long long>(irResults.value),
                  irResults.bits, irResults.decode_type);
    irReceiver.resume();
  }

  processSerialInput();
}
