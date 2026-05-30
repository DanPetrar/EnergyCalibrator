#pragma once
#include <LittleFS.h>
#include "Config.h"

// Binary append log of MinRecord structs.
// File: /energy.bin  (32 bytes per record, no header)
// LittleFS partition (~1.4 MB on default 4 MB scheme):
//   max records ≈ 44,000  →  ~30 days at 1 rec/min

static const char  ENERGY_FILE[]    = "/energy.bin";
static const char  PREV_BOX_FILE[]  = "/prev_box.bin";  // 24 bytes: float[3] kwh + float[3] kvarh
static const size_t ENERGY_MAX      = 1200000UL;  // ~1.2 MB cap (~26 days)

static bool _lfsOk = false;

static bool energyLogInit() {
  if (!LittleFS.begin(true)) {   // true = format on fail
    Serial.println("[LFS] Mount/format failed");
    _lfsOk = false;
    return false;
  }
  _lfsOk = true;
  size_t used  = LittleFS.usedBytes();
  size_t total = LittleFS.totalBytes();
  uint32_t recs = 0;
  if (LittleFS.exists(ENERGY_FILE)) {
    File f = LittleFS.open(ENERGY_FILE, "r");
    if (f) { recs = f.size() / sizeof(MinRecord); f.close(); }
  }
  Serial.printf("[LFS] Mounted — %u/%u bytes, %u energy records stored\n",
                (unsigned)used, (unsigned)total, (unsigned)recs);
  return true;
}

static void energyLogAppend(const MinRecord& rec) {
  if (!_lfsOk) return;

  // Rotate if approaching cap: delete oldest half by rewriting
  if (LittleFS.exists(ENERGY_FILE)) {
    File f = LittleFS.open(ENERGY_FILE, "r");
    if (f && f.size() >= ENERGY_MAX) {
      size_t sz    = f.size();
      size_t keep  = sz / 2;
      // Align to record boundary
      size_t skip  = (sz - keep) / sizeof(MinRecord) * sizeof(MinRecord);
      keep         = sz - skip;
      uint8_t* tmp = (uint8_t*)ps_malloc(keep);
      if (tmp) {
        f.seek(skip);
        f.read(tmp, keep);
        f.close();
        LittleFS.remove(ENERGY_FILE);
        File nf = LittleFS.open(ENERGY_FILE, "w");
        if (nf) { nf.write(tmp, keep); nf.close(); }
        free(tmp);
        Serial.printf("[LFS] Rotated energy.bin: kept %u bytes\n", (unsigned)keep);
      } else {
        f.close();
      }
    } else if (f) {
      f.close();
    }
  }

  File f = LittleFS.open(ENERGY_FILE, "a");
  if (!f) { Serial.println("[LFS] Append failed"); return; }
  f.write((const uint8_t*)&rec, sizeof(MinRecord));
  f.close();
}

static uint32_t energyLogCount() {
  if (!_lfsOk || !LittleFS.exists(ENERGY_FILE)) return 0;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return 0;
  uint32_t n = f.size() / sizeof(MinRecord);
  f.close();
  return n;
}

// Save prevBoxKwh[3] and prevBoxKvarh[3] to /prev_box.bin (24 bytes, fixed-size overwrite).
static void energySavePrevBox(const float kwh[3], const float kvarh[3]) {
  if (!_lfsOk) return;
  File f = LittleFS.open(PREV_BOX_FILE, "w");
  if (!f) { Serial.println("[LFS] prev_box write failed"); return; }
  f.write((const uint8_t*)kwh,   3 * sizeof(float));
  f.write((const uint8_t*)kvarh, 3 * sizeof(float));
  f.close();
}

// Load prevBoxKwh[3] and prevBoxKvarh[3] from /prev_box.bin.
// Returns true if the file existed and was read successfully.
static bool energyLoadPrevBox(float kwh[3], float kvarh[3]) {
  if (!_lfsOk || !LittleFS.exists(PREV_BOX_FILE)) return false;
  File f = LittleFS.open(PREV_BOX_FILE, "r");
  if (!f || f.size() < 6 * (int)sizeof(float)) { if (f) f.close(); return false; }
  bool ok = (f.read((uint8_t*)kwh,   3 * sizeof(float)) == 3 * sizeof(float)) &&
            (f.read((uint8_t*)kvarh, 3 * sizeof(float)) == 3 * sizeof(float));
  f.close();
  return ok;
}

// Read the last stored MinRecord into out. Returns true if a record was found.
static bool energyLoadLast(MinRecord& out) {
  if (!_lfsOk || !LittleFS.exists(ENERGY_FILE)) return false;
  File f = LittleFS.open(ENERGY_FILE, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz < sizeof(MinRecord)) { f.close(); return false; }
  f.seek(sz - sizeof(MinRecord));
  bool ok = (f.read((uint8_t*)&out, sizeof(MinRecord)) == sizeof(MinRecord));
  f.close();
  return ok;
}
