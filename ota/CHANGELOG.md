# EnergyCalibrator — OTA Firmware Changelog

## v1.0.3 — 2026-06-01

- Fix F1 (energy audit): on an SDM630 poll failure the skipped minute's box
  energy was dropped while the meter telescoped it into the next row, biasing
  box-vs-SDM deviation slightly negative per failure. Now the per-minute box
  delta + baseline (`prevCumKwhAtMin`) only advance on a successful paired
  publish, so a skipped minute folds into the next row symmetrically for both
  box and meter. Normal (no-failure) path unchanged.

## v1.0.2 — 2026-06-01

- Add task watchdog (esp_task_wdt) on the loop task, 60 s timeout, panic-reset
  on hang — auto-recovers a wedged Modbus/WiFi/MQTT call (Unit D is OTA-only,
  so a silent hang previously needed a physical trip)
- Feed the watchdog per chunk during OTA upload (loop is stalled for the whole
  transfer) so flashing a large .bin can never trip the WDT mid-write

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
