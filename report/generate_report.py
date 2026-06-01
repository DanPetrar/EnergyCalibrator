#!/usr/bin/env python3
"""
generate_report.py — EnergyCalibrator daily deviation report (PDF).

Energy methodology: all deviations are computed from energy counter deltas
(dkwh = counter_end - counter_start), never from averaged power snapshots.
  Box CT energy  : sum(R/S/T_dkwh) from cal_min  (firmware accumulator)
  SDM630 energy  : sum(mtr_dkwh)   from cal_min  (SDM kWh register delta)

Usage:
  python3 generate_report.py [--db PATH] [--unit UNIT] [--out OUTPUT.pdf]
                             [--date YYYY-MM-DD | --all]
"""

import argparse, sqlite3, os, sys
from collections import defaultdict
from datetime import datetime, timedelta

HERE       = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.join(HERE, '..', 'collector', 'cal_data.db')
REPORTS    = os.path.join(HERE, '..', 'reports')

THR_GREEN  = 3.0
THR_YELLOW = 6.0

COL_DARK    = '#2a4060'
COL_MID     = '#3d5a80'
COL_WHITE   = '#ffffff'
COL_STRIPE  = '#f0f4f8'
COL_GREEN   = '#d4edda'
COL_YELLOW  = '#fff3cd'
COL_RED     = '#f8d7da'
COL_DKGREEN = '#1d6b3c'
COL_DKAMBER = '#7a5200'
COL_DKRED   = '#7a1a1a'

CT_INFO = {
    'R': 'TDK 30A',
    'S': 'TDK 80A',
    'T': 'YHDC 120A',
}


def dev_bg(pct):
    if pct is None: return COL_WHITE
    a = abs(pct)
    return COL_GREEN if a < THR_GREEN else COL_YELLOW if a < THR_YELLOW else COL_RED


def kpi_bg(pct):
    if pct is None: return COL_DARK
    a = abs(pct)
    return COL_DKGREEN if a < THR_GREEN else COL_DKAMBER if a < THR_YELLOW else COL_DKRED


def _avg(vals): return sum(vals) / len(vals) if vals else None


# ── data ──────────────────────────────────────────────────────────────────────

def fetch_min(db, unit, ts_from, ts_to):
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    w = "ts >= ? AND ts < ?"
    p = [ts_from, ts_to]
    if unit: w += " AND unit = ?"; p.append(unit)
    rows = conn.execute(f"SELECT * FROM cal_min WHERE {w} ORDER BY ts", p).fetchall()
    conn.close()
    return rows


def fetch_sec_all(db, unit, ts_from, ts_to):
    """Return raw sec rows (ts, R_w, S_w, T_w, R_v, R_a, R_pf, ...)."""
    conn = sqlite3.connect(db)
    conn.row_factory = sqlite3.Row
    w = "ts >= ? AND ts < ?"
    p = [ts_from, ts_to]
    if unit: w += " AND unit = ?"; p.append(unit)
    rows = conn.execute(
        f"SELECT ts,R_w,S_w,T_w,R_v,R_a,R_pf,S_v,S_a,S_pf,T_v,T_a,T_pf "
        f"FROM cal_sec WHERE {w} ORDER BY ts", p
    ).fetchall()
    conn.close()
    return rows


def fetch_sec_by_hour(db, unit, ts_from, ts_to):
    rows = fetch_sec_all(db, unit, ts_from, ts_to)
    buckets = defaultdict(lambda: defaultdict(list))
    for r in rows:
        h = datetime.fromtimestamp(r['ts']).hour
        for ch in ('R', 'S', 'T'):
            for col in ('w', 'v', 'a', 'pf'):
                v = r[f'{ch}_{col}']
                if v is not None: buckets[h][f'{ch}_{col}'].append(v)
    out = {}
    for h, d in buckets.items():
        s = {'n': len(d.get('R_w', []))}
        for ch in ('R', 'S', 'T'):
            for col in ('w', 'v', 'a', 'pf'):
                vals = d.get(f'{ch}_{col}', [])
                if vals:
                    s[f'{ch}_{col}_avg'] = _avg(vals)
                    s[f'{ch}_{col}_min'] = min(vals)
                    s[f'{ch}_{col}_max'] = max(vals)
        out[h] = s
    return out


def min_by_hour(rows):
    b = defaultdict(list)
    for r in rows: b[datetime.fromtimestamp(r['ts']).hour].append(r)
    return b


def hour_energy(hrs):
    e = {'R': 0.0, 'S': 0.0, 'T': 0.0, 'SDM': 0.0}
    for r in hrs:
        for ch in ('R', 'S', 'T'):
            if r[f'{ch}_dkwh']: e[ch] += r[f'{ch}_dkwh']
        if r['mtr_dkwh']: e['SDM'] += r['mtr_dkwh']
    return e


