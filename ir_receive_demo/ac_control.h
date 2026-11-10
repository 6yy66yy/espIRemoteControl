#ifndef AC_CONTROL_H
#define AC_CONTROL_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>

enum class AcPower : uint8_t { Unknown, Off, On };
// These values match Bemfa's mode field, not the IR library's mode constants.
enum class AcMode : uint8_t { Cool = 2, Heat = 3 };
enum class AcAction : uint8_t { On, Off, Temperature, Automatic, Status };
enum class AcTransmission : uint8_t { None, On, Off };

struct AcState {
  AcPower power = AcPower::Unknown;
  AcMode mode = AcMode::Cool;
  uint8_t temperatureC = 26;
  bool automatic = true;
};

struct AcCommand {
  AcAction action = AcAction::Status;
  bool hasMode = false;
  AcMode mode = AcMode::Cool;
  // Zero means the command did not supply a temperature.
  uint8_t temperatureC = 0;
};

inline bool parseAcTemperature(const char* text, uint8_t& temperatureC) {
  if (!*text) return false;
  unsigned value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    value = value * 10 + (*text - '0');
    if (value > 99) return false;
  }
  // The Midea 48-bit protocol supported here accepts 17-30 C.
  temperatureC = value < 17 ? 17 : (value > 30 ? 30 : value);
  return true;
}

inline bool parseAcCommand(const char* payload, AcCommand& result) {
  if (!payload || strlen(payload) > 64) return false;
  char text[65];
  const char* begin = payload;
  while (isspace(static_cast<unsigned char>(*begin))) ++begin;
  size_t length = strlen(begin);
  while (length && isspace(static_cast<unsigned char>(begin[length - 1]))) {
    --length;
  }
  for (size_t i = 0; i < length; ++i) {
    text[i] = tolower(static_cast<unsigned char>(begin[i]));
  }
  text[length] = '\0';

  AcCommand command;
  if (strncmp(text, "on#", 3) == 0) {
    char* fields[6] = {text};
    uint8_t count = 1;
    for (char* p = text; *p; ++p) {
      if (*p != '#') continue;
      if (count == 6) return false;
      *p = '\0';
      fields[count++] = p + 1;
    }
    command.action = AcAction::On;
    if (*fields[1]) {
      if (strcmp(fields[1], "2") && strcmp(fields[1], "3")) return false;
      command.hasMode = true;
      command.mode = *fields[1] == '3' ? AcMode::Heat : AcMode::Cool;
    }
    if (count > 2 && *fields[2] &&
        !parseAcTemperature(fields[2], command.temperatureC)) {
      return false;
    }
    // Accept Bemfa's optional fan/swing fields, while keeping fan automatic
    // and leaving swing unchanged. No extra IR feature commands are emitted.
    for (uint8_t i = 3; i < count; ++i) {
      if (!*fields[i]) continue;
      const char maximum = i == 3 ? '5' : '1';
      if (fields[i][1] || *fields[i] < '0' || *fields[i] > maximum) return false;
    }
  } else {
    char* argument = text;
    while (*argument && !isspace(static_cast<unsigned char>(*argument))) {
      ++argument;
    }
    if (*argument) {
      *argument++ = '\0';
      while (isspace(static_cast<unsigned char>(*argument))) ++argument;
    }
    if (!strcmp(text, "cool") || !strcmp(text, "heat")) {
      command.action = AcAction::On;
      command.hasMode = true;
      command.mode = *text == 'h' ? AcMode::Heat : AcMode::Cool;
      if (*argument && !parseAcTemperature(argument, command.temperatureC)) {
        return false;
      }
    } else if (!strcmp(text, "temp")) {
      command.action = AcAction::Temperature;
      if (!parseAcTemperature(argument, command.temperatureC)) return false;
    } else {
      if (*argument) return false;
      if (!strcmp(text, "on")) command.action = AcAction::On;
      else if (!strcmp(text, "off")) command.action = AcAction::Off;
      else if (!strcmp(text, "auto")) command.action = AcAction::Automatic;
      else if (!strcmp(text, "status")) command.action = AcAction::Status;
      else return false;
    }
  }
  result = command;
  return true;
}

inline AcTransmission applyAcCommand(const AcCommand& command, AcState& state) {
  if (command.action == AcAction::Status) return AcTransmission::None;
  if (command.action == AcAction::Automatic) {
    state.automatic = true;
    return AcTransmission::None;
  }
  // A manual setting takes priority until AUTO is explicitly requested.
  state.automatic = false;
  if (command.hasMode) state.mode = command.mode;
  if (command.temperatureC) state.temperatureC = command.temperatureC;
  if (command.action == AcAction::Off) {
    state.power = AcPower::Off;
    return AcTransmission::Off;
  }
  if (command.action == AcAction::On) {
    state.power = AcPower::On;
    return AcTransmission::On;
  }
  // Changing the setpoint while off must not turn the A/C on.
  return state.power == AcPower::On ? AcTransmission::On : AcTransmission::None;
}

#endif
