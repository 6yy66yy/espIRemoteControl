#ifndef WEB_API_H
#define WEB_API_H

String webRequestBody;
bool webRequestTooLarge = false;
bool webRequestAllocationFailed = false;

void receiveWebBody() {
  if (!webServer.hasRaw()) return;
  const HTTPRaw& raw = webServer.raw();
  if (raw.status == RAW_START) {
    webRequestBody = "";
    webRequestTooLarge = false;
    webRequestAllocationFailed = false;
    webServer.client().setTimeout(1000);
  } else if (raw.status == RAW_WRITE) {
    // Bound allocation while receiving, before any JSON parsing takes place.
    if (raw.totalSize > MAX_RULE_JSON_BYTES) webRequestTooLarge = true;
    if (!webRequestTooLarge && !webRequestAllocationFailed &&
        !webRequestBody.concat(reinterpret_cast<const char*>(raw.buf), raw.currentSize))
      webRequestAllocationFailed = true;
  } else if (raw.status == RAW_ABORTED) {
    webRequestBody = "";
  }
}

void sendWebJson(int code, JsonDocument& doc) {
  String body;
  serializeJson(doc, body);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(code, "application/json; charset=utf-8", body);
}

void sendWebError(int code, const char* message) {
  StaticJsonDocument<512> doc;
  doc["error"] = message;
  sendWebJson(code, doc);
}

bool persistRules(const RuleSet& candidate, String& error) {
  const char* validation = validateRules(candidate);
  if (validation) {
    error = validation;
    return false;
  }
  JsonDocument doc;
  encodeRules(doc, candidate);
  String json;
  serializeJson(doc, json);
  if (doc.overflowed() || json.length() != measureJson(doc) ||
      json.length() > MAX_RULE_JSON_BYTES) {
    error = "规则配置过大或内存不足";
    return false;
  }
  // Replace the live rules only after the entire NVS value is committed.
  if (preferences.putString("rules", json) != json.length()) {
    error = "保存失败，原规则未修改";
    return false;
  }
  ruleEngine.rules = candidate;
  ruleEngine.reset();
  rulesWarning = "";
  Serial.printf("Saved %u temperature rules\n", candidate.count);
  return true;
}

void loadRules() {
  float on = preferences.getFloat("on", 28);
  float off = preferences.getFloat("off", 24);
  if (!isfinite(on) || !isfinite(off) || on <= off || on < -10 || on > 50 ||
      off < -10 || off > 50) {
    on = 28;
    off = 24;
  }
  ruleEngine.rules = legacyRules(lroundf(on * 10), lroundf(off * 10));
  if (preferences.isKey("rules")) {
    const String json = preferences.getString("rules");
    JsonDocument doc;
    const DeserializationError parse = deserializeJson(
        doc, json, DeserializationOption::NestingLimit(6));
    RuleSet stored;
    const char* error = parse ? "保存的规则无法解析" :
                        decodeRules(doc.as<JsonVariantConst>(), stored);
    if (json.length() <= MAX_RULE_JSON_BYTES && !error) {
      ruleEngine.rules = stored;
    } else {
      // Retain the bad stored value until an explicit save, so boot does not
      // silently destroy configuration that could be recovered later.
      rulesWarning = "保存的规则无效，临时使用原有温控规则；请检查并保存";
      Serial.println("Invalid saved rules; using legacy fallback without overwriting NVS");
    }
  } else {
    String error;
    if (!persistRules(ruleEngine.rules, error)) rulesWarning = error;
  }
  ruleEngine.reset();
  Serial.printf("Loaded %u temperature rules\n", ruleEngine.rules.count);
}

void sendWebRules() {
  JsonDocument doc;
  encodeRules(doc, ruleEngine.rules);
  if (!rulesWarning.isEmpty()) doc["warning"] = rulesWarning;
  sendWebJson(200, doc);
}

