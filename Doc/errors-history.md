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

### 2026-06-03 — Phase E session UI: report 500 when run from /tmp

**Symptom:** While smoke-testing `serve.py` from a copy in `/tmp` on the WS, `POST /sessions/<id>/report` returned 500 (no PDF).

**Root cause:** `GEN_REPORT` is built relative to the script dir (`os.path.dirname(__file__)/../report/generate_report.py`). Running the copy from `/tmp` resolved it to `/report/generate_report.py`, which doesn't exist. Test-harness artifact, not a code bug — in production `serve.py` lives in the repo `reports/` dir so the path is correct.

**Fix:** Verify via the real deployed service (git pull → `cal_reports` restart → test on `:8080`), not an out-of-tree copy. End-to-end then passed (real 5.4 KB PDF generated + downloaded).

---

### 2026-06-03 — Stale test server stopped the live PS-1110 session

**Symptom:** During Phase E.2 testing, the live session #1 (PS-1110) flipped from `running` to `stopped` (stop_ts 12:24) unexpectedly; pause/continue curls in a lifecycle test returned 404.

**Root cause:** An old alt-port (`:8099`) test server from a *previous session's* background task was still running and configured with `CAL_DB=` the **live** DB. The new test server failed to bind (port in use), so the lifecycle curls hit the **old** server — which lacked pause/continue (→404) but whose `stop` handler ran `UPDATE sessions SET status='stopped' WHERE status='running'` against the live DB, stopping PS-1110. (No data lost — the collector is independent; the one-running index blocked the stray `start`.)

**Fix:** `fuser -k 8099/tcp` to kill the stale server; restored session #1 (`status='running'`, `stop_ts=NULL`). **Rule:** test servers must use a **copy** DB (`cp cal_data.db /tmp/...`), and always free the port (`fuser -k`) before launching — never let an alt-port server point at the live DB.

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

---

### 2026-06-01 — Report mislabels a perfect CT (0.00%) as "Needs calibration"

**Symptom:** In a PDF report, CT-S showed deviation `+0.00%` but assessment
"Needs calibration" and was ranked last — the opposite of correct (0% is the
best result).

**Root cause:** The ranking/assessment used `abs(d or 999)` to substitute `999`
when the deviation was `None`. But in Python `0.0` is falsy, so `0.0 or 999`
evaluates to `999` → `abs(999)=999` ≥ 6% → "Needs calibration", and the sort key
became 999 so the best CT sorted worst. The displayed number still rendered
`+0.00%`.

**Fix:** Module-level `_absdev(d) = abs(d) if d is not None else 999`, used for
both ranking and assessment; also fixed the analogous `en['SDM'] and en[ch]`
guard on the Hourly dev. Regression test `test_absdev_exact_zero_is_best`.

**Rule:** never use `x or default` / `a and b` to default or guard a numeric
that can legitimately be 0 — see also the bash `${1:-{}}` JSON trap.

---

### 2026-06-01 — SDM-poll-failure energy misalignment (audit finding F1)

**Symptom:** Found during the firmware dkWh accumulation audit (not a field
failure). On an SDM630 poll failure the paired row isn't published, but
`prevCumKwhAtMin` was advanced every minute regardless — so the skipped minute's
box energy was dropped while the meter telescoped it into the next row, pairing
1 min of box vs 2 min of SDM and biasing box-vs-SDM deviation slightly negative
per failure.

**Root cause:** the per-minute box delta + baseline advanced *before* the poll
result was known, unconditionally.

**Fix (v1.0.3):** compute the box delta and advance `prevCumKwhAtMin` only inside
the successful-publish branch. A skipped minute now folds into the next published
row symmetrically with the meter (`prevMeterKwh` likewise advances only on
success). Verified by host model test `arduino/tests/test_energy_accumulator.py`.
See `energy-audit.md`.

---

### 2026-06-03 — LilyGO T7-S3 LED invisible (wrong driver type)

**Context:** LED on Unit D (LilyGO T7-S3) showed only a tiny dim white flash per NeoPixel data call — every colour appeared identical and nearly invisible since the first build.

