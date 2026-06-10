# EnergyCalibrator — OTA Firmware Changelog

## v1.0.10 — 2026-06-10

**Files:** `EnergyCalibrator_v1.0.10_s3zero.bin`

- **Fix: OTA ps_malloc failed on S3-Zero** — the OTA upload handler was allocating a 512 KB
  PSRAM buffer (`ps_malloc`) to scan for the `ZaxOtaMeta` magic before flashing. On the
  S3-Zero (2 MB OPI PSRAM) this always fails because the ring buffers consume nearly all
  available PSRAM. Replaced with a streaming scan: a 31-byte overlap window between
  consecutive HTTP upload chunks; no large allocation needed. Works on both S3-Zero (2 MB)
  and LilyGO (8 MB). OTA from the web UI and `curl -F` now complete successfully on Unit A.
- **Fix: board model name in System Info** — System Info page always showed
  "LilyGO T7 S3 WROOM-1" regardless of board. Now guarded by `BOARD_S3ZERO`:
  S3-Zero reports "Waveshare ESP32-S3-Zero", LilyGO reports "LilyGO T7 S3 WROOM-1".
- **Build scripts** — second-reset step added to both `build_lilygo.sh` and `build_s3zero.sh`:
  after flashing, a second `esptool chip_id --after hard-reset` simulates a power cycle
  so the smoke test captures the real boot the device will produce in the field (previously
  esptool's post-flash state masked boot failures such as the QIO+OPI PSRAM boot loop).

---

## v1.0.9 — 2026-06-10

**Files:** `EnergyCalibrator_v1.0.9_lilygo.bin` · `EnergyCalibrator_v1.0.9_s3zero.bin`

- **Fix: hw_target always zero in OTA meta** — `ZaxOtaMeta.hw_target` was hardcoded to `0`
  in the global initialiser. Added `HW_TARGET_LILYGO=0` / `HW_TARGET_S3ZERO=1` macros to
  `Config.h` and a compile-time `#if defined(BOARD_S3ZERO)` guard. The OTA guard in the
  upload handler now correctly rejects a LilyGO binary on an S3-Zero and vice versa.
- **Flash guard system** — `~/flash_guard.py` + `~/boards.json` on the Pi:
  pre-flash check reads the target board's MAC and flash size via esptool, verifies against
  a catalog of known units (board type + firmware + expected flash MB), and aborts if
  anything mismatches. Both build scripts now run the guard before every flash, preventing
  wrong-board writes (root cause: Unit D was bricked by an S3-Zero binary in a previous
  session). `REGISTER=Unit_X` env var triggers first-time enrollment.
- **Multi-device lockout** — build scripts abort with a clear error when more than one
  `/dev/ttyACM*` is present and no explicit port is given.
- **Post-smoke catalog update** — `flash_guard.py update` records the new version in
  `boards.json` after every successful smoke test.

---

## v1.0.8 — 2026-06-08

**Files:** `EnergyCalibrator_v1.0.8_s3zero.bin`

- **Fix: sec timestamp gap bug** — `latestSec.ts` was assigned from `time(nullptr)` at parse time (T-channel, ch==2). When `server.handleClient()` blocked the loop() for ~1 s handling an HTTP poll (directory service every 30 s), two consecutive R/S/T frame sets arrived in the UART buffer. Both were processed in the same wall-clock second, both got the same `ts`, and `INSERT OR REPLACE` in the collector kept only one → one second appeared as a gap every ~30 s (observed: 3.5% drop rate in `cal_sec`). Fix: `frameSetTs` counter, set on the R-channel (ch==0) frame — increments monotonically, resyncs to wall clock only if the gap exceeds 3 s. Two back-to-back buffered frame sets now get ts=N and ts=N+1 correctly. Both boards.

---

## build scripts — 2026-06-08

- `build_lilygo.sh`: now auto-saves OTA binary to `ota/EnergyCalibrator_vX.Y.Z_lilygo.bin` after every compile; was missing (binaries were manually copied before).
- `build_lilygo.sh`: added `--build-only` flag — compiles and saves OTA binary without flashing.
- `build_s3zero.sh`: `--build-only` flag added (compile + save OTA, skip flash + smoke).

---

## v1.0.7 — 2026-06-05

**Files:** `EnergyCalibrator_v1.0.7_lilygo.bin` · `EnergyCalibrator_v1.0.7_s3zero.bin`

- **BOOT button factory reset (GPIO0, both boards):**
  - Hold 5 s → LED rapid blink (100 ms on/off) — warning phase
  - Hold 3 s more (8 s total) → LED solid → NVS namespace cleared + WiFi credentials erased + reboot
  - Release before 8 s → abort, normal operation resumes
  - LilyGO: `ledLoop()` gated during warn; S3-Zero: `led_set()` gated, `led_force()` for direct writes
- **Configurable AP password** (`ap_pass`, NVS key, default `ZaxEnergy-123`):
  - WiFi Config section: "WiFi Password" + "AP Password" fields (blank = keep current, min 8 chars)
  - AP password resets to `ZaxEnergy-123` on factory reset
- **Reports list** sorted by modification time, newest first (was alphabetical).

---

## v1.0.6 — 2026-06-05

**Files:** `EnergyCalibrator_v1.0.6_lilygo.bin` · `EnergyCalibrator_v1.0.6_s3zero.bin`

- **Battery conventions — LilyGO only** (S3-Zero has no ADC; all ADC code skipped via `BAT_ADC_PIN=-1`):
  - BAT_ADC_PIN: 4 → 2 (GPIO2 is the actual battery ADC on T7-S3, R1=R2=100k divider)
  - `analogReadMilliVolts(2) × 2` replaces raw `analogRead` + stale constants
  - `bat_mv` added to `/api/sysinfo`
  - Power detection: `bat_mv > 4800` → USB Connected; below → Disconnected
  - Battery Low: `bat_mv < 3200` → MQTT `bat_low` + UI `⚠ Battery Low`
  - Critical Low: `bat_mv < 2850` → MQTT `bat_critical` + Error LED + UI `⚠ Critical Low Battery`
  - Recovery fires MQTT `bat_ok`
  - `/api/sysinfo` adds `bat_mv`, `bat_low`, `bat_critical`; Power row shows Connected/Disconnected
- **LilyGO LED redesign** — 3-state (IDLE/DATA/FAULT) → 4-state diagnostic patterns (same as ZaxMonitor v1.1.8):
  - **OK**: single 100 ms flash, 3 s period
  - **MQTT down**: double 100 ms flash, 3 s period
  - **No data** (no box OR no SDM): double 500 ms flash, 2 s period
  - **Error**: 1 s ON / 500 ms OFF, continuous — fires on no WiFi, 2+ bad conditions, or `bat_critical`
  - `ledIdle()` retained for S3-Zero NeoPixel; LilyGO LED fully driven by `ledLoop()`
- S3-Zero: no functional change.

---

## v1.0.5 — 2026-06-03

**Files:** `EnergyCalibrator_v1.0.5_lilygo.bin` · `EnergyCalibrator_v1.0.5_s3zero.bin`

- **LilyGO LED fix** — plain GPIO + priority blink state machine (same as ZaxMonitor v1.1.7).
  GPIO17 on T7-S3 is a single-color LED, not NeoPixel; replaced `Adafruit_NeoPixel` with
  `digitalWrite` under `#if defined(BOARD_LILYGO_T7S3)`. S3-Zero NeoPixel path unchanged.

---

## v1.0.4 — 2026-06-03

- Add Waveshare ESP32-S3-Zero board support (build_s3zero.sh):
  LED_PIN 17→21 (onboard NeoPixel), PWR_ADC/BAT_ADC disabled (-1)
  Pins unchanged: BOX_GPIO=5, SDM RX=15, TX=16 (unified pinout)
  Partition: min_spiffs (1.9MB OTA slots, 128KB LittleFS), 4MB QIO
- Boards: BOARD_LILYGO_T7S3 (default) / BOARD_S3ZERO (build flag)

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
