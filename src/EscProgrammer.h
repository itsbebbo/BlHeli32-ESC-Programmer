#pragma once

#include <functional>

#include "EscLayout.h"
#include "FourWay.h"
#include "Msp.h"
#include "UsbCdcHost.h"

// High-level read/write of all ESCs through Betaflight passthrough.
// Each operation enters passthrough, does its work and exits again, so the ESCs are never left in
// their bootloader while the user is in the UI.
class EscProgrammer {
public:
  using LogFn = std::function<void(const char*)>;
  static constexpr size_t kResultLen = 24;

  explicit EscProgrammer(UsbCdcHost& usb) : _usb(usb), _msp(usb), _4way(usb) {}

  // Reads every ESC into imgs. Returns false only if passthrough could not be entered.
  bool readAll(EscImage imgs[kEscCount], const LogFn& log);

  // Re-reads all ESCs, applies the guards, then writes values to each ESC with read-back verify.
  // Returns true only if every ESC passed.
  bool writeAll(const uint8_t values[P_COUNT], EscImage imgs[kEscCount], char results[kEscCount][kResultLen],
                const LogFn& log);

  int escCount() const { return _escCount; }

private:
  bool enterPassthrough(const LogFn& log);
  void leavePassthrough();
  bool readEsc(uint8_t esc, EscImage& img, const LogFn& log);
  bool writeEsc(uint8_t esc, const uint8_t values[P_COUNT], EscImage& img, char* result, const LogFn& log);
  void rememberGood(uint8_t esc, const EscImage& img);

  UsbCdcHost& _usb;
  Msp _msp;
  FourWay _4way;
  int _escCount = 0;

  // Last valid settings page seen per ESC, used to restore an ESC whose page was damaged by a failed write.
  bool _haveLastGood[kEscCount] = {};
  uint8_t _lastGoodInfo[kEscCount][4];
  uint8_t _lastGood[kEscCount][kPageSize];

  uint8_t _patched[kPageSize];
  uint8_t _guard[kPageSize];
  uint8_t _verify[kPageSize];
};
