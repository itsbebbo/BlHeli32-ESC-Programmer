#pragma once

#include "UsbCdcHost.h"

// Betaflight 4-way interface (src/main/io/serial_4way.c), used after MSP_SET_PASSTHROUGH.
namespace fourway {
constexpr uint8_t kCmdTestAlive = 0x30;
constexpr uint8_t kCmdExit = 0x34;
constexpr uint8_t kCmdDeviceInitFlash = 0x37;
constexpr uint8_t kCmdPageErase = 0x39;
constexpr uint8_t kCmdDeviceRead = 0x3A;
constexpr uint8_t kCmdDeviceWrite = 0x3B;

constexpr uint8_t kAckOk = 0x00;
constexpr uint8_t kAckNoReply = 0xFF;  // local marker: no valid reply frame received

constexpr uint16_t kMaxChunk = 128;    // bytes per read/write frame
}  // namespace fourway

class FourWay {
public:
  explicit FourWay(UsbCdcHost& link) : _link(link) {}

  bool initEsc(uint8_t esc, uint8_t info[4]);
  bool read(uint16_t addr, uint8_t* out, uint16_t len);
  bool write(uint16_t addr, const uint8_t* data, uint16_t len);
  bool erasePage(uint8_t page);
  bool exit(uint32_t timeoutMs = 500);
  uint8_t lastAck() const { return _lastAck; }

private:
  bool transact(uint8_t cmd, uint16_t addr, const uint8_t* params, uint16_t len, uint32_t timeoutMs);

  UsbCdcHost& _link;
  uint8_t _lastAck = fourway::kAckNoReply;
  uint16_t _replyLen = 0;
  uint8_t _reply[256];
  uint8_t _frame[5 + 256 + 2];
};
