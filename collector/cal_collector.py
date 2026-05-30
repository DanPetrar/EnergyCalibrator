#!/usr/bin/env python3
"""
cal_collector.py — EnergyCalibrator MQTT → SQLite collector.

Subscribes to:
  <topic>/sec   — binary SecRecord (76 bytes), box per-second readings
  <topic>/min   — JSON paired record, box + SDM630 + deviations

Stores to SQLite tables:
  cal_sec    — per-second box readings (1 Hz)
  cal_min    — per-minute paired record (box + meter + deviations)

Usage:
  python3 cal_collector.py
  CAL_DB=/path/to/cal.db CAL_MQTT_HOST=192.168.110.225 python3 cal_collector.py

Environment:
  CAL_DB         SQLite database path  (default: ./cal_data.db)
  CAL_MQTT_HOST  MQTT broker IP        (default: 192.168.110.225)
  CAL_MQTT_PORT  MQTT broker port      (default: 1883)
  CAL_TOPIC      MQTT topic pattern    (default: cal_+)
"""

import os, sys, time, sqlite3, struct, json, threading, queue, signal, logging, uuid
import paho.mqtt.client as paho

HERE      = os.path.dirname(os.path.abspath(__file__))
DB_PATH   = os.environ.get('CAL_DB',        os.path.join(HERE, 'cal_data.db'))
MQTT_HOST = os.environ.get('CAL_MQTT_HOST', '192.168.110.225')
MQTT_PORT = int(os.environ.get('CAL_MQTT_PORT', '1883'))
TOPIC_PAT = os.environ.get('CAL_TOPIC',    'cal_+')

logging.basicConfig(level=logging.INFO,
                    format='%(asctime)s %(levelname)-5s %(message)s',
                    datefmt='%H:%M:%S')
log = logging.getLogger('cal')

# ── schema ────────────────────────────────────────────────────────────────────
SCHEMA = """
CREATE TABLE IF NOT EXISTS cal_sec (
    ts      INTEGER NOT NULL,
    unit    TEXT    NOT NULL,
    R_v REAL, R_a REAL, R_w REAL, R_pf REAL, R_hz REAL,
    S_v REAL, S_a REAL, S_w REAL, S_pf REAL, S_hz REAL,
    T_v REAL, T_a REAL, T_w REAL, T_pf REAL, T_hz REAL,
    PRIMARY KEY (ts, unit)
);
CREATE TABLE IF NOT EXISTS cal_min (
    ts              INTEGER NOT NULL,
    unit            TEXT    NOT NULL,
    -- SDM630 reference meter
    mtr_v REAL, mtr_a REAL, mtr_w REAL, mtr_pf REAL, mtr_hz REAL, mtr_dkwh REAL,
    -- Box instantaneous (from latest sec at time of min frame)
    R_v REAL, R_a REAL, R_w REAL, R_pf REAL, R_hz REAL,
    S_v REAL, S_a REAL, S_w REAL, S_pf REAL, S_hz REAL,
    T_v REAL, T_a REAL, T_w REAL, T_pf REAL, T_hz REAL,
    -- Box energy delta this minute
    R_dkwh REAL, S_dkwh REAL, T_dkwh REAL,
    -- Deviations (abs + %) per CT vs meter
    R_dev_v_abs REAL,  R_dev_v_pct REAL,
    R_dev_a_abs REAL,  R_dev_a_pct REAL,
    R_dev_w_abs REAL,  R_dev_w_pct REAL,
    R_dev_pf_abs REAL, R_dev_pf_pct REAL,
    R_dev_dkwh_abs REAL, R_dev_dkwh_pct REAL,
    S_dev_v_abs REAL,  S_dev_v_pct REAL,
    S_dev_a_abs REAL,  S_dev_a_pct REAL,
    S_dev_w_abs REAL,  S_dev_w_pct REAL,
    S_dev_pf_abs REAL, S_dev_pf_pct REAL,
    S_dev_dkwh_abs REAL, S_dev_dkwh_pct REAL,
    T_dev_v_abs REAL,  T_dev_v_pct REAL,
    T_dev_a_abs REAL,  T_dev_a_pct REAL,
    T_dev_w_abs REAL,  T_dev_w_pct REAL,
    T_dev_pf_abs REAL, T_dev_pf_pct REAL,
    T_dev_dkwh_abs REAL, T_dev_dkwh_pct REAL,
    PRIMARY KEY (ts, unit)
);
CREATE INDEX IF NOT EXISTS idx_cal_sec_ts   ON cal_sec(ts);
CREATE INDEX IF NOT EXISTS idx_cal_min_ts   ON cal_min(ts);
CREATE INDEX IF NOT EXISTS idx_cal_min_unit ON cal_min(unit);
"""

