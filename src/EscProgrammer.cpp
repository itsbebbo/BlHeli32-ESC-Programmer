#include "EscProgrammer.h"

#include <stdarg.h>
#include <string.h>

namespace {
void logf(const EscProgrammer::LogFn& log, const char* fmt, ...) {
  char buf[48];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log(buf);
}

bool allErased(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (p[i] != 0xFF) return false;
  }
  return true;
}
}  // namespace

bool EscProgrammer::enterPassthrough(const LogFn& log) {
  _escCount = 0;
  // If an earlier session left the FC in 4-way mode this returns it to MSP; the MSP parser ignores it otherwise.
  _4way.exit(200);
  delay(100);
  _usb.flushInput();

  uint8_t buf[16];
  uint8_t n = 0;
  if (!_msp.request(msp::kFcVariant, nullptr, 0, buf, sizeof(buf), n) || n < 4 || memcmp(buf, "BTFL", 4) != 0) {
    log("FC did not answer as Betaflight");
    return false;
  }
  if (_msp.request(msp::kFcVersion, nullptr, 0, buf, sizeof(buf), n) && n >= 3) {
    logf(log, "Betaflight %u.%u.%u", buf[0], buf[1], buf[2]);
  }
  if (!_msp.request(msp::kSetPassthrough, nullptr, 0, buf, sizeof(buf), n) || n < 1) {
    log("Passthrough request failed");
    return false;
  }
  _escCount = buf[0];
  logf(log, "Passthrough: %d ESC(s) found", _escCount);
  delay(50);
  return true;
}

void EscProgrammer::leavePassthrough() {
  _4way.exit();
  delay(300);
  _usb.flushInput();
}

void EscProgrammer::rememberGood(uint8_t esc, const EscImage& img) {
  if (!img.valid) return;
  memcpy(_lastGood[esc], img.page, kPageSize);
  memcpy(_lastGoodInfo[esc], img.info, 4);
  _haveLastGood[esc] = true;
}

bool EscProgrammer::readEsc(uint8_t esc, EscImage& img, const LogFn& log) {
  memset(&img, 0, sizeof(img));
  img.base = -1;
  if (!_4way.initEsc(esc, img.info)) {
    snprintf(img.problem, sizeof(img.problem), "no response (ack %02X)", _4way.lastAck());
    logf(log, "ESC%u: %s", esc + 1, img.problem);
    return false;
  }
  if (!isBlheli32Signature(img.info)) {
    validateImage(img);  // fills in the "not BLHeli_32" problem text
    logf(log, "ESC%u: %s", esc + 1, img.problem);
    return false;
  }
  if (!_4way.read(kSettingsAddr, img.page, kPageSize)) {
    snprintf(img.problem, sizeof(img.problem), "settings read failed");
    logf(log, "ESC%u: %s", esc + 1, img.problem);
    return false;
  }
  img.present = true;
  if (validateImage(img)) {
    logf(log, "ESC%u: FW %u.%u layout %u OK", esc + 1, img.fwMain, img.fwSub, img.layout);
    rememberGood(esc, img);
  } else {
    logf(log, "ESC%u: %s", esc + 1, img.problem);
  }
  return img.valid;
}

bool EscProgrammer::readAll(EscImage imgs[kEscCount], const LogFn& log) {
  for (int i = 0; i < kEscCount; i++) {
    memset(&imgs[i], 0, sizeof(EscImage));
    imgs[i].base = -1;
    snprintf(imgs[i].problem, sizeof(imgs[i].problem), "not found");
  }
  if (!enterPassthrough(log)) {
    leavePassthrough();
    return false;
  }
  int n = _escCount < kEscCount ? _escCount : kEscCount;
  for (int i = 0; i < n; i++) readEsc(i, imgs[i], log);
  leavePassthrough();
  return true;
}

