#!/usr/bin/env python3
"""HTTP server for EnergyCalibrator reports + bench session UI (Phase E).

- Static PDF index/download.
- Session lifecycle: enter DUT serial -> start -> (pause/continue)* -> stop ->
  generate/download report. A session is a set of ACTIVE time segments over the
  single bench; paused spans are excluded from the report.
  Partial reports can be generated mid-run. See Doc/PhaseE-session-ui.md.
"""

import os
import re
import sys
import html
import time
import sqlite3
import subprocess
import datetime
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import unquote, parse_qs, quote

HERE        = os.path.dirname(os.path.abspath(__file__))
REPORTS_DIR = HERE
PORT        = 8080
# Live WS DB; generate_report.py's default points at collector/cal_data.db, so we
# must pass --db explicitly. Override with CAL_DB if needed.
CAL_DB      = os.environ.get('CAL_DB', '/workspace/cal-data/cal_data.db')
GEN_REPORT  = os.path.join(HERE, '..', 'report', 'generate_report.py')
GRAFANA      = 'http://192.168.110.11:3000/d/bench-calib/'
GRAFANA_SEC  = 'http://192.168.110.11:3000/d/bench-sec/'
GRAFANA_BASE = 'http://192.168.110.11:3000'
ZAX_UNITS   = ['Unit_A', 'Unit_B', 'Unit_C', 'Unit_D']

SAFE_RE = re.compile(r'[^A-Za-z0-9._-]+')


def db():
    conn = sqlite3.connect(CAL_DB, timeout=15)
    conn.execute("PRAGMA busy_timeout=15000")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS sessions (
          id       INTEGER PRIMARY KEY AUTOINCREMENT,
          serial   TEXT    NOT NULL,
          start_ts INTEGER NOT NULL,
          stop_ts  INTEGER,
          status   TEXT    NOT NULL DEFAULT 'running',
          notes    TEXT,
          report   TEXT
        )""")
    conn.execute("""
        CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_running
          ON sessions(status) WHERE status = 'running'""")
    conn.execute("""
        CREATE TABLE IF NOT EXISTS session_segments (
          id         INTEGER PRIMARY KEY AUTOINCREMENT,
          session_id INTEGER NOT NULL,
          seg_start  INTEGER NOT NULL,
          seg_stop   INTEGER
        )""")
    try:                                   # migration: rolling partial-report file
        conn.execute("ALTER TABLE sessions ADD COLUMN partial TEXT")
    except sqlite3.OperationalError:
        pass
    return conn


def _fmt(ts, fmt='%Y-%m-%d %H:%M'):
    return datetime.datetime.fromtimestamp(ts).strftime(fmt) if ts else ''


def _grafana_url(s):
    start_ms = s['start_ts'] * 1000
    to = 'now' if s['status'] in ('running', 'paused') else str((s['stop_ts'] or 0) * 1000)
    return f'{GRAFANA}?from={start_ms}&to={to}'


def _segments(conn, sid, close_at=None):
    """Active [start,stop] segments for a session. An open segment (seg_stop NULL)
    is closed at `close_at` (now) for partial reports, else skipped. Falls back to
    the session's own start/stop window for legacy sessions with no segment rows."""
    rows = conn.execute(
        "SELECT seg_start, seg_stop FROM session_segments WHERE session_id=? "
        "ORDER BY seg_start", (sid,)).fetchall()
    if not rows:
        sr = conn.execute("SELECT start_ts, stop_ts FROM sessions WHERE id=?",
                          (sid,)).fetchone()
        a = sr[0]; b = sr[1] if sr[1] else close_at
        return [(a, b)] if (b and b > a) else []
    out = []
    for a, b in rows:
        if b is None:
            if close_at is None:
                continue
            b = close_at
        if b > a:
            out.append((a, b))
    return out


