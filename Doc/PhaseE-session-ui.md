# Phase E — Bench Session UI (spec)

_Status: IMPLEMENTED 2026-06-03 (commit `a5629f1`, live on `cal_reports`). Built per §7;
end-to-end verified on http://192.168.110.11:8080/. Implementation plan retained in §7._

The originating objective of the Pi→Workstation migration: a small web app on the
bench report server to **enter a DUT box serial → start a test session → stop →
generate/download the session report**, with live monitoring via Grafana during the run.

## 1. Model & key decisions (confirmed with user 2026-06-03)

- **One physical bench.** There is a single bench unit (currently Unit A, MQTT `cal_E47730`; was Unit D `cal_F07F8C` until 2026-06-03 swap). A *session* is a **labeled time window** during which one DUT box (e.g. `PS-1110`) was under test. The entered **serial is metadata/label only** — it does **not** filter data. The session report filters by **time window** alone.
- **Report generation: manual, regeneratable.** Stop only closes the window. A per-session
  *Generate report* button (re)builds the PDF on demand — so you can re-run after late
  MQTT data settles.
- **Minimal metadata:** `serial, start_ts, stop_ts, status` + a free-text `notes` field.
  No operator/DUT-rating fields.
- **Grafana live link** per session, scoped to the session's time window (live `now`
  while running).
- **One running session at a time** — DB-enforced.

## 2. What already exists (no change needed)

- `report/generate_report.py` already supports `--from "YYYY-MM-DD HH:MM" --to "…"` and
  `--out <path>`. A session report = one call with the session's start/stop. **No
  report-engine changes.**
- `reports/serve.py` — GET-only static PDF index/download on `:8080`
  (`cal_reports.service`). Phase E **extends this file**; the systemd unit is unchanged.
- Grafana "Bench - calibration" dashboard, uid `bench-calib`, at
  `http://192.168.110.11:3000/d/bench-calib/` — already the live monitor.
- DB `cal_data.db` (`cal_sec`, `cal_min`, `cal_sec_hourly`). Live copy on the WS is
  `/workspace/cal-data/cal_data.db`.

## 3. Database — new `sessions` table

Added to the existing `cal_data.db` (created by the server on startup if absent):

```sql
CREATE TABLE IF NOT EXISTS sessions (
  id       INTEGER PRIMARY KEY AUTOINCREMENT,
  serial   TEXT    NOT NULL,
  start_ts INTEGER NOT NULL,          -- unix seconds, server clock at Start
  stop_ts  INTEGER,                   -- NULL while running
  status   TEXT    NOT NULL DEFAULT 'running',  -- 'running' | 'stopped'
  notes    TEXT,
  report   TEXT                       -- last generated PDF filename, else NULL
);
-- DB-enforced single running session:
CREATE UNIQUE INDEX IF NOT EXISTS sessions_one_running
  ON sessions(status) WHERE status = 'running';
```

The partial unique index makes a second `start` fail at the DB level (defence in depth on
top of the app check). `sessions` lives in the same DB as the readings; SQLite WAL (already
enabled by the collector) handles the concurrent reader/writer fine.

## 4. HTTP endpoints (extend `serve.py`, stdlib only)

Switch the server to `ThreadingHTTPServer` so a multi-second report build doesn't freeze
the index. POST bodies parsed with `urllib.parse.parse_qs` (no new dependencies).

| Method | Path | Action |
|--------|------|--------|
| GET  | `/` | Index: running-session banner + Stop button (if any); Start form; sessions table; existing PDF list below. |
| POST | `/sessions/start` | Form `serial`, optional `notes`. Insert running session, `start_ts=now`. Reject with 409 if one already running. Redirect to `/`. |
| POST | `/sessions/stop` | Close the running session: `stop_ts=now`, `status='stopped'`. Redirect to `/`. |
| POST | `/sessions/<id>/report` | Run the report (see §5); store PDF name in `report`; redirect to `/`. |
| GET  | `/<file>.pdf` | Existing download (unchanged). |

Each session row in the table shows: serial · start (local) · stop (local) · status ·
**Grafana** link (§6) · **Generate report** button · download link to its PDF if present.

## 5. Report generation (subprocess)

On `POST /sessions/<id>/report`, the server runs (synchronously, captured):

```
<venv python>  report/generate_report.py \
    --db   /workspace/cal-data/cal_data.db \
    --from "<start_ts as 'YYYY-MM-DD HH:MM'>" \
    --to   "<stop_ts  as 'YYYY-MM-DD HH:MM'>" \
    --out  reports/session_<safe_serial>_<id>.pdf
```

- **venv python** = `sys.executable` (the service already runs under
  `/workspace/projects/EnergyCalibrator/.venv/bin/python3`).
