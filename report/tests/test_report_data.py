#!/usr/bin/env python3
"""
Regression tests for the data layer of generate_report.py.

These cover the pure computation functions (no reportlab / no PDF):
aggregation, hourly bucketing, energy totals, and load-band deviation —
plus a round-trip through the SQL fetch_* helpers.

The oracle is independent plain-Python math, so a regression in the module's
implementation is caught even though both compute "the same" numbers.

Run standalone (no pytest needed):
    python3 report/tests/test_report_data.py
Or under pytest if available:
    pytest report/tests/
"""

import os, sys, sqlite3, tempfile, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # report/ dir, so we can import the module
import importlib.util as _u
_spec = _u.spec_from_file_location('gr', os.path.join(os.path.dirname(HERE), 'generate_report.py'))
gr = _u.module_from_spec(_spec)
_spec.loader.exec_module(gr)

CTS  = ('R', 'S', 'T')
COLS = ('w', 'v', 'a', 'pf')


def _approx(a, b, tol=1e-9):
    return a is not None and b is not None and abs(a - b) <= tol


# ── fixtures ────────────────────────────────────────────────────────────────────

def _sec_rows():
    """3 sec rows; R_w = 100/200/300 etc. so aggregates are hand-checkable."""
    rows = []
    for i, base in enumerate((100.0, 200.0, 300.0)):
        ts = 1780300800 + i  # all inside the same hour (00:xx local-independent)
        row = {'ts': ts}
        for j, ch in enumerate(CTS):
            row[f'{ch}_w']  = base + j * 1000      # R:100.. S:1100.. T:2100..
            row[f'{ch}_v']  = 230.0 + j            # constant-ish per ch
            row[f'{ch}_a']  = 0.5 + j
            row[f'{ch}_pf'] = 0.90
        rows.append(row)
    return rows


def _min_rows():
    """2 min rows in two different load bands with known energy deviations."""
    # band 200-500: mtr_w=300; band 500-1000: mtr_w=700
    return [
        {'ts': 1780300800, 'unit': 'cal_TEST',
         'mtr_v': 230.0, 'mtr_a': 1.3, 'mtr_w': 300.0, 'mtr_pf': 0.95, 'mtr_dkwh': 0.00500,
         'R_w': 300.0, 'S_w': 300.0, 'T_w': 300.0,
         'R_dkwh': 0.00505, 'S_dkwh': 0.00510, 'T_dkwh': 0.00495},   # +1%, +2%, -1%
        {'ts': 1780300860, 'unit': 'cal_TEST',
         'mtr_v': 231.0, 'mtr_a': 3.0, 'mtr_w': 700.0, 'mtr_pf': 0.97, 'mtr_dkwh': 0.01200,
         'R_w': 700.0, 'S_w': 700.0, 'T_w': 700.0,
         'R_dkwh': 0.01212, 'S_dkwh': 0.01224, 'T_dkwh': 0.01188},   # +1%, +2%, -1%
    ]


# ── tests: aggregation ──────────────────────────────────────────────────────────

def test_sec_aggregate():
    rows = _sec_rows()
    agg = gr.sec_aggregate(rows)
    assert agg['n'] == 3
    for j, ch in enumerate(CTS):
        vals = [r[f'{ch}_w'] for r in rows]
        assert _approx(agg[f'{ch}_w_avg'], statistics.mean(vals))
        assert _approx(agg[f'{ch}_w_min'], min(vals))
        assert _approx(agg[f'{ch}_w_max'], max(vals))
        assert _approx(agg[f'{ch}_w_std'], statistics.stdev(vals))
    # spot value: R_w avg = 200
    assert _approx(agg['R_w_avg'], 200.0)


def test_min_aggregate():
    rows = _min_rows()
    agg = gr.min_aggregate(rows)
    assert agg['n'] == 2
    assert _approx(agg['mtr_w_avg'], statistics.mean([300.0, 700.0]))
    assert _approx(agg['mtr_v_avg'], statistics.mean([230.0, 231.0]))


def test_min_by_hour_and_energy():
    rows = _min_rows()
    mbh = gr.min_by_hour(rows)
    # both rows are within the same wall-clock hour
    assert sum(len(v) for v in mbh.values()) == 2
    hrs = next(iter(mbh.values()))
    en = gr.hour_energy(hrs)
    assert _approx(en['SDM'], 0.00500 + 0.01200)
    assert _approx(en['R'],   0.00505 + 0.01212)


