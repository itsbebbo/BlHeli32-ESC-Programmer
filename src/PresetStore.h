#pragma once

#include "EscLayout.h"

constexpr int kPresetCount = 6;

// Presets saved from the editor, kept in NVS flash.
class PresetStore {
public:
  void begin();
  bool load(int slot, uint8_t values[P_COUNT]);
  void save(int slot, const uint8_t values[P_COUNT]);
};
