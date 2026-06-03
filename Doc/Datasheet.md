# EnergyCalibrator — Datasheet

_Firmware v1.0.5 · Last updated: 2026-06-03_

---

## Purpose

Parallel CT accuracy evaluation. Three CT sensors are clamped on the **same single-phase wire** and compared against an Eastron SDM630-M as a reference meter. The firmware collects both box and SDM data, publishes them via MQTT, and stores them for report generation.

---

## Supported Hardware

| Board | Build flag | Flash | PSRAM | LED |
|-------|-----------|-------|-------|-----|
| Waveshare ESP32-S3-Zero | `-DBOARD_S3ZERO` | 4 MB | 2 MB OPI | NeoPixel GPIO21 |
| LilyGO T7 S3 WROOM-1 N16R8 | *(default)* | 16 MB | 8 MB OPI | Plain LED GPIO17 |

---

## Connections

| GPIO | Signal | Details |
|------|--------|---------|
| 5 | Box serial RX | UART1, 115200 8N1 |
| 15 | RS485 RX (SDM630) | UART2, auto-direction module |
| 16 | RS485 TX (SDM630) | UART2 |
| 21 *(S3-Zero)* / 17 *(LilyGO)* | Status LED | NeoPixel / plain GPIO |

**SDM630:** Modbus RTU, address 1, 9600 8N1.  
**Power:** 5 V from box USB-C connector, or USB-C cable.

---

## CT Sensors (current bench)

| Channel | Sensor | Nominal |
|---------|--------|---------|
| R | TDK | 30 A |
| S | TDK | 80 A |
| T | YHDC | 120 A |

---

## Data Flow

1. Box → GPIO5 → per-second R/S/T frames (V/A/W/VAr/PF/Hz)
2. Per-minute box frame → triggers SDM630 Modbus poll (~226 ms)
3. Paired box+SDM record published to MQTT + stored in SQLite on collector

---

## MQTT Topics

| Topic | Rate | Content |
|-------|------|---------|
| `cal_XXXXXX/sec` | 1 Hz | Binary SecRecord — R/S/T V/A/W/VAr/PF/Hz |
| `cal_XXXXXX/min` | 1/min | JSON — box+SDM paired: V/A/W/PF/Hz/dkWh + deviations |
| `cal_XXXXXX/faults` | on change | JSON fault state |

`XXXXXX` = last 3 bytes of device MAC (e.g. `cal_E47730`).

---

## REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/data` | GET | Live box R/S/T + SDM snapshot, energy totals |
| `/api/sdm` | GET | On-demand SDM630 poll (V/A/W/PF/Hz/kWh) |
| `/api/config` | GET/POST | NVS config (WiFi, MQTT, thresholds) |
| `/api/ota` | POST | OTA firmware upload (multipart, field `firmware`) |
| `/api/ota_meta` | GET | `fw_version`, `data_version`, record sizes |

---

## LED Status

**S3-Zero (NeoPixel):**

| Color | Pattern | Meaning |
|-------|---------|---------|
| Blue | Dim steady | Idle, no fault |
| Red | Dim steady | Fault active |
| Green | 80 ms flash | Per-second box frame |
| Amber | 80 ms flash | Paired SDM publish |
| Orange | 80 ms flash | Unknown frame |

**LilyGO T7-S3 (plain LED):**

| Pattern | Meaning |
|---------|---------|
| Single tap / 4 s | Idle, no fault |
| Double tap / 2 s | Data flowing |
| 500 ms ON / 200 ms OFF | Fault active |

---

## OTA

```bash
# S3-Zero
curl -F "firmware=@EnergyCalibrator_vX.Y.Z_s3zero.bin;type=application/octet-stream" \
  http://<IP>/api/ota

# LilyGO
curl -F "firmware=@EnergyCalibrator_vX.Y.Z_lilygo.bin;type=application/octet-stream" \
  http://<IP>/api/ota
```

---

## Build

```bash
cd ~/EnergyCalibrator
SKIP_FLASH=1 bash arduino/build_s3zero.sh   # → ota/EnergyCalibrator_vX.Y.Z_s3zero.bin
SKIP_FLASH=1 bash arduino/build_lilygo.sh   # → ota/EnergyCalibrator_vX.Y.Z_lilygo.bin
```
