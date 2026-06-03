# EnergyCalibrator — Project Specification

**Version:** 1.2 (2026-06-01)  
**Firmware:** v1.0.3  
**Status:** Operational — real-load calibration session active

> Firmware energy accounting has been audited end-to-end; see
> [`energy-audit.md`](energy-audit.md). All findings are closed (F1 fixed in
> v1.0.3, rest documented as benign/inherent).

---

## 1. Purpose

Parallel CT calibration tool. Three CT sensors from a measurement box (R/S/T channels) are all clamped on the same single-phase wire simultaneously. An Eastron SDM630-M revenue-grade smart meter on the same wire acts as the reference. The system measures each CT's deviation from the reference over time (V, A, W, PF, kWh) to characterise accuracy.

**CT sensors under test:**

| Channel | Sensor | Rated current |
|---------|--------|---------------|
| R | TDK | 30 A |
| S | TDK | 80 A |
| T | YHDC | 120 A |

---

## 2. Hardware

### 2.1 Calibration Unit — Unit D

| Item | Value |
|------|-------|
| Board | LilyGO T7 S3 WROOM-1 N16R8 (16MB flash, 8MB OPI PSRAM) |
| MAC | 80:B5:4E:F0:7F:8C |
| MQTT prefix | `cal_F07F8C` |
| IP | 192.168.110.104 (static DHCP) |
| Power | Measurement box USB (no Pi connection) |
| Update method | OTA only |

**Pin assignments:**

| GPIO | Function |
|------|----------|
| 5 | Box serial RX — UART1, 115200 8N1 |
| 15 | SDM630 RS485 RX — UART2 (Module TX → GPIO15) |
| 16 | SDM630 RS485 TX — UART2 (Module RX → GPIO16) |
| 17 | NeoPixel status LED |
| 2 | PWR ADC |
| 4 | BAT ADC |

> **Shared pinout standard (2026-06-03):** box RX = **GPIO5** and RS485 (SDM630,
> auto-direction) RX/TX = **GPIO15/16** are the common comms pins across **both
> board targets** (LilyGO T7 S3 and ESP32-S3-Zero) and **both projects**
> (EnergyCalibrator + ZaxEnergySurvey). The S3-Zero exposes 5 on its header and
> 15/16 on board pads. EnergyCalibrator firmware is T7-only today, but the pinout
> is Zero-ready should a Zero build be added.

### 2.2 SDM630-M Smart Meter

| Item | Value |
|------|-------|
| Model | Eastron SDM630-M |
| Interface | RS485 Modbus RTU, auto-direction module (TX+RX only, no DE/RE) |
| Address | 1 |
| Baud | 9600 8N1 |
| Wiring | Module A(+)/B(-) → SDM630 RS485 A/B terminals |

**Modbus registers read:**

| Read | Start | Count | Fields |
|------|-------|-------|--------|
| 1 | 0x0000 | 32 | V1, A1, W1, PF1 |
| 2 | 0x0046 | 4 | Hz, kWh_import |

100ms inter-read delay required between Read 1 and Read 2.

**SDM630 poll timing:** triggered immediately after box minute frame is received. Total duration ~226ms (Read1 ~98ms + delay 100ms + Read2 ~28ms). The `cal_min.ts` field stores `latestMeter.ts` — the SDM read completion time, not the box minute timestamp. The ~226ms offset is consistent every minute so energy counter deltas are not affected.

### 2.3 Measurement Box

Sends per-second and per-minute serial frames on GPIO5 (115200 8N1) for channels R/S/T.

---

## 3. Firmware Architecture

### 3.1 Files

| File | Role |
|------|------|
| `Config.h` | Version, NVS key, buffer sizes, fault thresholds, record structs |
| `EnergyCalibrator.ino` | Main loop, SDM630 poll, box serial parser, MQTT publish, task watchdog |
| `WebUI.h` | Embedded web server, all HTTP routes, OTA handler |

**Shared modules** live in the **ZaxCommon** Arduino library
(`~/Arduino/libraries/ZaxCommon`, github.com/DanPetrar/ZaxCommon), included via
`<…>` and shared with ZaxMonitor so a fix is made once:

