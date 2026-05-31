# EnergyCalibrator — Errors History

Append one entry per non-trivial debugging session.

---

<!-- template
### YYYY-MM-DD — <short title>
**Symptom:** …
**Root cause:** …
**Fix:** …
-->

---

### 2026-05-31 — SDM630 second Modbus read timeout (err=226)

**Symptom:** `/api/sdm` returned `{"err":"SDM630 poll failed"}`. Error log showed `SDM630 read2 err=226` (ku8MBResponseTimedOut). Read 1 (0x0000, 32 regs → V/A/W/PF) succeeded; Read 2 (0x0046, 4 regs → Hz/kWh) timed out.

**Root cause:** SDM630 at 9600 baud needs recovery time between consecutive Modbus requests. The two reads were back-to-back with no delay.

**Fix:** Added `delay(100)` between Read 1 and Read 2 in `sdm630Poll()` (`EnergyCalibrator.ino`). Released in v1.0.1.

---

### 2026-05-31 — MQTT subscription `cal_+/sec` invalid filter

**Symptom:** `cal_collector.py` connected to broker but immediately logged `[MQTT] Invalid subscription filter` and reconnected in a loop. No data was stored.

**Root cause:** MQTT spec requires the `+` wildcard to occupy an entire topic level. `cal_+` embeds `+` inside a level name — invalid. The `paho` library rejected it.

**Fix:** Changed subscriptions to `+/sec` and `+/min` (valid single-level wildcards) with a `unit.startswith('cal_')` prefix filter in `on_message`.

---

### 2026-05-31 — Energy deviation inflated by W-snapshot integration

**Symptom:** Live monitor showed CT energy deviations of −4% to −7% vs SDM630. PDF report (using `sum(X_dkwh)` from cal_min) showed only ±1%. Same period, same data, different numbers.

**Root cause:** The monitor computed box energy as `sum(W_sec) / 3_600_000` — integrating per-second power snapshots from cal_sec. Approximately 3.5% of cal_sec rows are missing (MQTT packets dropped in transit). Those seconds are not counted, so the box energy is systematically under-counted by ~3.5%, making the deviation appear larger than it really is. The PDF used `sum(R_dkwh)` from cal_min, which is the box firmware's own energy accumulator — unaffected by MQTT drops.

**Fix:** Monitor updated to use `sum(R/S/T_dkwh)` from cal_min for box energy, consistent with the PDF. W-snapshot integration (`sum(W_sec)/3_600_000`) must never be used for energy deviation calculation.

**Rule:** All energy comparisons must use counter deltas (`dkwh` columns). `W` values in cal_sec and cal_min are instantaneous snapshots — valid for power display, not for energy accounting.

---

### 2026-05-31 — OTA silent failure with `--data-binary`

**Symptom:** `curl --data-binary @file.bin` returned exit 56 (connection reset), but Unit D rebooted and stayed on the old firmware version.

**Root cause:** The ESP32 WebServer upload handler (`handleOtaUpload`) is only triggered by `multipart/form-data` requests. Raw `application/octet-stream` body is not routed through the upload callbacks, so `_otaMetaOk` stays false and `Update.end(false)` aborts the flash — but the device still rebooted (from `Update.begin` or another cause), silently keeping old firmware.

**Fix:** Use `curl -F "firmware=@file.bin;type=application/octet-stream"` (multipart). This matches the web UI's `FormData` upload behaviour.