def hour_sdm_stats(hrs):
    def a(k): vals = [r[k] for r in hrs if r[k]]; return _avg(vals)
    return {k: a(k) for k in ('mtr_v', 'mtr_a', 'mtr_w', 'mtr_pf')}


def _mean_load(mbh, ch):
    vals = [r[f'{ch}_w'] for hrs in mbh.values() for r in hrs if r[f'{ch}_w']]
    return _avg(vals)


# ── reportlab helpers ─────────────────────────────────────────────────────────

def _styles():
    from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
    from reportlab.lib import colors
    base = getSampleStyleSheet()
    return {
        'Title': ParagraphStyle('T', parent=base['Title'], fontSize=16,
                                textColor=colors.HexColor(COL_DARK), spaceAfter=2),
        'Sub':   ParagraphStyle('Su', parent=base['Normal'], fontSize=8,
                                textColor=colors.HexColor('#666666')),
        'H2':    ParagraphStyle('H2', parent=base['Heading2'], fontSize=11,
                                textColor=colors.HexColor(COL_DARK),
                                spaceBefore=12, spaceAfter=4),
        'H3':    ParagraphStyle('H3', parent=base['Heading3'], fontSize=9,
                                textColor=colors.HexColor(COL_MID),
                                spaceBefore=6, spaceAfter=2),
        'N':     base['Normal'],
        'Sm':    ParagraphStyle('Sm', parent=base['Normal'], fontSize=8),
        'Warn':  ParagraphStyle('W',  parent=base['Normal'], fontSize=8,
                                textColor=colors.HexColor('#cc5500')),
    }


def _table(data, col_widths, row_styles=None):
    from reportlab.platypus import Table, TableStyle
    from reportlab.lib import colors
    t = Table(data, colWidths=col_widths, hAlign='LEFT')
    cmds = [
        ('BACKGROUND',    (0, 0), (-1, 0),  colors.HexColor(COL_DARK)),
        ('TEXTCOLOR',     (0, 0), (-1, 0),  colors.white),
        ('FONTNAME',      (0, 0), (-1, 0),  'Helvetica-Bold'),
        ('FONTSIZE',      (0, 0), (-1, -1), 8),
        ('ALIGN',         (0, 0), (-1, -1), 'RIGHT'),
        ('ALIGN',         (0, 0), (0, -1),  'LEFT'),
        ('GRID',          (0, 0), (-1, -1), 0.3, colors.HexColor('#cccccc')),
        ('TOPPADDING',    (0, 0), (-1, -1), 3),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 3),
        ('LEFTPADDING',   (0, 0), (-1, -1), 5),
        ('RIGHTPADDING',  (0, 0), (-1, -1), 5),
    ]
    for i in range(1, len(data)):
        c = COL_WHITE if i % 2 == 1 else COL_STRIPE
        cmds.append(('BACKGROUND', (0, i), (-1, i), colors.HexColor(c)))
    if row_styles:
        for row_idx, col_idx, bg in row_styles:
            cmds.append(('BACKGROUND', (col_idx, row_idx), (col_idx, row_idx),
                         colors.HexColor(bg)))
    t.setStyle(TableStyle(cmds))
    return t


def _kpi_row(sdm_kwh, ct_info):
    from reportlab.platypus import Table, TableStyle, Paragraph
    from reportlab.lib import colors
    from reportlab.lib.styles import ParagraphStyle
    from reportlab.lib.units import cm

    def _p(txt, sz, bold=False, color='#ffffff'):
        fn = 'Helvetica-Bold' if bold else 'Helvetica'
        return Paragraph(f'<font name="{fn}" size="{sz}" color="{color}">{txt}</font>',
                         ParagraphStyle('kpi', leading=sz + 3))

    def cell(label, kwh, dev_pct, subtitle=None):
        lines = [_p(label, 9, bold=True)]
        if subtitle:
            lines.append(_p(subtitle, 8, color='#ccddff'))
        lines.append(_p(f'{kwh:.4f} kWh', 13, bold=True))
        if dev_pct is not None:
            dc = '#90EE90' if abs(dev_pct) < THR_GREEN else \
                 '#FFD700' if abs(dev_pct) < THR_YELLOW else '#FF6B6B'
            lines.append(_p(f'{dev_pct:+.2f}% vs SDM630', 9, color=dc))
        else:
            lines.append(_p('Reference meter', 8, color='#aaaacc'))
        return lines

    cells = [cell('SDM630', sdm_kwh, None)]
    for ch in ('R', 'S', 'T'):
        cells.append(cell(f'CT-{ch}', ct_info[ch]['kwh'], ct_info[ch]['dev'],
                          subtitle=CT_INFO[ch]))

    bgs = [COL_DARK] + [kpi_bg(ct_info[ch]['dev']) for ch in ('R', 'S', 'T')]
    t = Table([cells], colWidths=[4.15 * cm] * 4, rowHeights=[2.4 * cm])
    cmds = [
        ('VALIGN',        (0, 0), (-1, -1), 'MIDDLE'),
        ('LEFTPADDING',   (0, 0), (-1, -1), 10),
        ('TOPPADDING',    (0, 0), (-1, -1), 8),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 8),
    ]
    for i, bg in enumerate(bgs):
        cmds.append(('BACKGROUND', (i, 0), (i, 0), colors.HexColor(bg)))
    t.setStyle(TableStyle(cmds))
    return t


