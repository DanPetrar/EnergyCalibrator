#!/usr/bin/env python3
"""Simple HTTP server for EnergyCalibrator reports directory."""

import os
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import unquote

REPORTS_DIR = os.path.dirname(os.path.abspath(__file__))
PORT = 8080

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # suppress access logs

    def do_GET(self):
        path = unquote(self.path.lstrip('/'))

        # Serve a specific PDF
        if path.endswith('.pdf'):
            fpath = os.path.join(REPORTS_DIR, os.path.basename(path))
            if os.path.isfile(fpath):
                self._send_file(fpath)
                return
            self._send(404, 'text/plain', b'Not found')
            return

        # Index page
        pdfs = sorted(
            [f for f in os.listdir(REPORTS_DIR) if f.endswith('.pdf')],
            reverse=True
        )
        rows = ''.join(
            f'<tr><td><a href="/{f}">{f}</a></td>'
            f'<td>{os.path.getsize(os.path.join(REPORTS_DIR, f)) // 1024} kB</td>'
            f'<td>{_mtime(os.path.join(REPORTS_DIR, f))}</td></tr>'
            for f in pdfs
        )
        html = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<title>EnergyCalibrator Reports</title>
<style>
  body {{ font-family: sans-serif; max-width: 800px; margin: 40px auto; color: #222; }}
  h1   {{ color: #2a4060; }}
  table {{ border-collapse: collapse; width: 100%; }}
  th   {{ background: #2a4060; color: #fff; padding: 8px 12px; text-align: left; }}
  td   {{ padding: 7px 12px; border-bottom: 1px solid #ddd; }}
  tr:hover td {{ background: #f0f4f8; }}
  a    {{ color: #2a4060; text-decoration: none; }}
  a:hover {{ text-decoration: underline; }}
  .empty {{ color: #888; font-style: italic; }}
</style>
</head><body>
<h1>EnergyCalibrator Reports</h1>
<p>Unit: cal_F07F8C &nbsp;|&nbsp; {len(pdfs)} report(s) available</p>
{'<table><tr><th>File</th><th>Size</th><th>Generated</th></tr>' + rows + '</table>'
 if pdfs else '<p class="empty">No reports yet.</p>'}
</body></html>"""
        self._send(200, 'text/html', html.encode())

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

def _mtime(path):
    import datetime
    return datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime('%Y-%m-%d %H:%M')

if __name__ == '__main__':
    server = HTTPServer(('0.0.0.0', PORT), Handler)
    print(f'Serving reports on http://192.168.110.225:{PORT}/')
    server.serve_forever()