- **`--db` is passed explicitly** because `generate_report.py`'s default
  (`collector/cal_data.db`) is *not* the live WS DB. Server reads it from a
  `CAL_DB` constant (env-overridable, default `/workspace/cal-data/cal_data.db`).
- `<safe_serial>` = serial sanitized to `[A-Za-z0-9._-]`.
- Output lands in `reports/` (= `serve.py`'s own dir), so the existing download path
  serves it unchanged. On non-zero exit, show the captured stderr on the page; don't
  update the `report` column.
- `--from/--to` use minute resolution (matches `generate_report.py`); session times are
  truncated to the minute for the report window. Per-minute energy counters are the
  report's basis, so minute granularity is correct.

## 6. Grafana link

Per session, link to the bench dashboard scoped to the window (Grafana accepts epoch-ms
`from`/`to`):

- Running: `…/d/bench-calib/?from=<start_ms>&to=now`
- Stopped: `…/d/bench-calib/?from=<start_ms>&to=<stop_ms>`

Base `http://192.168.110.11:3000/d/bench-calib/`.

## 7. Implementation plan (on agreement)

Develop on the Pi clone `/home/pi/EnergyCalibrator`, deploy to the WS via `ssh ws`.

1. **Schema** → add `sessions` table + partial unique index; server creates it on
   startup. *Verify:* table exists; second concurrent `running` insert rejected.
2. **Extend `serve.py`** → add `do_POST`, the three session endpoints, index template
   (banner / Start form / sessions table), `ThreadingHTTPServer`, `CAL_DB` constant.
   *Verify:* `curl` start → row created; duplicate start → 409; stop → `stop_ts` set.
3. **Report action** → wire `POST /sessions/<id>/report` to the subprocess (§5).
   *Verify:* PDF created at `reports/session_<serial>_<id>.pdf`, window matches start/stop,
   appears in the index and downloads.
4. **Grafana links** → add per-session URLs (§6). *Verify:* link opens dashboard at the
   session window.
5. **Deploy + live smoke test** → pull on WS, `sudo systemctl restart cal_reports`,
   run a short real session against the live bench, generate + download its report.
   *Verify:* end-to-end on `http://192.168.110.11:8080/`.
6. **Docs** → update `Doc/ProjectSpecification.md` (new endpoints/table), append an
   entry to `Doc/errors-history.md` if anything non-trivial surfaces, bump CHANGELOG if
   relevant. Commit to EnergyCalibrator; note in Workstation `INFRASTRUCTURE.md` that
   `cal_reports` now also serves the session UI.

## 8. Out of scope / notes

- No authentication (LAN-local, matches existing services).
- No multi-bench support (single bench unit by design — see §1).
- The serial is not read from firmware; it is operator-entered.
- `serve.py` currently prints a stale `192.168.110.225` startup line — fix to `.11`
  while editing (cosmetic, same file).
- Report build is synchronous; for the single-user bench that's acceptable.
  `ThreadingHTTPServer` keeps the index responsive during a build.

---

## 9. Addendum — Pause/Continue + Partial report (2026-06-03)

Added two buttons to the live-session UI; **verified** on the WS (report-engine
exclusion + lifecycle on a copy DB).

**Model change — sessions are now a set of active segments.** A session can be
`running` | `paused` | `stopped`; paused spans are **excluded** from the report
totals. New table:

```sql
session_segments(id, session_id, seg_start, seg_stop)   -- seg_stop NULL = open
```

- **Start** → session `running` + open segment.
- **Pause** (`POST /sessions/pause`) → close open segment, status `paused`.
- **Continue** (`POST /sessions/continue`) → open a new segment, status `running`.
- **Stop** → close open segment, `stop_ts=now`, status `stopped`.
- Only **one active** (running|paused) session at a time (app guard + the running
  unique index).

**Reports run over the union of active segments** (paused gaps dropped):
- **Final report** (`/sessions/<id>/report`, stopped only) → all closed segments.
- **Generate partial report** (`/sessions/<id>/partial`, running|paused) → segments
  with the open one virtually closed at *now*; writes a **rolling**
  `session_<serial>_<id>_partial.pdf` (overwritten each time); status unchanged.

**Report generator:** `generate_report.py` gained `--segments "a-b,c-d,…"` (epoch
pairs). The three `fetch_*` functions take the union of intervals; the
`--date/--from/--to/--all` single-window paths are untouched (daily cron safe).

**Legacy sessions** created before this change have no `session_segments` rows;
`_segments()` falls back to the session's own `start_ts/stop_ts` window (so the
pre-existing PS-1110 session #1 reports correctly as a single window). Do not
Pause/Continue a legacy session — that would add a partial segment and disable the
fallback.

UI: while a session is live the banner shows **Pause/Continue** (toggle by state),
**Generate partial report**, and **Stop**, plus a download link to the latest
partial PDF.
