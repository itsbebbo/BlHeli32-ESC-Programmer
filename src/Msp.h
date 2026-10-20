#pragma once

#include "UsbCdcHost.h"

// MSP v1 request/response over the flight controller's USB link.
namespace msp {
constexpr uint8_t kFcVariant = 2;
constexpr uint8_t kFcVersion = 3;
constexpr uint8_t kSetPassthrough = 245;
}  // namespace msp

class Msp {
public:
  explicit Msp(UsbCdcHost& link) : _link(link) {}

  // Sends cmd and waits for the matching reply. Returns false on timeout, bad checksum or an error reply.
  bool request(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* resp, uint8_t respMax, uint8_t& respLen,
               uint32_t timeoutMs = 500);

private:
  UsbCdcHost& _link;
};