def _active_unit():
    """Return (unit, age_seconds) for the most recent cal_sec row, or (None, None)."""
    conn = db()
    row = conn.execute(
        "SELECT unit, ts FROM cal_sec ORDER BY ts DESC LIMIT 1").fetchone()
    conn.close()
    if not row:
        return None, None
    return row[0], int(time.time()) - row[1]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress access logs

    # ---- GET ---------------------------------------------------------------
    def do_GET(self):
        path = unquote(self.path.split('?', 1)[0].lstrip('/'))
        if path.endswith('.pdf'):
            fpath = os.path.join(REPORTS_DIR, os.path.basename(path))
            if os.path.isfile(fpath):
                return self._send_file(fpath)
            return self._send(404, 'text/plain', b'Not found')
        if path == 'zax':
            return self._send(200, 'text/html', self._zax_page().encode())
        return self._send(200, 'text/html', self._index().encode())

    # ---- POST --------------------------------------------------------------
    def do_POST(self):
        path = unquote(self.path.split('?', 1)[0].lstrip('/'))
        length = int(self.headers.get('Content-Length', 0))
        form = parse_qs(self.rfile.read(length).decode()) if length else {}

        if path == 'sessions/start':
            return self._start(form)
        if path == 'sessions/pause':
            return self._pause()
        if path == 'sessions/continue':
            return self._continue()
        if path == 'sessions/stop':
            return self._stop()
        if path == 'sessions/change_unit':
            return self._change_unit()
        m = re.fullmatch(r'sessions/(\d+)/(report|partial)', path)
        if m:
            return self._gen(int(m.group(1)), partial=(m.group(2) == 'partial'))
        return self._send(404, 'text/plain', b'Not found')

    def _start(self, form):
        serial = (form.get('serial', [''])[0] or '').strip()
        notes  = (form.get('notes',  [''])[0] or '').strip() or None
        if not serial:
            return self._redirect('/?err=serial+required')
        now = int(time.time())
        conn = db()
        if conn.execute("SELECT 1 FROM sessions WHERE status IN ('running','paused')"
                        ).fetchone():
            conn.close()
            return self._redirect('/?err=a+session+is+already+active')
        try:
            cur = conn.execute(
                "INSERT INTO sessions(serial, start_ts, status, notes) "
                "VALUES(?,?, 'running', ?)", (serial, now, notes))
            conn.execute("INSERT INTO session_segments(session_id, seg_start) "
                         "VALUES(?,?)", (cur.lastrowid, now))
            conn.commit()
        except sqlite3.IntegrityError:
            conn.close()
            return self._redirect('/?err=a+session+is+already+active')
        conn.close()
        return self._redirect('/')

    def _pause(self):
        now = int(time.time())
        conn = db()
        row = conn.execute("SELECT id FROM sessions WHERE status='running'").fetchone()
        if not row:
            conn.close()
            return self._redirect('/?err=no+running+session')
        conn.execute("UPDATE session_segments SET seg_stop=? "
                     "WHERE session_id=? AND seg_stop IS NULL", (now, row[0]))
        conn.execute("UPDATE sessions SET status='paused' WHERE id=?", (row[0],))
        conn.commit()
        conn.close()
        return self._redirect('/')

    def _continue(self):
        now = int(time.time())
        conn = db()
        row = conn.execute("SELECT id FROM sessions WHERE status='paused'").fetchone()
        if not row:
            conn.close()
            return self._redirect('/?err=no+paused+session')
        conn.execute("INSERT INTO session_segments(session_id, seg_start) VALUES(?,?)",
                     (row[0], now))
        conn.execute("UPDATE sessions SET status='running' WHERE id=?", (row[0],))
        conn.commit()
        conn.close()
        return self._redirect('/')

    def _do_stop_session(self):
        """Stop the active session. Returns session id, or None if none active."""
        now = int(time.time())
        conn = db()
        row = conn.execute(
            "SELECT id FROM sessions WHERE status IN ('running','paused')").fetchone()
        if not row:
            conn.close()
            return None
        conn.execute("UPDATE session_segments SET seg_stop=? "
                     "WHERE session_id=? AND seg_stop IS NULL", (now, row[0]))
        conn.execute("UPDATE sessions SET stop_ts=?, status='stopped' WHERE id=?",
                     (now, row[0]))
        conn.commit()
        conn.close()
        return row[0]

    def _stop(self):
        if self._do_stop_session() is None:
            return self._redirect('/?err=no+active+session')
        return self._redirect('/')

    def _change_unit(self):
        prev_unit, _ = _active_unit()
        if self._do_stop_session() is None:
            return self._redirect('/?err=no+active+session')
        if prev_unit:
            return self._redirect(f'/?waiting={quote(prev_unit)}')
        return self._redirect('/')

    def _gen(self, sid, partial):
        now = int(time.time())
        conn = db()
        row = conn.execute(
            "SELECT id, serial, status FROM sessions WHERE id=?", (sid,)).fetchone()
        if not row:
            conn.close()
            return self._redirect('/?err=no+such+session')
        _id, serial, status = row
        if partial and status == 'stopped':
            conn.close()
            return self._redirect('/?err=session+stopped+use+the+report+button')
        if not partial and status != 'stopped':
            conn.close()
            return self._redirect('/?err=stop+the+session+first')
        segs = _segments(conn, sid, close_at=now if partial else None)
        conn.close()
        if not segs:
            return self._redirect('/?err=no+active+data+yet')

        safe = SAFE_RE.sub('-', serial) or 'dut'
        name = f'session_{safe}_{_id}_partial.pdf' if partial else f'session_{safe}_{_id}.pdf'
        out  = os.path.join(REPORTS_DIR, name)
        segstr = ','.join(f'{a}-{b}' for a, b in segs)
        cmd = [sys.executable, GEN_REPORT, '--db', CAL_DB,
               '--segments', segstr, '--out', out]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            msg = html.escape((proc.stderr or proc.stdout or 'report failed').strip())
            return self._send(500, 'text/html',
                              f'<pre>report generation failed:\n{msg}</pre>'
                              f'<p><a href="/">back</a></p>'.encode())
        conn = db()
        conn.execute(f"UPDATE sessions SET {'partial' if partial else 'report'}=? "
                     f"WHERE id=?", (name, _id))
        conn.commit()
        conn.close()
        return self._redirect('/')

    # ---- nav / pages -------------------------------------------------------
    _NAV_STYLE = """
  .nav { background:#1a2a40; padding:0 20px; display:flex; align-items:center; gap:4px;
         position:sticky; top:0; z-index:10; }
  .nav a { color:#8ab4d4; text-decoration:none; padding:10px 16px; font-size:0.9em;
           border-bottom:3px solid transparent; }
  .nav a:hover { color:#fff; }
  .nav a.active { color:#fff; border-bottom-color:#5ba3d4; }
  .nav .brand { color:#5ba3d4; font-weight:bold; font-size:1em;
                padding:10px 16px 10px 0; margin-right:8px; border-right:1px solid #2a3a50; }
"""

    def _nav(self, active):
        return (f'<nav class="nav">'
                f'<span class="brand">&#9889; ZaxEnergy</span>'
                f'<a href="/zax" class="{"active" if active=="zax" else ""}">ZaxEnergy</a>'
                f'<a href="/" class="{"active" if active=="cal" else ""}">EnergyCalibrator</a>'
                f'</nav>')

    def _zax_page(self):
        unit_btns = ''.join(
            f'<a href="{GRAFANA_BASE}/d/zax-power/?var-unit={u}" target="_blank" '
            f'class="chart-btn">{u}</a>'
            for u in ZAX_UNITS)
        return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>ZaxEnergy</title>
