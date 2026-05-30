#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Update.h>
#include <time.h>
#include "Config.h"
#include "RingBuf.h"

// ── globals from EnergyCalibrator.ino ────────────────────────────────────────
extern WebServer    server;
extern WiFiClient   wifiClient;
extern PubSubClient mqtt;
extern ZaxConfig    cfg;
extern SecRecord    latestSec;
extern MinRecord    latestMin;
extern MeterRecord  latestMeter;
extern bool         hasSec;
extern bool         hasMin;
extern bool         hasMeter;
extern bool         ntpSynced;
extern time_t       lastNtpTs;
extern String       apSSID;
extern bool         gWifiReconnect;
extern bool         gNtpResync;
extern bool         gDemoChanged;
extern bool         gBufModeChanged;
extern float        cumKwh[3];
extern float        cumKvarh[3];
extern float        prevBoxKwh[3];
extern float        prevBoxKvarh[3];
extern bool         lfsOk;
extern int16_t      gBatPct;
extern bool         gPwrOk;
extern RingBuf<SecRecord> secBuf;
extern RingBuf<MinRecord> minBuf;
extern uint32_t energyStartTs;
extern bool     gMqttReconnect;

static void nvsEnergyStartSave() {
  Preferences p;
  if (p.begin(CFG_NVS, false)) p.putUInt("nrg_start", energyStartTs);
}
static void energyStartReset() {
  energyStartTs = 0;
  Preferences p;
  if (p.begin(CFG_NVS, false)) p.remove("nrg_start");
}

// ── embedded HTML (PROGMEM) ───────────────────────────────────────────────────

