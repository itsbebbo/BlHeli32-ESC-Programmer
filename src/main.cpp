// M5Stack CoreS3 BLHeli_32 ESC programmer.
// CoreS3 USB-C (host) -> Betaflight FC (MSP_SET_PASSTHROUGH) -> 4-way interface -> 4 x BLHeli_32 ESCs.

#include <M5Unified.h>

#include "EscProgrammer.h"
#include "PresetStore.h"
#include "UsbCdcHost.h"

namespace {

enum class Screen { Home, Editor, Presets, Progress };

struct Button {
  int16_t x, y, w, h;
  const char* label;
};

constexpr int16_t kW = 320;
constexpr int16_t kHeaderH = 22;
constexpr int16_t kRowH = 16;

const Button kHomeRead = {4, 206, 100, 32, "Read"};
const Button kHomeEdit = {110, 206, 100, 32, "Edit"};
const Button kHomePresets = {216, 206, 100, 32, "Presets"};

const Button kEditBack = {4, 206, 100, 32, "Back"};
const Button kEditSave = {110, 206, 100, 32, "Save..."};
const Button kEditWrite = {216, 206, 100, 32, "Write"};

const Button kPresetBack = {4, 206, 74, 32, "Back"};
const Button kPresetLoad = {82, 206, 74, 32, "Edit"};
const Button kPresetApply = {160, 206, 74, 32, "Apply"};
const Button kPresetSave = {238, 206, 78, 32, "Save"};

const Button kProgressOk = {110, 206, 100, 32, "OK"};

UsbCdcHost usb;
EscProgrammer programmer(usb);
PresetStore presets;

Screen screen = Screen::Home;
const char* screenTitle = "BLHeli_32 Programmer";
bool lastUsbState = false;

EscImage escs[kEscCount];
uint8_t editValues[P_COUNT];
int selectedSlot = -1;

// Post-write check: after a power cycle the next Read must show the written values.
bool expectPending = false;
uint8_t expectValues[P_COUNT];

char banner[48] = "";
uint16_t bannerColor = TFT_WHITE;

constexpr int kLogLines = 10;
char logLines[kLogLines][48];
int logCount = 0;

void setBanner(uint16_t color, const char* text) {
  bannerColor = color;
  snprintf(banner, sizeof(banner), "%s", text);
}

bool hit(const Button& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

void drawButton(const Button& b, uint16_t color = TFT_DARKGREY) {
  M5.Display.fillRoundRect(b.x, b.y, b.w, b.h, 6, color);
  M5.Display.setTextColor(TFT_WHITE, color);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(b.label, b.x + b.w / 2, b.y + b.h / 2);
}

void drawHeader() {
  M5.Display.fillRect(0, 0, kW, kHeaderH, TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextDatum(middle_left);
  M5.Display.drawString(screenTitle, 4, kHeaderH / 2);
  bool c = usb.connected();
  M5.Display.setTextColor(c ? TFT_GREEN : TFT_RED, TFT_NAVY);
  M5.Display.setTextDatum(middle_right);
  M5.Display.drawString(c ? "FC USB OK" : "No FC", kW - 4, kHeaderH / 2);
}

void beginScreen(Screen s, const char* title) {
  screen = s;
  screenTitle = title;
  M5.Display.fillScreen(TFT_BLACK);
  drawHeader();
}

// ---------------------------------------------------------------- Home

// An ESC that answered but failed a check (as opposed to one that was never read or not found).
bool hasProblem(const EscImage& e) {
  return !e.valid && e.problem[0] && strcmp(e.problem, "not found") != 0;
}

void drawHome() {
  beginScreen(Screen::Home, "BLHeli_32 Programmer");
  constexpr int16_t colLabel = 4, colX = 112, colW = 52, top = kHeaderH + 4;
  char buf[16];

  auto cell = [&](int col, int row, const char* text, uint16_t color) {
    M5.Display.setTextColor(color, TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString(text, colX + col * colW + colW / 2, top + row * kRowH);
  };
  auto label = [&](int row, const char* text) {
    M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    M5.Display.setTextDatum(top_left);
    M5.Display.drawString(text, colLabel, top + row * kRowH);
  };

  label(1, "Firmware");
  label(2, "Layout");
  for (int p = 0; p < P_COUNT; p++) label(3 + p, kParams[p].name);
  label(9, "Status");

  for (int i = 0; i < kEscCount; i++) {
    const EscImage& e = escs[i];
    snprintf(buf, sizeof(buf), "ESC%d", i + 1);
    cell(i, 0, buf, TFT_CYAN);
    if (e.valid) {
      snprintf(buf, sizeof(buf), "%u.%u", e.fwMain, e.fwSub);
      cell(i, 1, buf, TFT_WHITE);
      snprintf(buf, sizeof(buf), "%u", e.layout);
      cell(i, 2, buf, TFT_WHITE);
    } else {
      cell(i, 1, "-", TFT_DARKGREY);
      cell(i, 2, "-", TFT_DARKGREY);
    }
    for (int p = 0; p < P_COUNT; p++) {
      if (!e.valid) {
        cell(i, 3 + p, "-", TFT_DARKGREY);
        continue;
      }
      bool differs = escs[0].valid && escs[0].values[p] != e.values[p];
      cell(i, 3 + p, formatValue(p, e.values[p], buf, sizeof(buf)), differs ? TFT_YELLOW : TFT_WHITE);
    }
    if (e.valid) {
      cell(i, 9, "OK", TFT_GREEN);
    } else if (hasProblem(e)) {
      cell(i, 9, "ERR", TFT_RED);
    } else {
      cell(i, 9, "--", TFT_DARKGREY);
    }
  }

  // Banner: explicit message first, otherwise the first ESC problem.
  char line[48];
  uint16_t color = bannerColor;
  snprintf(line, sizeof(line), "%s", banner);
  if (!line[0]) {
    for (int i = 0; i < kEscCount; i++) {
      if (hasProblem(escs[i])) {
        snprintf(line, sizeof(line), "ESC%d: %s", i + 1, escs[i].problem);
        color = TFT_RED;
        break;
      }
    }
  }
  M5.Display.setTextColor(color, TFT_BLACK);
  M5.Display.setTextDatum(top_left);
  M5.Display.drawString(line, 4, top + 10 * kRowH + 2);

  drawButton(kHomeRead, TFT_DARKGREEN);
  drawButton(kHomeEdit);
  drawButton(kHomePresets);
}

// ---------------------------------------------------------------- Progress

void drawLog() {
  M5.Display.fillRect(0, kHeaderH, kW, 182, TFT_BLACK);
  M5.Display.setTextDatum(top_left);
  for (int i = 0; i < logCount; i++) {
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(logLines[i], 4, kHeaderH + 4 + i * 17);
  }
}

void logLine(const char* text) {
  if (logCount == kLogLines) {
    for (int i = 1; i < kLogLines; i++) memcpy(logLines[i - 1], logLines[i], sizeof(logLines[0]));
    logCount--;
  }
  snprintf(logLines[logCount++], sizeof(logLines[0]), "%s", text);
  drawLog();
}

void beginProgress(const char* title) {
  beginScreen(Screen::Progress, title);
  logCount = 0;
}

void finishProgress() { drawButton(kProgressOk, TFT_DARKGREEN); }

// ---------------------------------------------------------------- Actions

bool requireUsb() {
  if (usb.connected()) return true;
  setBanner(TFT_RED, "Connect the FC to the USB-C port");
  drawHome();
  return false;
}

void doRead() {
  if (!requireUsb()) return;
  beginProgress("Reading ESCs");
  logLine("Battery on, props off!");
  banner[0] = '\0';
  if (!programmer.readAll(escs, logLine)) {
    setBanner(TFT_RED, "Could not enter passthrough");
  } else if (expectPending) {
    bool anyValid = false, match = true;
    for (int i = 0; i < kEscCount; i++) {
      if (!escs[i].valid) continue;
      anyValid = true;
      if (memcmp(escs[i].values, expectValues, P_COUNT) != 0) match = false;
    }
    if (anyValid) {
      expectPending = false;
      if (match) {
        setBanner(TFT_GREEN, "Confirmed: settings kept after restart");
      } else {
        setBanner(TFT_RED, "Values REVERTED - layout guess rejected");
      }
    }
  }
  logLine(banner[0] ? banner : "Done");
  finishProgress();
}

void doWrite(const uint8_t values[P_COUNT]) {
  if (!requireUsb()) return;
  beginProgress("Writing ESCs");
  logLine("Battery on, props off!");
  char results[kEscCount][EscProgrammer::kResultLen];
  bool ok = programmer.writeAll(values, escs, results, logLine);
  char line[48];
  for (int i = 0; i < kEscCount; i++) {
    snprintf(line, sizeof(line), "ESC%d: %s", i + 1, results[i]);
    logLine(line);
  }
  if (ok) {
    memcpy(expectValues, values, P_COUNT);
    expectPending = true;
    setBanner(TFT_YELLOW, "Power-cycle battery, then Read");
  } else {
    setBanner(TFT_RED, "Write failed - see log");
  }
  logLine(banner);
  finishProgress();
}

// ---------------------------------------------------------------- Editor

constexpr int16_t kEditTop = kHeaderH + 6;
constexpr int16_t kEditRowH = 29;

Button minusButton(int row) { return {232, (int16_t)(kEditTop + row * kEditRowH), 40, 26, "-"}; }
Button plusButton(int row) { return {276, (int16_t)(kEditTop + row * kEditRowH), 40, 26, "+"}; }

void drawEditorValue(int row) {
  char buf[16];
  int16_t y = kEditTop + row * kEditRowH;
  M5.Display.fillRect(150, y, 80, 26, TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(formatValue(row, editValues[row], buf, sizeof(buf)), 190, y + 13);
}

void drawEditor() {
  beginScreen(Screen::Editor, "Edit timing & drive");
  for (int p = 0; p < P_COUNT; p++) {
    int16_t y = kEditTop + p * kEditRowH;
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(middle_left);
    M5.Display.drawString(kParams[p].name, 4, y + 13);
    drawEditorValue(p);
    drawButton(minusButton(p));
    drawButton(plusButton(p));
  }
  drawButton(kEditBack);
  drawButton(kEditSave);
  drawButton(kEditWrite, TFT_RED);
}

void openEditor() {
  // Start from the ESCs' current values when available, otherwise keep the last edit / defaults.
  if (escs[0].valid) memcpy(editValues, escs[0].values, P_COUNT);
  drawEditor();
}

// ---------------------------------------------------------------- Presets

Button slotButton(int slot) {
  return {(int16_t)(4 + (slot % 2) * 158), (int16_t)(kHeaderH + 4 + (slot / 2) * 59), 154, 55, nullptr};
}

void drawSlot(int slot) {
  Button b = slotButton(slot);
  bool selected = slot == selectedSlot;
  M5.Display.fillRoundRect(b.x, b.y, b.w, b.h, 6, selected ? TFT_DARKCYAN : TFT_DARKGREY);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(top_left);

  uint8_t v[P_COUNT];
  char line[32], a[12], c[12];
  snprintf(line, sizeof(line), "Preset %d", slot + 1);
  M5.Display.drawString(line, b.x + 6, b.y + 3);
  if (!presets.load(slot, v)) {
    M5.Display.drawString("(empty)", b.x + 6, b.y + 20);
    return;
  }
  snprintf(line, sizeof(line), "T%u %ukHz %s", v[P_TIMING], v[P_PWM_FREQ],
           formatValue(P_DEMAG, v[P_DEMAG], a, sizeof(a)));
  M5.Display.drawString(line, b.x + 6, b.y + 20);
  snprintf(line, sizeof(line), "St%u%% Sin:%s Br:%s", v[P_STARTUP_PWR],
           formatValue(P_SINE_MODE, v[P_SINE_MODE], a, sizeof(a)),
           formatValue(P_BRAKE_ON_STOP, v[P_BRAKE_ON_STOP], c, sizeof(c)));
  M5.Display.drawString(line, b.x + 6, b.y + 37);
}

void drawPresets() {
  beginScreen(Screen::Presets, "Presets");
  for (int s = 0; s < kPresetCount; s++) drawSlot(s);
  drawButton(kPresetBack);
  drawButton(kPresetLoad);
  drawButton(kPresetApply, TFT_RED);
  drawButton(kPresetSave);
}

// ---------------------------------------------------------------- Touch

void handleTouch(int x, int y) {
  switch (screen) {
    case Screen::Home:
      if (hit(kHomeRead, x, y)) doRead();
      else if (hit(kHomeEdit, x, y)) openEditor();
      else if (hit(kHomePresets, x, y)) drawPresets();
      break;

    case Screen::Editor:
      for (int p = 0; p < P_COUNT; p++) {
        if (hit(minusButton(p), x, y) && editValues[p] > kParams[p].editMin) {
          editValues[p]--;
          drawEditorValue(p);
        } else if (hit(plusButton(p), x, y) && editValues[p] < kParams[p].editMax) {
          editValues[p]++;
          drawEditorValue(p);
        }
      }
      if (hit(kEditBack, x, y)) drawHome();
      else if (hit(kEditSave, x, y)) drawPresets();
      else if (hit(kEditWrite, x, y)) doWrite(editValues);
      break;

    case Screen::Presets: {
      for (int s = 0; s < kPresetCount; s++) {
        if (hit(slotButton(s), x, y)) {
          int old = selectedSlot;
          selectedSlot = s;
          if (old >= 0) drawSlot(old);
          drawSlot(s);
          return;
        }
      }
      uint8_t v[P_COUNT];
      bool loaded = selectedSlot >= 0 && presets.load(selectedSlot, v);
      if (hit(kPresetBack, x, y)) {
        drawHome();
      } else if (hit(kPresetLoad, x, y) && loaded) {
        memcpy(editValues, v, P_COUNT);
        drawEditor();
      } else if (hit(kPresetApply, x, y) && loaded) {
        memcpy(editValues, v, P_COUNT);
        doWrite(v);
      } else if (hit(kPresetSave, x, y) && selectedSlot >= 0) {
        presets.save(selectedSlot, editValues);
        drawSlot(selectedSlot);
      }
      break;
    }

    case Screen::Progress:
      if (hit(kProgressOk, x, y)) drawHome();
      break;
  }
}

}  // namespace

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setFont(&fonts::Font2);
  // The CoreS3 is the USB host, so it must supply 5 V to the flight controller's USB port.
  M5.Power.setUsbOutput(true);

  presets.begin();
  for (int p = 0; p < P_COUNT; p++) editValues[p] = kParams[p].defaultValue;
  for (int i = 0; i < kEscCount; i++) {
    memset(&escs[i], 0, sizeof(EscImage));
    escs[i].base = -1;
  }

  if (!usb.begin()) setBanner(TFT_RED, "USB host init failed");
  else setBanner(TFT_LIGHTGREY, "Connect FC + battery, then Read");
  drawHome();
}

void loop() {
  M5.update();

  bool c = usb.connected();
  if (c != lastUsbState) {
    lastUsbState = c;
    drawHeader();
  }

  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) handleTouch(t.x, t.y);
  delay(10);
}
