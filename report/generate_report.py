#!/usr/bin/env python3
"""
generate_report.py — EnergyCalibrator 24-hour deviation report (PDF).

Queries cal_min from SQLite for the last 24 hours and generates a PDF report
with per-hour deviation stats (min/max/avg) and energy deviation summary.

Usage:
  python3 generate_report.py [--db PATH] [--unit UNIT] [--out OUTPUT.pdf] [--hours N]

Defaults:
  --db     ../collector/cal_data.db
  --unit   (all units combined)
  --out    report_YYYYMMDD_HHMMSS.pdf
  --hours  24
"""

import argparse, sqlite3, os, sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DB = os.path.join(HERE, '..', 'collector', 'cal_data.db')

# ── data retrieval ────────────────────────────────────────────────────────────
def fetch_min_data(db_path, unit_filter, hours):
    cutoff = int(datetime.now(timezone.utc).timestamp()) - hours * 3600
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    where = "ts >= ?"
    params = [cutoff]
    if unit_filter:
        where += " AND unit = ?"
        params.append(unit_filter)
    rows = conn.execute(
        f"SELECT * FROM cal_min WHERE {where} ORDER BY ts ASC", params
    ).fetchall()
    conn.close()
    return rows

def fetch_units(db_path):
    conn = sqlite3.connect(db_path)
    rows = conn.execute("SELECT DISTINCT unit FROM cal_min ORDER BY unit").fetchall()
    conn.close()
    return [r[0] for r in rows]

# ── statistics ────────────────────────────────────────────────────────────────
def hour_stats(rows, col):
    """Return list of (hour_label, min, max, avg, count) grouped by hour."""
    from collections import defaultdict
    buckets = defaultdict(list)
    for r in rows:
        val = r[col]
        if val is None:
            continue
        dt = datetime.fromtimestamp(r['ts'], tz=timezone.utc)
        buckets[dt.strftime('%H:00')].append(val)
    result = []
    for h in sorted(buckets):
        vals = buckets[h]
        result.append((h, min(vals), max(vals), sum(vals)/len(vals), len(vals)))
    return result

def energy_summary(rows):
    """Return total deviation per CT and energy delivered by meter."""
    sums = {'R': 0.0, 'S': 0.0, 'T': 0.0}
    meter_total = 0.0
    ct_totals   = {'R': 0.0, 'S': 0.0, 'T': 0.0}
    for r in rows:
        for ct in ('R', 'S', 'T'):
            if r[f'{ct}_dkwh'] is not None:
                ct_totals[ct] += r[f'{ct}_dkwh']
            if r[f'{ct}_dev_dkwh_abs'] is not None:
                sums[ct] += r[f'{ct}_dev_dkwh_abs']
        if r['mtr_dkwh'] is not None:
            meter_total += r['mtr_dkwh']
    return meter_total, ct_totals, sums