Q     = queue.Queue()
_stop = threading.Event()

# ── MQTT reader thread ────────────────────────────────────────────────────────
class MqttReader(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True, name='mqtt')
        self._client = None

    def stop(self):
        if self._client:
            self._client.disconnect()

    def run(self):
        def on_connect(client, userdata, flags, reason_code, properties):
            if reason_code.is_failure:
                log.warning('[MQTT] connect failed: %s', reason_code)
                return
            log.info('[MQTT] connected to %s:%d', MQTT_HOST, MQTT_PORT)
            client.subscribe(f'{TOPIC_PAT}/sec')
            client.subscribe(f'{TOPIC_PAT}/min')

        def on_disconnect(client, userdata, flags, reason_code, properties):
            if _stop.is_set():
                return
            if reason_code.is_failure:
                log.warning('[MQTT] disconnected: %s', reason_code)

        def on_message(client, userdata, msg):
            pi_ts = int(time.time())
            # topic: cal_<MAC>/sec  or  cal_<MAC>/min
            parts = msg.topic.split('/')
            if len(parts) != 2:
                return
            unit   = parts[0]   # e.g. "cal_73DA28"
            suffix = parts[1]   # "sec" or "min"

            if suffix == 'sec':
                raw = msg.payload
                if len(raw) != 76:
                    return
                # SecRecord: ts(u32) v[3](f) a[3](f) w[3](f) hz[3](f) var[3](i) pf[3](f)
                f = struct.unpack('<I 3f 3f 3f 3f 3i 3f', raw)
                esp_ts = f[0]
                ts = esp_ts if esp_ts > 1000000 else pi_ts
                data = {
                    'R': {'v': f[1],  'a': f[4],  'w': f[7],  'hz': f[10], 'pf': f[16]},
                    'S': {'v': f[2],  'a': f[5],  'w': f[8],  'hz': f[11], 'pf': f[17]},
                    'T': {'v': f[3],  'a': f[6],  'w': f[9],  'hz': f[12], 'pf': f[18]},
                }
                Q.put(('sec', ts, unit, data))

            elif suffix == 'min':
                try:
                    d = json.loads(msg.payload)
                except Exception:
                    return
                if d.get('meter') is None:
                    return  # meter poll failed — skip
                ts = d.get('ts', pi_ts)
                Q.put(('min', ts, unit, d))

        client_id = 'cal-' + uuid.uuid4().hex[:8]
        client = paho.Client(paho.CallbackAPIVersion.VERSION2,
                             client_id=client_id, reconnect_on_failure=False)
        client.on_connect    = on_connect
        client.on_disconnect = on_disconnect
        client.on_message    = on_message
        self._client = client

        while not _stop.is_set():
            try:
                client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
                client.loop_forever()
            except Exception as e:
                log.warning('[MQTT] %s', e)
            if not _stop.is_set():
                log.info('[MQTT] reconnecting in 10 s')
                _stop.wait(10)

# ── DB write helpers ──────────────────────────────────────────────────────────
SEC_INSERT = """
INSERT OR REPLACE INTO cal_sec
  (ts, unit,
   R_v, R_a, R_w, R_pf, R_hz,
   S_v, S_a, S_w, S_pf, S_hz,
   T_v, T_a, T_w, T_pf, T_hz)
VALUES (?,?,  ?,?,?,?,?,  ?,?,?,?,?,  ?,?,?,?,?)
"""

def _sec_row(ts, unit, d):
    def g(ch, k): return d[ch].get(k)
    return (ts, unit,
            g('R','v'), g('R','a'), g('R','w'), g('R','pf'), g('R','hz'),
            g('S','v'), g('S','a'), g('S','w'), g('S','pf'), g('S','hz'),
            g('T','v'), g('T','a'), g('T','w'), g('T','pf'), g('T','hz'))

MIN_INSERT = """
INSERT OR REPLACE INTO cal_min
  (ts, unit,
   mtr_v, mtr_a, mtr_w, mtr_pf, mtr_hz, mtr_dkwh,
   R_v, R_a, R_w, R_pf, R_hz,
   S_v, S_a, S_w, S_pf, S_hz,
   T_v, T_a, T_w, T_pf, T_hz,
   R_dkwh, S_dkwh, T_dkwh,
   R_dev_v_abs,  R_dev_v_pct,  R_dev_a_abs,  R_dev_a_pct,
   R_dev_w_abs,  R_dev_w_pct,  R_dev_pf_abs, R_dev_pf_pct,
   R_dev_dkwh_abs, R_dev_dkwh_pct,
   S_dev_v_abs,  S_dev_v_pct,  S_dev_a_abs,  S_dev_a_pct,
   S_dev_w_abs,  S_dev_w_pct,  S_dev_pf_abs, S_dev_pf_pct,
   S_dev_dkwh_abs, S_dev_dkwh_pct,
   T_dev_v_abs,  T_dev_v_pct,  T_dev_a_abs,  T_dev_a_pct,
   T_dev_w_abs,  T_dev_w_pct,  T_dev_pf_abs, T_dev_pf_pct,
   T_dev_dkwh_abs, T_dev_dkwh_pct)
VALUES (?,?,
   ?,?,?,?,?,?,
   ?,?,?,?,?,  ?,?,?,?,?,  ?,?,?,?,?,
   ?,?,?,
   ?,?,?,?,?,?,?,?,?,?,
   ?,?,?,?,?,?,?,?,?,?,
   ?,?,?,?,?,?,?,?,?,?)
"""

