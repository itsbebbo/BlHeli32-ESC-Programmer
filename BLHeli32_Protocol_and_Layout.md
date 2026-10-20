# BLHeli_32 Programming via Betaflight Passthrough — Protocol & Parameter Layout

Reference for the M5Stack CoreS3 ESC programmer targeting 4× iFlight Blitz E80 Single (BLHeli_32) ESCs
connected to a Betaflight flight controller over USB.

> **Confidence legend**
> - ✅ **Confirmed** — taken from Betaflight source code or another primary source.
> - 🟡 **Likely** — consistent with multiple secondary sources / known BLHeli conventions, not verified on hardware.
> - 🔴 **Unverified guess** — no public source; must be validated on a real ESC before relying on it.

---

## 1. System overview

```
CoreS3 (ESP32-S3, USB OTG host)
   │  USB-C  (CDC-ACM virtual serial, baud ignored)
   ▼
Betaflight FC  ── MSP v1 ──►  MSP_SET_PASSTHROUGH  ──►  4-way interface (serial_4way)
   │  motor signal wires (1-wire half-duplex, bit-banged by FC)
   ▼
4 × Blitz E80 (BLHeli_32 ARM bootloader)  ── settings block in MCU flash @ 0x7C00
```

Prerequisites ✅ (from Betaflight behaviour / community reports):
- Flight battery **must** be connected (ESCs are not powered by USB). Props off.
- A motor protocol (DShot etc.) must be configured in Betaflight, otherwise passthrough finds no ESCs.
- Old Betaflight builds may carry a 4-way interface version too old for some BLHeli_32 MCUs.
  Target FC runs **Betaflight 4.5**, which includes a current 4-way interface with ARM bootloader support.
  The firmware still checks `MSP_FC_VARIANT` = `"BTFL"` and logs `MSP_FC_VERSION`.

---

## 2. Transport: USB CDC-ACM

- ✅ Betaflight FCs (STM32 F4/F7/H7/AT32) enumerate as USB CDC-ACM; baud rate is ignored on USB VCP, but set 115200 8N1 anyway.
- ESP32-S3 side: ESP-IDF `usb_host` + `usb_host_cdc_acm` component (callable from Arduino/PlatformIO).
- CoreS3 must enable 5 V output on the USB-C port to power the FC's USB rail (M5Unified power API; verify the exact call during implementation).
- Consequence: while hosting, the CoreS3 USB port cannot be used for PC serial logging — log to screen/microSD.

---

## 3. Entering passthrough: MSP v1

### 3.1 MSP v1 frame ✅
```
'$' 'M' '<' | size (u8) | cmd (u8) | payload[size] | checksum (u8)
checksum = XOR of size, cmd and every payload byte
Reply:  '$' 'M' '>' | size | cmd | payload | checksum      ('!' instead of '>' = error)
```

### 3.2 Useful commands
| Cmd | ID | Purpose |
|---|---|---|
| `MSP_API_VERSION` | 1 | Sanity check FC is alive / Betaflight |
| `MSP_FC_VARIANT` | 2 | Expect `"BTFL"` |
| `MSP_FC_VERSION` | 3 | Log FC firmware version |
| `MSP_SET_PASSTHROUGH` | 245 (0xF5) | Enter 4-way interface |

🟡 `MSP_SET_PASSTHROUGH` with **empty payload** → FC enters 4-way mode for all motors (ESC4WAY mode) and replies with a 1-byte payload = number of ESCs found. From that point the serial link speaks the 4-way protocol (section 4) — **not MSP** — until `cmd_InterfaceExit`.

---

## 4. 4-way interface protocol (Betaflight `src/main/io/serial_4way.c`)

### 4.1 Frames ✅
**Request (host → FC)**
| Byte | Field |
|---|---|
| 0 | `0x2F` start (`cmd_Local_Escape`) |
| 1 | command |
| 2 | address high |
| 3 | address low |
| 4 | param length N (**0 means 256**) |
| 5..5+N-1 | params |
| last 2 | CRC16 high, low |

**Response (FC → host)**
| Byte | Field |
|---|---|
| 0 | `0x2E` start (`cmd_Remote_Escape`) |
| 1 | command (echo) |
| 2–3 | address high, low |
| 4 | param length N (default 1 byte, `0x00`) |
| 5..5+N-1 | params |
| next | ACK |
| last 2 | CRC16 high, low |

### 4.2 CRC ✅
CRC-16/XMODEM: poly `0x1021`, init `0x0000`, no reflection, no final XOR. Covers start byte through last param
(request) / through ACK (response). Sent high byte first.