| Module | Role |
|--------|------|
| `EnergyLog.h` | LittleFS energy record storage |
| `ErrorLog.h` | Error/warning log with NVS persistence |
| `FaultMonitor.h` | Per-phase fault detection and MQTT alerting |
| `RingBuf.h` | PSRAM ring buffer (SecRecord, MinRecord) |
| `Snapshot.h` | Power-loss snapshot save/restore |

These require the sketch's `Config.h` (macro-guarded, **not** `#pragma once`) to
define `SecRecord`/`MinRecord`/`DATA_VERSION`/`ZaxConfig` first.

### 3.1a Firmware revisions

| Version | Change |
|---------|--------|
| v1.0.1 | SDM630 inter-read 100 ms delay; `/api/sdm` endpoint |
| v1.0.2 | Task watchdog (esp_task_wdt, 60 s, panic-reset) on the loop task; fed per-chunk during OTA upload |
| v1.0.3 | Fix F1 energy-audit finding — per-minute box delta + baseline advance only on a successful SDM-paired publish (a skipped minute folds into the next row symmetrically with the meter) |

### 3.2 Data Flow

```
Box serial (UART1, GPIO5)
  └─ per-second frames (R/S/T) ──→ SecRecord ──→ MQTT cal_F07F8C/sec (binary, 76 bytes)
  └─ per-minute frames (ch==2)  ──→ trigger SDM630 poll (~226ms)
                                       │
                          UART2 (GPIO15/16, Modbus RTU, 9600 baud)
                                       │
                                 SDM630 Read1 + 100ms + Read2
                                       │
                                  MinRecord ──→ MQTT cal_F07F8C/min (JSON)
                                              ──→ LittleFS energy log
```

### 3.3 MQTT Topics

| Topic | Type | Content |
|-------|------|---------|
| `cal_F07F8C/sec` | Binary (76 bytes) | Per-second SecRecord: ts, V/A/W/Hz/VAr/PF × 3 channels |
| `cal_F07F8C/min` | JSON | Paired record: box_sec, box_min, meter, dev (abs+% per CT) |
| `cal_F07F8C/faults` | JSON | Active fault state per phase |

### 3.4 HTTP API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/data` | GET | Live readings: sec, min, meter, deviation table |
| `/api/config` | GET/POST | Device configuration (MQTT, SDM address, thresholds) |
| `/api/sdm` | GET | On-demand SDM630 poll — returns V/A/W/PF/Hz/kWh (~10s at 9600 baud) |
| `/api/sysinfo` | GET | Firmware version, uptime, MQTT state, battery |
| `/api/netinfo` | GET | WiFi SSID, IP, RSSI, NTP state |
| `/api/history` | GET | Ring-buffer history (type=sec\|min, n=N) |
| `/api/errors` | GET/DELETE | Error log |
| `/api/ota_meta` | GET | OTA compatibility metadata |
| `/api/ota` | POST | OTA firmware upload (multipart/form-data) |
| `/api/export` | GET | CSV export of stored data |

### 3.5 OTA Procedure

Requires the **ZaxCommon** library in `~/Arduino/libraries/` (the build scripts
fail early with install instructions if it is missing).

```bash
# Build (compiles + flashes over USB + smoke test)
bash arduino/build_lilygo.sh /dev/ttyACM<N>

# Push OTA — MUST use -F (multipart), NOT --data-binary
curl -X POST http://192.168.110.104/api/ota \
  -F "firmware=@ota/EnergyCalibrator_vX.Y.Z_lilygo.bin;type=application/octet-stream"
```

Unit D reboots on success (~30s). Verify with `curl http://192.168.110.104/api/sysinfo`.

---

## 4. Collector