void sendWebStatus() {
  JsonDocument doc;
  const unsigned long now = millis();
  doc["automatic"] = acState.automatic;
  doc["cloudConnected"] = bemfaClient.connected();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["uptimeMs"] = now;
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["minFreeHeap"] = ESP.getMinFreeHeap();
  doc["rulesWarning"] = rulesWarning;
  JsonObject ac = doc["ac"].to<JsonObject>();
  ac["power"] = acState.power == AcPower::On ? "on" :
                acState.power == AcPower::Off ? "off" : "unknown";
  ac["mode"] = acState.mode == AcMode::Heat ? "heat" : "cool";
  ac["targetC"] = acState.temperatureC;
  JsonObject sensor = doc["sensor"].to<JsonObject>();
  const bool valid = sensorValid && now - lastValidSensorMs < 10000;
  sensor["valid"] = valid;
  sensor["code"] = sensorError;
  if (sensorHasReading) sensor["ageMs"] = now - lastValidSensorMs;
  else sensor["ageMs"] = nullptr;
  if (valid) {
    sensor["temperatureC"] = sensorTemperatureC;
    sensor["humidity"] = sensorHumidity;
  } else {
    sensor["temperatureC"] = nullptr;
    sensor["humidity"] = nullptr;
    sensor["error"] = sensorHasReading ? "传感器读数失效" : "等待有效传感器读数";
  }
  JsonArray runtime = doc["rules"].to<JsonArray>();
  for (uint8_t i = 0; i < ruleEngine.rules.count; ++i) {
    JsonObject value = runtime.add<JsonObject>();
    value["id"] = ruleEngine.rules.items[i].id;
    value["waiting"] = ruleEngine.waiting[i];
  }
  if (hasLastRule) {
    JsonObject event = doc["lastRule"].to<JsonObject>();
    event["id"] = lastRule.id;
    event["action"] = lastRule.action == RuleAction::Off ? "off" :
                      lastRule.action == RuleAction::Heat ? "heat" : "cool";
    event["targetC"] = lastRule.targetC;
    event["sent"] = lastRuleSent;
    event["ageMs"] = now - lastRuleMs;
  } else {
    doc["lastRule"] = nullptr;
  }
  sendWebJson(200, doc);
}

bool readWebJson(JsonDocument& doc) {
  // Only the browser's same-origin JSON API is used; no form mutations.
  if (!webServer.header("Content-Type").startsWith("application/json")) {
    sendWebError(415, "请发送application/json格式");
    return false;
  }
  if (webRequestTooLarge) {
    sendWebError(413, "请求不能超过4 KiB");
    return false;
  }
  if (webRequestAllocationFailed) {
    sendWebError(503, "内存不足，请稍后重试");
    return false;
  }
  const DeserializationError error = deserializeJson(
      doc, webRequestBody, DeserializationOption::NestingLimit(6));
  if (error) {
    sendWebError(400, "JSON格式错误或请求过大");
    return false;
  }
  return true;
}

void setupWebServer() {
  const char* headers[] = {"Content-Type"};
  webServer.collectHeaders(headers, 1);
  webServer.on("/", HTTP_GET, []() {
    webServer.sendHeader("Cache-Control", "no-cache");
    webServer.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });
  webServer.on("/api/status", HTTP_GET, sendWebStatus);
  webServer.on("/api/rules", HTTP_GET, sendWebRules);
  webServer.on("/api/rules", HTTP_PUT, []() {
    JsonDocument doc;
    if (!readWebJson(doc)) return;
    RuleSet candidate;
    const char* validation = decodeRules(doc.as<JsonVariantConst>(), candidate);
    if (validation) {
      sendWebError(400, validation);
      return;
    }
    String error;
    if (!persistRules(candidate, error)) {
      sendWebError(500, error.c_str());
      return;
    }
    publishThresholds();
    sendWebRules();
  }, receiveWebBody);
  webServer.on("/api/control", HTTP_POST, []() {
    JsonDocument doc;
    if (!readWebJson(doc)) return;
    if (!doc["command"].is<const char*>() ||
        !handleAcCommand(doc["command"].as<const char*>())) {
      sendWebError(400, "无效指令，请使用ON、OFF、COOL、HEAT、TEMP或AUTO");
      return;
    }
    sendWebStatus();
  }, receiveWebBody);
  webServer.onNotFound([]() { sendWebError(404, "页面或接口不存在"); });
}

#endif
