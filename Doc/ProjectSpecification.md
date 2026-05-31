# EnergyCalibrator — Project Specification

**Version:** 1.0 (2026-05-31)  
**Firmware:** v1.0.1  
**Status:** Operational — collecting calibration data

---

## 1. Purpose

Parallel CT calibration tool. Three CT sensors from a measurement box (R/S/T channels) are all clamped on the same single-phase wire simultaneously. An Eastron SDM630-M revenue-grade smart meter on the same wire acts as the reference. The system measures each CT's deviation from the reference over time (V, A, W, PF, kWh) to characterise accuracy.

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
  └─ per-minute frames (ch==2)  ──→ trigger SDM630 poll
                                       │
                          UART2 (GPIO15/16, Modbus RTU)
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
**Service:** `collector/cal_collector.service`  
**DB:** `collector/cal_data.db` (SQLite)  
**Broker:** 192.168.110.225:1883  
**Subscribes:** `+/sec`, `+/min` (filters `cal_` prefix in handler)

### DB Schema

**`cal_sec`** — 1 Hz box readings  
`ts, unit, R_v/a/w/pf/hz, S_v/a/w/pf/hz, T_v/a/w/pf/hz`

**`cal_min`** — paired box + meter + deviations  
`ts, unit, mtr_v/a/w/pf/hz/dkwh, R/S/T_v/a/w/pf/hz, R/S/T_dkwh, R/S/T_dev_v/a/w/pf/dkwh_abs/pct`

### Start collector as service

```bash
sudo cp collector/cal_collector.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable cal_collector
sudo systemctl start cal_collector
```

---

## 5. Report Generator (Workstation)

**File:** `report/generate_report.py`  
**Requires:** `pip3 install reportlab`  
**DB source:** `collector/cal_data.db` (copy to workstation or run on Pi)

```bash
python3 generate_report.py [--db PATH] [--unit cal_F07F8C] [--hours 24] [--out report.pdf]
```

Produces a PDF with:
- Per-CT hourly deviation stats (min/max/avg for V, A, W, PF, kWh)
- Energy summary: meter total vs each CT total, absolute and % deviation

---

## 6. Commissioning Record (2026-05-31)

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

## 7. Known Issues / Observations

- **No load on circuit at commissioning:** A/W/PF/kWh all zero during initial test. Meaningful calibration data requires a connected load.
- **bat_pct reads -1:** Unit D is powered by box USB — no battery. Normal for this board when no battery is connected.
- **Box comm losses during setup:** Multiple `Box comm lost` entries in error log are from the pre-wiring period (USB resets during flashing). Not a fault.