> **Deployment (since 2026-06-02):** the backend (collector + reports + crons +
> the InfluxDB/Grafana feed) was migrated off the Raspberry Pi onto the **I3
> Workstation** (`192.168.110.11`, user `dan-linux`). It runs there from
> `/workspace/projects/EnergyCalibrator`, against DB
> `/workspace/cal-data/cal_data.db`, using the venv
> `/workspace/projects/EnergyCalibrator/.venv/bin/python3`. Unit D publishes to
> the Workstation broker (`mqtt_host=192.168.110.11`). The Pi bench services are
> disabled and the old Pi DB is archived. Firmware build/flash still happens on
> the Pi (USB). See the `DanPetrar/Workstation` repo for the migration runbook.

**File:** `collector/cal_collector.py`  
**Service:** systemd `cal_collector.service` on the Workstation (auto-starts on boot)  
**DB:** `/workspace/cal-data/cal_data.db` (SQLite, `journal_mode=WAL`, `synchronous=NORMAL`; `sec` commits batched ~1 Hz, `min` committed immediately). Path set via `CAL_DB` env.  
**Broker:** `127.0.0.1:1883` (the Workstation's local Mosquitto; set via `CAL_MQTT_HOST`)  
**Subscribes:** `+/sec`, `+/min` (filters `cal_` prefix in handler)

**Retention:** `collector/prune.py` rolls `cal_sec` rows older than 10 days into
`cal_sec_hourly` (avg/min/max W/V/A/PF per CT per hour) then deletes the raw
rows; `cal_min` is kept indefinitely. Cron: `--apply` Mon–Sat 00:30,
`--apply --vacuum` Sun 00:30.

### DB Schema

**`cal_sec`** — 1 Hz box readings  
`ts, unit, R_v/a/w/pf/hz, S_v/a/w/pf/hz, T_v/a/w/pf/hz`

**`cal_min`** — paired box + meter + deviations  
`ts, unit, mtr_v/a/w/pf/hz/dkwh, R/S/T_v/a/w/pf/hz, R/S/T_dkwh, R/S/T_dev_v/a/w/pf/dkwh_abs/pct`

**`cal_sec_hourly`** — hourly rollup of aged-out `cal_sec` rows (see Retention)  
`hour_ts, unit, n, {R,S,T}_{w,v,a,pf}_{avg,min,max}`

Note: `ts` = `latestMeter.ts` (SDM read completion). `R/S/T_dkwh` = box firmware energy accumulator delta. `mtr_dkwh` = SDM kWh register delta. On an SDM poll failure the box minute folds into the next paired row (v1.0.3, finding F1).

### Start collector as service

```bash
sudo cp collector/cal_collector.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable cal_collector
sudo systemctl start cal_collector
```

---

## 5. Live Monitor

**File:** `collector/monitor.py`  
**Usage:** `python3 collector/monitor.py`

Prints a 30-min snapshot to the terminal:
- Instantaneous V/A/W/PF for box R/S/T and SDM630 (from latest sec + closest min row)
- SDM internal consistency check: V×A×PF vs reported W
- Energy deviation for last 30 min and cumulative from START_TS (2026-05-31 15:30)
- sec row coverage

**Energy method:** `sum(X_dkwh)` from cal_min for both box and SDM — energy counter deltas, never W-snapshot averaging.

---

## 6. Report Generator

**File:** `report/generate_report.py`  
**Requires:** `reportlab` — installed in the Workstation venv (`/workspace/projects/EnergyCalibrator/.venv`)  
**Usage:**
```bash
python3 report/generate_report.py --date YYYY-MM-DD --unit cal_F07F8C
python3 report/generate_report.py --all --unit cal_F07F8C
```

### Energy methodology

All deviations use **energy counter deltas** exclusively:
- Box CT energy: `sum(R/S/T_dkwh)` from `cal_min` (firmware accumulator, unaffected by MQTT drops)
- SDM630 energy: `sum(mtr_dkwh)` from `cal_min` (SDM kWh register delta)

W-snapshot averaging (`sum(W_sec)/3_600_000`) is NOT used for energy — it under-counts by ~3.5% due to MQTT packet drops in `cal_sec`.

### Report layout

Energy-counter (dkWh) views only — the snapshot-based comparison sections were
removed (2026-06-01) because they mix CT per-second and SDM per-minute snapshots
and can produce misleading percentages:

1. **Day Overview** — KPI boxes (SDM + CT-R/S/T with sensor model) and CT ranking table (ranked by absolute energy deviation; assessment Best/Good/Needs-calibration)
2. **Hourly Summary** — one row per hour: SDM avg W, SDM kWh, CT-R/S/T energy deviation (colour coded), peak W, coverage %. *(CT dev here is per-hour `dkwh`, not a snapshot comparison — retained.)*
3. **Sensor footnote** — CT-R: TDK 30A · CT-S: TDK 80A · CT-T: YHDC 120A

**Removed:** ~~All-day Measurement Statistics~~ and ~~Deviation by Load Band~~
(snapshot-based; can be reintroduced with a corrected methodology later).

Deviation thresholds: green < 3%, yellow 3–6%, red > 6%. Ranking/assessment use
an explicit None-check (`abs(d) if d is not None else 999`) — **never** the
falsy `d or 999`, which would mis-flag an exact 0.00% deviation. Data-layer
regression tests: `report/tests/test_report_data.py`.

---

## 7. Daily Report Delivery

**Script:** `report/daily_report.sh`  
**Cron:** `5 0 * * *` (in the Workstation `dan-linux` crontab) — generates report for previous calendar day automatically. On the Workstation the cron runs `/workspace/cal-data/ws_daily_report.sh`, a thin wrapper that calls `generate_report.py --db /workspace/cal-data/cal_data.db` with the venv python.  
**Output:** `/workspace/projects/EnergyCalibrator/reports/report_YYYYMMDD_HHMMSS.pdf`  
**Log:** `reports/cron.log`

### Report web server

**File:** `reports/serve.py`  
**Service:** systemd `cal_reports.service` on the Workstation (port 8080, auto-starts)  
**URL:** http://192.168.110.11:8080/  

Lists all available PDFs with download links, plus the **bench session UI** (§7c).

---

## 7c. Bench session UI (Phase E)

`reports/serve.py` (same `cal_reports.service`, port 8080) also runs a session
workflow: **enter a DUT box serial → Start → Stop → generate/download the session
report**. Spec: `Doc/PhaseE-session-ui.md`.

A *session* is a labeled time window over the single bench (Unit D, `cal_F07F8C`);
the serial is a label only — the session report filters by **time window** (it calls
`generate_report.py --from start --to stop`, passing `--db` for the live WS DB).

**New table** in `cal_data.db`:

```sql
sessions(id, serial, start_ts, stop_ts, status, notes, report)
-- partial unique index sessions_one_running ON (status) WHERE status='running'
```

The partial index DB-enforces **one running session at a time**. The server creates
the table/index on startup.

**Endpoints** (stdlib `http.server`, `ThreadingHTTPServer`, no new deps):

| Method | Path | Action |
|--------|------|--------|
| GET  | `/` | Index: running banner + Stop, Start form, sessions table, PDF list |
| POST | `/sessions/start` | `serial` (+`notes`); creates running session (`start_ts=now`); 409-equivalent redirect if one already running |
| POST | `/sessions/stop` | Closes the running session (`stop_ts=now`, `status='stopped'`) |
| POST | `/sessions/<id>/report` | Runs `generate_report.py` for the session window → `reports/session_<serial>_<id>.pdf`; regeneratable |
| GET  | `/<file>.pdf` | Download (unchanged) |

Each session row links to the Grafana dashboard scoped to its window
(`?from=<start_ms>&to=<stop_ms|now>`). No auth (LAN-local).

---

## 7b. Live feed — InfluxDB + Grafana (Workstation)

A separate parser streams the bench MQTT into InfluxDB so Grafana shows live
bench health alongside the daily PDFs.

**Parser:** `infrastructure/cal_parser.py` (in the `DanPetrar/Workstation` repo),
deployed on the Workstation as systemd `cal-parser.service` →
`/opt/cal-parser/cal_parser.py` (venv python). Subscribes the local broker
`cal_F07F8C/#`; writes InfluxDB bucket `zaxenergy` (org `zax`):

| Measurement | Source | Tags | Fields |
|-------------|--------|------|--------|
| `power`     | `sec` (76-byte) | unit, phase | v, a, w, hz, pf |
| `cal_meter` | `min`.meter (SDM630) | unit | v, a, w, pf, hz, dkwh |
| `cal_box`   | `min`.box | unit, phase | w, dkwh |
| `cal_dev`   | `min`.dev | unit, phase | w_pct, dkwh_pct |

`unit` tag is the stable MQTT id `cal_F07F8C` (the device-under-test name is
tracked separately, not in the tag).

**Grafana dashboard:** "Bench - calibration" (uid `bench-calib`) →
http://192.168.110.11:3000/d/bench-calib/ — box CT W vs SDM, power deviation %,
energy deviation %, live SDM stats.

---

## 8. Commissioning Record (2026-05-31)

| Step | Result |
|------|--------|
| Unit D initial USB flash | PASS — v1.0.1, smoke test OK |
| WiFi join (ZAXSense) | PASS — IP 192.168.110.104, RSSI -51 dBm |
| MQTT connectivity | PASS — cal_F07F8C/faults publishing |
| Box serial connection | PASS — has_sec=true, has_min=true |
| SDM630 RS485 | PASS — V=246–247V, Hz=50.01, 9600 baud |
| OTA firmware update | PASS — multipart/form-data required |
| Collector DB writes | PASS — cal_sec and cal_min rows confirmed |
| NTP sync | PASS — pool.ntp.org, tz+3 |

---

## 9. Soak Test Results (2026-05-31, 10:49–13:31)

2h45m unattended run, 12 checks at 15-min intervals. No load on circuit (idle validation only).

| Metric | Value |
|--------|-------|
| Duration | 2h 42m |
| Checks passed | 12 / 12 |
| New errors | 0 |
| sec rows collected | 9908 |
| min rows collected | 169 |
| Voltage range (box) | 239.6–249.9V |
| Voltage range (SDM630) | 239.4–249.2V |
| Frequency range | 49.92–50.03Hz |

**Conclusion:** All communication paths stable. Ready for real load test.

---

## 10. First Real-Load Calibration Session (2026-05-31, 15:30–23:00)

Clean data start: 15:30 (pre-15:30 rows deleted — hardware unstable before that point).

**Results after ~7.5 hours / ~450 minute samples:**

| CT | Sensor | Cumulative deviation vs SDM630 |
|----|--------|-------------------------------|
| R | TDK 30A | ≈ −0.1% |
| S | TDK 80A | ≈ +0.3% |
| T | YHDC 120A | ≈ −0.7% |

All three CTs within ±1% — highly accurate at real-load conditions.

**Key finding:** apparent −4% to −7% deviation seen earlier was an artefact of W-snapshot integration (missing MQTT packets). Correct energy counter method gives ±1%. See errors-history.md.

---

## 11. Known Issues / Observations

- **dkwh quantization:** the box energy counter has **0.01 kWh (10 Wh) resolution — 2 decimals** per channel (verified 2026-06-03: all box `dkwh` values land exactly on the 0.01 grid). At typical bench loads (~450 W ≈ 7 Wh/min) a channel earns *less than one count per minute*, so most minutes read 0 and the counter releases an accumulated 0.02–0.03 kWh when it ticks (you never see 0.01). The **SDM630 reference is 10× finer — 0.001 kWh (1 Wh), 3 decimals** — so it has a value every minute. Consequence: **per-minute box energy is not meaningful**; compare `Σ dkwh` over ≥15-min windows (full-day for totals). Use the cumulative / 15-min Grafana views, not per-minute.
- **MQTT sec drop rate:** ~3.5% of per-second packets lost in transit. Irrelevant for energy (uses cal_min dkwh), but reduces sec coverage in cal_sec to ~96.5%.
- **Instantaneous mismatch:** SDM `ts` in cal_min is ~226ms after box minute. Rapid load changes between the two reads will show a gap in the instantaneous comparison — not a measurement error.
- **bat_pct reads -1:** Unit D powered by box USB — no battery. Normal.
