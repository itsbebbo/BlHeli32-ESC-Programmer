#include "FourWay.h"

#include <string.h>

using namespace fourway;

namespace {
constexpr uint8_t kLocalEscape = 0x2F;
constexpr uint8_t kRemoteEscape = 0x2E;

// CRC-16/XMODEM, as used by Betaflight's 4-way interface.
uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc = 0) {
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  }
  return crc;
}
}  // namespace

bool FourWay::transact(uint8_t cmd, uint16_t addr, const uint8_t* params, uint16_t len, uint32_t timeoutMs) {
  _lastAck = kAckNoReply;
  _replyLen = 0;
  if (len == 0 || len > 256) return false;

  size_t n = 0;
  _frame[n++] = kLocalEscape;
  _frame[n++] = cmd;
  _frame[n++] = addr >> 8;
  _frame[n++] = addr & 0xFF;
  _frame[n++] = (uint8_t)len;  // 256 is sent as 0
  memcpy(_frame + n, params, len);
  n += len;
  uint16_t crc = crc16(_frame, n);
  _frame[n++] = crc >> 8;
  _frame[n++] = crc & 0xFF;

  _link.flushInput();
  if (!_link.write(_frame, n)) return false;

  uint32_t deadline = millis() + timeoutMs;
  auto left = [&]() -> uint32_t {
    int32_t r = (int32_t)(deadline - millis());
    return r > 0 ? r : 0;
  };

  uint8_t hdr[5];
  do {
    if (_link.read(hdr, 1, left()) != 1) return false;
  } while (hdr[0] != kRemoteEscape);
  if (_link.read(hdr + 1, 4, left()) != 4) return false;

  uint16_t dataLen = hdr[4] ? hdr[4] : 256;
  uint8_t tail[3];  // ACK, CRC hi, CRC lo
  if (_link.read(_reply, dataLen, left()) != dataLen) return false;
  if (_link.read(tail, 3, left()) != 3) return false;

  uint16_t c = crc16(hdr, 5);
  c = crc16(_reply, dataLen, c);
  c = crc16(tail, 1, c);
  if (c != (uint16_t)((tail[1] << 8) | tail[2]) || hdr[1] != cmd) return false;

  _replyLen = dataLen;
  _lastAck = tail[0];
  return _lastAck == kAckOk;
}

bool FourWay::initEsc(uint8_t esc, uint8_t info[4]) {
  if (!transact(kCmdDeviceInitFlash, 0, &esc, 1, 3000) || _replyLen < 4) return false;
  memcpy(info, _reply, 4);
  return true;
}

bool FourWay::read(uint16_t addr, uint8_t* out, uint16_t len) {
  while (len) {
    uint8_t chunk = len < kMaxChunk ? len : kMaxChunk;
    if (!transact(kCmdDeviceRead, addr, &chunk, 1, 1000) || _replyLen != chunk) return false;
    memcpy(out, _reply, chunk);
    out += chunk;
    addr += chunk;
    len -= chunk;
  }
  return true;
}

bool FourWay::write(uint16_t addr, const uint8_t* data, uint16_t len) {
  while (len) {
    uint16_t chunk = len < kMaxChunk ? len : kMaxChunk;
    if (!transact(kCmdDeviceWrite, addr, data, chunk, 2000)) return false;
    data += chunk;
    addr += chunk;
    len -= chunk;
  }
  return true;
}

bool FourWay::erasePage(uint8_t page) {
  return transact(kCmdPageErase, 0, &page, 1, 3000);
}

bool FourWay::exit(uint32_t timeoutMs) {
  const uint8_t dummy = 0;
  return transact(kCmdExit, 0, &dummy, 1, timeoutMs);
}