```c
uint16_t crc16_xmodem_update(uint16_t crc, uint8_t data) {
    crc ^= (uint16_t)data << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    return crc;
}
```

### 4.3 Commands
✅ = value seen in Betaflight source during research; 🟡 = from Betaflight `serial_4way` headers as recalled, re-check against source before use.

| Command | ID | Notes for BLHeli_32 (`imARM_BLB`) |
|---|---|---|
| `cmd_InterfaceTestAlive` | 0x30 ✅ | Keep-alive; send ≲ every 500 ms while idle, or the ESC bootloader times out |
| `cmd_ProtocolGetVersion` | 0x31 🟡 | |
| `cmd_InterfaceGetName` | 0x32 🟡 | |
| `cmd_InterfaceGetVersion` | 0x33 🟡 | |
| `cmd_InterfaceExit` | 0x34 ✅ | ACK, then FC restores motor pins and leaves 4-way mode |
| `cmd_DeviceReset` | 0x35 🟡 | Address low byte = ESC index |
| `cmd_DeviceInitFlash` | 0x37 ✅ | Param[0] = ESC index (0–3). Up to 3 connect retries. Returns 4 bytes device info; byte 3 = interface mode |
| `cmd_DeviceEraseAll` | 0x38 🟡 | **Never use** |
| `cmd_DevicePageErase` | 0x39 ✅ | Param[0] = page number; ARM page = **1024 bytes** (addr = page << 10). Settings page 0x7C00 → page **31** |
| `cmd_DeviceRead` | 0x3A ✅ | Address in header, Param[0] = byte count (use ≤ 128 per read to be safe) |
| `cmd_DeviceWrite` | 0x3B ✅ | Address in header, params = data (length 0 = 256) |
| `cmd_DeviceReadEEprom` | 0x3D ✅ | **Not supported for ARM** → ACK 0x02. Settings are read from flash with `cmd_DeviceRead` |
| `cmd_DeviceWriteEEprom` | 0x3E 🟡 | Not supported for ARM |
| `cmd_InterfaceSetMode` | 0x3F ✅ | Param[0] = mode; `imARM_BLB = 4` ✅ |
| `cmd_DeviceVerify` | 0x40 🟡 | ARM only |

Interface modes ✅: `imC2=0, imSIL_BLB=1, imATM_BLB=2, imSK=3, imARM_BLB=4`.

### 4.4 ACK codes ✅
| ACK | Meaning |
|---|---|
| 0x00 | OK |
| 0x02 | Invalid command |
| 0x03 | Invalid CRC (command not executed) |
| 0x04 | Verify error |
| 0x08 | Invalid channel (ESC index ≥ ESC count) |
| 0x09 | Invalid parameter |
| 0x0F | General device error |

### 4.5 BLHeli_32 identification ✅
After `cmd_DeviceInitFlash`, device-info signature: **low byte == 0x06** and **0x00 < high byte < 0x90** ⇒ BLHeli_32 ARM
bootloader. Anything else ⇒ refuse (mismatch guard).

---

## 5. Settings storage

| Item | Value | Confidence |
|---|---|---|
| Settings location | MCU flash, address **0x7C00** (ArduPilot `AP_BLHeli` reads ARM ESCs here) | ✅ address used by ArduPilot; 🟡 that it holds the full settings block on the E80's MCU |
| Erase granularity | 1024-byte page (page 31) | ✅ (Betaflight page math) |
| Settings block size | Unknown; read the **full 1024-byte page** | 🔴 |
| Checksum over block | Unknown — firmware might reset to defaults if a checksum mismatches | 🔴 |

### 5.1 Safe write procedure (read-modify-write of the whole page)
1. Read all 1024 bytes of 0x7C00–0x7FFF (8 × 128-byte reads). Keep the image in RAM.
2. Validate the header (section 6.1) and that every target field holds an in-range value. Do this for **all 4 ESCs**. Abort if any ESC differs in revision/layout or fails validation.
3. Patch only the target bytes in the RAM copy.
4. `cmd_DevicePageErase` page 31.
5. `cmd_DeviceWrite` the full 1024 bytes back (4 × 256 bytes, or smaller chunks).
6. Read back the full page; compare byte-for-byte with the patched image → PASS/FAIL.
7. After all ESCs: `cmd_InterfaceExit`. Recommended field check: power-cycle and re-read — if values reverted to defaults, the firmware rejected the block (likely checksum) → stop using this layout.

⚠️ If power is lost between steps 4 and 5, that ESC's settings are erased. Keep the page image in CoreS3 flash during
the operation so it can be retried.

---

## 6. Parameter layout