# ── PDF generation ────────────────────────────────────────────────────────────
def generate_pdf(rows, out_path, unit_label, hours):
    try:
        from reportlab.lib.pagesizes import A4
        from reportlab.lib import colors
        from reportlab.platypus import (SimpleDocTemplate, Paragraph, Spacer,
                                        Table, TableStyle)
        from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
        from reportlab.lib.units import cm
    except ImportError:
        print("ERROR: reportlab not installed. Run: pip3 install reportlab")
        sys.exit(1)

    doc = SimpleDocTemplate(out_path, pagesize=A4,
                            leftMargin=2*cm, rightMargin=2*cm,
                            topMargin=2*cm, bottomMargin=2*cm)
    styles = getSampleStyleSheet()
    title_style = ParagraphStyle('Title2', parent=styles['Title'], fontSize=16)
    h2_style    = ParagraphStyle('H2', parent=styles['Heading2'], fontSize=12,
                                 spaceAfter=4)
    h3_style    = ParagraphStyle('H3', parent=styles['Heading3'], fontSize=10,
                                 spaceAfter=2)
    normal      = styles['Normal']

    ts_now = datetime.now().strftime('%Y-%m-%d %H:%M')
    story  = []

    story.append(Paragraph('EnergyCalibrator — Deviation Report', title_style))
    story.append(Paragraph(
        f'Generated: {ts_now} &nbsp;&nbsp; Unit: {unit_label} &nbsp;&nbsp; '
        f'Period: last {hours} h &nbsp;&nbsp; Records: {len(rows)}',
        normal))
    story.append(Spacer(1, 0.4*cm))

    if not rows:
        story.append(Paragraph('No data in selected period.', normal))
        doc.build(story)
        return

    # ── Energy summary ────────────────────────────────────────────────────────
    meter_total, ct_totals, dev_sums = energy_summary(rows)
    story.append(Paragraph('Energy Summary (24 h totals)', h2_style))
    edata = [['Source', 'Total kWh', 'vs SDM630 (abs kWh)', 'vs SDM630 (%)']]
    edata.append(['SDM630 (reference)', f'{meter_total:.4f}', '—', '—'])
    for ct in ('R', 'S', 'T'):
        pct = (ct_totals[ct] - meter_total) / meter_total * 100 if meter_total else 0
        edata.append([f'CT {ct}', f'{ct_totals[ct]:.4f}',
                      f'{ct_totals[ct]-meter_total:+.4f}', f'{pct:+.2f}%'])
    story.append(_table(edata))
    story.append(Spacer(1, 0.4*cm))

    # ── Per-CT hourly deviation tables ────────────────────────────────────────
    params = [
        ('Voltage deviation (V)', 'v_abs', 'v_pct'),
        ('Current deviation (A)', 'a_abs', 'a_pct'),
        ('Active Power deviation (W)', 'w_abs', 'w_pct'),
    ]
    for ct in ('R', 'S', 'T'):
        story.append(Paragraph(f'CT {ct} — Hourly Deviations vs SDM630', h2_style))
        for (title, abs_col, pct_col) in params:
            stats = hour_stats(rows, f'{ct}_dev_{abs_col}')
            if not stats:
                continue
            story.append(Paragraph(title, h3_style))
            tdata = [['Hour', 'Min', 'Max', 'Avg', 'N']]
            for (h, mn, mx, av, n) in stats:
                tdata.append([h, f'{mn:+.3f}', f'{mx:+.3f}', f'{av:+.3f}', str(n)])
            story.append(_table(tdata))
            story.append(Spacer(1, 0.2*cm))
        story.append(Spacer(1, 0.3*cm))

    doc.build(story)

def _table(data):
    from reportlab.platypus import Table, TableStyle
    from reportlab.lib import colors
    t = Table(data, hAlign='LEFT')
    t.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#2a4060')),
        ('TEXTCOLOR',  (0,0), (-1,0), colors.white),
        ('FONTSIZE',   (0,0), (-1,-1), 8),
        ('ROWBACKGROUNDS', (0,1), (-1,-1), [colors.white, colors.HexColor('#f0f4f8')]),
        ('GRID',       (0,0), (-1,-1), 0.3, colors.HexColor('#cccccc')),
        ('ALIGN',      (1,1), (-1,-1), 'RIGHT'),
        ('TOPPADDING', (0,0), (-1,-1), 3),
        ('BOTTOMPADDING', (0,0), (-1,-1), 3),
    ]))
    return t

# ── CLI ───────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description='EnergyCalibrator PDF report')
    ap.add_argument('--db',    default=DEFAULT_DB)
    ap.add_argument('--unit',  default=None,  help='Filter by unit (e.g. cal_73DA28)')
    ap.add_argument('--out',   default=None,  help='Output PDF path')
    ap.add_argument('--hours', type=int, default=24)
    args = ap.parse_args()

    if not os.path.exists(args.db):
        print(f'ERROR: database not found: {args.db}')
        sys.exit(1)

    if args.out is None:
        ts = datetime.now().strftime('%Y%m%d_%H%M%S')
        args.out = os.path.join(HERE, f'report_{ts}.pdf')

    units = fetch_units(args.db)
    if not units:
        print('No data in database.')
        sys.exit(0)

    unit_label = args.unit if args.unit else ', '.join(units)
    print(f'Units in DB: {units}')
    print(f'Generating report for: {unit_label}  ({args.hours}h)')

    rows = fetch_min_data(args.db, args.unit, args.hours)
    print(f'Records found: {len(rows)}')

    generate_pdf(rows, args.out, unit_label, args.hours)
    print(f'Report saved: {args.out}')

if __name__ == '__main__':
    main()