static const char PAGE_INDEX[] PROGMEM = R"WEBUI(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EnergyCalibrator</title>
<script defer src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#12131a;color:#dde;padding:14px;font-size:14px}
h1{color:#7ec8e3;font-size:1.1em;margin-bottom:14px;letter-spacing:.5px}
.tabs{display:flex;gap:4px;border-bottom:2px solid #1e2235}
.tab-btn{padding:8px 18px;border:none;border-radius:5px 5px 0 0;cursor:pointer;background:#1a1c2b;color:#778;font-size:0.92em;transition:background .15s}
.tab-btn:hover{background:#232540;color:#aab}
.tab-btn.active{background:#12131a;color:#7ec8e3;border:2px solid #1e2235;border-bottom:2px solid #12131a;margin-bottom:-2px}
.panel{display:none;padding-top:16px}.panel.active{display:block}
.cfg-section{background:#16192a;border:1px solid #252840;border-radius:7px;padding:14px;margin-bottom:12px}
.cfg-section h3{font-size:0.88em;color:#7ec8e3;margin-bottom:12px;padding-bottom:7px;border-bottom:1px solid #252840;letter-spacing:.3px}
.cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 20px}
@media(max-width:520px){.cfg-grid{grid-template-columns:1fr}}
.pr label{display:block;font-size:0.8em;color:#778;margin-bottom:3px}
.pr input{width:100%;background:#0d0f1a;color:#dde;border:1px solid #2a2d42;padding:5px 7px;border-radius:4px;font-size:0.88em;outline:none}
.pr input:focus{border-color:#7ec8e3}
input:-webkit-autofill,input:-webkit-autofill:hover,input:-webkit-autofill:focus{-webkit-box-shadow:0 0 0 1000px #0d0f1a inset;-webkit-text-fill-color:#dde;caret-color:#dde;}
.status-bar{background:#0d1020;border:1px solid #252840;border-radius:5px;padding:8px 12px;margin-bottom:12px;font-size:0.82em;display:flex;flex-wrap:wrap;gap:4px 18px;align-items:center}
.status-bar span{color:#556}.status-bar b{color:#7ec8e3}
.sb-ok{border-color:#1a4a1a}.sb-warn{border-color:#3a3a10}.sb-err{border-color:#4a1a1a}
.sysrow{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid #1e2235;font-size:0.87em}
.sysrow:last-child{border-bottom:none}
.sysrow .sl{color:#778}.sysrow .sv{color:#dde;font-weight:600}
.btn-row{display:flex;gap:8px;margin-top:12px;flex-wrap:wrap;align-items:center}
.btn{padding:7px 16px;border:1px solid #3a4060;border-radius:4px;cursor:pointer;background:#1a1c2b;color:#9ab;font-size:0.88em;transition:all .15s}
.btn:hover{background:#7ec8e3;color:#000;border-color:#7ec8e3}
.btn-ok{border-color:#1a5a1a;color:#5d5}
.btn-ok:hover{background:#2a8a2a;color:#fff;border-color:#2a8a2a}
.btn-warn{border-color:#603030;color:#c77}
.btn-warn:hover{background:#c44;color:#fff;border-color:#c44}
.msg{min-height:20px;margin-top:10px;padding:5px 9px;border-radius:4px;font-size:0.85em}
.ok{background:#0e2b14;color:#5d5}.err{background:#2b0e10;color:#e55}
.card{background:#16192a;border:1px solid #252840;border-radius:7px;padding:14px;margin-bottom:12px}
.card h3{font-size:0.88em;color:#7ec8e3;margin-bottom:10px;padding-bottom:6px;border-bottom:1px solid #252840}
.ts-row{font-size:0.82em;color:#556;margin-bottom:12px}
table{width:100%;border-collapse:collapse;font-size:0.86em}
th{background:#0d0f1a;padding:5px 7px;text-align:right;color:#778;font-size:0.8em;font-weight:600}
th:first-child{text-align:left}
td{padding:5px 7px;text-align:right;border-top:1px solid #1e2235;color:#dde}
td:first-child{text-align:left;font-weight:700;color:#7ec8e3;width:28px}
.na{color:#556;font-style:italic;font-weight:normal}
.sel{background:#0d0f1a;color:#dde;border:1px solid #2a2d42;padding:5px 8px;border-radius:4px;font-size:0.88em;outline:none}
.hist-controls{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap;align-items:center}
</style>
</head>
<body>
<h1>&#9889; EnergyCalibrator &mdash; <span id="dn">&#8230;</span></h1>
<div class="tabs">
  <button class="tab-btn active" id="btn-cfg"  onclick="showTab('cfg')">&#9881;&#xFE0E;&nbsp;Config</button>
  <button class="tab-btn"        id="btn-live" onclick="showTab('live')">&#128202;&nbsp;Live</button>
  <button class="tab-btn"        id="btn-hist" onclick="showTab('hist')">&#128200;&nbsp;History</button>
</div>

<!-- ═══ Config tab ═══════════════════════════════════════════════════════════ -->
<div class="panel active" id="panel-cfg">
<div class="msg" id="msg"></div>

<!-- WiFi -->
<div class="cfg-section">
<h3>&#128246; WiFi</h3>
<div class="status-bar sb-warn" id="net-status"><span>Loading&#8230;</span></div>
<div class="cfg-grid">
  <div class="pr"><label>SSID &mdash; <small style="color:#556">empty&nbsp;=&nbsp;AP&nbsp;only</small></label><input name="ssid" type="text" maxlength="63"></div>
  <div class="pr"><label>Password</label><input name="pass" type="password" maxlength="63" placeholder="blank = keep current"></div>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveWifi()">&#128190;&nbsp;Save WiFi</button>
  <button class="btn" onclick="loadNet(true)">&#8635;&nbsp;Refresh</button>
</div>
</div>

<!-- NTP / Time -->
<div class="cfg-section">
<h3>&#128336; NTP / Time</h3>
<div class="status-bar" id="ntp-status">
  <span>Current time</span><b id="ntp-local">&#8230;</b>
</div>
<div class="cfg-grid">
  <div class="pr"><label>NTP Server</label><input name="ntp_srv" type="text" maxlength="63"></div>
  <div class="pr"><label>Timezone (h, e.g. +2)</label><input name="tz_offset" type="number" min="-12" max="14"></div>
  <div class="pr" style="grid-column:1/-1"><label>Manual time (until NTP sync)</label><input name="man_time_str" type="text" placeholder="2026-01-01T12:00:00"></div>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveNtp()">&#128190;&nbsp;Save NTP</button>
  <button class="btn" onclick="loadNet(true)">&#8635;&nbsp;Refresh</button>
</div>
</div>

<!-- MQTT -->
<div class="cfg-section">
<h3>&#128236; MQTT</h3>
<div class="pr" style="margin-bottom:10px">
  <label style="display:flex;align-items:center;gap:7px;font-size:0.9em;color:#dde;cursor:pointer">
    <input type="checkbox" id="mq_en" style="accent-color:#7ec8e3;width:15px;height:15px" onchange="mqToggle()"> Enable MQTT
  </label>
</div>
<div id="mq_body" style="display:none">
<div class="status-bar" id="mqtt-status"><span>&#8230;</span></div>
<div class="cfg-grid">
  <div class="pr"><label>Host</label><input name="mqtt_host" type="text" maxlength="63"></div>
  <div class="pr"><label>Port</label><input name="mqtt_port" type="number" min="1" max="65535"></div>
  <div class="pr"><label>Topic prefix</label><input name="mqtt_topic" type="text" maxlength="31"></div>
  <div class="pr"><label>Username</label><input name="mqtt_user" type="text" maxlength="31"></div>
  <div class="pr" style="grid-column:1/-1"><label>Password</label><input name="mqtt_pass" type="password" maxlength="31" placeholder="blank = keep current"></div>
</div>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveMqtt()">&#128190;&nbsp;Save MQTT</button>
</div>
</div>

<!-- Device Setup -->
<div class="cfg-section">
<h3>&#128268; Device Setup</h3>
<div class="cfg-grid">
  <div class="pr"><label>Name</label><input name="dev_name" type="text" maxlength="31"></div>
  <div class="pr"><label>Memo</label><input name="memo" type="text" maxlength="63"></div>
</div>
<div class="pr" style="margin-top:10px">
  <label>Buffer mode</label>
  <select id="buf-mode" class="sel" style="margin-top:4px">
    <option value="0">LongTimeEnergy &mdash; long min history, smaller sec window</option>
    <option value="1">AllDataFidelity &mdash; equal sec/min time coverage</option>
  </select>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveDeviceSetup()">&#128190;&nbsp;Save</button>
</div>
</div>

<!-- SDM630 Setup -->
<div class="cfg-section">
<h3>&#128207; SDM630 Meter</h3>
<p style="font-size:0.82em;color:#778;margin:0 0 10px">Eastron SDM630-M reference meter. All 3 CT channels (R/S/T) are compared against SDM630 Phase 1 (V1/A1/W1). RS485 wiring: UART2 RX=GPIO15, TX=GPIO16.</p>
<div class="cfg-grid">
  <div class="pr"><label>Modbus Address</label><input name="sdm_addr" type="number" min="1" max="247"></div>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveSdmCfg()">&#128190;&nbsp;Save</button>
</div>
</div>

<!-- Fault Monitor -->
<div class="cfg-section">
<h3>&#9888; Fault Monitor</h3>
<p style="font-size:0.82em;color:#778;margin:0 0 12px">Monitors electrical anomalies and writes alerts to the error log and MQTT (<code style="font-size:0.95em;color:#9ab">cal/fault</code>, <code style="font-size:0.95em;color:#9ab">cal/faults</code>). Enable only the checks relevant to your installation &mdash; thresholds apply only when the corresponding check is enabled.</p>
<div style="margin-bottom:14px">
  <div class="pr" style="max-width:220px">
    <label>Repeat interval (min)<br><small style="color:#556">Re-send active faults if not resolved. 0&nbsp;=&nbsp;disabled (1&ndash;60).</small></label>
    <input name="fault_repeat_min" type="number" min="0" max="60">
  </div>
</div>
<p style="font-size:0.8em;color:#7ec8e3;margin:0 0 8px;font-weight:600;letter-spacing:.3px">ENABLED CHECKS</p>
<div class="cfg-grid" style="gap:10px 20px">
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_comm" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Communication lost<br><small style="color:#556">No data from measurement box for timeout period</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_volt_zero" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Zero voltage<br><small style="color:#556">Phase completely dead (V &lt; 1 V)</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_volt_under" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Undervoltage<br><small style="color:#556">Voltage below minimum threshold</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_volt_over" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Overvoltage<br><small style="color:#556">Voltage above maximum threshold</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_curr_over" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Overcurrent<br><small style="color:#556">Current exceeds maximum threshold</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_curr_zero" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Zero current<br><small style="color:#556">Voltage present but no load detected (A &lt; 0.05)</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_pf_low" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Low power factor<br><small style="color:#556">|PF| drops below minimum threshold</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_freq" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Frequency out of band<br><small style="color:#556">Hz outside configured min/max range</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_chan_miss" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>Missing channel<br><small style="color:#556">Expected R or S frame not received in a cycle</small></span></label></div>
  <div class="pr"><label style="display:flex;align-items:flex-start;gap:8px;cursor:pointer"><input type="checkbox" id="fm_kwh_rollback" style="accent-color:#7ec8e3;width:15px;height:15px;flex-shrink:0;margin-top:2px"><span>kWh rollback<br><small style="color:#556">Measurement box restarted, energy counter reset</small></span></label></div>
</div>
<p style="font-size:0.8em;color:#7ec8e3;margin:14px 0 8px;font-weight:600;letter-spacing:.3px">THRESHOLDS</p>
<div class="cfg-grid">
  <div class="pr"><label>No-data timeout (s)<br><small style="color:#556">Trigger comm_lost after X seconds without a frame (5&ndash;120)</small></label><input name="comm_timeout_s" type="number" min="5" max="120"></div>
  <div class="pr"><label>Undervoltage (V)<br><small style="color:#556">Alert when V drops below this value</small></label><input name="volt_min" type="number" step="0.1"></div>
  <div class="pr"><label>Overvoltage (V)<br><small style="color:#556">Alert when V exceeds this value</small></label><input name="volt_max" type="number" step="0.1"></div>
  <div class="pr"><label>Overcurrent (A)<br><small style="color:#556">Alert when current exceeds this value</small></label><input name="current_max" type="number" step="0.1"></div>
  <div class="pr"><label>Min power factor<br><small style="color:#556">Alert when |PF| drops below this (0&ndash;1, only when A &gt; 0.1)</small></label><input name="pf_min" type="number" step="0.01" min="0" max="1"></div>
  <div class="pr"><label>Frequency low (Hz)<br><small style="color:#556">Alert when Hz falls below this bound</small></label><input name="freq_min" type="number" step="0.1"></div>
  <div class="pr"><label>Frequency high (Hz)<br><small style="color:#556">Alert when Hz exceeds this bound</small></label><input name="freq_max" type="number" step="0.1"></div>
</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="saveFaultCfg()">&#128190;&nbsp;Save</button>
</div>
</div>

<!-- Error Log -->
<div class="cfg-section">
<h3>&#128196; Error Log</h3>
<div class="btn-row" style="margin-bottom:8px">
  <button class="btn" onclick="loadErrors()">&#8635;&nbsp;Refresh</button>
  <button class="btn btn-warn" onclick="clearErrors()">&#128465;&nbsp;Clear</button>
  <span id="err-status" style="font-size:0.82em;color:#556"></span>
</div>
<pre id="err-log" style="background:#0a0c16;border:1px solid #252840;border-radius:4px;padding:8px;font-size:0.75em;color:#9ab;max-height:220px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;margin:0">(not loaded)</pre>
</div>

<!-- Firmware Update -->
<div class="cfg-section">
<h3>&#128260; Firmware Update</h3>
<div class="sysrow"><span class="sl">Running version</span><span class="sv" id="ota-cur-ver">&#8230;</span></div>
<div class="sysrow"><span class="sl">sec_rec_size</span><span class="sv" id="ota-sec-sz">&#8230;</span></div>
<div class="sysrow" style="margin-bottom:10px"><span class="sl">min_rec_size</span><span class="sv" id="ota-min-sz">&#8230;</span></div>
<p style="font-size:0.82em;color:#778;margin:0 0 10px">Upload a compiled EnergyCalibrator .bin. Struct compatibility is verified before flashing. Device reboots on success.</p>
<div style="margin-bottom:8px"><input type="file" id="ota-file" accept=".bin" style="color:#aab;font-size:0.85em;width:100%"></div>
<div class="btn-row">
  <button class="btn btn-warn" onclick="uploadOta()">&#9889;&nbsp;Upload &amp; Flash</button>
  <span id="ota-status" style="font-size:0.82em;color:#778"></span>
</div>
<progress id="ota-prog" value="0" max="100" style="display:none;width:100%;margin-top:8px"></progress>
</div>

<!-- Demo Mode -->
<div class="cfg-section">
<h3>&#127926; Demo Mode</h3>
<div id="demo-status" style="padding:7px 10px;border-radius:4px;font-size:0.88em;margin-bottom:12px;background:#0a0c16;border:1px solid #252840;color:#556">&#8230;</div>
<div class="btn-row">
  <button class="btn btn-ok" onclick="startDemo()">&#9654;&#xFE0E;&nbsp;Start Demo</button>
  <button class="btn btn-warn" onclick="stopDemo()">&#9632;&#xFE0E;&nbsp;Stop Demo</button>
</div>
</div>

<!-- System Info -->
<div class="cfg-section">
<h3>&#128295; System Info</h3>
<div class="sysrow"><span class="sl">Firmware</span><span class="sv" id="sys-fw">&#8230;</span></div>
<div class="sysrow"><span class="sl">Model</span><span class="sv" id="sys-model">&#8230;</span></div>
<div class="sysrow"><span class="sl">Uptime</span><span class="sv" id="sys-uptime">&#8230;</span></div>
<div class="sysrow" id="pwr-row" style="display:none"><span class="sl">Power</span><span class="sv" id="sys-pwr">&#8230;</span></div>
<div class="sysrow" id="bat-row" style="display:none"><span class="sl">Battery</span><span class="sv" id="sys-bat">&#8230;</span></div>
<div class="btn-row" style="margin-top:12px">
  <button class="btn btn-warn" onclick="doResetCounters()">&#8635;&nbsp;Reset Counters</button>
  <button class="btn btn-warn" onclick="doEraseEnergy()" style="border-color:#6a1010;color:#e55">&#128465;&nbsp;Erase Energy History</button>
  <button class="btn btn-warn" onclick="doRestart()">&#9211;&nbsp;Restart</button>
</div>
</div>

</div><!-- /panel-cfg -->

<!-- ═══ Live tab ══════════════════════════════════════════════════════════════ -->
<div class="panel" id="panel-live">
<div class="ts-row">Updated: <span id="ts">&mdash;</span></div>
<div class="card">
<h3>Instantaneous</h3>
<table>
<tr><th>Ch</th><th>V</th><th>A</th><th>W</th><th>VAr</th><th>PF</th><th>Hz</th></tr>
<tr id="row-R"><td>R&nbsp;<small style="color:#778">(CT1)</small></td><td id="Rv" class="na">&mdash;</td><td id="Ra" class="na">&mdash;</td><td id="Rw" class="na">&mdash;</td><td id="Rvr" class="na">&mdash;</td><td id="Rpf" class="na">&mdash;</td><td id="Rhz" class="na">&mdash;</td></tr>
<tr id="row-S"><td>S&nbsp;<small style="color:#778">(CT2)</small></td><td id="Sv" class="na">&mdash;</td><td id="Sa" class="na">&mdash;</td><td id="Sw" class="na">&mdash;</td><td id="Svr" class="na">&mdash;</td><td id="Spf" class="na">&mdash;</td><td id="Shz" class="na">&mdash;</td></tr>
<tr id="row-T"><td>T&nbsp;<small style="color:#778">(CT3)</small></td><td id="Tv" class="na">&mdash;</td><td id="Ta" class="na">&mdash;</td><td id="Tw" class="na">&mdash;</td><td id="Tvr" class="na">&mdash;</td><td id="Tpf" class="na">&mdash;</td><td id="Thz" class="na">&mdash;</td></tr>
<tr id="row-M" style="color:#7ec8e3"><td>SDM630</td><td id="Mv" class="na">&mdash;</td><td id="Ma" class="na">&mdash;</td><td id="Mw" class="na">&mdash;</td><td class="na">&mdash;</td><td id="Mpf" class="na">&mdash;</td><td id="Mhz" class="na">&mdash;</td></tr>
</table>
</div>
<div class="card">
<h3>Deviation vs SDM630 (last min poll)</h3>
<table>
<tr><th>CT</th><th>V&nbsp;abs</th><th>V&nbsp;%</th><th>A&nbsp;abs</th><th>A&nbsp;%</th><th>W&nbsp;abs</th><th>W&nbsp;%</th><th>kWh&nbsp;abs</th><th>kWh&nbsp;%</th></tr>
<tr><td>R</td><td id="dRva" class="na">&mdash;</td><td id="dRvp" class="na">&mdash;</td><td id="dRaa" class="na">&mdash;</td><td id="dRap" class="na">&mdash;</td><td id="dRwa" class="na">&mdash;</td><td id="dRwp" class="na">&mdash;</td><td id="dRea" class="na">&mdash;</td><td id="dRep" class="na">&mdash;</td></tr>
<tr><td>S</td><td id="dSva" class="na">&mdash;</td><td id="dSvp" class="na">&mdash;</td><td id="dSaa" class="na">&mdash;</td><td id="dSap" class="na">&mdash;</td><td id="dSwa" class="na">&mdash;</td><td id="dSwp" class="na">&mdash;</td><td id="dSea" class="na">&mdash;</td><td id="dSep" class="na">&mdash;</td></tr>
<tr><td>T</td><td id="dTva" class="na">&mdash;</td><td id="dTvp" class="na">&mdash;</td><td id="dTaa" class="na">&mdash;</td><td id="dTap" class="na">&mdash;</td><td id="dTwa" class="na">&mdash;</td><td id="dTwp" class="na">&mdash;</td><td id="dTea" class="na">&mdash;</td><td id="dTep" class="na">&mdash;</td></tr>
</table>
</div>
<div class="card">
<h3>Energy</h3>
<div class="ts-row">Since: <span id="energySince">&mdash;</span></div>
<table>
<tr><th>Ch</th><th>kWh</th><th>kVArh</th></tr>
<tr id="rowE-R"><td>R</td><td id="Rk" class="na">&mdash;</td><td id="Rkv" class="na">&mdash;</td></tr>
<tr id="rowE-S"><td>S</td><td id="Sk" class="na">&mdash;</td><td id="Skv" class="na">&mdash;</td></tr>
<tr id="rowE-T"><td>T</td><td id="Tk" class="na">&mdash;</td><td id="Tkv" class="na">&mdash;</td></tr>
<tr style="border-top:2px solid #252840"><td style="color:#aab">Total</td><td id="totalKwh" class="na">&mdash;</td><td id="totalKvarh" class="na">&mdash;</td></tr>
</table>
</div>
</div>

<!-- ═══ History tab ════════════════════════════════════════════════════════════ -->
<div class="panel" id="panel-hist">

<div class="card">
<h3>Per-second trend</h3>
<div class="hist-controls">
  <select id="sec-field" class="sel">
    <option value="v">Voltage (V)</option>
    <option value="w">Active Power (W)</option>
    <option value="a">Current (A)</option>
    <option value="hz">Frequency (Hz)</option>
  </select>
  <select id="sec-n" class="sel">
    <option value="30">30 s</option>
    <option value="60">60 s</option>
    <option value="300" selected>5 min</option>
  </select>
  <button class="btn" onclick="loadSecChart()">&#8635;&nbsp;Refresh</button>
  <span id="sec-status" style="font-size:0.82em;color:#556"></span>
</div>
<canvas id="chartSec" height="160"></canvas>
</div>

<div class="card">
<h3>Cumulative energy (per-minute)</h3>
<div class="hist-controls">
  <select id="min-field" class="sel">
    <option value="kwh">Active energy (kWh)</option>
    <option value="kvarh">Reactive energy (kVArh)</option>
  </select>
  <select id="min-n" class="sel">
    <option value="30">30 min</option>
    <option value="60">60 min</option>
    <option value="360" selected>6 h</option>
    <option value="1440">24 h</option>
  </select>
  <button class="btn" onclick="loadMinChart()">&#8635;&nbsp;Refresh</button>
  <span id="min-status" style="font-size:0.82em;color:#556"></span>
</div>
<canvas id="chartMin" height="160"></canvas>
</div>

<div class="card">
<h3>&#128229; Export CSV</h3>
<p style="font-size:0.82em;color:#778;margin:0 0 12px">Download raw data as a CSV file for the selected time window. Timestamps use local time.</p>
<div class="hist-controls" style="align-items:flex-end">
  <div><label style="font-size:0.8em;color:#778;display:block;margin-bottom:3px">From</label><input type="datetime-local" id="exp-from" class="pr" style="background:#0d0f1a;color:#dde;border:1px solid #2a2d42;padding:5px 7px;border-radius:4px;font-size:0.85em;outline:none"></div>
  <div><label style="font-size:0.8em;color:#778;display:block;margin-bottom:3px">To</label><input type="datetime-local" id="exp-to" class="pr" style="background:#0d0f1a;color:#dde;border:1px solid #2a2d42;padding:5px 7px;border-radius:4px;font-size:0.85em;outline:none"></div>
  <select id="exp-type" class="sel"><option value="sec">Per-second</option><option value="min">Per-minute</option></select>
  <button class="btn btn-ok" onclick="doExport()">&#128229;&nbsp;Download</button>
  <span id="exp-status" style="font-size:0.82em;color:#c77"></span>
</div>
</div>

</div><!-- /panel-hist -->

<script>
var TABS=['cfg','live','hist'];
function showTab(t){
  TABS.forEach(function(id){
    document.getElementById('panel-'+id).className='panel'+(t===id?' active':'');
    document.getElementById('btn-'+id).className='tab-btn'+(t===id?' active':'');
  });
  if(t==='live')refresh();
  if(t==='hist'){
    loadSecChart();loadMinChart();
    var _fmt=function(d){return d.getFullYear()+'-'+('0'+(d.getMonth()+1)).slice(-2)+'-'+('0'+d.getDate()).slice(-2)+'T'+('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2);};
    var _ef=document.getElementById('exp-from');if(_ef&&!_ef.value)_ef.value=_fmt(new Date(Date.now()-3600000));
    var _et=document.getElementById('exp-to');  if(_et&&!_et.value)_et.value=_fmt(new Date());
  }
}
function F(n){var e=document.querySelector('[name='+n+']');return e?e.value:'';}
function fld(n,v){var e=document.querySelector('[name='+n+']');if(e)e.value=(v==null)?'':v;}
function showMsg(txt,cls){var m=document.getElementById('msg');m.textContent=txt;m.className='msg '+cls;setTimeout(function(){m.className='msg';m.textContent='';},3000);}
function mqToggle(){document.getElementById('mq_body').style.display=document.getElementById('mq_en').checked?'block':'none';}

function loadCfg(){
  fetch('/api/config').then(function(r){return r.json();}).then(function(d){
    document.getElementById('dn').textContent=d.dev_name||'EnergyCalibrator';
    fld('dev_name',d.dev_name);fld('memo',d.memo);
    fld('ssid',d.ssid);fld('ntp_srv',d.ntp_srv);fld('tz_offset',d.tz_offset);
    fld('mqtt_host',d.mqtt_host);fld('mqtt_port',d.mqtt_port);
    fld('mqtt_user',d.mqtt_user);fld('mqtt_topic',d.mqtt_topic);
    document.getElementById('mq_en').checked=!!d.mqtt_en;
    document.getElementById('mq_body').style.display=d.mqtt_en?'block':'none';
    document.getElementById('buf-mode').value=d.buf_mode||0;
    fld('comm_timeout_s',d.comm_timeout_s);fld('volt_min',d.volt_min);fld('volt_max',d.volt_max);
    fld('current_max',d.current_max);fld('pf_min',d.pf_min);fld('freq_min',d.freq_min);fld('freq_max',d.freq_max);
    fld('sdm_addr',d.sdm_addr!==undefined?d.sdm_addr:1);
    var fm=d.fault_mask!==undefined?d.fault_mask:1;
    document.getElementById('fm_comm').checked=!!(fm&1);
    document.getElementById('fm_volt_zero').checked=!!(fm&2);
    document.getElementById('fm_volt_under').checked=!!(fm&4);
    document.getElementById('fm_volt_over').checked=!!(fm&8);
    document.getElementById('fm_curr_over').checked=!!(fm&16);
    document.getElementById('fm_curr_zero').checked=!!(fm&32);
    document.getElementById('fm_pf_low').checked=!!(fm&64);
    document.getElementById('fm_freq').checked=!!(fm&128);
    document.getElementById('fm_chan_miss').checked=!!(fm&256);
    document.getElementById('fm_kwh_rollback').checked=!!(fm&512);
    fld('fault_repeat_min',d.fault_repeat_min!==undefined?d.fault_repeat_min:0);
    updateDemoStatus(!!d.demo_en);
  });
}

function loadNet(fb){
  fetch('/api/netinfo').then(function(r){return r.json();}).then(function(d){
    var ns=document.getElementById('net-status');
    var ip='style="color:#7ec8e3"';
    var s='<span>AP</span><b>'+d.ap_ssid+'</b><span '+ip+'>'+d.ap_ip+'</span>';
    if(d.sta_connected){
      ns.className='status-bar sb-ok';
      s+='<span>STA</span><b>'+d.sta_ssid+'</b><span '+ip+'>'+d.sta_ip+' &bull; '+d.sta_rssi+' dBm</span>';
    } else {
      ns.className='status-bar sb-warn';
      s+='<span>STA</span><b style="color:#c77">not connected</b>';
    }
    ns.innerHTML=s;
    var ntpEl=document.getElementById('ntp-status');
    ntpEl.className='status-bar '+(d.ntp_synced?'sb-ok':'sb-warn');
    document.getElementById('ntp-local').textContent=d.local_time_str||'—';
    if(fb)showMsg('Refreshed','ok');
  });
}

function loadSysinfo(){
  fetch('/api/sysinfo').then(function(r){return r.json();}).then(function(d){
    document.getElementById('sys-fw').textContent=d.fw_version||'—';
    document.getElementById('sys-model').textContent=d.model||'—';
    document.getElementById('sys-uptime').textContent=d.uptime_str||'—';
    var ms=document.getElementById('mqtt-status');
    if(d.mqtt_connected){
      ms.className='status-bar sb-ok';
      ms.innerHTML='<span>MQTT</span><b>connected</b>';
    } else {
      ms.className='status-bar sb-err';
      ms.innerHTML='<span>MQTT</span><b style="color:#c77">not connected</b>';
    }
    if(d.pwr_ok!==undefined){
      document.getElementById('pwr-row').style.display='';
      var pe=document.getElementById('sys-pwr');
      pe.textContent=d.pwr_ok?'OK':'LOST';
      pe.style.color=d.pwr_ok?'#5d5':'#e55';
    }
    if(d.bat_pct!==undefined && d.bat_pct>=0){
      document.getElementById('bat-row').style.display='';
      document.getElementById('sys-bat').textContent=d.bat_pct+'%';
    }
  });
}

function postCfg(d,cb){
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})
    .then(function(r){return r.json();})
    .then(function(r){showMsg(r.msg||'Saved','ok');if(cb)cb();})
    .catch(function(){showMsg('Error saving','err');});
}
function saveWifi()  { postCfg({ssid:F('ssid'), pass:F('pass')}); }
function saveNtp()   { postCfg({ntp_srv:F('ntp_srv'), tz_offset:+F('tz_offset'), man_time_str:F('man_time_str')}, function(){setTimeout(loadNet,2000);}); }
function saveMqtt()  { postCfg({mqtt_en:document.getElementById('mq_en').checked, mqtt_host:F('mqtt_host'), mqtt_port:+F('mqtt_port'), mqtt_user:F('mqtt_user'), mqtt_pass:F('mqtt_pass'), mqtt_topic:F('mqtt_topic')}); }
function saveDeviceSetup(){ postCfg({dev_name:F('dev_name'), memo:F('memo'), buf_mode:+document.getElementById('buf-mode').value}); }
function saveSdmCfg(){ postCfg({sdm_addr:+F('sdm_addr')}); }
function saveFaultCfg(){
  var fm=(document.getElementById('fm_comm').checked?1:0)
        |(document.getElementById('fm_volt_zero').checked?2:0)
        |(document.getElementById('fm_volt_under').checked?4:0)
        |(document.getElementById('fm_volt_over').checked?8:0)
        |(document.getElementById('fm_curr_over').checked?16:0)
        |(document.getElementById('fm_curr_zero').checked?32:0)
        |(document.getElementById('fm_pf_low').checked?64:0)
        |(document.getElementById('fm_freq').checked?128:0)
        |(document.getElementById('fm_chan_miss').checked?256:0)
        |(document.getElementById('fm_kwh_rollback').checked?512:0);
  postCfg({comm_timeout_s:+F('comm_timeout_s'),volt_min:+F('volt_min'),volt_max:+F('volt_max'),
           current_max:+F('current_max'),pf_min:+F('pf_min'),freq_min:+F('freq_min'),freq_max:+F('freq_max'),
           fault_mask:fm,fault_repeat_min:+F('fault_repeat_min')});
}
function updateDemoStatus(active){
  var el=document.getElementById('demo-status');
  if(active){
    el.textContent='Demo mode is Active';
    el.style.color='#7ec8e3';
    el.style.borderColor='#1a4a5a';
    el.style.background='#0a1a20';
  } else {
    el.textContent='Demo mode is Inactive';
    el.style.color='#556';
    el.style.borderColor='#252840';
    el.style.background='#0a0c16';
  }
}
function startDemo(){ postCfg({demo_en:true},  function(){updateDemoStatus(true);}); }
function stopDemo() { postCfg({demo_en:false}, function(){updateDemoStatus(false);}); }
function loadErrors(){
  var st=document.getElementById('err-status');
  st.textContent='Loading…';
  fetch('/api/errors').then(function(r){return r.text();}).then(function(t){
    var el=document.getElementById('err-log');
    el.textContent=t||'(empty)';
    el.scrollTop=el.scrollHeight;
    st.textContent='';
  }).catch(function(){st.textContent='Error';});
}
function clearErrors(){
  if(!confirm('Clear error log?'))return;
  fetch('/api/errors',{method:'DELETE'}).then(function(){
    document.getElementById('err-log').textContent='(empty)';
    showMsg('Error log cleared','ok');
  });
}

function doRestart(){
  if(!confirm('Restart device?'))return;
  if(!confirm('Confirm restart — device will be unreachable for ~10 s.'))return;
  fetch('/restart',{method:'POST'}).then(function(){showMsg('Restarting…','ok');});
}
function doResetCounters(){
  if(!confirm('Reset energy counters to zero?\nCumulative kWh/kVArh will be zeroed. History log is kept.'))return;
  if(!confirm('SECOND CONFIRMATION\nZero all energy counters now? This cannot be undone.'))return;
  fetch('/api/reset_energy',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"mode":"counters"}'})
    .then(function(r){return r.json();}).then(function(r){showMsg(r.msg||'Done','ok');})
    .catch(function(){showMsg('Error','err');});
}
function doEraseEnergy(){
  if(!confirm('ERASE ALL energy history?\nThis deletes stored kWh/kVArh data and clears the history ring. Cannot be undone.'))return;
  if(!confirm('FINAL CONFIRMATION\nPermanently delete all energy history now?'))return;
  fetch('/api/reset_energy',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"mode":"full"}'})
    .then(function(r){return r.json();}).then(function(r){showMsg(r.msg||'Done','ok');})
    .catch(function(){showMsg('Error','err');});
}

function sv(id,v){var e=document.getElementById(id);if(e){e.textContent=v;e.className='';}}
function refresh(){
  fetch('/api/data').then(function(r){return r.json();}).then(function(d){
    if(d.dev_name)document.getElementById('dn').textContent=d.dev_name;
    document.getElementById('ts').textContent=d.ts_str||'—';
    if(d.has_sec){
      ['R','S','T'].forEach(function(c){
        var ch=d.sec[c];
        sv(c+'v',ch.v.toFixed(1));sv(c+'a',ch.a.toFixed(3));sv(c+'w',ch.w.toFixed(2));
        sv(c+'vr',ch.var);sv(c+'pf',ch.pf.toFixed(2));sv(c+'hz',ch.hz.toFixed(2));
      });
    }
    if(d.has_meter){
      var m=d.meter;
      sv('Mv',m.v.toFixed(1));sv('Ma',m.a.toFixed(3));sv('Mw',m.w.toFixed(2));
      sv('Mpf',m.pf.toFixed(3));sv('Mhz',m.hz.toFixed(2));
    }
    if(d.last_dev){
      ['R','S','T'].forEach(function(c){
        var dv=d.last_dev[c];
        sv('d'+c+'va',dv.v_abs.toFixed(2));sv('d'+c+'vp',dv.v_pct.toFixed(2)+'%');
        sv('d'+c+'aa',dv.a_abs.toFixed(4));sv('d'+c+'ap',dv.a_pct.toFixed(2)+'%');
        sv('d'+c+'wa',dv.w_abs.toFixed(1));sv('d'+c+'wp',dv.w_pct.toFixed(2)+'%');
        sv('d'+c+'ea',dv.dkwh_abs.toFixed(4));sv('d'+c+'ep',dv.dkwh_pct.toFixed(2)+'%');
      });
    }
    if(d.has_min){
      ['R','S','T'].forEach(function(c){
        var ch=d.min[c];
        sv(c+'k',ch.kwh.toFixed(3));sv(c+'kv',ch.kvarh.toFixed(3));
      });
      var tk=0,tv=0;['R','S','T'].forEach(function(c){tk+=d.min[c].kwh;tv+=d.min[c].kvarh;});
      sv('totalKwh',tk.toFixed(3));sv('totalKvarh',tv.toFixed(3));
    }
    if(d.energy_since>0){var _d=new Date(d.energy_since*1000),_p=function(n){return('0'+n).slice(-2);};sv('energySince',_d.getFullYear()+'-'+_p(_d.getMonth()+1)+'-'+_p(_d.getDate())+' '+_p(_d.getHours())+':'+_p(_d.getMinutes()));}
  }).catch(function(){document.getElementById('ts').textContent='error';});
}

function doExport(){
  var f=document.getElementById('exp-from').value;
  var t=document.getElementById('exp-to').value;
  var st=document.getElementById('exp-status');
  if(!f||!t){st.textContent='Set From and To first';return;}
  var ts0=Math.floor(new Date(f).getTime()/1000);
  var ts1=Math.floor(new Date(t).getTime()/1000);
  if(ts1<=ts0){st.textContent='To must be after From';return;}
  st.textContent='';
  window.location.href='/api/export?type='+document.getElementById('exp-type').value+'&from='+ts0+'&to='+ts1;
}

// ── History charts ────────────────────────────────────────────────────────────
var secChartInst=null, minChartInst=null;
var CHART_COLORS=['#e06060','#60b060','#6090e0'];
var CH_NAMES=['R','S','T'];

function makeChart(canvasId, labels, datasets){
  var ctx=document.getElementById(canvasId).getContext('2d');
  return new Chart(ctx,{
    type:'line',
    data:{labels:labels,datasets:datasets},
    options:{
      animation:false,
      responsive:true,
      plugins:{legend:{labels:{color:'#dde',boxWidth:14,font:{size:12}}}},
      scales:{
        x:{ticks:{color:'#778',maxTicksLimit:10,maxRotation:0},grid:{color:'#1e2235'}},
        y:{ticks:{color:'#778'},grid:{color:'#1e2235'}}
      }
    }
  });
}

function loadSecChart(){
  var n=+document.getElementById('sec-n').value;
  var field=document.getElementById('sec-field').value;
  var st=document.getElementById('sec-status');
  st.textContent='Loading…';
  fetch('/api/history?type=sec&n='+n)
    .then(function(r){return r.json();})
    .then(function(d){
      var recs=d.records;
      if(!recs||recs.length===0){st.textContent='No data yet';return;}
      var cnt=recs.length;
      var labels=recs.map(function(_,i){var s=-(cnt-1-i);return s===0?'now':(s+'s');});
      var datasets=CH_NAMES.map(function(ch,ci){
        return{
          label:ch,
          data:recs.map(function(r){return r[field][ci];}),
          borderColor:CHART_COLORS[ci],
          borderWidth:1.5,
          pointRadius:0,
          tension:0.2
        };
      });
      if(secChartInst)secChartInst.destroy();
      secChartInst=makeChart('chartSec',labels,datasets);
      st.textContent=cnt+' records';
    })
    .catch(function(){st.textContent='Error';});
}

function loadMinChart(){
  var n=+document.getElementById('min-n').value;
  var field=document.getElementById('min-field').value;
  var st=document.getElementById('min-status');
  st.textContent='Loading…';
  fetch('/api/history?type=min&n='+n)
    .then(function(r){return r.json();})
    .then(function(d){
      var recs=d.records;
      if(!recs||recs.length===0){st.textContent='No data yet';return;}
      var labels=recs.map(function(r){
        if(!r.ts)return '?';
        var dt=new Date(r.ts*1000);
        return dt.getHours().toString().padStart(2,'0')+':'+dt.getMinutes().toString().padStart(2,'0');
      });
      var yLabel=field==='kwh'?'kWh':'kVArh';
      var datasets=CH_NAMES.map(function(ch,ci){
        return{
          label:ch+' '+yLabel,
          data:recs.map(function(r){return r[field][ci];}),
          borderColor:CHART_COLORS[ci],
          borderWidth:1.5,
          pointRadius:2,
          tension:0.1
        };
      });
      if(minChartInst)minChartInst.destroy();
      minChartInst=makeChart('chartMin',labels,datasets);
      st.textContent=recs.length+' records';
    })
    .catch(function(){st.textContent='Error';});
}

function loadOtaMeta(){
  fetch('/api/ota_meta').then(function(r){return r.json();}).then(function(d){
    document.getElementById('ota-cur-ver').textContent=d.fw_version||'?';
    document.getElementById('ota-sec-sz').textContent=d.sec_rec_size||'?';
    document.getElementById('ota-min-sz').textContent=d.min_rec_size||'?';
  });
}

function uploadOta(){
  var f=document.getElementById('ota-file').files[0];
  if(!f){alert('Select a .bin file first');return;}
  if(!confirm('Flash '+f.name+' ('+Math.round(f.size/1024)+' KB)?\nDevice will reboot on success.')){return;}
  var st=document.getElementById('ota-status');
  var pr=document.getElementById('ota-prog');
  var fd=new FormData();
  fd.append('firmware',f);
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/api/ota');
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);pr.value=p;st.textContent='Uploading... '+p+'%';st.style.color='#aab';}
  };
  xhr.onload=function(){
    pr.style.display='none';
    var r;try{r=JSON.parse(xhr.responseText);}catch(e){r={err:'invalid response'};}
    if(r.err){st.textContent='Error: '+r.err;st.style.color='#f66';}
    else{st.textContent=r.msg||'OK';st.style.color='#5d5';}
  };
  xhr.onerror=function(){pr.style.display='none';st.textContent='Upload failed';st.style.color='#f66';};
  pr.value=0;pr.style.display='block';
  st.textContent='Starting...';st.style.color='#aab';
  xhr.send(fd);
}

loadCfg();loadNet();loadSysinfo();loadOtaMeta();
setInterval(function(){
  loadNet();
  loadSysinfo();
  refresh();
  if(document.getElementById('panel-hist').className.indexOf('active')>=0){loadSecChart();loadMinChart();}
},60000);
</script>
</body>
</html>
)WEBUI";

// ── helpers ───────────────────────────────────────────────────────────────────

static void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

// ── route handlers ────────────────────────────────────────────────────────────

static void handleIndex() {
  server.send_P(200, "text/html", PAGE_INDEX);
}

static void handleGetConfig() {
  JsonDocument doc;
  doc["dev_name"]   = cfg.dev_name;
  doc["memo"]       = cfg.memo;
  doc["ssid"]       = cfg.ssid;
  doc["ntp_srv"]    = cfg.ntp_srv;
  doc["tz_offset"]  = cfg.tz_offset;
  doc["mqtt_en"]    = cfg.mqtt_en;
  doc["mqtt_host"]  = cfg.mqtt_host;
  doc["mqtt_port"]  = cfg.mqtt_port;
  doc["mqtt_user"]  = cfg.mqtt_user;
  doc["mqtt_topic"] = cfg.mqtt_topic;
  doc["demo_en"]       = cfg.demo_en;
  doc["buf_mode"]      = cfg.buf_mode;
  doc["comm_timeout_s"] = cfg.comm_timeout_s;
  doc["volt_min"]      = cfg.volt_min;
  doc["volt_max"]      = cfg.volt_max;
  doc["current_max"]   = cfg.current_max;
  doc["pf_min"]        = cfg.pf_min;
  doc["freq_min"]      = cfg.freq_min;
  doc["freq_max"]      = cfg.freq_max;
  doc["fault_mask"]       = cfg.fault_mask;
  doc["fault_repeat_min"] = cfg.fault_repeat_min;
  doc["sdm_addr"]         = cfg.sdm_addr;
  // password never returned
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handlePostConfig() {
  String body = server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendJson(400, "{\"err\":\"bad JSON\"}");
    return;
  }

  auto gs = [&](const char* key, char* buf, size_t sz) {
    if (doc[key].is<const char*>()) {
      strncpy(buf, doc[key].as<const char*>(), sz - 1);
      buf[sz - 1] = '\0';
    }
  };

  gs("dev_name", cfg.dev_name, sizeof(cfg.dev_name));
  gs("memo",     cfg.memo,     sizeof(cfg.memo));

  // WiFi — only update password if non-empty
  bool wifiChanged = false;
  if (doc["ssid"].is<const char*>()) {
    String newSSID = doc["ssid"].as<const char*>();
    if (newSSID != String(cfg.ssid)) wifiChanged = true;
    gs("ssid", cfg.ssid, sizeof(cfg.ssid));
  }
  if (doc["pass"].is<const char*>()) {
    String newPass = doc["pass"].as<const char*>();
    if (newPass.length() > 0) {
      gs("pass", cfg.pass, sizeof(cfg.pass));
      wifiChanged = true;
    }
  }

  bool ntpChanged = false;
  if (doc["ntp_srv"].is<const char*>()) {
    if (String(doc["ntp_srv"].as<const char*>()) != String(cfg.ntp_srv)) ntpChanged = true;
    gs("ntp_srv", cfg.ntp_srv, sizeof(cfg.ntp_srv));
  }
  if (doc["tz_offset"].is<int>()) {
    int16_t newTz = (int16_t)doc["tz_offset"].as<int>();
    if (newTz != cfg.tz_offset) ntpChanged = true;
    cfg.tz_offset = newTz;
  }

  // Manual time — apply immediately, not stored in NVS
  if (doc["man_time_str"].is<const char*>()) {
    const char* ts = doc["man_time_str"].as<const char*>();
    struct tm t = {};
    int y, mo, d, h, mi, s;
    if (strlen(ts) >= 19 &&
        sscanf(ts, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
      t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
      t.tm_hour = h;        t.tm_min  = mi;     t.tm_sec  = s;
      time_t epoch = mktime(&t);
      if (epoch > 0) {
        struct timeval tv = {epoch, 0};
        settimeofday(&tv, nullptr);
        Serial.printf("[NTP] Manual time set: %s\n", ts);
      }
    }
  }

  // Snapshot MQTT fields before applying — used to detect changes for F1 reconnect
  bool     oldMqttEn = cfg.mqtt_en;
  char     oldHost[sizeof(cfg.mqtt_host)];   strlcpy(oldHost,  cfg.mqtt_host,  sizeof(oldHost));
  uint16_t oldPort   = cfg.mqtt_port;
  char     oldUser[sizeof(cfg.mqtt_user)];   strlcpy(oldUser,  cfg.mqtt_user,  sizeof(oldUser));
  char     oldTopic[sizeof(cfg.mqtt_topic)]; strlcpy(oldTopic, cfg.mqtt_topic, sizeof(oldTopic));
  bool mqttPassChanged = false;

  if (doc["mqtt_en"].is<bool>())
    cfg.mqtt_en = doc["mqtt_en"].as<bool>();
  gs("mqtt_host",  cfg.mqtt_host,  sizeof(cfg.mqtt_host));
  if (doc["mqtt_port"].is<int>())
    cfg.mqtt_port = (uint16_t)doc["mqtt_port"].as<int>();
  gs("mqtt_user",  cfg.mqtt_user,  sizeof(cfg.mqtt_user));
  if (doc["mqtt_pass"].is<const char*>()) {
    String mp = doc["mqtt_pass"].as<const char*>();
    if (mp.length() > 0) { gs("mqtt_pass", cfg.mqtt_pass, sizeof(cfg.mqtt_pass)); mqttPassChanged = true; }
  }
  gs("mqtt_topic", cfg.mqtt_topic, sizeof(cfg.mqtt_topic));

  if (doc["demo_en"].is<bool>()) {
    bool newDemo = doc["demo_en"].as<bool>();
    if (newDemo != cfg.demo_en) { cfg.demo_en = newDemo; gDemoChanged = true; }
  }

  if (doc["buf_mode"].is<int>()) {
    uint8_t newMode = (uint8_t)doc["buf_mode"].as<int>();
    if (newMode != cfg.buf_mode) { cfg.buf_mode = newMode; gBufModeChanged = true; }
  }

  if (!doc["comm_timeout_s"].isNull())
    cfg.comm_timeout_s = (uint8_t)constrain(doc["comm_timeout_s"].as<int>(), 5, 120);
  if (!doc["volt_min"].isNull())    cfg.volt_min    = doc["volt_min"].as<float>();
  if (!doc["volt_max"].isNull())    cfg.volt_max    = doc["volt_max"].as<float>();
  if (!doc["current_max"].isNull()) cfg.current_max = doc["current_max"].as<float>();
  if (!doc["pf_min"].isNull())      cfg.pf_min      = doc["pf_min"].as<float>();
  if (!doc["freq_min"].isNull())    cfg.freq_min    = doc["freq_min"].as<float>();
  if (!doc["freq_max"].isNull())    cfg.freq_max    = doc["freq_max"].as<float>();

  if (!doc["sdm_addr"].isNull())
    cfg.sdm_addr = (uint8_t)constrain(doc["sdm_addr"].as<int>(), 1, 247);

  if (!doc["fault_mask"].isNull())
    cfg.fault_mask = (uint16_t)(doc["fault_mask"].as<int>() & 0x03FF);
  if (!doc["fault_repeat_min"].isNull())
    cfg.fault_repeat_min = (uint8_t)constrain(doc["fault_repeat_min"].as<int>(), 0, 60);

  saveConfig(cfg);
  if (wifiChanged)    gWifiReconnect = true;
  if (ntpChanged)     gNtpResync     = true;
  if (cfg.mqtt_en != oldMqttEn || strcmp(cfg.mqtt_host, oldHost) != 0 ||
      cfg.mqtt_port != oldPort  || strcmp(cfg.mqtt_user, oldUser) != 0 ||
      strcmp(cfg.mqtt_topic, oldTopic) != 0 || mqttPassChanged)
    gMqttReconnect = true;

  sendJson(200, "{\"msg\":\"Saved\"}");
}

static void handleGetData() {
  JsonDocument doc;
  doc["dev_name"] = cfg.dev_name;
  doc["has_sec"]  = hasSec;
  doc["has_min"]  = hasMin;
  // Compute totals
  float tw = 0, tk = 0, tv = 0;
  if (hasSec) for (int i = 0; i < 3; i++) tw += latestSec.w[i];
  if (hasMin) for (int i = 0; i < 3; i++) { tk += latestMin.kwh[i]; tv += latestMin.kvarh[i]; }
  doc["energy_since"] = energyStartTs;
  if (hasSec) doc["total_w"]   = tw;
  if (hasMin) { doc["total_kwh"] = tk; doc["total_kvarh"] = tv; }

  time_t now = time(nullptr);
  if (now > 1000000L) {
    char tsBuf[20]; struct tm ti; localtime_r(&now, &ti);
    strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%d %H:%M:%S", &ti);
    doc["ts_str"] = tsBuf;
  } else {
    doc["ts_str"] = "clock not set";
  }

  const char* chNames[] = {"R", "S", "T"};

  if (hasSec) {
    for (int i = 0; i < 3; i++) {
      JsonObject ch = doc["sec"][chNames[i]].to<JsonObject>();
      ch["v"] = latestSec.v[i]; ch["a"] = latestSec.a[i]; ch["w"] = latestSec.w[i];
      ch["var"] = latestSec.var[i]; ch["pf"] = latestSec.pf[i]; ch["hz"] = latestSec.hz[i];
    }
  }

  if (hasMeter) {
    JsonObject mt = doc["meter"].to<JsonObject>();
    mt["v"] = latestMeter.v; mt["a"] = latestMeter.a; mt["w"] = latestMeter.w;
    mt["pf"] = latestMeter.pf; mt["hz"] = latestMeter.hz; mt["kwh"] = latestMeter.kwh;
    // Last deviation snapshot for the live tab (recomputed from latest stored values)
    if (hasSec) {
      auto pct = [](float n, float d) -> float {
        return (fabsf(d) > 1e-6f) ? n / d * 100.0f : 0.0f;
      };
      for (int i = 0; i < 3; i++) {
        float vd = latestSec.v[i] - latestMeter.v;
        float ad = latestSec.a[i] - latestMeter.a;
        float wd = latestSec.w[i] - latestMeter.w;
        float pd = latestSec.pf[i] - latestMeter.pf;
        JsonObject dv = doc["last_dev"][chNames[i]].to<JsonObject>();
        dv["v_abs"]  = vd;   dv["v_pct"]  = pct(vd, latestMeter.v);
        dv["a_abs"]  = ad;   dv["a_pct"]  = pct(ad, latestMeter.a);
        dv["w_abs"]  = wd;   dv["w_pct"]  = pct(wd, latestMeter.w);
        dv["pf_abs"] = pd;   dv["pf_pct"] = pct(pd, latestMeter.pf);
        dv["dkwh_abs"] = 0;  dv["dkwh_pct"] = 0;  // only available in paired min publish
      }
    }
  }

  if (hasMin) {
    for (int i = 0; i < 3; i++) {
      JsonObject ch = doc["min"][chNames[i]].to<JsonObject>();
      ch["kwh"] = latestMin.kwh[i]; ch["kvarh"] = latestMin.kvarh[i];
    }
  }

  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleGetNetinfo() {
  JsonDocument doc;
  doc["ap_ssid"]       = apSSID;
  doc["ap_ip"]         = "192.168.99.1";
  doc["sta_ssid"]      = cfg.ssid;
  doc["sta_ip"]        = cfg.sta_ip;
  doc["sta_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["sta_rssi"]      = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  doc["ntp_synced"]    = ntpSynced;

  // NTP time display fields
  time_t now = time(nullptr);
  char buf[20];
  struct tm ti;

  if (now > 1000000L) {
    localtime_r(&now, &ti);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    doc["local_time_str"] = buf;
  } else {
    doc["local_time_str"] = "not set";
  }

  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleGetSysinfo() {
  JsonDocument doc;
  doc["fw_version"] = FW_VERSION;
  doc["model"] = "LilyGO T7 S3 WROOM-1";

  uint32_t sec = millis() / 1000;
  char buf[24];
  snprintf(buf, sizeof(buf), "%ud %uh %um",
           (unsigned)(sec / 86400),
           (unsigned)((sec % 86400) / 3600),
           (unsigned)((sec % 3600) / 60));
  doc["uptime_str"]     = buf;
  doc["mqtt_connected"] = mqtt.connected();
  doc["bat_pct"]        = gBatPct;    // -1 if no battery ADC
  doc["pwr_ok"]         = gPwrOk;

  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleGetExport() {
  String type = server.hasArg("type") ? server.arg("type") : "sec";
  uint32_t fromTs = server.hasArg("from") ? (uint32_t)server.arg("from").toInt() : 0;
  uint32_t toTs   = server.hasArg("to")   ? (uint32_t)server.arg("to").toInt()   : 0;

  time_t ref = fromTs ? (time_t)fromTs : time(nullptr);
  ref += (time_t)cfg.tz_offset * 3600;
  struct tm tm0; gmtime_r(&ref, &tm0);
  char fname[48];
  snprintf(fname, sizeof(fname), "cal_%s_%04d%02d%02d_%02d%02d.csv",
           type.c_str(), tm0.tm_year + 1900, tm0.tm_mon + 1, tm0.tm_mday,
           tm0.tm_hour, tm0.tm_min);
  char disp[72];
  snprintf(disp, sizeof(disp), "attachment; filename=%s", fname);

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Content-Disposition", disp);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");

  char buf[256];
  if (type == "min") {
    server.sendContent("ts,datetime,kwh_r,kwh_s,kwh_t,kvarh_r,kvarh_s,kvarh_t\r\n");
    int total = (int)minBuf.cnt;
    for (int i = total - 1; i >= 0; i--) {
      MinRecord r;
      if (!minBuf.get((uint32_t)i, r)) continue;
      if (fromTs && r.ts < fromTs) continue;
      if (toTs   && r.ts > toTs)   continue;
      time_t rt = (time_t)r.ts + (time_t)cfg.tz_offset * 3600;
      struct tm rtm; gmtime_r(&rt, &rtm);
      snprintf(buf, sizeof(buf),
               "%lu,%04d-%02d-%02dT%02d:%02d:%02d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\r\n",
               (unsigned long)r.ts,
               rtm.tm_year+1900, rtm.tm_mon+1, rtm.tm_mday,
               rtm.tm_hour, rtm.tm_min, rtm.tm_sec,
               r.kwh[0], r.kwh[1], r.kwh[2],
               r.kvarh[0], r.kvarh[1], r.kvarh[2]);
      server.sendContent(buf);
    }
  } else {
    server.sendContent("ts,datetime,v_r,v_s,v_t,a_r,a_s,a_t,w_r,w_s,w_t,hz_r,hz_s,hz_t\r\n");
    int total = (int)secBuf.cnt;
    for (int i = total - 1; i >= 0; i--) {
      SecRecord r;
      if (!secBuf.get((uint32_t)i, r)) continue;
      if (fromTs && r.ts < fromTs) continue;
      if (toTs   && r.ts > toTs)   continue;
      time_t rt = (time_t)r.ts + (time_t)cfg.tz_offset * 3600;
      struct tm rtm; gmtime_r(&rt, &rtm);
      snprintf(buf, sizeof(buf),
               "%lu,%04d-%02d-%02dT%02d:%02d:%02d,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f\r\n",
               (unsigned long)r.ts,
               rtm.tm_year+1900, rtm.tm_mon+1, rtm.tm_mday,
               rtm.tm_hour, rtm.tm_min, rtm.tm_sec,
               r.v[0], r.v[1], r.v[2],
               r.a[0], r.a[1], r.a[2],
               r.w[0], r.w[1], r.w[2],
               r.hz[0], r.hz[1], r.hz[2]);
      server.sendContent(buf);
    }
  }
  server.sendContent("");
}

static void handleGetHistory() {
  String type = server.hasArg("type") ? server.arg("type") : "sec";
  uint32_t fromTs = server.hasArg("from") ? (uint32_t)server.arg("from").toInt() : 0;
  uint32_t toTs   = server.hasArg("to")   ? (uint32_t)server.arg("to").toInt()   : 0;
  bool filtered = (fromTs > 0 || toTs > 0);
  int n = server.hasArg("n") ? server.arg("n").toInt() : 60;

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  char buf[200];
  if (type == "min") {
    if (filtered) {
      int total = (int)minBuf.cnt;
      int cnt = 0;
      for (int i = total - 1; i >= 0 && cnt < 1440; i--) {
        MinRecord r;
        if (!minBuf.get((uint32_t)i, r)) continue;
        if (fromTs && r.ts < fromTs) continue;
        if (toTs   && r.ts > toTs)   continue;
        cnt++;
      }
      snprintf(buf, sizeof(buf), "{\"type\":\"min\",\"cnt\":%d,\"records\":[", cnt);
      server.sendContent(buf);
      bool first = true;
      int emitted = 0;
      for (int i = total - 1; i >= 0 && emitted < 1440; i--) {
        MinRecord r;
        if (!minBuf.get((uint32_t)i, r)) continue;
        if (fromTs && r.ts < fromTs) continue;
        if (toTs   && r.ts > toTs)   continue;
        if (!first) server.sendContent(",");
        first = false;
        emitted++;
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lu,\"kwh\":[%.2f,%.2f,%.2f],\"kvarh\":[%.2f,%.2f,%.2f]}",
                 (unsigned long)r.ts, r.kwh[0], r.kwh[1], r.kwh[2],
                 r.kvarh[0], r.kvarh[1], r.kvarh[2]);
        server.sendContent(buf);
      }
    } else {
      n = constrain(n, 1, 1440);
      int cnt = min((int)minBuf.cnt, n);
      snprintf(buf, sizeof(buf), "{\"type\":\"min\",\"cnt\":%d,\"records\":[", cnt);
      server.sendContent(buf);
      bool first = true;
      for (int i = cnt - 1; i >= 0; i--) {
        MinRecord r;
        if (!minBuf.get((uint32_t)i, r)) continue;
        if (!first) server.sendContent(",");
        first = false;
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lu,\"kwh\":[%.2f,%.2f,%.2f],\"kvarh\":[%.2f,%.2f,%.2f]}",
                 (unsigned long)r.ts, r.kwh[0], r.kwh[1], r.kwh[2],
                 r.kvarh[0], r.kvarh[1], r.kvarh[2]);
        server.sendContent(buf);
      }
    }
  } else {
    if (filtered) {
      int total = (int)secBuf.cnt;
      int cnt = 0;
      for (int i = total - 1; i >= 0 && cnt < 300; i--) {
        SecRecord r;
        if (!secBuf.get((uint32_t)i, r)) continue;
        if (fromTs && r.ts < fromTs) continue;
        if (toTs   && r.ts > toTs)   continue;
        cnt++;
      }
      snprintf(buf, sizeof(buf), "{\"type\":\"sec\",\"cnt\":%d,\"records\":[", cnt);
      server.sendContent(buf);
      bool first = true;
      int emitted = 0;
      for (int i = total - 1; i >= 0 && emitted < 300; i--) {
        SecRecord r;
        if (!secBuf.get((uint32_t)i, r)) continue;
        if (fromTs && r.ts < fromTs) continue;
        if (toTs   && r.ts > toTs)   continue;
        if (!first) server.sendContent(",");
        first = false;
        emitted++;
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lu,\"v\":[%.1f,%.1f,%.1f],\"a\":[%.3f,%.3f,%.3f],\"w\":[%.1f,%.1f,%.1f],\"hz\":[%.2f,%.2f,%.2f]}",
                 (unsigned long)r.ts,
                 r.v[0], r.v[1], r.v[2], r.a[0], r.a[1], r.a[2],
                 r.w[0], r.w[1], r.w[2], r.hz[0], r.hz[1], r.hz[2]);
        server.sendContent(buf);
      }
    } else {
      n = constrain(n, 1, 300);
      int cnt = min((int)secBuf.cnt, n);
      snprintf(buf, sizeof(buf), "{\"type\":\"sec\",\"cnt\":%d,\"records\":[", cnt);
      server.sendContent(buf);
      bool first = true;
      for (int i = cnt - 1; i >= 0; i--) {
        SecRecord r;
        if (!secBuf.get((uint32_t)i, r)) continue;
        if (!first) server.sendContent(",");
        first = false;
        snprintf(buf, sizeof(buf),
                 "{\"ts\":%lu,\"v\":[%.1f,%.1f,%.1f],\"a\":[%.3f,%.3f,%.3f],\"w\":[%.1f,%.1f,%.1f],\"hz\":[%.2f,%.2f,%.2f]}",
                 (unsigned long)r.ts,
                 r.v[0], r.v[1], r.v[2], r.a[0], r.a[1], r.a[2],
                 r.w[0], r.w[1], r.w[2], r.hz[0], r.hz[1], r.hz[2]);
        server.sendContent(buf);
      }
    }
  }
  server.sendContent("]}");
  server.sendContent("");
}

static void handleGetErrors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!lfsOk || !LittleFS.exists(ERR_LOG_FILE)) {
    server.send(200, "text/plain", "(empty)");
    return;
  }
  File f = LittleFS.open(ERR_LOG_FILE, "r");
  if (!f) { server.send(200, "text/plain", "(cannot open)"); return; }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  uint8_t buf[256];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0)
    server.sendContent((const char*)buf, n);
  server.sendContent("");
  f.close();
}

static void handleClearErrors() {
  errorLogClear();
  sendJson(200, "{\"msg\":\"Cleared\"}");
}

static void handleRestart() {
  sendJson(200, "{\"msg\":\"Restarting\"}");
  delay(300);
  ESP.restart();
}

static void handleResetEnergy() {
  if (!server.hasArg("plain")) { sendJson(400, "{\"err\":\"no body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"err\":\"bad json\"}"); return; }
  const char* mode = doc["mode"] | "";

  // Both modes: zero running totals and reset prevBox baseline
  memset(cumKwh,   0, sizeof(cumKwh));
  memset(cumKvarh, 0, sizeof(cumKvarh));
  prevBoxKwh[0]   = prevBoxKwh[1]   = prevBoxKwh[2]   = -1.0f;
  prevBoxKvarh[0] = prevBoxKvarh[1] = prevBoxKvarh[2] = -1.0f;
  energySavePrevBox(prevBoxKwh, prevBoxKvarh);
  memset(&latestMin, 0, sizeof(latestMin));
  hasMin = false;
  energyStartReset();
  errorLog("INFO", "Energy counters reset");

  if (strcmp(mode, "full") == 0) {
    if (lfsOk) {
      LittleFS.remove(ENERGY_FILE);
      LittleFS.remove(SEC_SNAP_BIN);
      LittleFS.remove(MIN_SNAP_BIN);
    }
    secBuf.clear();
    minBuf.clear();
    errorLog("INFO", "Energy history erased");
    sendJson(200, "{\"msg\":\"Energy history erased\"}");
  } else {
    sendJson(200, "{\"msg\":\"Counters reset\"}");
  }
}

// ── test injection (TEST_MODE only) ──────────────────────────────────────────
#if TEST_MODE
// mqttPublishEvent is defined later in EnergyCalibrator.ino — forward-declare here.
static void mqttPublishEvent(const char*, uint32_t, uint32_t);
static void handlePostTest() {
  if (!server.hasArg("plain")) { sendJson(400, "{\"err\":\"no body\"}"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) { sendJson(400, "{\"err\":\"bad json\"}"); return; }
  const char* action = doc["action"] | "";

  if (strcmp(action, "power_loss") == 0) {
    gPwrOk = false;
    errorLog("WARN", "Power loss — emergency snapshot [TEST]");
    mqttPublishEvent("power_loss", 0, 0);
    snapshotSave(secBuf, minBuf);
    sendJson(200, "{\"msg\":\"power_loss triggered\"}");

  } else if (strcmp(action, "power_restore") == 0) {
    if (!gPwrOk) {
      gPwrOk = true;
      errorLog("INFO", "Power restored [TEST]");
      mqttPublishEvent("power_restored", 0, 0);
    }
    sendJson(200, "{\"msg\":\"power_restore triggered\"}");

  } else if (strcmp(action, "set_bat") == 0) {
    int pct = doc["pct"] | -1;
    if (pct < 0 || pct > 100) { sendJson(400, "{\"err\":\"pct out of range\"}"); return; }
    gBatPct = (int16_t)pct;
    sendJson(200, "{\"msg\":\"bat_pct set\"}");

  } else if (strcmp(action, "snapshot") == 0) {
    snapshotSave(secBuf, minBuf);
    sendJson(200, "{\"msg\":\"snapshot triggered\"}");

  } else if (strcmp(action, "mqtt_disconnect") == 0) {
    mqtt.disconnect();
    sendJson(200, "{\"msg\":\"mqtt disconnected\"}");

  } else if (strcmp(action, "mqtt_gap_start") == 0) {
    // Disconnect and disable auto-reconnect so a gap accumulates without auto-recovery
    mqtt.disconnect();
    cfg.mqtt_en = false;
    sendJson(200, "{\"msg\":\"mqtt gap started\"}");

  } else if (strcmp(action, "mqtt_gap_end") == 0) {
    // Re-enable MQTT; loop() will reconnect and trigger replay within ~10 s
    cfg.mqtt_en = true;
    sendJson(200, "{\"msg\":\"mqtt gap ended\"}");

  } else {
    sendJson(400, "{\"err\":\"unknown action\"}");
  }
}
#endif

// ── OTA update ────────────────────────────────────────────────────────────────

static bool _otaMetaOk  = false;
static char _otaErr[64] = "";

static bool _scanOtaMeta(const uint8_t* buf, size_t len) {
  const uint32_t MAGIC = 0x5A415843UL;
  for (size_t i = 0; i + sizeof(ZaxOtaMeta) <= len; i++) {
    uint32_t m; memcpy(&m, buf + i, 4);
    if (m != MAGIC) continue;
    ZaxOtaMeta meta; memcpy(&meta, buf + i, sizeof(meta));
    if (meta.hw_target != ZAX_META.hw_target) {
      snprintf(_otaErr, sizeof(_otaErr), "hw_target mismatch: %d", meta.hw_target);
      return false;
    }
    if (meta.sec_rec_size != ZAX_META.sec_rec_size) {
      snprintf(_otaErr, sizeof(_otaErr), "sec_rec_size %d!=%d", meta.sec_rec_size, ZAX_META.sec_rec_size);
      return false;
    }
    if (meta.min_rec_size != ZAX_META.min_rec_size) {
      snprintf(_otaErr, sizeof(_otaErr), "min_rec_size %d!=%d", meta.min_rec_size, ZAX_META.min_rec_size);
      return false;
    }
    return true;
  }
  snprintf(_otaErr, sizeof(_otaErr), "ZAX_META magic not found in binary");
  return false;
}

static void handleGetOtaMeta() {
  StaticJsonDocument<128> doc;
  doc["fw_version"]   = ZAX_META.fw_version;
  doc["hw_target"]    = ZAX_META.hw_target;
  doc["data_version"] = ZAX_META.data_version;
  doc["sec_rec_size"] = ZAX_META.sec_rec_size;
  doc["min_rec_size"] = ZAX_META.min_rec_size;
  String out; serializeJson(doc, out);
  sendJson(200, out);
}

static void handleOtaComplete() {
  if (_otaMetaOk && !Update.hasError()) {
    sendJson(200, "{\"msg\":\"OTA OK — rebooting\"}");
    server.client().flush();  // ensure response reaches client before reset
    delay(500);
    ESP.restart();
  } else {
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"err\":\"%s\"}", _otaErr[0] ? _otaErr : "OTA write failed");
    sendJson(400, resp);
  }
}

static void handleOtaUpload() {
  static uint8_t* _buf     = nullptr;
  static size_t   _filled  = 0;
  static bool     _checked = false;

  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    _otaMetaOk = false; _otaErr[0] = '\0';
    _filled = 0; _checked = false;
    _buf = (uint8_t*)ps_malloc(512 * 1024);
    if (!_buf) { strlcpy(_otaErr, "ps_malloc failed", sizeof(_otaErr)); return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      strlcpy(_otaErr, "Update.begin failed", sizeof(_otaErr));
      free(_buf); _buf = nullptr;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (_buf && _filled < 512 * 1024) {
      size_t n = min((size_t)up.currentSize, 512 * 1024 - _filled);
      memcpy(_buf + _filled, up.buf, n);
      _filled += n;
    }
    if (!_checked && _filled >= 512 * 1024) {
      _checked = true;
      _otaMetaOk = _scanOtaMeta(_buf, 512 * 1024);
      free(_buf); _buf = nullptr;
    }
    if (!Update.hasError()) Update.write(up.buf, up.currentSize);
    if (_checked && !_otaMetaOk) Update.abort();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!_checked) {
      _checked = true;
      _otaMetaOk = _buf ? _scanOtaMeta(_buf, _filled) : false;
      if (_buf) { free(_buf); _buf = nullptr; }
    }
    if (_otaMetaOk && !Update.hasError()) Update.end(true);
    else Update.end(false);
  }
}

static void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ── route registration ────────────────────────────────────────────────────────

static void setupWebRoutes() {
  server.on("/",             HTTP_GET,  handleIndex);
  server.on("/api/config",   HTTP_GET,  handleGetConfig);
  server.on("/api/config",   HTTP_POST, handlePostConfig);
  server.on("/api/data",     HTTP_GET,  handleGetData);
  server.on("/api/netinfo",  HTTP_GET,  handleGetNetinfo);
  server.on("/api/sysinfo",  HTTP_GET,  handleGetSysinfo);
  server.on("/api/history",  HTTP_GET,  handleGetHistory);
  server.on("/api/export",   HTTP_GET,  handleGetExport);
  server.on("/api/errors",   HTTP_GET,    handleGetErrors);
  server.on("/api/errors",   HTTP_DELETE, handleClearErrors);
  server.on("/restart",        HTTP_POST,   handleRestart);
  server.on("/api/reset_energy", HTTP_POST, handleResetEnergy);
  server.on("/api/ota_meta",     HTTP_GET,  handleGetOtaMeta);
  server.on("/api/ota",          HTTP_POST, handleOtaComplete, handleOtaUpload);
#if TEST_MODE
  server.on("/api/test",     HTTP_POST,   handlePostTest);
#endif
  server.onNotFound(handleNotFound);
}