def test_load_band_stats():
    rows = _min_rows()
    bands = gr.load_band_stats(rows)
    by_label = {b['label']: b for b in bands}
    # one minute in 200-500, one in 500-1000
    assert by_label['200 – 500 W']['n'] == 1
    assert by_label['500 – 1000 W']['n'] == 1
    # energy deviation per CT is +1% / +2% / -1% in both bands
    for label in ('200 – 500 W', '500 – 1000 W'):
        d = by_label[label]['devs']
        assert _approx(d['R'], 1.0,  tol=1e-6)
        assert _approx(d['S'], 2.0,  tol=1e-6)
        assert _approx(d['T'], -1.0, tol=1e-6)
    # percent-of-time sums to 100 over populated bands
    assert _approx(sum(b['pct'] for b in bands), 100.0, tol=1e-6)


# ── tests: SQL fetch round-trip ─────────────────────────────────────────────────

def _build_fixture_db(path):
    conn = sqlite3.connect(path)
    # minimal columns the fetch_* helpers + tested functions read
    conn.execute("""CREATE TABLE cal_min (
        ts INTEGER, unit TEXT,
        mtr_v REAL, mtr_a REAL, mtr_w REAL, mtr_pf REAL, mtr_dkwh REAL,
        R_w REAL, S_w REAL, T_w REAL,
        R_dkwh REAL, S_dkwh REAL, T_dkwh REAL)""")
    conn.execute("""CREATE TABLE cal_sec (
        ts INTEGER, unit TEXT,
        R_w REAL, R_v REAL, R_a REAL, R_pf REAL,
        S_w REAL, S_v REAL, S_a REAL, S_pf REAL,
        T_w REAL, T_v REAL, T_a REAL, T_pf REAL)""")
    for r in _min_rows():
        conn.execute(
            "INSERT INTO cal_min (ts,unit,mtr_v,mtr_a,mtr_w,mtr_pf,mtr_dkwh,"
            "R_w,S_w,T_w,R_dkwh,S_dkwh,T_dkwh) VALUES "
            "(:ts,:unit,:mtr_v,:mtr_a,:mtr_w,:mtr_pf,:mtr_dkwh,"
            ":R_w,:S_w,:T_w,:R_dkwh,:S_dkwh,:T_dkwh)", r)
    for r in _sec_rows():
        conn.execute(
            "INSERT INTO cal_sec (ts,unit,R_w,R_v,R_a,R_pf,S_w,S_v,S_a,S_pf,"
            "T_w,T_v,T_a,T_pf) VALUES "
            "(:ts,'cal_TEST',:R_w,:R_v,:R_a,:R_pf,:S_w,:S_v,:S_a,:S_pf,"
            ":T_w,:T_v,:T_a,:T_pf)", r)
    conn.commit()
    conn.close()


def test_fetch_roundtrip():
    fd, path = tempfile.mkstemp(suffix='.db')
    os.close(fd)
    try:
        _build_fixture_db(path)
        mn = gr.fetch_min(path, 'cal_TEST', 1780300000, 1780400000)
        sc = gr.fetch_sec_all(path, 'cal_TEST', 1780300000, 1780400000)
        assert len(mn) == 2
        assert len(sc) == 3
        # fetched rows feed the aggregators cleanly
        assert gr.min_aggregate(mn)['n'] == 2
        assert _approx(gr.sec_aggregate(sc)['R_w_avg'], 200.0)
    finally:
        os.remove(path)


# ── standalone runner ───────────────────────────────────────────────────────────

if __name__ == '__main__':
    tests = [v for k, v in sorted(globals().items())
             if k.startswith('test_') and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f'  PASS  {t.__name__}')
        except AssertionError as e:
            failed += 1
            print(f'  FAIL  {t.__name__}: {e}')
        except Exception as e:
            failed += 1
            print(f'  ERROR {t.__name__}: {type(e).__name__}: {e}')
    print(f'\n{len(tests) - failed}/{len(tests)} passed')
    sys.exit(1 if failed else 0)
