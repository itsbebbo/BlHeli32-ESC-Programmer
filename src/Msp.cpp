#include "Msp.h"

#include <string.h>

bool Msp::request(uint8_t cmd, const uint8_t* payload, uint8_t len, uint8_t* resp, uint8_t respMax,
                  uint8_t& respLen, uint32_t timeoutMs) {
  uint8_t frame[6 + 255];
  size_t n = 0;
  frame[n++] = '$';
  frame[n++] = 'M';
  frame[n++] = '<';
  frame[n++] = len;
  frame[n++] = cmd;
  uint8_t ck = len ^ cmd;
  for (uint8_t i = 0; i < len; i++) {
    frame[n++] = payload[i];
    ck ^= payload[i];
  }
  frame[n++] = ck;

  _link.flushInput();
  if (!_link.write(frame, n)) return false;

  uint32_t deadline = millis() + timeoutMs;
  auto readByte = [&](uint8_t& b) {
    int32_t left = (int32_t)(deadline - millis());
    return left > 0 && _link.read(&b, 1, left) == 1;
  };

  // Scan for "$M>" (or "$M!" error), skipping any unrelated frames.
  for (;;) {
    uint8_t b;
    if (!readByte(b)) return false;
    if (b != '$') continue;
    if (!readByte(b)) return false;
    if (b != 'M') continue;
    uint8_t dir;
    if (!readByte(dir)) return false;
    if (dir != '>' && dir != '!') continue;

    uint8_t size, rcmd;
    if (!readByte(size) || !readByte(rcmd)) return false;
    uint8_t sum = size ^ rcmd;
    uint8_t data[255];
    for (uint8_t i = 0; i < size; i++) {
      if (!readByte(data[i])) return false;
      sum ^= data[i];
    }
    uint8_t rxCk;
    if (!readByte(rxCk)) return false;
    if (rxCk != sum || rcmd != cmd) continue;
    if (dir == '!') return false;

    respLen = size < respMax ? size : respMax;
    if (respLen) memcpy(resp, data, respLen);
    return true;
  }
}
