#!/usr/bin/env python3
"""
prune.py — EnergyCalibrator DB retention + hourly rollup.

Keeps `cal_sec` (1 Hz, ~13 MB/day) bounded:
  - Rows older than RETENTION_DAYS are rolled into `cal_sec_hourly`
    (avg/min/max of W/V/A/PF per CT per hour, per unit), then the raw rows
    are deleted. The cutoff is hour-aligned, so no hour is ever split.
  - `cal_min` (the calibration record) is never touched.

Default is a DRY RUN — pass --apply to actually write/delete. The cron job
passes --apply; add --vacuum (weekly) to reclaim file space afterwards.

Usage:
  python3 prune.py                 # dry run, report only
  python3 prune.py --apply         # roll up + delete old cal_sec rows
  python3 prune.py --apply --vacuum
  CAL_DB=/path/to/cal.db RETENTION_DAYS=10 python3 prune.py --apply

Environment:
  CAL_DB           SQLite database path  (default: ./cal_data.db)
  RETENTION_DAYS   raw cal_sec retention (default: 10)
"""

import os, sys, time, sqlite3, argparse, logging

HERE      = os.path.dirname(os.path.abspath(__file__))
DB_PATH   = os.environ.get('CAL_DB', os.path.join(HERE, 'cal_data.db'))
RETENTION = int(os.environ.get('RETENTION_DAYS', '10'))

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s %(levelname)-5s %(message)s',
                    datefmt='%H:%M:%S')
log = logging.getLogger('prune')

CTS  = ('R', 'S', 'T')
COLS = ('w', 'v', 'a', 'pf')

HOURLY_SCHEMA = """
CREATE TABLE IF NOT EXISTS cal_sec_hourly (
    hour_ts INTEGER NOT NULL,
    unit    TEXT    NOT NULL,
    n       INTEGER NOT NULL,
    {cols}
    PRIMARY KEY (hour_ts, unit)
);
CREATE INDEX IF NOT EXISTS idx_cal_sec_hourly_ts ON cal_sec_hourly(hour_ts);
""".format(cols=',\n    '.join(
    f"{ch}_{c}_avg REAL, {ch}_{c}_min REAL, {ch}_{c}_max REAL"
    for ch in CTS for c in COLS) + ',')


def _agg_select(cutoff):
    """Build the GROUP BY hour rollup SELECT for rows older than cutoff."""
    aggs = []
    for ch in CTS:
        for c in COLS:
            col = f'{ch}_{c}'
            aggs.append(f"AVG({col}), MIN({col}), MAX({col})")
    return (
        "SELECT (ts/3600)*3600 AS hour_ts, unit, COUNT(*) AS n, "
        + ", ".join(aggs)
        + " FROM cal_sec WHERE ts < ? GROUP BY hour_ts, unit",
        (cutoff,))


def _insert_sql():
    cols = ", ".join(f"{ch}_{c}_avg, {ch}_{c}_min, {ch}_{c}_max"
                     for ch in CTS for c in COLS)
    nq   = 3 + 3 * len(CTS) * len(COLS)   # hour_ts, unit, n + 3 aggs each
    return (f"INSERT OR REPLACE INTO cal_sec_hourly (hour_ts, unit, n, {cols}) "
            f"VALUES ({','.join('?' * nq)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply',  action='store_true', help='write changes (default: dry run)')
    ap.add_argument('--vacuum', action='store_true', help='VACUUM after pruning')
    args = ap.parse_args()

    now    = int(time.time())
    cutoff = ((now - RETENTION * 86400) // 3600) * 3600   # hour-aligned

    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA journal_mode=WAL")
    for stmt in HOURLY_SCHEMA.strip().split(';'):
        if stmt.strip():
            conn.execute(stmt)
    conn.commit()

    old_n = conn.execute("SELECT COUNT(*) FROM cal_sec WHERE ts < ?", (cutoff,)).fetchone()[0]
    if old_n == 0:
        log.info('Nothing to prune (no cal_sec rows older than %d days).', RETENTION)
        conn.close()
        return

    sel_sql, sel_p = _agg_select(cutoff)
    rows = conn.execute(sel_sql, sel_p).fetchall()
    log.info('Cutoff %s — %d raw cal_sec rows in %d hourly buckets to roll up.',
             time.strftime('%Y-%m-%d %H:%M', time.localtime(cutoff)), old_n, len(rows))

    if not args.apply:
        log.info('DRY RUN — no changes. Re-run with --apply to roll up + delete.')
        conn.close()
        return

    conn.executemany(_insert_sql(), rows)
    deleted = conn.execute("DELETE FROM cal_sec WHERE ts < ?", (cutoff,)).rowcount
    conn.commit()
    log.info('Rolled %d buckets, deleted %d raw rows.', len(rows), deleted)

    if args.vacuum:
        log.info('VACUUM …')
        conn.execute("VACUUM")
        log.info('VACUUM done.')

    conn.close()


if __name__ == '__main__':
    main()
