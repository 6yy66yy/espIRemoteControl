#include <iostream>
#include <string>
#include "../ir_receive_demo/rules_config.h"

void check(bool condition, const char* message);

RuleEngine fallingRule(uint8_t window10 = 20) {
  RuleEngine engine;
  engine.rules.count = 1;
  engine.rules.items[0].id = 3;
  engine.rules.items[0].window10 = window10;
  return engine;
}

void testRuleEngine() {
  for (const uint8_t window : {10, 20}) {
    RuleEngine engine = fallingRule(window);
    check(engine.evaluate(250, true, true) == -1, "falling initial above threshold");
    check(engine.evaluate(220, true, true) == 0, "falling inclusive threshold");
    for (const int16_t value : {210, 220, 225, 215})
      check(engine.evaluate(value, true, true) == -1, "falling jitter consumed");
    check(engine.evaluate(220 + window - 1, true, true) == -1 && engine.waiting[0],
          "not yet at reset boundary");
    check(engine.evaluate(220 + window, true, true) == -1 && !engine.waiting[0],
          "falling reset is inclusive and sends nothing");
    check(engine.evaluate(200, true, true) == 0, "falling jump after reset");
  }
  RuleEngine rising = fallingRule();
  rising.rules.items[0].direction = RuleDirection::Rising;
  rising.rules.items[0].threshold10 = 280;
  check(rising.evaluate(300, true, true) == 0, "initial already hot triggers once");
  check(rising.evaluate(281, true, true) == -1, "rising remains consumed");
  check(rising.evaluate(261, true, true) == -1 && rising.waiting[0], "rising reset margin");
  check(rising.evaluate(260, true, true) == -1 && !rising.waiting[0], "rising reset equality");
  check(rising.evaluate(280, true, true) == 0, "rising trigger equality");

  RuleEngine engine = fallingRule();
  check(engine.evaluate(200, false, true) == -1 && !engine.waiting[0], "invalid first reading");
  check(engine.evaluate(200, true, false) == -1 && !engine.waiting[0], "manual pauses rules");
  check(engine.evaluate(200, true, true) == 0, "initial already cold triggers");
  check(engine.evaluate(250, false, true) == -1 && engine.waiting[0], "invalid cannot reset");
  check(engine.evaluate(250, true, false) == -1 && engine.waiting[0], "manual cannot reset");
  engine.reset();
  check(engine.evaluate(200, true, true) == 0, "AUTO reevaluates satisfied condition");
  engine.rules.items[0].enabled = false;
  engine.reset();
  check(engine.evaluate(200, true, true) == -1, "disabled rule ignored");
  engine.rules.items[0].enabled = true;
  engine.reset();
  check(engine.evaluate(200, true, true) == 0, "enabled rule first reading");

  engine = fallingRule();
  engine.rules.count = 2;
  engine.rules.items[1] = engine.rules.items[0];
  engine.rules.items[1].id = 4;
  engine.rules.items[1].action = RuleAction::Off;
  check(engine.evaluate(200, true, true) == 0 && engine.waiting[0] && engine.waiting[1],
        "priority consumes all simultaneous matches");
  check(engine.evaluate(200, true, true) == -1, "loser must not run next sample");
  auto swap = engine.rules.items[0];
  engine.rules.items[0] = engine.rules.items[1];
  engine.rules.items[1] = swap;
  engine.reset();
  check(engine.evaluate(200, true, true) == 0 && engine.rules.items[0].id == 4,
        "list order defines priority");

  AcState ac;
  const TemperatureRule rule = fallingRule().rules.items[0];
  check(applyRuleAction(rule, ac) == AcTransmission::On && ac.automatic &&
        ac.power == AcPower::On && ac.mode == AcMode::Heat && ac.temperatureC == 24,
        "rule sends heat without disabling automation");
  check(applyRuleAction(rule, ac) == AcTransmission::None, "same target sends no IR");
  TemperatureRule off = rule;
  off.action = RuleAction::Off;
  check(applyRuleAction(off, ac) == AcTransmission::Off && ac.automatic, "rule off");
  check(applyRuleAction(off, ac) == AcTransmission::None, "already off skips IR");
  engine = fallingRule();
  ac.power = AcPower::On;
  engine.evaluate(200, true, true);
  check(applyRuleAction(rule, ac) == AcTransmission::None, "unchanged rule target");
  check(engine.evaluate(200, true, true) == -1, "unchanged target still consumes trigger");
  RuleEngine rebooted;
  rebooted.rules = engine.rules;
  check(rebooted.evaluate(200, true, true) == 0, "reboot resets runtime trigger state");
  TemperatureRule cool = rule;
  cool.action = RuleAction::Cool;
  cool.targetC = 26;
  check(applyRuleAction(cool, ac) == AcTransmission::On && ac.automatic &&
        ac.mode == AcMode::Cool && ac.temperatureC == 26, "automatic heat-to-cool transition");
  RuleEngine ten;
  ten.rules.count = 10;
  for (uint8_t i = 0; i < 10; ++i) {
    ten.rules.items[i].id = i + 3;
    ten.rules.items[i].threshold10 = 220;
  }
  check(ten.evaluate(-100, true, true) == 0, "ten simultaneous rules select first");
  for (uint8_t i = 0; i < 10; ++i) check(ten.waiting[i], "all ten matches consumed");

  int on, offIndex;
  RuleSet legacy = legacyRules(285, 235);
  check(legacyRuleIndexes(legacy, on, offIndex) && on == 0 && offIndex == 1 &&
        legacy.items[0].threshold10 == 285 && legacy.items[1].window10 == 20,
        "legacy migration retains thresholds with two degree windows");
  legacy.items[0].action = RuleAction::Heat;
  check(!legacyRuleIndexes(legacy, on, offIndex), "repurposed legacy pair refused");
  legacy = legacyRules(280, 240);
  legacy.count = 1;
  check(!legacyRuleIndexes(legacy, on, offIndex), "deleted legacy pair refused");
  RuleEngine empty;
  check(empty.evaluate(300, true, true) == -1, "empty configuration");
}

