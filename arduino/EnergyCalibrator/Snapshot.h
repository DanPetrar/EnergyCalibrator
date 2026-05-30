#pragma once
#include <LittleFS.h>
#include "RingBuf.h"
#include "Config.h"
#include "ErrorLog.h"

// Periodic atomic snapshot of PSRAM rings to LittleFS.
// Write flow: write to .tmp → rename to .bin (atomic; old file survives a write failure).
// Restore flow: push records oldest-first into ring on boot.

#if TEST_MODE
#define SNAP_INTERVAL_S  120UL    // 2 min in test mode
#else
#define SNAP_INTERVAL_S  3600UL   // 1 hour in production
#endif

static const char SEC_SNAP_TMP[] = "/sec_snap.tmp";
static const char SEC_SNAP_BIN[] = "/sec_snap.bin";
static const char MIN_SNAP_TMP[] = "/min_snap.tmp";
static const char MIN_SNAP_BIN[] = "/min_snap.bin";

extern bool lfsOk;

// 8-byte header prepended to every snapshot file.
// If magic, data_ver, or rec_size mismatches on load, the file is discarded.
struct SnapHeader {
  uint32_t magic;     // 0x5A415853 ('ZAXS')
  uint8_t  data_ver;  // DATA_VERSION at write time
  uint8_t  rec_size;  // sizeof(T) — 76 for SecRecord, 32 for MinRecord
  uint16_t reserved;
};

// Write ring contents (oldest→newest) to tmp, then rename to dst.
// Returns true on success; leaves dst untouched on any failure.
template<typename T>
static bool _snapWrite(RingBuf<T>& ring, const char* tmp, const char* dst) {
  uint32_t cnt = ring.cnt;
  if (cnt == 0) return true;

  File f = LittleFS.open(tmp, "w");
  if (!f) return false;

  SnapHeader hdr = { 0x5A415853UL, DATA_VERSION, (uint8_t)sizeof(T), 0 };
  bool ok = (f.write((const uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr));

  for (uint32_t age = cnt; age-- > 0 && ok; ) {
    T rec;
    if (!ring.get(age, rec)) { ok = false; break; }
    if (f.write((const uint8_t*)&rec, sizeof(T)) != sizeof(T)) ok = false;
  }
  f.close();

  if (!ok) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(dst);
  return LittleFS.rename(tmp, dst);
}

// Read snapshot file and push records (oldest-first) into ring.
// If the snapshot has more records than ring.cap, the oldest are naturally overwritten.
template<typename T>
static uint32_t _snapRead(RingBuf<T>& ring, const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;

  SnapHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic    != 0x5A415853UL ||
      hdr.data_ver != DATA_VERSION  ||
      hdr.rec_size != (uint8_t)sizeof(T)) {
    f.close();
    LittleFS.remove(path);
    errorLog("WARN", "Snap header mismatch — discarded");
    return 0;
  }

  uint32_t n = 0;
  T rec;
  while (f.read((uint8_t*)&rec, sizeof(T)) == sizeof(T)) {
    ring.push(rec);
    n++;
  }
  f.close();
  return n;
}

// Save both rings to LittleFS. Skipped if LittleFS is not mounted.
static void snapshotSave(RingBuf<SecRecord>& secBuf, RingBuf<MinRecord>& minBuf) {
  if (!lfsOk) return;
  uint32_t t0 = millis();
  bool ok1 = _snapWrite(secBuf, SEC_SNAP_TMP, SEC_SNAP_BIN);
  bool ok2 = _snapWrite(minBuf, MIN_SNAP_TMP, MIN_SNAP_BIN);
  if (!ok1) errorLog("ERROR", "Snapshot write failed: sec");
  if (!ok2) errorLog("ERROR", "Snapshot write failed: min");
  Serial.printf("[SNAP] sec=%s (%u rec)  min=%s (%u rec)  %u ms\n",
                ok1 ? "ok" : "FAIL", (unsigned)secBuf.cnt,
                ok2 ? "ok" : "FAIL", (unsigned)minBuf.cnt,
                (unsigned)(millis() - t0));
}

// Restore both rings from LittleFS snapshots. Call once after ring init + LittleFS mount.
static void snapshotLoad(RingBuf<SecRecord>& secBuf, RingBuf<MinRecord>& minBuf) {
  if (!lfsOk) return;
  uint32_t n1 = _snapRead(secBuf, SEC_SNAP_BIN);
  uint32_t n2 = _snapRead(minBuf, MIN_SNAP_BIN);
  if (n1 || n2)
    Serial.printf("[SNAP] Restored sec=%u  min=%u records from flash\n", n1, n2);
}