<style>
  body {{ font-family: sans-serif; margin: 0; color: #222; }}
  h1, h2 {{ color: #2a4060; margin-left: 0; }}
  .page {{ max-width: 900px; margin: 40px auto; padding: 0 20px; }}
  .card {{ border: 1px solid #dde; border-radius: 8px; padding: 20px 24px;
           margin-bottom: 24px; }}
  .card h2 {{ margin-top: 0; font-size: 1.1em; }}
  .chart-btn {{ display: inline-block; padding: 8px 20px; margin: 4px 6px 4px 0;
               background: #2a4060; color: #fff; border-radius: 5px;
               text-decoration: none; font-size: 0.95em; }}
  .chart-btn:hover {{ background: #3a5a80; }}
  .chart-link {{ display: inline-block; padding: 8px 20px; margin: 4px 0;
                background: #2a7040; color: #fff; border-radius: 5px;
                text-decoration: none; font-size: 0.95em; }}
  .chart-link:hover {{ background: #3a9060; }}
  {self._NAV_STYLE}
</style></head><body>
{self._nav('zax')}
<div class="page">
<h1>ZaxEnergy</h1>
<div class="card">
  <h2>Power &#8594; Grafana</h2>
  <p>Select unit to open the power dashboard:</p>
  {unit_btns}
</div>
<div class="card">
  <h2>Energy &#8594; Grafana</h2>
  <a href="{GRAFANA_BASE}/d/zax-energy/" target="_blank" class="chart-link">
    Open Energy dashboard &#8599;
  </a>
</div>
</div></body></html>"""

    # ---- index page --------------------------------------------------------
    def _index(self):
        conn = db()
        cols = ('id', 'serial', 'start_ts', 'stop_ts', 'status', 'notes',
                'report', 'partial')
        sessions = [dict(zip(cols, r)) for r in conn.execute(
            "SELECT id, serial, start_ts, stop_ts, status, notes, report, partial "
            "FROM sessions ORDER BY id DESC").fetchall()]
        conn.close()
        active = next((s for s in sessions if s['status'] in ('running', 'paused')), None)

        qs = parse_qs(self.path.split('?', 1)[1]) if '?' in self.path else {}
        err           = qs.get('err',      [''])[0]
        waiting_prev  = qs.get('waiting',  [''])[0]
        new_unit_hint = qs.get('new_unit', [''])[0]

        err_html   = f'<p class="err">{html.escape(err)}</p>' if err else ''
        head_extra = ''

        # --- connected unit display ---
        unit, unit_age = _active_unit()
        if unit and unit_age <= 15:
            unit_html = (f'<p>Unit: <b>{html.escape(unit)}</b>'
                         f' <span style="color:#080">&#9679;&nbsp;live</span></p>')
        elif unit:
            unit_html = (f'<p>Unit: <b>{html.escape(unit)}</b>'
                         f' <span style="color:#888">(last seen {unit_age}s ago)</span></p>')
        else:
            unit_html = '<p>Unit: <span style="color:#888">none — no recent data</span></p>'

        # --- waiting for new unit ---
        if waiting_prev and not active:
            cur_unit, cur_age = _active_unit()
            if cur_unit and cur_unit != waiting_prev and cur_age < 30:
                return self._redirect(f'/?new_unit={quote(cur_unit)}')
            head_extra = (f'<meta http-equiv="refresh" '
                          f'content="5; url=/?waiting={html.escape(waiting_prev)}">')
            top = (f'<div class="banner" style="background:#e8f0fe;border-color:#4a7fd4">'
                   f'Waiting for new unit&ensp;'
                   f'<span style="color:#555">(previously: '
                   f'<b>{html.escape(waiting_prev)}</b>)</span>'
                   f'&ensp;&#8987;</div>')
        elif active:
            sid = active['id']
            if active['status'] == 'running':
                toggle = ('<form method="post" action="/sessions/pause" '
                          'style="display:inline"><button>Pause</button></form>')
                state = 'running'
            else:
                toggle = ('<form method="post" action="/sessions/continue" '
                          'style="display:inline"><button>Continue</button></form>')
                state = '<span class="paused">paused</span>'
            partial_btn = (f'<form method="post" action="/sessions/{sid}/partial" '
                           f'style="display:inline"><button>Generate partial report'
                           f'</button></form>')
            partial_dl = (f' <a href="/{active["partial"]}">partial&nbsp;PDF</a>'
                          if active['partial'] else '')
            change_btn = ('<form method="post" action="/sessions/change_unit" '
                          'style="display:inline"><button>Change unit</button></form>')
            top = (f'<div class="banner"><b>{html.escape(active["serial"])}</b> — {state}'
                   f', since {_fmt(active["start_ts"])} '
                   f'(<a href="{_grafana_url(active)}" target="_blank">live in Grafana</a>)'
                   f'<div style="margin-top:8px">{toggle}{partial_btn}'
                   f'<form method="post" action="/sessions/stop" style="display:inline">'
                   f'<button>Stop</button></form>{change_btn}{partial_dl}</div></div>')
        else:
            hint_html  = ''
            notes_val  = ''
            if new_unit_hint:
                hint_html = (f'<p style="color:#2a6020"><b>New unit detected: '
                             f'{html.escape(new_unit_hint)}</b>'
                             f' — enter DUT serial to start a new session.</p>')
                notes_val = f' value="{html.escape(new_unit_hint)}"'
            top = (f'{hint_html}'
                   f'<form method="post" action="/sessions/start" class="startform">'
                   f'<label>DUT serial <input name="serial" required></label>'
                   f'<label>notes <input name="notes"{notes_val}></label>'
                   f'<button>Start session</button></form>')

        srows = ''.join(
            f'<tr><td>{s["id"]}</td><td>{html.escape(s["serial"])}</td>'
            f'<td>{_fmt(s["start_ts"])}</td><td>{_fmt(s["stop_ts"]) or "—"}</td>'
            f'<td>{s["status"]}</td>'
            f'<td><a href="{_grafana_url(s)}" target="_blank">Grafana</a></td>'
            f'<td>{self._report_cell(s)}</td></tr>'
            for s in sessions)
        sess_table = (
            '<table><tr><th>#</th><th>Serial</th><th>Start</th><th>Stop</th>'
            '<th>Status</th><th>Live</th><th>Report</th></tr>' + srows + '</table>'
            if sessions else '<p class="empty">No sessions yet.</p>')

        pdfs = sorted([f for f in os.listdir(REPORTS_DIR) if f.endswith('.pdf')],
                      reverse=True)
        prows = ''.join(
            f'<tr><td><a href="/{f}">{f}</a></td>'
            f'<td>{os.path.getsize(os.path.join(REPORTS_DIR, f)) // 1024} kB</td>'
            f'<td>{_fmt(os.path.getmtime(os.path.join(REPORTS_DIR, f)))}</td></tr>'
            for f in pdfs)
        pdf_table = ('<table><tr><th>File</th><th>Size</th><th>Generated</th></tr>'
                     + prows + '</table>' if pdfs
                     else '<p class="empty">No reports yet.</p>')

        return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>EnergyCalibrator Bench</title>
{head_extra}
<style>
  body {{ font-family: sans-serif; margin: 0; color: #222; }}
  .page {{ max-width: 900px; margin: 40px auto; padding: 0 20px; }}
  h1, h2 {{ color: #2a4060; }}{self._NAV_STYLE}
  table {{ border-collapse: collapse; width: 100%; margin-bottom: 28px; }}
  th {{ background: #2a4060; color: #fff; padding: 8px 12px; text-align: left; }}
  td {{ padding: 7px 12px; border-bottom: 1px solid #ddd; }}
  tr:hover td {{ background: #f0f4f8; }}
  a {{ color: #2a4060; text-decoration: none; }} a:hover {{ text-decoration: underline; }}
  .empty {{ color: #888; font-style: italic; }}
  .err {{ color: #b00; }}
  .paused {{ color: #b07000; font-weight: bold; }}
  .banner {{ background: #fdf3d0; border: 1px solid #e0c860; padding: 12px 16px;
             border-radius: 6px; margin-bottom: 20px; }}
  .startform label {{ margin-right: 16px; }}
  .startform input {{ padding: 4px 6px; }}
  button {{ padding: 5px 14px; margin-left: 10px; cursor: pointer; }}
  .grafana-links {{ margin: 0 0 20px; display: flex; gap: 12px; align-items: center; }}
  .grafana-links span {{ color: #888; font-size: 0.9em; }}
  .grafana-links a {{ background: #1f4e79; color: #fff; padding: 5px 14px;
                      border-radius: 4px; font-size: 0.9em; text-decoration: none; }}
  .grafana-links a:hover {{ background: #2e6da4; }}
</style></head><body>
{self._nav('cal')}
<div class="page">
<h1>EnergyCalibrator Bench</h1>
{unit_html}
{err_html}
{top}
<div class="grafana-links">
  <span>Grafana:</span>
  <a href="{GRAFANA}" target="_blank">Bench - calibration</a>
  <a href="{GRAFANA_SEC}" target="_blank">Bench - per second</a>
</div>
<h2>Sessions</h2>
{sess_table}
<h2>Reports</h2>
{pdf_table}
</div></body></html>"""

    def _report_cell(self, s):
        partial_dl = (f' <a href="/{s["partial"]}">partial</a>'
                      if s['partial'] else '')
        if s['status'] != 'stopped':
            return f'<span class="empty">{s["status"]}</span>{partial_dl}'
        gen = (f'<form method="post" action="/sessions/{s["id"]}/report" '
               f'style="display:inline"><button>'
               f'{"Regenerate" if s["report"] else "Generate report"}</button></form>')
        link = f' <a href="/{s["report"]}">download</a>' if s['report'] else ''
        return gen + link + partial_dl

    # ---- helpers -----------------------------------------------------------
    def _send_file(self, fpath):
        with open(fpath, 'rb') as f:
            data = f.read()
        self.send_response(200)
        self.send_header('Content-Type', 'application/pdf')
        self.send_header('Content-Disposition',
                         f'attachment; filename="{os.path.basename(fpath)}"')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _send(self, code, ctype, body):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, location):
        self.send_response(303)
        self.send_header('Location', location)
        self.send_header('Content-Length', '0')
        self.end_headers()


if __name__ == '__main__':
    server = ThreadingHTTPServer(('0.0.0.0', PORT), Handler)
    print(f'Serving EnergyCalibrator bench on http://192.168.110.11:{PORT}/')
    server.serve_forever()