### 6.1 Known field names & sample values ✅ (from a BLHeliSuite32 export of an **iFlight 35A** BLHeli_32 ESC, FW 32.6, layout revision 42)

Listed in the order BLHeliSuite32 exports them:

| Name | Sample value | Notes |
|---|---|---|
| Eep_FW_Main_Revision | 32 | |
| Eep_FW_Sub_Revision | 60 | encoding unclear vs "32.6.4" |
| Eep_Layout_Revision | 42 | Newer suites support up to ≥ 48. **Gate writes on this value.** |
| Eep_Pgm_Direction | 5 | enum |
| **Eep_Pgm_Rampup_Pwr** | 50 | **Startup power** |
| **Eep_Pgm_Pwm_Freq** | 24 | **PWM frequency, kHz** (manual: 16–48 kHz) |
| **Eep_Pgm_Comm_Timing** | 3 | **Motor timing** — enum in old FW; newer FW (manual) uses ~1°–31° plus Auto 🔴 |
| **Eep_Pgm_Demag_Comp** | 1 | **Demag compensation** — BLHeli convention 1=Off, 2=Low, 3=High 🟡 |
| Eep_Pgm_Ppm_Min_Throttle | 1040 | 16-bit |
| Eep_Pgm_Ppm_Center_Throttle | 1500 | 16-bit |
| Eep_Pgm_Ppm_Max_Throttle | 1960 | 16-bit |
| Eep_Pgm_Enable_Throttle_Cal | 1 | |
| Eep_Pgm_Temp_Prot_Enable | 7 | |
| Eep_Pgm_Volt_Prot | 0 | |
| Eep_Pgm_Enable_Power_Prot | 0 | |
| **Eep_Pgm_Brake_On_Stop** | 0 | **Brake on stop** (0 = off) |
| Eep_Pgm_Beep_Strength | 152 | |
| Eep_Pgm_Beacon_Strength | 80 | |
| Eep_Pgm_Beacon_Delay | 300 | >255 ⇒ 16-bit or encoded |
| Eep_Pgm_LED_Control | 12 | |
| Eep_Pgm_Max_Acceleration | 0 | |
| Eep_Pgm_Nondamped_Mode | 0 | |
| Eep_Note_Config | 255 | |
| **Eep_Pgm_Sine_Mode** | 0 | **Sine modulation** (0 = off) |
| Eep_Pgm_Auto_Tlm_Mode | 0 | |
| Eep_Hw_* / Eep_Nondamped_Capable | — | hardware capability flags (read-only) |
| Eep_Note_Array | — | startup tune data |

**Bold** = the six timing/drive parameters this project writes.

### 6.2 Byte offsets 🔴 — NOT PUBLICLY DOCUMENTED
BLHeli_32 is closed source and discontinued (May 2024). No published offset table was found. Offsets in the firmware
must therefore come from one of:

1. **Signature search (planned approach for "best-guess" mode):** in the 0x7C00 page, locate the header bytes
   `[32, sub_rev, layout_rev]` (main revision = 32 is a strong anchor). Treat the bytes after the header as the
   parameter area and assume BLHeliSuite32 export order with 1 byte per field, except the 16-bit throttle and
   beacon-delay fields 🔴.
2. **Plausibility gate:** before any write, every target byte must hold a value in that parameter's valid range
   (e.g. PWM freq 16–48, demag 1–3, brake 0/1, sine 0/1) **and** match across all 4 ESCs. If not, the guessed map
   is wrong for this layout → refuse to write.
3. **Visual check:** the Home screen shows the decoded values for all 4 ESCs before any write, so implausible
   decodes are visible.
4. **Post-write check:** after a write, power-cycle the battery and Read again. The app reports
   "Values REVERTED" if the firmware discarded the block (e.g. a checksum that this map does not maintain).

### 6.3 Offset table implemented (`src/EscLayout.cpp`) 🔴
Offsets relative to the header base (the FW main revision byte), following BLHeliSuite32 export order:

| Off | Field | Off | Field |
|---|---|---|---|
| 0 | FW main (must be 32) | 14 | Throttle cal enable |
| 1 | FW sub | 15 | Temp protection |
| 2 | Layout revision (20–80) | 16 | Low-voltage protection |
| 3 | Direction | 17 | Low-RPM power protection |
| **4** | **Startup (rampup) power** | **18** | **Brake on stop** |
| **5** | **PWM frequency** | 19 | Beep strength |
| **6** | **Motor timing** | 20 | Beacon strength |
| **7** | **Demag compensation** | 21–22 | Beacon delay |
| 8–9 | PPM min throttle (u16 LE, 900–1500) | 23 | LED control |
| 10–11 | PPM center throttle (u16 LE) | 24 | Max acceleration |
| 12–13 | PPM max throttle (u16 LE, 1500–2100) | 25 | Non-damped mode |
| | | 26 | Note config |
| | | **27** | **Sine mode** |

