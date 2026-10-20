#pragma once

#include <stddef.h>
#include <stdint.h>

// BLHeli_32 settings layout.
// The byte offsets are UNVERIFIED best guesses (BLHeli32_Protocol_and_Layout.md, section 6). Every write is
// gated by validateImage(): if the guessed map does not decode to plausible values, nothing is written.

constexpr uint16_t kSettingsAddr = 0x7C00;
constexpr uint8_t kSettingsPage = 31;    // Betaflight ARM page number = address / 1024
constexpr uint16_t kPageSize = 1024;
constexpr uint16_t kGuardAddr = 0x7800;  // page below the settings; checked in case the MCU erases 2 KB pages
constexpr int kEscCount = 4;

enum ParamId : uint8_t { P_TIMING, P_PWM_FREQ, P_DEMAG, P_STARTUP_PWR, P_SINE_MODE, P_BRAKE_ON_STOP, P_COUNT };

struct ParamDef {
  const char* name;
  uint8_t offset;              // relative to the header (FW main revision byte)
  uint8_t validMin, validMax;  // plausibility gate on values read from the ESC
  uint8_t editMin, editMax;    // range offered in the editor
  uint8_t defaultValue;
  const char* const* labels;   // optional enum labels, indexed by value - labelBase
  uint8_t labelBase;
  uint8_t labelCount;
  const char* unit;
};

extern const ParamDef kParams[P_COUNT];

struct EscImage {
  bool present;  // ESC answered and its settings page was read
  bool valid;    // page passed the layout plausibility checks
  uint8_t info[4];
  uint8_t page[kPageSize];
  int base;      // offset of the header within the page, -1 if not found
  uint8_t fwMain, fwSub, layout;
  uint16_t thrMin, thrCenter, thrMax;
  uint8_t values[P_COUNT];
  char problem[40];
};

bool isBlheli32Signature(const uint8_t info[4]);
bool validateImage(EscImage& img);
void patchPage(const EscImage& img, uint8_t* page, const uint8_t values[P_COUNT]);
bool sameFirmware(const EscImage& a, const EscImage& b);
bool valuesEditable(const uint8_t values[P_COUNT]);
const char* formatValue(uint8_t id, uint8_t v, char* buf, size_t n);
