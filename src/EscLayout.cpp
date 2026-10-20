#include "EscLayout.h"

#include <stdio.h>
#include <string.h>

namespace {
const char* const kOffOn[] = {"Off", "On"};
const char* const kDemag[] = {"Off", "Low", "High"};

// Guessed offsets relative to the header, following BLHeliSuite32's export order (1 byte per field,
// 16-bit little-endian throttle values):
//  0 FW main   1 FW sub   2 layout rev   3 direction   4 rampup pwr   5 PWM freq   6 timing   7 demag
//  8-9 PPM min   10-11 PPM center   12-13 PPM max   14 thr cal   15 temp prot   16 volt prot
//  17 power prot   18 brake on stop   19 beep   20 beacon   21-22 beacon delay   23 LED
//  24 max accel   25 nondamped   26 note config   27 sine mode
constexpr uint8_t kOffFwMain = 0;
constexpr uint8_t kOffFwSub = 1;
constexpr uint8_t kOffLayout = 2;
constexpr uint8_t kOffThrMin = 8;
constexpr uint8_t kOffThrCenter = 10;
constexpr uint8_t kOffThrMax = 12;
constexpr uint8_t kHeaderSpan = 28;
constexpr int kMaxBaseSearch = 64;

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// FW main revision 32, a sane layout revision and ordered PPM throttle endpoints must all line up.
bool headerPlausible(const uint8_t* p) {
  if (p[kOffFwMain] != 32) return false;
  if (p[kOffLayout] < 20 || p[kOffLayout] > 80) return false;
  uint16_t mn = rd16(p + kOffThrMin), ct = rd16(p + kOffThrCenter), mx = rd16(p + kOffThrMax);
  return mn >= 900 && mn <= 1500 && mx >= 1500 && mx <= 2100 && ct > mn && ct < mx;
}
}  // namespace

const ParamDef kParams[P_COUNT] = {
    // name           off  validMin/Max  editMin/Max  default labels  base count unit
    {"Timing (raw)",   6,  1,  31,       1,  31,      16,     nullptr, 0,  0,    ""},
    {"PWM freq",       5,  8,  128,      16, 48,      24,     nullptr, 0,  0,    "k"},
    {"Demag comp",     7,  1,  3,        1,  3,       2,      kDemag,  1,  3,    ""},
    {"Startup power",  4,  1,  150,      3,  150,     50,     nullptr, 0,  0,    "%"},
    {"Sine mode",      27, 0,  1,        0,  1,       0,      kOffOn,  0,  2,    ""},
    {"Brake on stop",  18, 0,  100,      0,  1,       0,      kOffOn,  0,  2,    ""},
};

bool isBlheli32Signature(const uint8_t info[4]) {
  // Betaflight ARM_DEVICE_MATCH: ID low byte 0x06, high byte 0x01..0x8F; byte 3 = interface mode (imARM_BLB = 4).
  return info[0] == 0x06 && info[1] > 0x00 && info[1] < 0x90 && info[3] == 4;
}

bool validateImage(EscImage& img) {
  img.valid = false;
  img.base = -1;
  if (!img.present) {
    snprintf(img.problem, sizeof(img.problem), "not read");
    return false;
  }
  if (!isBlheli32Signature(img.info)) {
    snprintf(img.problem, sizeof(img.problem), "not BLHeli_32 (%02X%02X)", img.info[1], img.info[0]);
    return false;
  }

  int found = -1, matches = 0;
  for (int i = 0; i <= kMaxBaseSearch && i + kHeaderSpan <= kPageSize; i++) {
    if (headerPlausible(img.page + i)) {
      if (found < 0) found = i;
      matches++;
    }
  }
  if (matches != 1) {
    // BLHeli_S keeps an "initialised" signature 0x55 0xAA at bytes 13-14; flag it if BLHeli_32 turns out to do the same.
    bool sStyle = img.page[kOffFwMain] == 32 && img.page[13] == 0x55 && img.page[14] == 0xAA;
    snprintf(img.problem, sizeof(img.problem), "%s",
             matches ? "layout ambiguous" : (sStyle ? "BLHeli_S-style layout" : "layout not recognised"));
    return false;
  }

  const uint8_t* p = img.page + found;
  img.base = found;
  img.fwMain = p[kOffFwMain];
  img.fwSub = p[kOffFwSub];
  img.layout = p[kOffLayout];
  img.thrMin = rd16(p + kOffThrMin);
  img.thrCenter = rd16(p + kOffThrCenter);
  img.thrMax = rd16(p + kOffThrMax);
  for (uint8_t i = 0; i < P_COUNT; i++) {
    uint8_t v = p[kParams[i].offset];
    if (v < kParams[i].validMin || v > kParams[i].validMax) {
      snprintf(img.problem, sizeof(img.problem), "%s implausible (%u)", kParams[i].name, v);
      return false;
    }
    img.values[i] = v;
  }
  img.problem[0] = '\0';
  img.valid = true;
  return true;
}

void patchPage(const EscImage& img, uint8_t* page, const uint8_t values[P_COUNT]) {
  for (uint8_t i = 0; i < P_COUNT; i++) page[img.base + kParams[i].offset] = values[i];
}

bool sameFirmware(const EscImage& a, const EscImage& b) {
  return memcmp(a.info, b.info, 4) == 0 && a.base == b.base && a.fwMain == b.fwMain && a.fwSub == b.fwSub &&
         a.layout == b.layout;
}

bool valuesEditable(const uint8_t values[P_COUNT]) {
  for (uint8_t i = 0; i < P_COUNT; i++) {
    if (values[i] < kParams[i].editMin || values[i] > kParams[i].editMax) return false;
  }
  return true;
}

const char* formatValue(uint8_t id, uint8_t v, char* buf, size_t n) {
  const ParamDef& d = kParams[id];
  if (d.labels && v >= d.labelBase && v - d.labelBase < d.labelCount) {
    snprintf(buf, n, "%s", d.labels[v - d.labelBase]);
  } else {
    snprintf(buf, n, "%u%s", v, d.unit);
  }
  return buf;
}
