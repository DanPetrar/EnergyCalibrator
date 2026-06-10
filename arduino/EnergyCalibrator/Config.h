// Macro include guard (not #pragma once): this header is reached via two
// different paths — the sketch and the ZaxCommon library headers — and
// #pragma once dedupes by file path, which would let the structs be defined twice.
#ifndef ZAX_CONFIG_H
#define ZAX_CONFIG_H
#include <Arduino.h>
#include <Preferences.h>

// ── data records ──────────────────────────────────────────────────────────────

struct SecRecord {          // 76 bytes — one burst of R/S/T per-second frames
  uint32_t ts;
  float    v[3];
  float    a[3];
  float    w[3];
  float    hz[3];
  int32_t  var[3];
  float    pf[3];
};

struct MinRecord {          // 28 bytes used — per-minute cumulative kWh from box
  uint32_t ts;
  float    kwh[3];
  float    kvarh[3];
  uint8_t  _pad[4];
};

struct MeterRecord {        // SDM630 reading (R phase = reference)
  uint32_t ts;
  float    v;               // V1
  float    a;               // A1
  float    w;               // W1 active power
  float    pf;              // PF1
  float    hz;              // frequency
  float    kwh;             // import kWh cumulative (register 0x0048)
};

// ── device configuration ──────────────────────────────────────────────────────

#define CFG_NVS    "cal"
#define FW_VERSION "1.0.10"
#define DATA_VERSION 1

#define BUF_MODE_LTE 0
#define BUF_MODE_ADF 1

#define HW_TARGET_LILYGO  0
#define HW_TARGET_S3ZERO  1
#if defined(BOARD_S3ZERO)
  #define HW_TARGET HW_TARGET_S3ZERO
#else
  #define HW_TARGET HW_TARGET_LILYGO
#endif

struct ZaxConfig {
  char     dev_name[32];
  char     memo[64];
  char     ssid[64];
  char     pass[64];
  char     ap_pass[32];     // AP mode password (default ZaxEnergy-123)
  char     sta_ip[16];
  char     ntp_srv[64];
  int16_t  tz_offset;
  bool     mqtt_en;
  char     mqtt_host[64];
  uint16_t mqtt_port;
  char     mqtt_user[32];
  char     mqtt_pass[32];
  char     mqtt_topic[32];
  bool     demo_en;
  uint8_t  buf_mode;
  uint8_t  comm_timeout_s;
  float    volt_min;
  float    volt_max;
  float    current_max;
  float    pf_min;
  float    freq_min;
  float    freq_max;
  uint16_t fault_mask;
  uint8_t  fault_repeat_min;
  uint8_t  sdm_addr;        // Modbus address of SDM630 (default 1)
  uint8_t  ch_mask;         // always 0x07 — all 3 CTs active; kept for FaultMonitor compatibility
};

// OTA compatibility descriptor
struct ZaxOtaMeta {
  uint32_t magic;
  char     fw_version[12];
  uint8_t  hw_target;
  uint8_t  data_version;
  uint16_t sec_rec_size;
  uint16_t min_rec_size;
  uint8_t  _pad[10];
};

extern ZaxOtaMeta ZAX_META;

static void cfgDefaults(ZaxConfig& c) {
  strncpy(c.dev_name, "EnergyCalibrator", sizeof(c.dev_name) - 1);
  c.memo[0]      = '\0';
  c.ssid[0]      = '\0';
  c.pass[0]      = '\0';
  strncpy(c.ap_pass, "ZaxEnergy-123", sizeof(c.ap_pass) - 1);
  c.sta_ip[0]    = '\0';
  strncpy(c.ntp_srv, "pool.ntp.org", sizeof(c.ntp_srv) - 1);
  c.tz_offset    = 0;
  c.mqtt_en      = false;
  c.mqtt_host[0] = '\0';
  c.mqtt_port    = 1883;
  c.mqtt_user[0] = '\0';
  c.mqtt_pass[0] = '\0';
  strncpy(c.mqtt_topic, "cal", sizeof(c.mqtt_topic) - 1);
  c.demo_en          = false;
  c.buf_mode         = BUF_MODE_LTE;
  c.comm_timeout_s   = 10;
  c.volt_min         = 180.0f;
  c.volt_max         = 260.0f;
  c.current_max      = 20.0f;
  c.pf_min           = 0.5f;
  c.freq_min         = 49.5f;
  c.freq_max         = 50.5f;
  c.fault_mask       = 0x0101;
  c.fault_repeat_min = 10;
  c.sdm_addr         = 1;
  c.ch_mask          = 0x07;
}