void testRuleConfig() {
  JsonDocument doc;
  RuleSet rules = legacyRules(280, 240);
  for (uint8_t i = rules.count; i < MAX_RULES; ++i) {
    rules.items[i].id = i + 1;
    rules.items[i].threshold10 = -100 + i;
    rules.items[i].window10 = i + 1;
  }
  rules.count = MAX_RULES;
  encodeRules(doc, rules);
  std::string json;
  serializeJson(doc, json);
  check(!doc.overflowed() && json.size() <= MAX_RULE_JSON_BYTES, "ten rules fit payload");
  doc.clear();
  check(!deserializeJson(doc, json), "parse serialized rules");
  RuleSet decoded;
  check(!decodeRules(doc.as<JsonVariantConst>(), decoded) && decoded.count == 10 &&
        decoded.items[9].threshold10 == -91 && decoded.items[9].window10 == 10,
        "ten-rule config round trip preserves tenths and IDs");

  auto reject = [&](const char* message) {
    RuleSet untouched = legacyRules(300, 200);
    check(decodeRules(doc.as<JsonVariantConst>(), untouched) != nullptr, message);
    check(untouched.count == 2 && untouched.items[0].threshold10 == 300,
          "invalid config is not partially applied");
  };
  encodeRules(doc, rules);
  doc["rules"][1]["id"] = 1;
  reject("duplicate ID rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["thresholdC"] = 22.05;
  reject("extra decimal precision rejected");
  for (const char* key : {"thresholdC", "windowC", "targetC", "enabled", "id"}) {
    encodeRules(doc, rules);
    doc["rules"][0][key] = "22";
    reject("numeric strings rejected");
  }
  encodeRules(doc, rules);
  doc["rules"][0]["windowC"] = 0;
  reject("zero window rejected");
  doc["rules"][0]["windowC"] = 10.1;
  reject("oversize window rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["thresholdC"] = -10.1;
  reject("temperature out of range rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["direction"] = "sideways";
  reject("invalid direction rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["action"] = "dry";
  reject("unsupported action rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["targetC"] = 22.5;
  reject("fractional setpoint rejected");
  encodeRules(doc, rules);
  doc["rules"][0]["id"] = 65536;
  reject("ID overflow rejected");
  encodeRules(doc, rules);
  doc["rules"].as<JsonArray>().add<JsonObject>();
  reject("eleven rules rejected");
  encodeRules(doc, rules);
  doc["version"] = 2;
  reject("unknown version rejected");
  check(!deserializeJson(doc, "{\"version\":1,\"rules\":[]}"), "empty JSON parses");
  check(!decodeRules(doc.as<JsonVariantConst>(), decoded) && decoded.count == 0,
        "empty rules allowed");
  std::cout << "PASS: rule hysteresis, priorities, automatic/manual behavior, migration, JSON validation\n";
}

void testRules() {
  testRuleEngine();
  testRuleConfig();
}
