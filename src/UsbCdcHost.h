#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <usb/usb_host.h>

// Minimal USB CDC-ACM host for a single device (the flight controller's virtual COM port).
// USB events are serviced by two background tasks; read()/write() are called from the app task.
class UsbCdcHost {
public:
  bool begin();
  bool connected() const { return _ready; }
  uint16_t vid() const { return _vid; }
  uint16_t pid() const { return _pid; }

  bool write(const uint8_t* data, size_t len, uint32_t timeoutMs = 1000);
  size_t read(uint8_t* buf, size_t len, uint32_t timeoutMs);
  void flushInput();

private:
  static void hostLibTask(void* arg);
  static void clientTask(void* arg);
  static void clientEventCb(const usb_host_client_event_msg_t* msg, void* arg);
  static void inXferCb(usb_transfer_t* xfer);
  static void outXferCb(usb_transfer_t* xfer);
  static void ctrlXferCb(usb_transfer_t* xfer);

  void openDevice(uint8_t addr);
  void closeDevice();
  bool parseConfig(const uint8_t* desc, uint16_t totalLen);
  void advanceConfig();
  bool submitControl(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     const uint8_t* data, uint16_t len);
  void submitIn();

  usb_host_client_handle_t _client = nullptr;
  usb_device_handle_t volatile _dev = nullptr;
  usb_transfer_t* _in = nullptr;
  usb_transfer_t* _out = nullptr;
  usb_transfer_t* _ctrl = nullptr;
  StreamBufferHandle_t _rx = nullptr;
  SemaphoreHandle_t _txDone = nullptr;

  volatile uint8_t _pendingAddr = 0;
  volatile bool _gone = false;
  volatile bool _ready = false;
  volatile bool _claimed = false;
  volatile bool _inBusy = false;
  volatile bool _outBusy = false;
  volatile bool _ctrlBusy = false;
  volatile bool _outOk = false;

  uint8_t _cfgStage = 0;
  uint8_t _commIf = 0xFF;
  uint8_t _dataIf = 0xFF;
  uint8_t _epIn = 0;
  uint8_t _epOut = 0;
  uint16_t _vid = 0;
  uint16_t _pid = 0;
};
