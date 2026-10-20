#include "PresetStore.h"

#include <Preferences.h>
#include <stdio.h>

namespace {
Preferences prefs;

void slotKey(int slot, char* key, size_t n) { snprintf(key, n, "p%d", slot); }
}  // namespace

void PresetStore::begin() { prefs.begin("escpresets", false); }

bool PresetStore::load(int slot, uint8_t values[P_COUNT]) {
  char key[8];
  slotKey(slot, key, sizeof(key));
  uint8_t tmp[P_COUNT];
  if (prefs.getBytesLength(key) != P_COUNT || prefs.getBytes(key, tmp, P_COUNT) != P_COUNT) return false;
  if (!valuesEditable(tmp)) return false;
  for (int i = 0; i < P_COUNT; i++) values[i] = tmp[i];
  return true;
}

void PresetStore::save(int slot, const uint8_t values[P_COUNT]) {
  char key[8];
  slotKey(slot, key, sizeof(key));
  prefs.putBytes(key, values, P_COUNT);
}
