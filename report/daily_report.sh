#!/bin/bash
# Daily report generator — runs at 00:05 via cron.
# Generates PDF for the previous calendar day and saves to reports/.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/../reports/cron.log"
YESTERDAY=$(date -d 'yesterday' +%Y-%m-%d)

echo "$(date '+%Y-%m-%d %H:%M:%S') generating report for $YESTERDAY" >> "$LOG"

# Resolve the DUT serial for yesterday: find the session that overlapped that day.
SERIAL=$(python3 - <<'PYEOF'
import sqlite3, datetime, sys
db = '/workspace/cal-data/cal_data.db'
yesterday = datetime.date.today() - datetime.timedelta(days=1)
ts_start = int(datetime.datetime(yesterday.year, yesterday.month, yesterday.day).timestamp())
ts_end   = ts_start + 86400
c = sqlite3.connect(db)
r = c.execute(
    'SELECT serial FROM sessions '
    'WHERE start_ts < ? AND (stop_ts IS NULL OR stop_ts > ?) '
    'ORDER BY id DESC LIMIT 1',
    (ts_end, ts_start)
).fetchone()
print(r[0] if r else 'unknown')
PYEOF
)

python3 "$SCRIPT_DIR/generate_report.py" \
    --date "$YESTERDAY" \
    --unit cal_E47730 \
    --serial "$SERIAL" \
    >> "$LOG" 2>&1

echo "$(date '+%Y-%m-%d %H:%M:%S') done" >> "$LOG"
