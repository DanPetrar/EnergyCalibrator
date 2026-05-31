#!/bin/bash
# Daily report generator — runs at 00:05 via cron.
# Generates PDF for the previous calendar day and saves to reports/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/../reports/cron.log"
YESTERDAY=$(date -d 'yesterday' +%Y-%m-%d)

echo "$(date '+%Y-%m-%d %H:%M:%S') generating report for $YESTERDAY" >> "$LOG"

python3 "$SCRIPT_DIR/generate_report.py" \
    --date "$YESTERDAY" \
    --unit cal_F07F8C \
    >> "$LOG" 2>&1

echo "$(date '+%Y-%m-%d %H:%M:%S') done" >> "$LOG"