def _min_row(ts, unit, d):
    m   = d.get('meter', {})
    bs  = d.get('box_sec', {})
    bm  = d.get('box_min', {})
    dev = d.get('dev', {})

    def mg(k):  return m.get(k)
    def bg(c,k): return bs.get(c, {}).get(k)
    def dg(c,k): return dev.get(c, {}).get(k)

    return (ts, unit,
            mg('v'), mg('a'), mg('w'), mg('pf'), mg('hz'), mg('dkwh'),
            bg('R','v'), bg('R','a'), bg('R','w'), bg('R','pf'), bg('R','hz'),
            bg('S','v'), bg('S','a'), bg('S','w'), bg('S','pf'), bg('S','hz'),
            bg('T','v'), bg('T','a'), bg('T','w'), bg('T','pf'), bg('T','hz'),
            bm.get('R',{}).get('dkwh'), bm.get('S',{}).get('dkwh'), bm.get('T',{}).get('dkwh'),
            dg('R','v_abs'),  dg('R','v_pct'),  dg('R','a_abs'),  dg('R','a_pct'),
            dg('R','w_abs'),  dg('R','w_pct'),  dg('R','pf_abs'), dg('R','pf_pct'),
            dg('R','dkwh_abs'), dg('R','dkwh_pct'),
            dg('S','v_abs'),  dg('S','v_pct'),  dg('S','a_abs'),  dg('S','a_pct'),
            dg('S','w_abs'),  dg('S','w_pct'),  dg('S','pf_abs'), dg('S','pf_pct'),
            dg('S','dkwh_abs'), dg('S','dkwh_pct'),
            dg('T','v_abs'),  dg('T','v_pct'),  dg('T','a_abs'),  dg('T','a_pct'),
            dg('T','w_abs'),  dg('T','w_pct'),  dg('T','pf_abs'), dg('T','pf_pct'),
            dg('T','dkwh_abs'), dg('T','dkwh_pct'))

# ── write loop ────────────────────────────────────────────────────────────────
def write_loop(conn):
    cur = conn.cursor()
    sec_count = min_count = 0
    t_report = time.time()

    while not (_stop.is_set() and Q.empty()):
        try:
            kind, ts, unit, data = Q.get(timeout=1)
        except queue.Empty:
            continue

        if kind == 'sec':
            cur.execute(SEC_INSERT, _sec_row(ts, unit, data))
            conn.commit()
            sec_count += 1
            log.debug('[SEC] unit=%s ts=%d R=%.1fV %.3fA', unit, ts,
                      data['R']['v'], data['R']['a'])

        elif kind == 'min':
            cur.execute(MIN_INSERT, _min_row(ts, unit, data))
            conn.commit()
            min_count += 1
            m = data.get('meter', {})
            log.info('[MIN] unit=%s ts=%d mtr=%.1fV %.3fA %.1fW dkwh=%.4f',
                     unit, ts, m.get('v',0), m.get('a',0), m.get('w',0), m.get('dkwh',0))

        now = time.time()
        if now - t_report >= 60:
            log.info('[STATS] sec=%d  min=%d', sec_count, min_count)
            t_report = now

# ── main ──────────────────────────────────────────────────────────────────────
def main():
    conn = sqlite3.connect(DB_PATH)
    for stmt in SCHEMA.strip().split(';'):
        stmt = stmt.strip()
        if stmt:
            conn.execute(stmt)
    conn.commit()
    log.info('DB: %s', DB_PATH)

    mqtt_reader = MqttReader()
    mqtt_reader.start()

    def _stop_handler(sig, frame):
        log.info('stopping (signal %d)…', sig)
        _stop.set()
        mqtt_reader.stop()

    signal.signal(signal.SIGINT,  _stop_handler)
    signal.signal(signal.SIGTERM, _stop_handler)

    try:
        write_loop(conn)
    finally:
        conn.close()
        log.info('collector stopped. DB: %s', DB_PATH)

if __name__ == '__main__':
    main()