static void loadConfig(ZaxConfig& c) {
  cfgDefaults(c);
  Preferences p;
  if (!p.begin(CFG_NVS, true)) return;

  // Default to the value cfgDefaults() already put in buf, NOT "" — an absent
  // NVS key (first boot, or after the BOOT-button factory reset) must keep the
  // compiled-in default. A "" default here would wipe ntp_srv → NTP never syncs,
  // and mqtt_topic → the "cal"→"cal_<mac>" auto-set guard in wifiSetup() misses.
  auto gs = [&](const char* k, char* buf, size_t sz) {
    String s = p.getString(k, buf);
    strncpy(buf, s.c_str(), sz - 1);
    buf[sz - 1] = '\0';
  };

  gs("dev_name",   c.dev_name,   sizeof(c.dev_name));
  gs("memo",       c.memo,       sizeof(c.memo));
  gs("ssid",       c.ssid,       sizeof(c.ssid));
  gs("pass",       c.pass,       sizeof(c.pass));
  gs("ap_pass",    c.ap_pass,    sizeof(c.ap_pass));
  gs("sta_ip",     c.sta_ip,     sizeof(c.sta_ip));
  gs("ntp_srv",    c.ntp_srv,    sizeof(c.ntp_srv));
  c.tz_offset = (int16_t)p.getInt("tz_offset", 0);
  c.mqtt_en   = p.getBool("mqtt_en", false);
  gs("mqtt_host",  c.mqtt_host,  sizeof(c.mqtt_host));
  c.mqtt_port = (uint16_t)p.getUInt("mqtt_port", 1883);
  gs("mqtt_user",  c.mqtt_user,  sizeof(c.mqtt_user));
  gs("mqtt_pass",  c.mqtt_pass,  sizeof(c.mqtt_pass));
  gs("mqtt_topic", c.mqtt_topic, sizeof(c.mqtt_topic));
  c.demo_en          = p.getBool  ("demo_en",   false);
  c.buf_mode         = (uint8_t)p.getUChar("buf_mode",      BUF_MODE_LTE);
  c.comm_timeout_s   = (uint8_t)p.getUChar("comm_tmo",      10);
  c.volt_min         = p.getFloat ("volt_min",  180.0f);
  c.volt_max         = p.getFloat ("volt_max",  260.0f);
  c.current_max      = p.getFloat ("curr_max",  20.0f);
  c.pf_min           = p.getFloat ("pf_min",    0.5f);
  c.freq_min         = p.getFloat ("freq_min",  49.5f);
  c.freq_max         = p.getFloat ("freq_max",  50.5f);
  c.fault_mask       = (uint16_t)p.getUInt ("fault_mask",    0x0101);
  c.fault_repeat_min = (uint8_t) p.getUChar("fault_rep_min", 10);
  c.sdm_addr         = (uint8_t) p.getUChar("sdm_addr",      1);

  p.end();
  Serial.printf("[CFG] dev=%s ssid=%s tz=%d mqtt=%d sdm_addr=%d\n",
                c.dev_name, c.ssid, c.tz_offset, c.mqtt_en, c.sdm_addr);
}

static void saveConfig(const ZaxConfig& c) {
  Preferences p;
  if (!p.begin(CFG_NVS, false)) { Serial.println("[CFG] NVS open failed"); return; }

  p.putString("dev_name",   c.dev_name);
  p.putString("memo",       c.memo);
  p.putString("ssid",       c.ssid);
  p.putString("pass",       c.pass);
  p.putString("ap_pass",    c.ap_pass);
  p.putString("ntp_srv",    c.ntp_srv);
  p.putInt   ("tz_offset",  c.tz_offset);
  p.putBool  ("mqtt_en",    c.mqtt_en);
  p.putString("mqtt_host",  c.mqtt_host);
  p.putUInt  ("mqtt_port",  c.mqtt_port);
  p.putString("mqtt_user",  c.mqtt_user);
  p.putString("mqtt_pass",  c.mqtt_pass);
  p.putString("mqtt_topic", c.mqtt_topic);
  p.putBool  ("demo_en",    c.demo_en);
  p.putUChar ("buf_mode",   c.buf_mode);
  p.putUChar ("comm_tmo",   c.comm_timeout_s);
  p.putFloat ("volt_min",   c.volt_min);
  p.putFloat ("volt_max",   c.volt_max);
  p.putFloat ("curr_max",   c.current_max);
  p.putFloat ("pf_min",     c.pf_min);
  p.putFloat ("freq_min",   c.freq_min);
  p.putFloat ("freq_max",   c.freq_max);
  p.putUInt  ("fault_mask",    c.fault_mask);
  p.putUChar ("fault_rep_min", c.fault_repeat_min);
  p.putUChar ("sdm_addr",      c.sdm_addr);

  p.end();
  Serial.println("[CFG] Saved.");
}

static void saveStaIp(const char* ip) {
  Preferences p;
  if (!p.begin(CFG_NVS, false)) return;
  p.putString("sta_ip", ip);
  p.end();
}

#endif // ZAX_CONFIG_H