bool EscProgrammer::writeAll(const uint8_t values[P_COUNT], EscImage imgs[kEscCount],
                             char results[kEscCount][kResultLen], const LogFn& log) {
  for (int i = 0; i < kEscCount; i++) snprintf(results[i], kResultLen, "not written");

  if (!valuesEditable(values)) {
    log("Values out of range - aborted");
    return false;
  }
  if (!enterPassthrough(log)) {
    leavePassthrough();
    return false;
  }

  // Phase 1: fresh read + guards. Nothing is erased unless every ESC passes.
  bool ok = true;
  if (_escCount != kEscCount) {
    logf(log, "Expected %d ESCs, FC reports %d", kEscCount, _escCount);
    ok = false;
  }
  for (int i = 0; ok && i < kEscCount; i++) {
    if (readEsc(i, imgs[i], log)) continue;
    // A page damaged by an earlier failed write can be rebuilt from the last good copy of the same ESC.
    if (imgs[i].present && _haveLastGood[i] && memcmp(_lastGoodInfo[i], imgs[i].info, 4) == 0) {
      memcpy(imgs[i].page, _lastGood[i], kPageSize);
      if (validateImage(imgs[i])) {
        logf(log, "ESC%u: restoring last good settings", i + 1);
        continue;
      }
    }
    snprintf(results[i], kResultLen, "guard: unreadable");
    ok = false;
  }
  for (int i = 1; ok && i < kEscCount; i++) {
    if (!sameFirmware(imgs[0], imgs[i])) {
      logf(log, "ESC%u firmware/layout differs from ESC1", i + 1);
      snprintf(results[i], kResultLen, "guard: mismatch");
      ok = false;
    }
  }
  if (!ok) {
    log("Write aborted - nothing was changed");
    leavePassthrough();
    return false;
  }

  // Phase 2: write each ESC; stop at the first failure.
  bool allPass = true;
  for (int i = 0; i < kEscCount; i++) {
    if (!allPass) {
      snprintf(results[i], kResultLen, "skipped");
      continue;
    }
    allPass = writeEsc(i, values, imgs[i], results[i], log);
  }
  leavePassthrough();
  return allPass;
}

bool EscProgrammer::writeEsc(uint8_t esc, const uint8_t values[P_COUNT], EscImage& img, char* result,
                             const LogFn& log) {
  memcpy(_patched, img.page, kPageSize);
  patchPage(img, _patched, values);

  uint8_t info[4];
  if (!_4way.initEsc(esc, info)) {
    snprintf(result, kResultLen, "FAIL: no response");
    return false;
  }
  _4way.read(kSettingsAddr, _verify, kPageSize);
  if (memcmp(_verify, _patched, kPageSize) == 0) {
    snprintf(result, kResultLen, "PASS (no change)");
    return true;
  }
  if (!_4way.read(kGuardAddr, _guard, kPageSize)) {
    snprintf(result, kResultLen, "FAIL: guard read");
    return false;
  }

  logf(log, "ESC%u: erase + write", esc + 1);
  if (!_4way.erasePage(kSettingsPage)) logf(log, "ESC%u: erase ack %02X", esc + 1, _4way.lastAck());
  bool written = _4way.write(kSettingsAddr, _patched, kPageSize);
  bool verified = _4way.read(kSettingsAddr, _verify, kPageSize) && memcmp(_verify, _patched, kPageSize) == 0;

  // If the MCU really erases 2 KB pages, the page below the settings was wiped too: put it back.
  bool guardFixed = false;
  if (!_4way.read(kGuardAddr, _verify, kPageSize)) {
    snprintf(result, kResultLen, "FAIL: guard re-read");
    return false;
  }
  if (memcmp(_verify, _guard, kPageSize) != 0) {
    if (!allErased(_verify, kPageSize)) {
      snprintf(result, kResultLen, "FAIL: guard changed");
      return false;
    }
    logf(log, "ESC%u: 2K erase detected, restoring", esc + 1);
    guardFixed = _4way.write(kGuardAddr, _guard, kPageSize) && _4way.read(kGuardAddr, _verify, kPageSize) &&
                 memcmp(_verify, _guard, kPageSize) == 0;
    if (!guardFixed) {
      snprintf(result, kResultLen, "FAIL: guard restore");
      return false;
    }
  }

  if (!verified) {
    snprintf(result, kResultLen, written ? "FAIL: verify" : "FAIL: write");
    return false;
  }
  memcpy(img.page, _patched, kPageSize);
  validateImage(img);
  rememberGood(esc, img);
  snprintf(result, kResultLen, guardFixed ? "PASS (2K page fixed)" : "PASS");
  return true;
}
