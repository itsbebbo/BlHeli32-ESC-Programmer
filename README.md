# CoreS3 BLHeli_32 ESC Programmer

An M5Stack CoreS3 program that sets the timing and drive parameters on 4 × iFlight Blitz E80 Single (BLHeli_32) ESCs
through a Betaflight 4.5 flight controller's USB port. You don't need a PC or BLHeliSuite32.

Parameters it sets: motor timing, PWM frequency, demag compensation, startup power, sine mode, brake on stop.

> ⚠️ **The settings byte positions are unverified best guesses.** BLHeli_32 is closed source and its settings layout
> is not published. Guards in the code refuse to write unless the guessed layout decodes to plausible, matching
> values on all 4 ESCs, but there is no guarantee the guess is right. See
> [BLHeli32_Protocol_and_Layout.md](BLHeli32_Protocol_and_Layout.md) for details.

## Hardware

```
CoreS3 USB-C ──(USB-C OTG cable/adapter)──► FC USB port
                                            FC ──► 4 × E80 ESCs (motor outputs 1–4)
Flight battery ──► ESCs / FC   (required: ESCs are not powered by USB)
```

- The CoreS3 acts as the **USB host** and supplies 5 V to the flight controller. Run the CoreS3 from its internal
  battery (or a DIN/M-Bus base), because its USB-C port is in use.
- In Betaflight, a motor protocol (e.g. DShot) must be configured, otherwise passthrough finds no ESCs.
- **Remove the props.** Connect the flight battery before tapping Read or Write.

## Build & flash (PlatformIO)

```
pio run -t upload
```

The firmware switches the CoreS3's USB port to host mode, so once it's running the PC can no longer auto-reset it
for uploads. To re-flash, put the CoreS3 into download mode first: hold the reset button for about 3 s until the
green LED lights, then run the upload.

## Using it

| Screen | What it does |
|---|---|
| **Home** | Table of firmware, layout revision and the 6 values for ESC1–4. Yellow = differs from ESC1. **Read** fetches the current settings. |
| **Edit** | Change each value with `-`/`+`. **Write** writes the values to all 4 ESCs. **Save...** opens Presets to store them. |
| **Presets** | 6 slots stored in CoreS3 flash. Tap a slot to select it, then **Edit** (load into the editor), **Apply** (one-tap write), or **Save** (store the editor's current values). |

### What a write does
1. Enters passthrough and re-reads all 4 ESCs.
2. **Mismatch guard:** aborts with nothing changed unless all 4 are BLHeli_32, have the same firmware, layout
   revision and MCU ID, and decode to plausible values.
3. For each ESC it reads the whole 1 KB settings page, patches the 6 bytes, erases the page, writes it back and
   **verifies by read-back**. It stops at the first failure.
4. **Page-size safety net:** it snapshots the 1 KB below the settings page before erasing. If the MCU erased 2 KB,
   that snapshot is restored and the result says `PASS (2K page fixed)`.
5. Afterwards **power-cycle the battery and tap Read.** The banner shows either "Confirmed" or "Values REVERTED".
   REVERTED means the firmware rejected the block, so the layout guess is wrong; stop using Write.

If a write fails partway, tapping Write again rebuilds that ESC's settings from the last good copy read in this
session (this copy lives in RAM, so don't power off the CoreS3 in between).

### Value notes
- **Timing (raw)**: shown as the stored number, range 1–31. Its encoding differs between BLHeli_32 versions.
- **PWM freq**: kHz, 16–48. **Startup power**: %, 3–150. **Demag**: Off/Low/High.
- **Sine mode** and **Brake on stop**: Off/On.

## Source layout

| File | Purpose |
|---|---|
| `src/UsbCdcHost.*` | ESP32-S3 USB host CDC-ACM driver (FC virtual COM port) |
| `src/Msp.*` | MSP v1 (FC variant/version, `MSP_SET_PASSTHROUGH`) |
| `src/FourWay.*` | Betaflight 4-way interface protocol (CRC16-XMODEM frames) |
| `src/EscLayout.*` | Settings layout guess, validation and value formatting |
| `src/EscProgrammer.*` | Read/write orchestration, guards, verify |
| `src/PresetStore.*` | Presets in NVS |
| `src/main.cpp` | Touch UI |
