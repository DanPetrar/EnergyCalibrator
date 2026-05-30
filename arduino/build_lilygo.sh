#!/bin/bash
# build_lilygo.sh — compile + flash EnergyCalibrator for LilyGO T7 S3 WROOM-1 N16R8
# Board: 16MB DIO flash, 8MB OPI PSRAM, native USB CDC
# Usage: ./build_lilygo.sh [port]   (port defaults to /dev/ttyACM0)

PORT="${1:-/dev/ttyACM0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SKETCH="${SCRIPT_DIR}/EnergyCalibrator/EnergyCalibrator.ino"
BUILD_DIR="/tmp/arduino_build_EnergyCalibrator_lilygo"
CORE_PARTS_DIR="${HOME}/.arduino15/packages/esp32/hardware/esp32/3.3.7/tools/partitions"
CSV_SRC="${SCRIPT_DIR}/partitions_16MB.csv"
CSV_DST="${CORE_PARTS_DIR}/cal_16MB.csv"

if ! cmp -s "$CSV_SRC" "$CSV_DST" 2>/dev/null; then
  echo "[prep] Installing partitions_16MB.csv as cal_16MB.csv ..."
  cp "$CSV_SRC" "$CSV_DST"
fi

echo "[1/3] Compiling for LilyGO T7 S3 (16MB flash, OPI PSRAM, DIO mode) ..."
mkdir -p "$BUILD_DIR"
cp "$CSV_SRC" "${SCRIPT_DIR}/EnergyCalibrator/partitions.csv"
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:UploadSpeed=921600,FlashMode=dio,FlashSize=16M,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc" \
  --build-property "compiler.cpp.extra_flags=-DBOARD_LILYGO_T7S3" \
  --build-property "compiler.c.extra_flags=-DBOARD_LILYGO_T7S3" \
  --build-property "build.partitions=cal_16MB" \
  --build-property "upload.maximum_size=2097152" \
  --output-dir "$BUILD_DIR" \
  "$SKETCH" 2>&1 | tee /tmp/arduino_cal_compile.log | grep -E "Sketch uses|error:|Error|warning:"

COMPILE_STATUS=${PIPESTATUS[0]}
rm -f "${SCRIPT_DIR}/EnergyCalibrator/partitions.csv"
if [ $COMPILE_STATUS -ne 0 ]; then
  echo "Compile failed (see /tmp/arduino_cal_compile.log)."
  exit 1
fi

BIN=$(ls  "$BUILD_DIR"/*.ino.bin 2>/dev/null | head -1)
BOOT=$(ls "$BUILD_DIR"/*.bootloader.bin 2>/dev/null | head -1)
PART=$(ls "$BUILD_DIR"/*.partitions.bin 2>/dev/null | head -1)

if [ -z "$BIN" ]; then echo "Binary not found in $BUILD_DIR"; exit 1; fi

echo "[2/3] Flashing to $PORT ..."
source /home/pi/esp/esp-idf/export.sh > /dev/null 2>&1
python -m esptool --chip esp32s3 -p "$PORT" -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0000  "$BOOT" \
  0x8000  "$PART" \
  0x10000 "$BIN" 2>&1 | grep -E "Wrote|Hash|Connected|Hard|error"

if [ ${PIPESTATUS[0]} -ne 0 ]; then echo "Flash failed."; exit 1; fi

# ── Smoke test ────────────────────────────────────────────────────────────────
if [ "${SKIP_SMOKE:-0}" = "1" ]; then echo "[3/3] Smoke test skipped."; exit 0; fi

LOG="/tmp/arduino_smoke_cal.log"
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
grep -E "\[CFG\]|\[BUF\]|\[LFS\]|\[ERRLOG\]|\[SNAP\]|\[WIFI\]|\[SDM\]" "$LOG" | sed 's/^/  /'
exit 0
