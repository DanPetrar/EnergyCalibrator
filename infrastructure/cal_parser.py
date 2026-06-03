#!/usr/bin/env python3
"""EnergyCalibrator bench MQTT -> InfluxDB parser.

Subscribes to the local Workstation broker and feeds bench calibration data
into InfluxDB so Grafana can show live bench health.

Topics (unit cal_E47730, Unit A since 2026-06-03; was cal_F07F8C / Unit D):
  cal_E47730/sec  -- 76-byte binary SecRecord (same layout as ZAX sec)
  cal_E47730/min  -- JSON: box_sec / box_min / meter / dev

InfluxDB measurements written (bucket zaxenergy):
  power      tag unit,phase   v,a,w,hz,pf          (box CT per-second readings)
  cal_meter  tag unit         v,a,w,pf,hz,dkwh     (SDM630 reference)
  cal_box    tag unit,phase   w,dkwh               (box CT per-minute)
  cal_dev    tag unit,phase   w_pct,dkwh_pct       (deviation vs SDM -- the KPI)
"""
import struct
import json
import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient
from influxdb_client.client.write_api import SYNCHRONOUS

BROKER_HOST = "127.0.0.1"
BROKER_PORT = 1883

INFLUX_URL    = "http://localhost:8086"
INFLUX_TOKEN  = "ZQ7dEWGvC_N_GB4jSKDKWPN1F7S2R_2GWrY_WPGoJZip_tJEF9gOjN8o3HVItsmHoYoQ_Y40nammeb1T4fQkyQ=="
INFLUX_ORG    = "zax"
INFLUX_BUCKET = "zaxenergy"

UNIT = "cal_E47730"                # stable MQTT identity (DUT name tracked separately)
PHASES = ["R", "S", "T"]
MIN_TS = 1_577_836_800             # 2020-01-01; drop frames with pre-NTP boot timestamps

influx = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx.write_api(write_options=SYNCHRONOUS)


def handle_sec(payload):
    if len(payload) < 76:
        return
    f = struct.unpack('<I 3f 3f 3f 3f 3i 3f', payload[:76])
    if f[0] < MIN_TS:
        return
    ts_ns = f[0] * 1_000_000_000
    points = []
    for i, phase in enumerate(PHASES):
        points.append(
            f"power,unit={UNIT},phase={phase} "
            f"v={f[1+i]},a={f[4+i]},w={f[7+i]},hz={f[10+i]},pf={f[16+i]} {ts_ns}"
        )
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record="\n".join(points))


def handle_min(payload):
    d = json.loads(payload)
    meter = d.get("meter")
    if meter is None:
        return
    if int(d["ts"]) < MIN_TS:
        return
    ts_ns = int(d["ts"]) * 1_000_000_000
    points = []

    points.append(
        f"cal_meter,unit={UNIT} "
        f"v={meter['v']},a={meter['a']},w={meter['w']},"
        f"pf={meter['pf']},hz={meter['hz']},dkwh={meter['dkwh']} {ts_ns}"
    )

    box_sec = d.get("box_sec", {})
    box_min = d.get("box_min", {})
    dev = d.get("dev", {})
    for phase in PHASES:
        w = box_sec.get(phase, {}).get("w")
        dkwh = box_min.get(phase, {}).get("dkwh")
        if w is not None and dkwh is not None:
            points.append(
                f"cal_box,unit={UNIT},phase={phase} w={w},dkwh={dkwh} {ts_ns}"
            )
        dv = dev.get(phase, {})
        if "w_pct" in dv and "dkwh_pct" in dv:
            points.append(
                f"cal_dev,unit={UNIT},phase={phase} "
                f"w_pct={dv['w_pct']},dkwh_pct={dv['dkwh_pct']} {ts_ns}"
            )
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record="\n".join(points))


def on_message(client, userdata, msg):
    mtype = msg.topic.split("/")[-1]
    try:
        if mtype == "sec":
            handle_sec(msg.payload)
        elif mtype == "min":
            handle_min(msg.payload)
    except Exception as e:
        print(f"[ERROR] {msg.topic}: {e}")


def on_connect(client, userdata, flags, rc, properties=None):
    print(f"[MQTT] Connected (rc={rc})")
    client.subscribe(f"{UNIT}/#")
    print(f"[MQTT] Subscribed to {UNIT}/#")


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="cal-parser")
client.on_connect = on_connect
client.on_message = on_message

print(f"[PARSER] Connecting to {BROKER_HOST}:{BROKER_PORT} ...")
client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
client.loop_forever()
