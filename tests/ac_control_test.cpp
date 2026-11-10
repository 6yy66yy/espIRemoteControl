#include <cstdlib>
#include <iostream>
#include "../ir_receive_demo/ac_control.h"
#include "ir_Midea.h"
#include "IRsend_test.h"

void testRules();

void check(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

AcTransmission execute(const char* text, AcState& state) {
  AcCommand command;
  check(parseAcCommand(text, command), text);
  return applyAcCommand(command, state);
}

void testCommands() {
  AcState state;
  check(execute("  hEaT 22 \r\n", state) == AcTransmission::On, "heat on");
  check(state.mode == AcMode::Heat && state.temperatureC == 22 &&
        state.power == AcPower::On && !state.automatic, "manual heat state");
  check(execute("TEMP 24", state) == AcTransmission::On &&
        state.mode == AcMode::Heat && state.temperatureC == 24, "adjust heat");
  check(execute("OFF", state) == AcTransmission::Off, "off");
  check(execute("TEMP 23", state) == AcTransmission::None &&
        state.power == AcPower::Off && state.temperatureC == 23, "adjust off");
  check(execute("ON", state) == AcTransmission::On &&
        state.mode == AcMode::Heat && state.temperatureC == 23, "resume heat");
  execute("COOL 26", state);
  check(state.mode == AcMode::Cool && state.temperatureC == 26, "cool state");
  execute("HEAT", state);
  check(state.mode == AcMode::Heat && state.temperatureC == 26, "keep setpoint");
  execute("on#2#27#0", state);
  check(state.mode == AcMode::Cool && state.temperatureC == 27, "Bemfa cooling");
  execute("on#3#22#0#0#0", state);
  check(state.mode == AcMode::Heat && state.temperatureC == 22, "Bemfa heating");
  execute("on##24", state);
  check(state.mode == AcMode::Heat && state.temperatureC == 24, "partial update");
  execute("on#2", state);
  check(state.mode == AcMode::Cool && state.temperatureC == 24, "mode only");
  execute("TEMP 16", state);
  check(state.temperatureC == 17, "lower temperature limit");
  execute("TEMP 31", state);
  check(state.temperatureC == 30, "upper temperature limit");
  execute("auto", state);
  check(state.automatic, "restore thermostat");
  check(execute("STATUS", state) == AcTransmission::None && state.automatic,
        "status preserves automatic mode");

  const char* invalid[] = {
      "", "only", "onward", "on junk", "off now", "temp", "temp abc",
      "temp 24.5", "temp -1", "temp 24 25", "heat abc", "cool 23junk",
      "on#1#26#0", "on#3#no#0", "on#2#26#bad", "on#2#26#0#2",
      "on#2#26#0#0#0#0",
      "on                                                               x"};
  for (const char* text : invalid) {
    AcCommand command;
    check(!parseAcCommand(text, command), text);
    check(state.automatic, "invalid input preserves automatic mode");
  }
  AcCommand emptyCommand;
  check(!parseAcCommand(nullptr, emptyCommand), "null command");
  AcState unknown;
  check(execute("TEMP 25", unknown) == AcTransmission::None &&
        unknown.power == AcPower::Unknown, "setpoint does not assume power on");
}

void testMideaFrames() {
  // Use the same protocol flags as the ESP32 build, and decode the emitted
  // pulse train rather than merely checking the sender's internal state.
  IRMideaAC sender(1);
  IRMideaAC decoded(1);
  IRrecv receiver(0);
  sender.begin();
  unsigned count = 0;
  for (const uint8_t mode : {kMideaACCool, kMideaACHeat}) {
    for (uint8_t temperature = 17; temperature <= 30; ++temperature) {
      for (const bool power : {true, false}) {
        sender.setUseCelsius(true);
        sender.setMode(mode);
        sender.setTemp(temperature, true);
        sender.setFan(kMideaACFanAuto);
        sender.setPower(power);
        sender._irsend.reset();
        sender.send();
        sender._irsend.makeDecodeResult();
        check(receiver.decode(&sender._irsend.capture), "decode emitted pulses");
        check(sender._irsend.capture.decode_type == MIDEA &&
              sender._irsend.capture.bits == kMideaBits, "Midea 48-bit frame");
        check(IRMideaAC::validChecksum(sender._irsend.capture.value), "checksum");
        decoded.setRaw(sender._irsend.capture.value);
        check(decoded.getPower() == power && decoded.getMode() == mode &&
              decoded.getTemp(true) == temperature && decoded.getUseCelsius() &&
              decoded.getFan() == kMideaACFanAuto, "decoded A/C settings");
        ++count;
      }
    }
  }
  // Unknown protocols must still produce a hash for serial hex logging.
  IRsendTest unknownSender(1);
  const uint16_t raw[] = {1500, 2500, 400, 700, 400, 700, 400, 1300,
                          400, 700, 400, 1300, 400, 700, 400, 1300};
  for (unsigned i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i) {
    if (i % 2) unknownSender.space(raw[i]);
    else unknownSender.mark(raw[i]);
  }
  unknownSender.makeDecodeResult();
  check(receiver.decode(&unknownSender.capture) &&
        unknownSender.capture.decode_type == UNKNOWN, "unknown-code hash");
  std::cout << "PASS: commands, state transitions, " << count
            << " Midea pulse round trips, unknown-code reception\n";
}

int main() {
  testCommands();
  testRules();
  testMideaFrames();
}