# ── PDF build ─────────────────────────────────────────────────────────────────

def build_pdf(min_rows, sec_rows, sec_hourly, out_path, unit_label, period_label):
    try:
        from reportlab.lib.pagesizes import A4
        from reportlab.lib import colors
        from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer,
                                        HRFlowable, KeepTogether)
        from reportlab.lib.units import cm
    except ImportError:
        print("ERROR: reportlab not installed."); sys.exit(1)

    doc = SimpleDocTemplate(out_path, pagesize=A4,
                            leftMargin=1.8*cm, rightMargin=1.8*cm,
                            topMargin=1.8*cm, bottomMargin=1.8*cm)
    S = _styles()
    story = []
    ct_footnote = '  ·  '.join(f'CT-{ch}: {CT_INFO[ch]}' for ch in ('R','S','T'))

    def hr(thick=0.5):
        return HRFlowable(width='100%', thickness=thick,
                          color=colors.HexColor(COL_DARK), spaceAfter=4)

    # ── Header ────────────────────────────────────────────────────────────────
    story += [
        Paragraph('EnergyCalibrator — Daily Report', S['Title']),
        Paragraph(
            f'Unit: <b>{unit_label}</b> &nbsp;·&nbsp; '
            f'Period: <b>{period_label}</b> &nbsp;·&nbsp; '
            f'Generated: {datetime.now().strftime("%Y-%m-%d %H:%M")} &nbsp;·&nbsp; '
            f'{len(min_rows)} min records &nbsp;·&nbsp; {len(sec_rows)} sec records',
            S['Sub']),
        Spacer(1, 0.2*cm), hr(1.0), Spacer(1, 0.2*cm),
    ]

    if not min_rows:
        story.append(Paragraph('No data in selected period.', S['N']))
        doc.build(story); return

    # ── Totals ────────────────────────────────────────────────────────────────
    sdm_total = sum(r['mtr_dkwh'] for r in min_rows if r['mtr_dkwh'])
    ct_kwh = {}
    ct_dev = {}
    for ch in ('R', 'S', 'T'):
        k = sum(r[f'{ch}_dkwh'] for r in min_rows if r[f'{ch}_dkwh'])
        ct_kwh[ch] = k
        ct_dev[ch] = (k - sdm_total) / sdm_total * 100 if sdm_total else None
    ct_info = {ch: {'kwh': ct_kwh[ch], 'dev': ct_dev[ch]} for ch in ('R','S','T')}
    mbh   = min_by_hour(min_rows)
    hours = sorted(set(list(mbh.keys()) + list(sec_hourly.keys())))

    # ── Section 1: Day Overview ───────────────────────────────────────────────
    story.append(Paragraph('Day Overview', S['H2']))
    story.append(_kpi_row(sdm_total, ct_info))
    story.append(Spacer(1, 0.35*cm))

    ranking = sorted(('R','S','T'), key=lambda c: abs(ct_dev[c] or 999))
    medals  = ['1st ★', '2nd', '3rd']
    rank_tbl = [['Rank', 'CT', 'Total energy (kWh)', 'Deviation vs SDM630',
                 'Mean load (W)', 'Assessment']]
    rank_styles = []
    for i, ch in enumerate(ranking):
        d = ct_dev[ch]
        asmnt = ('Best accuracy'   if abs(d or 999) < THR_GREEN  else
                 'Good accuracy'   if abs(d or 999) < THR_YELLOW else
                 'Needs calibration')
        rank_tbl.append([medals[i], f'CT-{ch}',
                         f'{ct_kwh[ch]:.4f} kWh',
                         f'{d:+.2f}%' if d is not None else 'n/a',
                         f'{_mean_load(mbh, ch):.0f} W',
                         asmnt])
        rank_styles.append((i + 1, 3, dev_bg(d)))
    story.append(_table(rank_tbl,
                        col_widths=[1.5*cm, 1.5*cm, 3.2*cm, 3.2*cm, 2.8*cm, 4.8*cm],
                        row_styles=rank_styles))
    story.append(Spacer(1, 0.35*cm))

    # ── Section 2: Hourly Summary (energy counters only) ──────────────────────
    # CT dev columns here are per-hour energy-counter deviations (dkWh), not
    # snapshot comparisons — they stay.
    story.append(Paragraph('Hourly Summary', S['H2']))
    sum_hdr = ['Hour', 'SDM630 avg (W)', 'SDM630 (kWh)',
               'CT-R dev', 'CT-S dev', 'CT-T dev', 'Peak (W)', 'Cover (%)']
    sum_rows  = [sum_hdr]
    sum_styles = []
    for h in hours:
        hrs  = mbh.get(h, [])
        sec  = sec_hourly.get(h, {})
        en   = hour_energy(hrs)
        sdms = hour_sdm_stats(hrs)
        devs = {}
        for ch in ('R', 'S', 'T'):
            devs[ch] = (en[ch] - en['SDM']) / en['SDM'] * 100 \
                       if en['SDM'] and en[ch] else None
        peak = max((sec.get(f'{ch}_w_max', 0) for ch in ('R','S','T')), default=None)
        cov  = int(sec.get('n', 0) * 100 // 3600)
        ri   = len(sum_rows)
        sum_rows.append([
            f'{h:02d}:00',
            f'{sdms["mtr_w"]:.0f} W'       if sdms['mtr_w'] else '—',
            f'{en["SDM"]:.4f} kWh',
            f'{devs["R"]:+.1f}%'           if devs['R'] is not None else '—',
            f'{devs["S"]:+.1f}%'           if devs['S'] is not None else '—',
            f'{devs["T"]:+.1f}%'           if devs['T'] is not None else '—',
            f'{peak:.0f} W'                if peak else '—',
            f'{cov}%',
        ])
        for ci, ch in enumerate(('R','S','T'), start=3):
            sum_styles.append((ri, ci, dev_bg(devs.get(ch))))
        if cov < 80:
            sum_styles.append((ri, 7, COL_YELLOW))
    story.append(_table(sum_rows,
                        col_widths=[1.5*cm, 2.7*cm, 2.7*cm,
                                    2.0*cm, 2.0*cm, 2.0*cm, 2.2*cm, 1.9*cm],
                        row_styles=sum_styles))
    story.append(Spacer(1, 0.4*cm))

    story.append(Paragraph(f'Sensors — {ct_footnote}', S['Sm']))

    doc.build(story)


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db',   default=DEFAULT_DB)
    ap.add_argument('--unit', default=None)
    ap.add_argument('--out',  default=None)
    ap.add_argument('--date', default=None, help='YYYY-MM-DD (default: today)')
    ap.add_argument('--all',  action='store_true', help='All data in DB')
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print(f'ERROR: database not found: {args.db}'); sys.exit(1)

    conn  = sqlite3.connect(args.db)
    units = [r[0] for r in conn.execute(
        "SELECT DISTINCT unit FROM cal_min ORDER BY unit").fetchall()]
    conn.close()
    if not units: print('No data.'); sys.exit(0)

    unit_label = args.unit or ', '.join(units)

    if args.all:
        conn = sqlite3.connect(args.db)
        r    = conn.execute("SELECT MIN(ts), MAX(ts) FROM cal_min").fetchone()
        conn.close()
        ts_from, ts_to = r[0], r[1] + 1
        period_label = (
            f'{datetime.fromtimestamp(ts_from).strftime("%Y-%m-%d %H:%M")} – '
            f'{datetime.fromtimestamp(ts_to-1).strftime("%Y-%m-%d %H:%M")}'
        )
    else:
        day     = (datetime.strptime(args.date, '%Y-%m-%d') if args.date
                   else datetime.now().replace(hour=0, minute=0,
                                               second=0, microsecond=0))
        ts_from = int(day.timestamp())
        ts_to   = int((day + timedelta(days=1)).timestamp())
        period_label = day.strftime('%Y-%m-%d')

    if args.out is None:
        safe = period_label[:10].replace('-', '')
        hms  = datetime.now().strftime('%H%M%S')
        os.makedirs(REPORTS, exist_ok=True)
        args.out = os.path.join(REPORTS, f'report_{safe}_{hms}.pdf')

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    print(f'Unit: {unit_label}  Period: {period_label}')
    min_rows   = fetch_min(args.db, args.unit, ts_from, ts_to)
    sec_rows   = fetch_sec_all(args.db, args.unit, ts_from, ts_to)
    sec_hourly = fetch_sec_by_hour(args.db, args.unit, ts_from, ts_to)
    print(f'Min rows: {len(min_rows)}  Sec rows: {len(sec_rows)}  Hours: {len(sec_hourly)}')

    build_pdf(min_rows, sec_rows, sec_hourly, args.out, unit_label, period_label)
    print(f'Saved: {args.out}')


if __name__ == '__main__':
    main()
