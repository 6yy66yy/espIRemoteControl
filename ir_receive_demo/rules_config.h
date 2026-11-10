#ifndef RULES_CONFIG_H
#define RULES_CONFIG_H

#include <ArduinoJson.h>
#include <math.h>
#include "rule_engine.h"

constexpr size_t MAX_RULE_JSON_BYTES = 4096;

inline bool jsonTenths(JsonVariantConst value, int minimum, int maximum, int& result) {
  if (!value.is<double>()) return false;
  const double scaled = value.as<double>() * 10;
  if (!isfinite(scaled) || scaled < minimum || scaled > maximum ||
      fabs(scaled - round(scaled)) > 0.0001) return false;
  result = static_cast<int>(round(scaled));
  return true;
}

inline const char* decodeRules(JsonVariantConst root, RuleSet& result) {
  if (!root.is<JsonObjectConst>() || !root["version"].is<int>() ||
      root["version"].as<int>() != 1) return "规则配置版本无效";
  if (!root["rules"].is<JsonArrayConst>()) return "rules必须是数组";
  const JsonArrayConst array = root["rules"].as<JsonArrayConst>();
  if (array.size() > MAX_RULES) return "最多保存10条规则";
  RuleSet candidate;
  for (JsonVariantConst value : array) {
    if (!value.is<JsonObjectConst>() || !value["id"].is<unsigned int>() ||
        value["id"].as<unsigned int>() == 0 ||
        value["id"].as<unsigned int>() > 65535 || !value["enabled"].is<bool>())
      return "规则ID或启用状态无效";
    auto& rule = candidate.items[candidate.count++];
    rule.id = value["id"].as<uint16_t>();
    rule.enabled = value["enabled"].as<bool>();
    const char* direction = value["direction"] | "";
    if (!strcmp(direction, "rising")) rule.direction = RuleDirection::Rising;
    else if (!strcmp(direction, "falling")) rule.direction = RuleDirection::Falling;
    else return "触发方向应为rising或falling";
    int number;
    if (!jsonTenths(value["thresholdC"], -100, 500, number))
      return "触发温度应为-10至50°C，最多一位小数";
    rule.threshold10 = number;
    if (!jsonTenths(value["windowC"], 1, 100, number))
      return "复位窗口应为0.1至10°C，最多一位小数";
    rule.window10 = number;
    const char* action = value["action"] | "";
    if (!strcmp(action, "off")) rule.action = RuleAction::Off;
    else if (!strcmp(action, "cool")) rule.action = RuleAction::Cool;
    else if (!strcmp(action, "heat")) rule.action = RuleAction::Heat;
    else return "规则动作应为off、cool或heat";
    if (!value["targetC"].is<int>() || value["targetC"].as<int>() < 17 ||
        value["targetC"].as<int>() > 30) return "目标温度应为17至30°C的整数";
    rule.targetC = value["targetC"].as<uint8_t>();
  }
  const char* error = validateRules(candidate);
  if (error) return error;
  result = candidate;
  return nullptr;
}

inline void encodeRules(JsonDocument& doc, const RuleSet& rules) {
  doc.clear();
  doc["version"] = 1;
  JsonArray array = doc["rules"].to<JsonArray>();
  for (uint8_t i = 0; i < rules.count; ++i) {
    const auto& rule = rules.items[i];
    JsonObject value = array.add<JsonObject>();
    value["id"] = rule.id;
    value["enabled"] = rule.enabled;
    value["direction"] = rule.direction == RuleDirection::Rising ? "rising" : "falling";
    value["thresholdC"] = rule.threshold10 / 10.0;
    value["windowC"] = rule.window10 / 10.0;
    value["action"] = rule.action == RuleAction::Off ? "off" :
                      rule.action == RuleAction::Heat ? "heat" : "cool";
    value["targetC"] = rule.targetC;
  }
}

#endif