The header base is searched for in the first 64 bytes of the page and must match exactly once (FW main = 32,
plausible layout revision, ordered throttle endpoints).

### 6.4 What the open BLHeli source (bitdump/BLHeli) tells us
- The `BLHeli_32 ARM` folder has no source code or layout, only the manual, a README (project shut down May 2024)
  and `Digital_Cmd_Spec.txt`.
- `Digital_Cmd_Spec.txt` lists the DShot special commands (beeps 1–5, ESC info 6, direction 7/8/20/21, 3D 9/10,
  save settings 12, extended telemetry 13/14, LEDs 22–29). **None of them set timing, PWM frequency, demag,
  startup power, sine mode or brake**, so DShot can't replace passthrough for this project.
- The BLHeli_S source (`BLHeli_S SiLabs/BLHeli_S.asm`, GPL) shows the family's settings convention:
  bytes 0–2 = FW main / FW sub / layout revision (**supports the header assumption above**), and an
  "initialised" signature `0x55 0xAA` at bytes 13–14. BLHeli_S's parameter order differs from BLHeli_32's
  (single-byte throttle, governor fields, no sine mode), so its offsets are **not** reused. The app reports
  "BLHeli_S-style layout" if it sees `32 … 0x55 0xAA` at those positions instead of the guessed map.

BLHeli_S layout for comparison (offset: field): 0 FW main, 1 FW sub, 2 layout, 3–8 governor/legacy, 9 startup power,
10 PWM freq (unused), 11 direction, 12 input polarity, 13 `0x55`, 14 `0xAA`, 15 TX program enable, 16–20 legacy,
21 comm timing, 22–24 legacy, 25 min throttle, 26 max throttle, 27 beep strength, 28 beacon strength, 29 beacon delay,
30 throttle rate, 31 demag comp, 32 BEC voltage, 33 center throttle, 34 spoolup time, 35 temp protection,
36 power protection, 37 PWM input, 38 PWM dither, 39 brake on stop, 40 LED control.

---

## 7. Parameter value encodings (from the BLHeli_32 manual + sample export)

| Parameter | Range / options | Encoding | Confidence |
|---|---|---|---|
| Motor timing | ~1°–31° in 1° steps, or Auto | 🔴 raw byte; old FW enum (sample = 3) | 🔴 |
| PWM frequency | 16–48 kHz (some newer FW higher) | byte = kHz | 🟡 |
| Demag compensation | Off / Low / High | 1 / 2 / 3 | 🟡 |
| Startup (rampup) power | percentage-like scale (sample 50) | byte | 🟡 |
| Sine modulation mode | Off / On | 0 / 1 | 🟡 |
| Brake on stop | Off / On (newer FW: brake strength) | 0 / 1+ | 🟡 |

---

## 8. Sources
- Betaflight `serial_4way.c` / `serial_4way.h` — frame format, CRC, commands, ARM handling: <https://github.com/betaflight/betaflight/tree/master/src/main/io>
- ArduPilot `AP_BLHeli.cpp` — ARM ESC flash address 0x7C00, BLHeli_32 ID rule: <https://github.com/ArduPilot/ardupilot/blob/master/libraries/AP_BLHeli/AP_BLHeli.cpp>
- BLHeliSuite32 export example (iFlight BLHeli_32): <https://discuss.bluerobotics.com/t/blhelisuite32-and-suggested-firmware-parameters-new-bidirection-options-etc/6046>
- BLHeli_32 manual Rev32.x (parameter ranges): <https://flymod.net/download/manual_bl_heli_32>
- BLHeliSuite32 revision history (layout revisions up to 48): <https://img.staticdj.com/5523e7c5c0d8f90764a469dcace215ab.pdf>
- BLHeli_32 discontinuation / repo: <https://github.com/bitdump/BLHeli/tree/master/BLHeli_32%20ARM>
- BLHeli_32 DShot command spec: <https://github.com/bitdump/BLHeli/blob/master/BLHeli_32%20ARM/BLHeli_32%20Firmware%20specs/Digital_Cmd_Spec.txt>
- BLHeli_S EEPROM layout source: <https://github.com/bitdump/BLHeli/blob/master/BLHeli_S%20SiLabs/BLHeli_S.asm>
- Passthrough troubleshooting (battery, motor protocol): <https://intofpv.com/t-blheli-32-version-error-with-passthrough>
