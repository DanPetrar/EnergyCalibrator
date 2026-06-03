#!/bin/bash
# build_s3zero.sh — compile + flash EnergyCalibrator for Waveshare ESP32-S3-Zero (4MB QIO, 2MB PSRAM)
# Partition: min_spiffs — 1.9MB APP x2 (OTA) + 128KB SPIFFS (LittleFS)
# Usage: ./build_s3zero.sh [port]   (port defaults to /dev/ttyACM0)

PORT="${1:-/dev/ttyACM0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKETCH="${SCRIPT_DIR}/EnergyCalibrator/EnergyCalibrator.ino"
BUILD_DIR="/tmp/arduino_build_EnergyCalibrator_s3zero"

if [ ! -d "${HOME}/Arduino/libraries/ZaxCommon/src" ]; then
  echo "ERROR: ZaxCommon library not found in ~/Arduino/libraries/."
  echo "Install: git clone git@github.com:DanPetrar/ZaxCommon.git ~/Arduino/libraries/ZaxCommon"
  exit 1
fi

FQBN="esp32:esp32:waveshare_esp32_s3_zero:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,FlashMode=qio,PartitionScheme=min_spiffs,PSRAM=enabled"

echo "[1/3] Compiling for Waveshare S3-Zero (4MB QIO, min_spiffs, OTA) ..."
mkdir -p "$BUILD_DIR"
arduino-cli compile \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=-DBOARD_S3ZERO" \
  --build-property "compiler.c.extra_flags=-DBOARD_S3ZERO" \
  --output-dir "$BUILD_DIR" \
  "$SKETCH" 2>&1 | tee /tmp/arduino_cal_s3zero_compile.log | grep -E "Sketch uses|error:|Error|warning:"

COMPILE_STATUS=${PIPESTATUS[0]}
if [ $COMPILE_STATUS -ne 0 ]; then
  echo "Compile failed (see /tmp/arduino_cal_s3zero_compile.log)."
  exit 1
fi

BIN=$(ls  "$BUILD_DIR"/*.ino.bin 2>/dev/null | head -1)
BOOT=$(ls "$BUILD_DIR"/*.bootloader.bin 2>/dev/null | head -1)
PART=$(ls "$BUILD_DIR"/*.partitions.bin 2>/dev/null | head -1)
BOOT_APP0="${HOME}/.arduino15/packages/esp32/hardware/esp32/3.3.7/tools/partitions/boot_app0.bin"

if [ -z "$BIN" ]; then echo "Binary not found in $BUILD_DIR"; exit 1; fi

# Copy binary to ota/
VER=$(grep 'FW_VERSION' "${SCRIPT_DIR}/EnergyCalibrator/Config.h" | grep -oP '"[^"]+"' | tr -d '"')
OTA_BIN="${SCRIPT_DIR}/../ota/EnergyCalibrator_v${VER}_s3zero.bin"
cp "$BIN" "$OTA_BIN"
echo "[prep] Saved: $OTA_BIN ($(ls -lh "$OTA_BIN" | awk '{print $5}'))"

echo "[2/3] Flashing to $PORT ..."
source /home/pi/esp/esp-idf/export.sh > /dev/null 2>&1
python -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode qio --flash-size 4MB --flash-freq 80m \
  0x0000  "$BOOT" \
  0x8000  "$PART" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$BIN" 2>&1 | grep -E "Wrote|Hash|Connected|Hard|error"

if [ ${PIPESTATUS[0]} -ne 0 ]; then echo "Flash failed."; exit 1; fi

# ── Smoke test ────────────────────────────────────────────────────────────────
if [ "${SKIP_SMOKE:-0}" = "1" ]; then echo "[3/3] Smoke test skipped."; exit 0; fi

LOG="/tmp/arduino_smoke_cal_s3zero.log"
DURATION="${SMOKE_DURATION:-12}"
echo "[3/3] Smoke test — capturing ${DURATION}s of boot serial on $PORT ..."

stty -F "$PORT" 115200 cs8 -cstopb -parenb -icanon -echo -echoe -echok \
            -icrnl -inlcr -ixon -ixoff -opost min 0 time 5 2>/dev/null
timeout 1 cat "$PORT" > /dev/null 2>&1
timeout "$DURATION" cat "$PORT" > "$LOG" 2>/dev/null

FATAL_PATTERNS='\[BOOT\] FATAL|Guru Meditation|abort\(\)|assert failed|panic|Brownout detector|rst:0x10 \(RTCWDT'
RESETS=$(grep -c "^ets " "$LOG" 2>/dev/null); RESETS=${RESETS:-0}

if grep -E -q "$FATAL_PATTERNS" "$LOG" 2>/dev/null; then
  echo "----- SMOKE TEST: FAIL -----"
  grep -E "$FATAL_PATTERNS" "$LOG" | sed 's/^/  /'
  exit 2
fi
if [ "$RESETS" -gt 3 ]; then
  echo "----- SMOKE TEST: FAIL (boot loop — $RESETS resets in ${DURATION}s) -----"
  exit 2
fi

echo "----- SMOKE TEST: PASS -----"
grep -E "\[CFG\]|\[BUF\]|\[WIFI\]|\[EnergyCalibrator\]|\[SDM\]|\[SNAP\]|\[ERRLOG\]" "$LOG" | sed 's/^/  /'
exit 0
