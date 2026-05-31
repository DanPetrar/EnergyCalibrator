# EnergyCalibrator — OTA Firmware Changelog

## v1.0.1 — 2026-05-31

- Fix SDM630 consecutive read timeout: add 100ms delay between read1 and read2
- Add `/api/sdm` GET endpoint for on-demand SDM630 poll (always available, returns V/A/W/PF/Hz/kWh)
- Unit D (MAC F0:7F:8C) commissioned: box serial + SDM630 RS485, powered by box

## v1.0.0 — 2026-05-30

Initial release.

- Dual-source parallel recording: measurement box (3 CT channels R/S/T) + SDM630 smart meter
- Box serial on UART1 GPIO5 (115200 8N1); SDM630 Modbus RTU on UART2 GPIO15/16 (9600 8N1)
- SDM630 polled once per minute immediately after box minute frame (ch==2 / W line)
- SDM630 reads: V1, A1, W1, PF1, Hz (0x0000–0x001F), kWh import (0x0048) — two Modbus reads
- Paired JSON published to MQTT `<topic>/min` with box_sec, box_min, meter, dev sections
- Deviation computed per CT (R/S/T) vs SDM630: V/A/W/PF absolute and percentage; dkWh abs/pct
- Box per-second data published to `<topic>/sec` (binary SecRecord, 76 bytes)
- MQTT topic auto-generated from MAC: `cal` → `cal_<MAC3>` on first boot
- Web UI: Live tab shows box readings + SDM630 row + deviation table (last min poll)
- Web UI: Config tab → SDM630 Modbus address field
- PSRAM ring buffers, LittleFS snapshots, NTP, FaultMonitor, ErrorLog — all inherited from ZaxEnergySurvey v1.1.5
- Board: LilyGO T7 S3 WROOM-1 N16R8 only (BOX_GPIO=5, LED=17, SDM RX=15, TX=16)
