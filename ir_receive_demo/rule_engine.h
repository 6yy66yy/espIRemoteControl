#ifndef RULE_ENGINE_H
#define RULE_ENGINE_H

#include "ac_control.h"

constexpr uint8_t MAX_RULES = 10;
enum class RuleDirection : uint8_t { Rising, Falling };
enum class RuleAction : uint8_t { Off, Cool, Heat };

struct TemperatureRule {
  uint16_t id = 0;
  bool enabled = true;
  RuleDirection direction = RuleDirection::Falling;
  // Integer tenths keep threshold equality and reset boundaries predictable.
  int16_t threshold10 = 220;
  uint8_t window10 = 20;
  RuleAction action = RuleAction::Heat;
  uint8_t targetC = 24;
};

struct RuleSet {
  uint8_t count = 0;
  TemperatureRule items[MAX_RULES];
};

inline const char* validateRules(const RuleSet& rules) {
  if (rules.count > MAX_RULES) return "最多保存10条规则";
  for (uint8_t i = 0; i < rules.count; ++i) {
    const TemperatureRule& rule = rules.items[i];
    if (!rule.id) return "规则ID必须大于0";
    for (uint8_t j = 0; j < i; ++j) {
      if (rule.id == rules.items[j].id) return "规则ID不能重复";
    }
    if (rule.threshold10 < -100 || rule.threshold10 > 500)
      return "触发温度应为-10至50°C";
    if (rule.window10 < 1 || rule.window10 > 100)
      return "复位窗口应为0.1至10°C";
    if (rule.direction != RuleDirection::Rising &&
        rule.direction != RuleDirection::Falling) return "触发方向无效";
    if (rule.action != RuleAction::Off && rule.action != RuleAction::Cool &&
        rule.action != RuleAction::Heat) return "规则动作无效";
    if (rule.targetC < 17 || rule.targetC > 30)
      return "目标温度应为17至30°C";
  }
  return nullptr;
}

inline RuleSet legacyRules(int16_t on10, int16_t off10) {
  RuleSet rules;
  rules.count = 2;
  rules.items[0].id = 1;
  rules.items[0].direction = RuleDirection::Rising;
  rules.items[0].threshold10 = on10;
  rules.items[0].action = RuleAction::Cool;
  rules.items[0].targetC = 26;
  rules.items[1].id = 2;
  rules.items[1].threshold10 = off10;
  rules.items[1].action = RuleAction::Off;
  return rules;
}

inline bool legacyRuleIndexes(const RuleSet& rules, int& on, int& off) {
  on = off = -1;
  for (uint8_t i = 0; i < rules.count; ++i) {
    const auto& rule = rules.items[i];
    if (rule.id == 1 && rule.enabled && rule.action == RuleAction::Cool &&
        rule.direction == RuleDirection::Rising && rule.targetC == 26) on = i;
    if (rule.id == 2 && rule.enabled && rule.action == RuleAction::Off &&
        rule.direction == RuleDirection::Falling) off = i;
  }
  return on >= 0 && off >= 0 &&
         rules.items[on].threshold10 > rules.items[off].threshold10;
}

class RuleEngine {
 public:
  RuleSet rules;
  bool waiting[MAX_RULES] = {};

  void reset() {
    for (uint8_t i = 0; i < MAX_RULES; ++i) waiting[i] = false;
  }

  int evaluate(int16_t temperature10, bool valid, bool automatic) {
    if (!valid || !automatic) return -1;
    int selected = -1;
    for (uint8_t i = 0; i < rules.count; ++i) {
      const auto& rule = rules.items[i];
      if (!rule.enabled) continue;
      const bool rising = rule.direction == RuleDirection::Rising;
      if (waiting[i]) {
        const int reset10 = rule.threshold10 + (rising ? -rule.window10 : rule.window10);
        if (rising ? temperature10 <= reset10 : temperature10 >= reset10)
          waiting[i] = false;
      } else if (rising ? temperature10 >= rule.threshold10 :
                         temperature10 <= rule.threshold10) {
        // Consume every simultaneous match, but execute only the first one.
        waiting[i] = true;
        if (selected < 0) selected = i;
      }
    }
    return selected;
  }
};

inline AcTransmission applyRuleAction(const TemperatureRule& rule, AcState& state) {
  if (rule.action == RuleAction::Off) {
    if (state.power == AcPower::Off) return AcTransmission::None;
    state.power = AcPower::Off;
    return AcTransmission::Off;
  }
  const AcMode mode = rule.action == RuleAction::Heat ? AcMode::Heat : AcMode::Cool;
  if (state.power == AcPower::On && state.mode == mode &&
      state.temperatureC == rule.targetC) return AcTransmission::None;
  state.power = AcPower::On;
  state.mode = mode;
  state.temperatureC = rule.targetC;
  return AcTransmission::On;
}

#endif
