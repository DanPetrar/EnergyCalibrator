# EnergyCalibrator — Project Specification

**Version:** 1.1 (2026-05-31)  
**Firmware:** v1.0.1  
**Status:** Operational — real-load calibration session active

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
| `Config.h` | Version, NVS key, buffer sizes, fault thresholds |
| `EnergyCalibrator.ino` | Main loop, SDM630 poll, box serial parser, MQTT publish |
| `WebUI.h` | Embedded web server, all HTTP routes, OTA handler |
| `EnergyLog.h` | LittleFS energy record storage |
| `ErrorLog.h` | Error/warning log with NVS persistence |
| `FaultMonitor.h` | Per-phase fault detection and MQTT alerting |
| `RingBuf.h` | PSRAM ring buffer (SecRecord, MinRecord) |
| `Snapshot.h` | Power-loss snapshot save/restore |

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

```bash
# Build
SKIP_SMOKE=1 bash arduino/build_lilygo.sh /dev/ttyACM<N>

# Push OTA — MUST use -F (multipart), NOT --data-binary
curl -X POST http://192.168.110.104/api/ota \
  -F "firmware=@ota/EnergyCalibrator_vX.Y.Z_lilygo.bin;type=application/octet-stream"
```

Unit D reboots on success (~30s). Verify with `curl http://192.168.110.104/api/sysinfo`.

---

## 4. Collector (Pi-side)

**File:** `collector/cal_collector.py`  
**Service:** `collector/cal_collector.service` (systemd, auto-starts on boot)  
**DB:** `collector/cal_data.db` (SQLite)  
**Broker:** 192.168.110.225:1883  
**Subscribes:** `+/sec`, `+/min` (filters `cal_` prefix in handler)

### DB Schema

**`cal_sec`** — 1 Hz box readings  
`ts, unit, R_v/a/w/pf/hz, S_v/a/w/pf/hz, T_v/a/w/pf/hz`

**`cal_min`** — paired box + meter + deviations  
`ts, unit, mtr_v/a/w/pf/hz/dkwh, R/S/T_v/a/w/pf/hz, R/S/T_dkwh, R/S/T_dev_v/a/w/pf/dkwh_abs/pct`

Note: `ts` = `latestMeter.ts` (SDM read completion). `R/S/T_dkwh` = box firmware energy accumulator delta. `mtr_dkwh` = SDM kWh register delta.

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
**Requires:** `pip3 install reportlab` (installed on Pi)  
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

1. **Day Overview** — KPI boxes (SDM + CT-R/S/T with sensor model) and CT ranking table
2. **Hourly Summary** — one row per hour: SDM avg W, SDM kWh, CT-R/S/T energy deviation (colour coded green/yellow/red), peak W, coverage %
3. **All-day Measurement Statistics** — avg V/A/W/PF with deviation % per CT vs SDM (from 1 Hz cal_sec samples), std W
4. **Deviation by Load Band** — 5 bands (0–200, 200–500, 500–1000, 1000–1500, >1500 W); per-minute energy counter deviation `(X_dkwh − mtr_dkwh) / mtr_dkwh` averaged per band
5. **Sensor footnote** — CT-R: TDK 30A · CT-S: TDK 80A · CT-T: YHDC 120A

Deviation thresholds: green < 3%, yellow 3–6%, red > 6%.

---

## 7. Daily Report Delivery

**Script:** `report/daily_report.sh`  
**Cron:** `5 0 * * *` — generates report for previous calendar day automatically  
**Output:** `/home/pi/EnergyCalibrator/reports/report_YYYYMMDD_HHMMSS.pdf`  
**Log:** `reports/cron.log`

### Report web server

**File:** `reports/serve.py`  
**Service:** `reports/cal_reports.service` (systemd, port 8080, auto-starts)  
**URL:** http://192.168.110.225:8080/  

Lists all available PDFs with download links. New reports appear automatically.

```bash
# Install service (already done)
sudo cp reports/cal_reports.service /etc/systemd/system/
sudo systemctl enable cal_reports
sudo systemctl start cal_reports
```

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

- **dkwh quantization:** box firmware reports dkwh at ~1 Wh/min resolution per channel. Causes noise in short windows (≤30 min); averages out over full-day totals.
- **MQTT sec drop rate:** ~3.5% of per-second packets lost in transit. Irrelevant for energy (uses cal_min dkwh), but reduces sec coverage in cal_sec to ~96.5%.
- **Instantaneous mismatch:** SDM `ts` in cal_min is ~226ms after box minute. Rapid load changes between the two reads will show a gap in the instantaneous comparison — not a measurement error.
- **bat_pct reads -1:** Unit D powered by box USB — no battery. Normal.
