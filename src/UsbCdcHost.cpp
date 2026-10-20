#include "UsbCdcHost.h"

#include <esp_intr_alloc.h>
#include <string.h>

namespace {
constexpr uint8_t kDescInterface = 0x04;
constexpr uint8_t kDescEndpoint = 0x05;
constexpr uint8_t kClassCdcComm = 0x02;
constexpr uint8_t kClassCdcData = 0x0A;
constexpr uint8_t kXferBulk = 0x02;
constexpr uint8_t kNoInterface = 0xFF;
constexpr size_t kInSize = 512;   // multiple of the 64-byte full-speed bulk packet size
constexpr size_t kOutSize = 512;
constexpr size_t kCtrlSize = 64;
constexpr size_t kSetupSize = 8;
}  // namespace

bool UsbCdcHost::begin() {
  _rx = xStreamBufferCreate(4096, 1);
  _txDone = xSemaphoreCreateBinary();
  if (!_rx || !_txDone) return false;

  usb_host_config_t cfg = {};
  cfg.skip_phy_setup = false;
  cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&cfg) != ESP_OK) return false;

  xTaskCreatePinnedToCore(hostLibTask, "usbLib", 4096, this, 5, nullptr, 0);
  xTaskCreatePinnedToCore(clientTask, "usbCdc", 4096, this, 5, nullptr, 0);
  return true;
}

void UsbCdcHost::hostLibTask(void*) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void UsbCdcHost::clientTask(void* arg) {
  auto* self = static_cast<UsbCdcHost*>(arg);

  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = clientEventCb;
  cc.async.callback_arg = self;
  if (usb_host_client_register(&cc, &self->_client) != ESP_OK) vTaskDelete(nullptr);

  usb_host_transfer_alloc(kInSize, 0, &self->_in);
  usb_host_transfer_alloc(kOutSize, 0, &self->_out);
  usb_host_transfer_alloc(kCtrlSize, 0, &self->_ctrl);

  for (;;) {
    usb_host_client_handle_events(self->_client, pdMS_TO_TICKS(10));
    if (self->_pendingAddr) {
      uint8_t addr = self->_pendingAddr;
      self->_pendingAddr = 0;
      self->openDevice(addr);
    }
    // Device unplugged: wait for in-flight transfers to be returned before closing.
    if (self->_gone && !self->_inBusy && !self->_outBusy && !self->_ctrlBusy) self->closeDevice();
  }
}

void UsbCdcHost::clientEventCb(const usb_host_client_event_msg_t* msg, void* arg) {
  auto* self = static_cast<UsbCdcHost*>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!self->_dev) self->_pendingAddr = msg->new_dev.address;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (self->_dev && msg->dev_gone.dev_hdl == self->_dev) {
      self->_ready = false;
      self->_gone = true;
    }
  }
}

void UsbCdcHost::openDevice(uint8_t addr) {
  usb_device_handle_t dev = nullptr;
  if (usb_host_device_open(_client, addr, &dev) != ESP_OK) return;
  _dev = dev;

  const usb_device_desc_t* dd = nullptr;
  if (usb_host_get_device_descriptor(_dev, &dd) == ESP_OK) {
    _vid = dd->idVendor;
    _pid = dd->idProduct;
  }

  const usb_config_desc_t* cd = nullptr;
  if (usb_host_get_active_config_descriptor(_dev, &cd) != ESP_OK ||
      !parseConfig(reinterpret_cast<const uint8_t*>(cd), cd->wTotalLength) ||
      usb_host_interface_claim(_client, _dev, _dataIf, 0) != ESP_OK) {
    usb_host_device_close(_client, _dev);
    _dev = nullptr;
    return;
  }
  _claimed = true;
  _cfgStage = 0;
  advanceConfig();
}

void UsbCdcHost::closeDevice() {
  if (_claimed) usb_host_interface_release(_client, _dev, _dataIf);
  _claimed = false;
  usb_host_device_close(_client, _dev);
  _dev = nullptr;
  _gone = false;
  _ready = false;
  xStreamBufferReset(_rx);
}

// Finds the CDC data interface's bulk IN/OUT endpoints and the CDC communication interface number.
bool UsbCdcHost::parseConfig(const uint8_t* p, uint16_t totalLen) {
  int curClass = -1;
  uint8_t curIf = kNoInterface;
  uint8_t curAlt = 0;
  _commIf = kNoInterface;
  _dataIf = kNoInterface;
  _epIn = _epOut = 0;

  for (uint16_t i = 0; i + 1 < totalLen && p[i] >= 2; i += p[i]) {
    const uint8_t* d = p + i;
    if (d[1] == kDescInterface && d[0] >= 9) {
      curIf = d[2];
      curAlt = d[3];
      curClass = d[5];
      if (curClass == kClassCdcComm && _commIf == kNoInterface) _commIf = curIf;
    } else if (d[1] == kDescEndpoint && d[0] >= 7 && curClass == kClassCdcData && curAlt == 0) {
      if ((d[3] & 0x03) != kXferBulk) continue;
      if (_dataIf != kNoInterface && _dataIf != curIf) continue;
      _dataIf = curIf;
      if (d[2] & 0x80) {
        if (!_epIn) _epIn = d[2];
      } else if (!_epOut) {
        _epOut = d[2];
      }
    }
  }
  return _epIn && _epOut;
}