- **Root cause:** GPIO17 on the LilyGO T7-S3 is a plain single-color LED, not a WS2812/NeoPixel. The `Adafruit_NeoPixel` protocol sent serial data to a GPIO that only responds to HIGH/LOW. Each `led.show()` produced a brief transition flash.
- **Fix (v1.0.5):** `#if defined(BOARD_LILYGO_T7S3)` conditional compile. LilyGO path uses plain `digitalWrite` with a priority blink state machine (`ledLoop()` called every `loop()`): idle heartbeat / data double-tap / fault long-flash. `led_flash()` on LilyGO only updates a `gLastDataMs` timestamp; `ledLoop()` owns the GPIO exclusively. S3-Zero NeoPixel path unchanged.
- **Prevention:** Verify LED type from the official board schematic before assuming NeoPixel. LilyGO T7-S3 official example (`test.ino`) uses `digitalWrite(LED_PIN, HIGH)` — no NeoPixel library.

---

### 2026-06-02 — Pi→Workstation migration: operational gotchas

**Context:** migrated the bench backend (collector + reports + crons + new InfluxDB/Grafana feed) from the Pi to the Workstation, data-safe (parallel-run → verify → cutover). Several non-obvious traps surfaced:

- **`ssh-copy-id` silently failed.** The Pi key never landed in the Workstation's `authorized_keys` despite the user running it twice; key auth kept returning `Permission denied (publickey)`. Verbose SSH showed the key *was* offered and *rejected* server-side → it was missing, not a perms issue. Fix: append the key directly.
- **`pgrep -f cal_collector.py` self-matches over SSH.** A guard like `pgrep -f cal_collector.py` matches the remote shell running the command string itself (the pattern is literally in argv), giving false "already running". Use `pgrep -x python3` + check `/proc/<pid>/cmdline` instead.
- **`cal_sec_hourly` doesn't exist on a fresh DB.** The collector only creates `cal_sec`/`cal_min`; `cal_sec_hourly` is created by `prune.py` on first run. A merge that referenced it while a snapshot was ATTACHed silently resolved to the *attached* DB's table (SQLite unqualified-name resolution searches main → attached), so it was never created in the main DB. Guard table ops with a `sqlite_master` existence check.
- **DB looked 33 MB after deleting 89% of rows + VACUUM.** The size was uncheckpointed WAL. `PRAGMA wal_checkpoint(TRUNCATE)` then `VACUUM` brought the main file to 3.4 MB.

**No data lost:** Unit D buffers/replays on broker switch, and `INSERT OR REPLACE` (PK `ts,unit`) makes reconciliation merges idempotent. A final VACUUM-INTO snapshot+merge swept the switch-window seconds.

---

### 2026-06-05 — LilyGO T7-S3 battery ADC: wrong pin (GPIO4) and wrong multiplier

**Context:** Battery connected to Unit D (LilyGO T7-S3, ZaxMonitor). Web UI showed 0% / 792 mV; multimeter measured 3.99V.

- **Root cause (1) — wrong pin:** Firmware had `BAT_ADC_PIN = 4` (GPIO4). GPIO4 is not connected to the battery voltage divider on the T7-S3. Official board example uses GPIO2. GPIO4 floated near 0V, giving a near-zero reading regardless of battery state.
- **Root cause (2) — wrong multiplier:** After fixing pin to GPIO2 with USB connected, the ×320/220 formula (assumed R1=100k/R2=220k) appeared to give a correct ~3.87V reading — but only because USB charges via the same node, and the formula happened to reduce the ~5V charging rail to a battery-like value by coincidence. With USB disconnected and battery at 3.87V, the same formula gave 2.83V. Actual divider is R1=R2=100k (×2): 3.87V → 1.94V at ADC → ×2 = 3.88V ✓.
- **Fix (v1.0.6):** `BAT_ADC_PIN = 2`; `analogReadMilliVolts(pin) * 2`; thresholds in mV rather than raw ADC counts.
- **Prevention:** Always disconnect USB before calibrating a battery ADC formula. The USB charging rail on GPIO2 contaminates readings and can make a wrong multiplier appear correct.
