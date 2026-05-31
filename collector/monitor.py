#!/usr/bin/env python3
"""
30-min EnergyCalibrator monitor — box CT vs SDM630.

Energy methodology: energy = sum(dkwh) from cal_min for both box and SDM.
Never uses averaged W snapshots for energy calculation.
"""

import sqlite3
from datetime import datetime

DB        = '/home/pi/EnergyCalibrator/collector/cal_data.db'
START_TS  = 1780230574   # 2026-05-31 15:30 clean start
WINDOW_S  = 1800         # 30 minutes

def ts_str(ts):
    return datetime.fromtimestamp(ts).strftime('%H:%M:%S')

def dev(box, ref):
    if not ref:
        return "  n/a "
    return f"{(box - ref) / ref * 100:+.1f}%"

def run():
    conn = sqlite3.connect(DB)
    conn.row_factory = sqlite3.Row

    last_sec = conn.execute(
        "SELECT * FROM cal_sec ORDER BY ts DESC LIMIT 1"
    ).fetchone()
    if not last_sec:
        print("No sec data yet."); conn.close(); return

    now_ts     = last_sec['ts']
    win_start  = now_ts - WINDOW_S

    # closest cal_min row (for instantaneous SDM values)
    last_min = conn.execute(
        "SELECT * FROM cal_min ORDER BY ABS(ts - ?) LIMIT 1", (now_ts,)
    ).fetchone()

    # sec row count for coverage only
    s30 = conn.execute(
        "SELECT COUNT(*) n FROM cal_sec WHERE ts >= ?", (win_start,)
    ).fetchone()
    s_all = conn.execute(
        "SELECT COUNT(*) n FROM cal_sec WHERE ts >= ?", (START_TS,)
    ).fetchone()

    # 30-min energy from cal_min dkwh (box firmware accumulator — unaffected by MQTT drops)
    m30 = conn.execute(
        "SELECT COUNT(*) n, SUM(R_dkwh) Re, SUM(S_dkwh) Se, SUM(T_dkwh) Te, SUM(mtr_dkwh) sdm "
        "FROM cal_min WHERE ts >= ?", (win_start,)
    ).fetchone()

    # cumulative energy from cal_min dkwh
    m_all = conn.execute(
        "SELECT COUNT(*) n, SUM(R_dkwh) Re, SUM(S_dkwh) Se, SUM(T_dkwh) Te, SUM(mtr_dkwh) sdm "
        "FROM cal_min WHERE ts >= ?", (START_TS,)
    ).fetchone()

    conn.close()

    elapsed_min = (now_ts - START_TS) // 60
    elapsed_str = f"{elapsed_min // 60}h {elapsed_min % 60:02d}m"
    bar = "═" * 56

    print(bar)
    print(f"  EnergyCalibrator Monitor — "
          f"{datetime.fromtimestamp(now_ts).strftime('%Y-%m-%d %H:%M:%S')}")
    print(bar)

    # ── Instantaneous ──────────────────────────────────────────
    print(f"\n  INSTANTANEOUS  (last sec: {ts_str(now_ts)})")
    print(f"         {'V':>6}  {'A':>6}  {'W':>5}  {'PF':>5}")
    for ch in ('R', 'S', 'T'):
        v  = last_sec[f'{ch}_v'];  a  = last_sec[f'{ch}_a']
        w  = last_sec[f'{ch}_w'];  pf = last_sec[f'{ch}_pf']
        print(f"  Box {ch}  {v:>6.1f}  {a:>6.3f}  {w:>5.0f}  {pf:>5.2f}")

    if last_min:
        mv = last_min['mtr_v']; ma = last_min['mtr_a']
        mw = last_min['mtr_w']; mpf = last_min['mtr_pf']
        print(f"  SDM    {mv:>6.1f}  {ma:>6.3f}  {mw:>5.1f}  {mpf:>5.3f}"
              f"  ← min {ts_str(last_min['ts'])}")
        # SDM internal consistency: V×A×PF should ≈ W
        vap = mv * ma * mpf
        d_pct = (mw - vap) / vap * 100 if vap else 0
        flag = "  ⚠ INCONSISTENT" if abs(d_pct) > 5 else "  ✓"
        print(f"\n  SDM check  V×A×PF = {vap:.0f}W  reported = {mw:.0f}W"
              f"  Δ={d_pct:+.1f}%{flag}")

    # ── 30-min energy ──────────────────────────────────────────
    r30   = m30['Re']  or 0
    s30k  = m30['Se']  or 0
    t30   = m30['Te']  or 0
    sdm30 = m30['sdm'] or 0
    print(f"\n  ENERGY last 30 min  ({s30['n']} sec / {m30['n']} min rows)")
    print(f"         {'box kWh':>9}  {'SDM kWh':>9}  {'dev':>7}")
    print(f"  R      {r30:>9.4f}  {sdm30:>9.4f}  {dev(r30, sdm30)}")
    print(f"  S      {s30k:>9.4f}  {sdm30:>9.4f}  {dev(s30k, sdm30)}")
    print(f"  T      {t30:>9.4f}  {sdm30:>9.4f}  {dev(t30, sdm30)}")

    # ── Cumulative energy ──────────────────────────────────────
    ra      = m_all['Re']  or 0
    sa      = m_all['Se']  or 0
    ta      = m_all['Te']  or 0
    sdm_all = m_all['sdm'] or 0
    print(f"\n  ENERGY from 15:30  ({elapsed_str}"
          f"  {s_all['n']} sec / {m_all['n']} min rows)")
    print(f"         {'box kWh':>9}  {'SDM kWh':>9}  {'dev':>7}")
    print(f"  R      {ra:>9.4f}  {sdm_all:>9.4f}  {dev(ra, sdm_all)}")
    print(f"  S      {sa:>9.4f}  {sdm_all:>9.4f}  {dev(sa, sdm_all)}")
    print(f"  T      {ta:>9.4f}  {sdm_all:>9.4f}  {dev(ta, sdm_all)}")

    # ── Data coverage ──────────────────────────────────────────
    total_span = now_ts - START_TS
    cov30 = f"{s30['n']}/{min(WINDOW_S, total_span)}"
    cov_all = f"{s_all['n']}/{total_span}"
    print(f"\n  Coverage  30m: {cov30} sec    total: {cov_all} sec")
    print(bar)

if __name__ == '__main__':
    run()