// Runs the CDC setup sequence (line coding, then DTR/RTS) from transfer callbacks, then starts reading.
// Betaflight ignores both requests, so failures are tolerated.
void UsbCdcHost::advanceConfig() {
  while (!_gone) {
    uint8_t stage = _cfgStage++;
    if (stage == 0) {
      if (_commIf == kNoInterface) continue;
      const uint8_t lineCoding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};  // 115200 8N1
      if (submitControl(0x21, 0x20, 0, _commIf, lineCoding, sizeof(lineCoding))) return;
    } else if (stage == 1) {
      if (_commIf == kNoInterface) continue;
      if (submitControl(0x21, 0x22, 0x0003, _commIf, nullptr, 0)) return;
    } else {
      submitIn();
      _ready = _inBusy;
      return;
    }
  }
}

bool UsbCdcHost::submitControl(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                               const uint8_t* data, uint16_t len) {
  uint8_t* b = _ctrl->data_buffer;
  b[0] = bmRequestType;
  b[1] = bRequest;
  b[2] = wValue & 0xFF;
  b[3] = wValue >> 8;
  b[4] = wIndex & 0xFF;
  b[5] = wIndex >> 8;
  b[6] = len & 0xFF;
  b[7] = len >> 8;
  if (len) memcpy(b + kSetupSize, data, len);
  _ctrl->num_bytes = kSetupSize + len;
  _ctrl->device_handle = _dev;
  _ctrl->bEndpointAddress = 0;
  _ctrl->callback = ctrlXferCb;
  _ctrl->context = this;
  _ctrl->timeout_ms = 1000;
  _ctrlBusy = true;
  if (usb_host_transfer_submit_control(_client, _ctrl) != ESP_OK) {
    _ctrlBusy = false;
    return false;
  }
  return true;
}

void UsbCdcHost::ctrlXferCb(usb_transfer_t* xfer) {
  auto* self = static_cast<UsbCdcHost*>(xfer->context);
  self->_ctrlBusy = false;
  self->advanceConfig();
}

void UsbCdcHost::submitIn() {
  _in->device_handle = _dev;
  _in->bEndpointAddress = _epIn;
  _in->num_bytes = kInSize;
  _in->callback = inXferCb;
  _in->context = this;
  _in->timeout_ms = 0;
  _inBusy = usb_host_transfer_submit(_in) == ESP_OK;
}

void UsbCdcHost::inXferCb(usb_transfer_t* xfer) {
  auto* self = static_cast<UsbCdcHost*>(xfer->context);
  bool ok = xfer->status == USB_TRANSFER_STATUS_COMPLETED;
  if (ok && xfer->actual_num_bytes > 0) {
    xStreamBufferSend(self->_rx, xfer->data_buffer, xfer->actual_num_bytes, 0);
  }
  if (ok && !self->_gone) {
    self->submitIn();
  } else {
    self->_inBusy = false;
  }
}

void UsbCdcHost::outXferCb(usb_transfer_t* xfer) {
  auto* self = static_cast<UsbCdcHost*>(xfer->context);
  self->_outOk = xfer->status == USB_TRANSFER_STATUS_COMPLETED;
  self->_outBusy = false;
  xSemaphoreGive(self->_txDone);
}

bool UsbCdcHost::write(const uint8_t* data, size_t len, uint32_t timeoutMs) {
  while (len) {
    if (!_ready || _outBusy) return false;
    size_t n = len < kOutSize ? len : kOutSize;
    memcpy(_out->data_buffer, data, n);
    _out->device_handle = _dev;
    _out->bEndpointAddress = _epOut;
    _out->num_bytes = n;
    _out->callback = outXferCb;
    _out->context = this;
    _out->timeout_ms = timeoutMs;
    xSemaphoreTake(_txDone, 0);
    _outBusy = true;
    if (usb_host_transfer_submit(_out) != ESP_OK) {
      _outBusy = false;
      return false;
    }
    if (xSemaphoreTake(_txDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE || !_outOk) return false;
    data += n;
    len -= n;
  }
  return true;
}

size_t UsbCdcHost::read(uint8_t* buf, size_t len, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < len) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= timeoutMs) break;
    got += xStreamBufferReceive(_rx, buf + got, len - got, pdMS_TO_TICKS(timeoutMs - elapsed));
  }
  return got;
}

void UsbCdcHost::flushInput() {
  uint8_t tmp[64];
  while (xStreamBufferReceive(_rx, tmp, sizeof(tmp), 0) > 0) {
  }
}
