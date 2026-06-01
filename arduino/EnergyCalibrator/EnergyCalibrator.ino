#define TEST_MODE 0

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ModbusMaster.h>
#include <time.h>
#include "esp_task_wdt.h"
#include "Config.h"
#include <RingBuf.h>
#include <EnergyLog.h>
#include <ErrorLog.h>
#include <FaultMonitor.h>
#include <Snapshot.h>
#include "WebUI.h"

// LilyGO T7 S3 only — EnergyCalibrator runs exclusively on this board
#define BOX_GPIO     5      // UART1 RX — measurement box serial
#define LED_PIN      17
#define PWR_ADC_PIN  2
#define BAT_ADC_PIN  4
#define SDM_RX_PIN   15     // UART2 RX ← RS485 module RX
#define SDM_TX_PIN   16     // UART2 TX → RS485 module TX

#define LED_COUNT     1
#define WDT_TIMEOUT_S 60     // task watchdog budget (loop must reset within this)
#define BOX_BAUD      115200
#define SDM_BAUD      9600
#define BOOT_GRACE_MS 30000UL

#define PWR_LOSS_ADC  1000
#define BAT_ADC_FULL  3588
#define BAT_ADC_EMPTY 2558

#if TEST_MODE
  #define BOX Serial
#else
  #define BOX Serial1
#endif

#define AP_PASS    "CalEnergy-123"
#define AP_IP_1    192
#define AP_IP_2    168
#define AP_IP_3    99
#define AP_IP_4    1

// ── globals ───────────────────────────────────────────────────────────────────
WebServer    server(80);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
ModbusMaster meter;
ZaxConfig    cfg;
SecRecord    latestSec    = {};
MinRecord    latestMin    = {};
MeterRecord  latestMeter  = {};
bool         hasSec       = false;
bool         hasMin       = false;
bool         hasMeter     = false;
bool         ntpSynced    = false;
time_t       lastNtpTs    = 0;
String       apSSID;
bool         gWifiReconnect  = false;
bool         gNtpResync      = false;
bool         gMqttReconnect  = false;
bool         gDemoChanged    = false;
bool         gBufModeChanged = false;
bool         lfsOk           = false;
uint32_t     lastPublishedSecTs = 0;
uint32_t     lastPublishedMinTs = 0;
bool         mqttJustConnected  = false;
int16_t      gBatPct            = -1;
bool         gPwrOk             = true;
FaultState   faults             = {};
bool         gFaultChanged      = false;
bool         bootGraceDone      = false;
uint32_t     lastDataMs         = 0;
uint32_t     lastFaultsMs       = 0;

ZaxOtaMeta ZAX_META = { 0x5A415843UL, FW_VERSION, 0, DATA_VERSION,
                        (uint16_t)sizeof(SecRecord), (uint16_t)sizeof(MinRecord), {} };

// Box energy accumulators (same rollover-safe logic as ZaxMonitor).
//
// Two distinct quantities are tracked per channel — NOT a redundant pair:
//   cumKwh[ch]          — lifetime running total. Each minute parse_min adds
//                         dKwh = kwh - prevBoxKwh (rollback-safe: a box counter
//                         reset is detected as dKwh<0 and handled). cumKwh is
//                         persisted to energyLog, restored on boot, and exposed
//                         as /api/data total_kwh. Monotonic increasing.
//   prevCumKwhAtMin[]   — (loop-static, below) the cumKwh value at the LAST
//                         published row. The per-minute figure sent to MQTT is
//                         dKwhThisMin = cumKwh - prevCumKwhAtMin = energy pending
//                         since the last publish.
// Invariant: prevCumKwhAtMin advances ONLY on a successful paired publish, so a
// skipped minute (failed SDM poll) folds into the next row (F1), and its -1
// sentinel makes the first post-boot publish emit 0 to stay symmetric with the
// SDM side's prevMeterKwh (F2). See Doc/energy-audit.md.
float cumKwh[3]       = {0, 0, 0};
float cumKvarh[3]     = {0, 0, 0};
float prevBoxKwh[3]   = {-1, -1, -1};   // last raw box kwh counter, per channel
float prevBoxKvarh[3] = {-1, -1, -1};
uint32_t energyStartTs = 0;

// SDM630 energy accumulator (for per-minute delta)
float prevMeterKwh = -1.0f;

RingBuf<SecRecord> secBuf;
RingBuf<MinRecord> minBuf;

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

// ── LED ───────────────────────────────────────────────────────────────────────
static void led_set(uint8_t r, uint8_t g, uint8_t b) {
  led.setPixelColor(0, led.Color(r, g, b));
  led.show();
}
static void led_flash(uint8_t r, uint8_t g, uint8_t b) { led_set(r, g, b); delay(80); }

static void ledIdle() {
  bool alert = faults.commLost;
  for (int i = 0; i < 3; i++) {
    if (faults.voltState[i] == 1 || faults.voltState[i] == 3) alert = true;
    if (faults.currOver[i]) alert = true;
  }
  led_set(alert ? 32 : 0, 0, alert ? 0 : 8);
}

// ── SDM630 Modbus reader ──────────────────────────────────────────────────────
static inline float mb_f32(uint16_t hi, uint16_t lo) {
  uint32_t raw = ((uint32_t)hi << 16) | lo;
  float v; memcpy(&v, &raw, 4); return v;
}

bool sdm630Poll() {
  // Read 1: registers 0x0000..0x001F (32 regs = V1-V3, A1-A3, W1-W3, VA1-VA3, VAr1-VAr3, PF1-PF3)
  uint8_t r1 = meter.readInputRegisters(0x0000, 32);
  if (r1 != meter.ku8MBSuccess) {
    char msg[48]; snprintf(msg, sizeof(msg), "SDM630 read1 err=%d", r1);
    errorLog("WARN", msg);
    return false;
  }
  latestMeter.v  = mb_f32(meter.getResponseBuffer(0),  meter.getResponseBuffer(1));   // V1  @ 0x0000
  latestMeter.a  = mb_f32(meter.getResponseBuffer(6),  meter.getResponseBuffer(7));   // A1  @ 0x0006
  latestMeter.w  = mb_f32(meter.getResponseBuffer(12), meter.getResponseBuffer(13));  // W1  @ 0x000C
  latestMeter.pf = mb_f32(meter.getResponseBuffer(30), meter.getResponseBuffer(31));  // PF1 @ 0x001E

  delay(100);
  // Read 2: registers 0x0046..0x0049 (4 regs = Hz, kWh_import)
  uint8_t r2 = meter.readInputRegisters(0x0046, 4);
  if (r2 != meter.ku8MBSuccess) {
    char msg[48]; snprintf(msg, sizeof(msg), "SDM630 read2 err=%d", r2);
    errorLog("WARN", msg);
    return false;
  }
  latestMeter.hz  = mb_f32(meter.getResponseBuffer(0), meter.getResponseBuffer(1));  // Hz         @ 0x0046
  latestMeter.kwh = mb_f32(meter.getResponseBuffer(2), meter.getResponseBuffer(3));  // kWh_import @ 0x0048

  latestMeter.ts = (uint32_t)time(nullptr);
  hasMeter = true;
  Serial.printf("[SDM] V=%.2f A=%.3f W=%.1f PF=%.3f Hz=%.2f kWh=%.3f\n",
                latestMeter.v, latestMeter.a, latestMeter.w,
                latestMeter.pf, latestMeter.hz, latestMeter.kwh);
  return true;
}

// ── line reader (box serial) ───────────────────────────────────────────────────
static char    linebuf[128];
static uint8_t linelen = 0;

static bool read_line() {
  while (BOX.available()) {
    char c = BOX.read();
    if (c == '\n') {
      linebuf[linelen] = '\0';
      if (linelen > 0 && linebuf[linelen - 1] == '\r') linebuf[--linelen] = '\0';
      uint8_t len = linelen; linelen = 0;
      if (len > 0) return true;
    } else if (c != '\r' && linelen < sizeof(linebuf) - 1) {
      linebuf[linelen++] = c;
    }
  }
  return false;
}

#define FT_SEC     0
#define FT_MIN     1
#define FT_UNKNOWN 2

static int classify(const char* line) {
  if (!line[0] || line[1] != ':') return FT_UNKNOWN;
  char ch = line[0];
  if (ch == 'R' || ch == 'S' || ch == 'T') return FT_SEC;
  if (ch == 'U' || ch == 'V' || ch == 'W') return FT_MIN;
  return FT_UNKNOWN;
}

static void parse_sec(const char* line, int ch) {
  float v, a, pf, hz;
  int w_int, var_v, va_skip;
  if (sscanf(line + 2, "%f,%f,%f,%d,%d,%d,%f", &v, &a, &pf, &w_int, &var_v, &va_skip, &hz) != 7) {
    if (bootGraceDone) { char msg[48]; snprintf(msg, sizeof(msg), "Bad frame: ch=%c", line[0]); errorLog("WARN", msg); }
    return;
  }
  if (v < 0 || v > 999 || a < 0 || a > 5000 ||
      w_int < -1500000 || w_int > 1500000 || var_v < -1500000 || var_v > 1500000 ||
      pf < -1.0f || pf > 1.0f || hz < 0 || hz > 99) {
    if (bootGraceDone) { char msg[48]; snprintf(msg, sizeof(msg), "Bad frame: ch=%c OOR", line[0]); errorLog("WARN", msg); }
    return;
  }
  latestSec.v[ch]   = v;
  latestSec.a[ch]   = a;
  latestSec.w[ch]   = (float)w_int;
  latestSec.var[ch] = (int32_t)var_v;
  latestSec.pf[ch]  = pf;
  latestSec.hz[ch]  = hz;
  if (ch == 2) {
    latestSec.ts = (uint32_t)time(nullptr);
    hasSec = true;
    if (bootGraceDone) faultCheckSec(latestSec, cfg);
  }
}

static void parse_min(const char* line, int ch) {
  float kwh, kvarh;
  if (sscanf(line + 2, "%f,%f", &kwh, &kvarh) < 2) return;
  if (kwh < 0 || kvarh < 0) return;

  if (prevBoxKwh[ch] >= 0) {
    float dKwh   = kwh   - prevBoxKwh[ch];
    float dKvarh = kvarh - prevBoxKvarh[ch];
    if (dKwh   < 0) { if (bootGraceDone) { faultOnKwhRollback(ch, cfg); energyStartReset(); } dKwh = kwh; }
    if (dKvarh < 0) dKvarh = kvarh;
    cumKwh[ch]   += dKwh;
    cumKvarh[ch] += dKvarh;
  } else {
    cumKwh[ch]   += kwh;
    cumKvarh[ch] += kvarh;
  }
  prevBoxKwh[ch]   = kwh;
  prevBoxKvarh[ch] = kvarh;
  latestMin.kwh[ch]   = cumKwh[ch];
  latestMin.kvarh[ch] = cumKvarh[ch];

  if (ch == 2) {
    latestMin.ts = (uint32_t)time(nullptr);
    hasMin = true;
    energySavePrevBox(prevBoxKwh, prevBoxKvarh);
  }
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
static void mqttConnect() {
  if (!cfg.mqtt_en || strlen(cfg.mqtt_host) == 0) return;
  if (WiFi.getMode() == WIFI_OFF) return;
  if (mqtt.connected()) return;

  mqtt.setServer(cfg.mqtt_host, cfg.mqtt_port);
  mqtt.setBufferSize(1024);

  String clientId = String("CalEnergy-") + apSSID.substring(10);
  bool ok = (strlen(cfg.mqtt_user) > 0)
    ? mqtt.connect(clientId.c_str(), cfg.mqtt_user, cfg.mqtt_pass)
    : mqtt.connect(clientId.c_str());

  if (ok) {
    Serial.printf("[MQTT] Connected to %s:%d\n", cfg.mqtt_host, cfg.mqtt_port);
    mqttJustConnected = true;
  } else {
    Serial.printf("[MQTT] Connect failed, state=%d\n", mqtt.state());
    char emsg[80];
    snprintf(emsg, sizeof(emsg), "MQTT connect failed state=%d host=%.40s", mqtt.state(), cfg.mqtt_host);
    errorLog("WARN", emsg);
  }
}

static void mqttPublishSec() {
  if (!cfg.mqtt_en || !mqtt.connected()) return;
  String topic = String(cfg.mqtt_topic) + "/sec";
  if (mqtt.publish(topic.c_str(), (const uint8_t*)&latestSec, sizeof(SecRecord)))
    lastPublishedSecTs = latestSec.ts;
}

// Publish paired record (box min + SDM630) as JSON to <topic>/min
static void mqttPublishPaired(float dKwh[3]) {
  if (!cfg.mqtt_en || !mqtt.connected()) return;

  float dMeterKwh = 0.0f;
  if (prevMeterKwh >= 0.0f) {
    dMeterKwh = latestMeter.kwh - prevMeterKwh;
    if (dMeterKwh < 0.0f) dMeterKwh = 0.0f;  // meter reset
  }
  prevMeterKwh = latestMeter.kwh;

  auto r2 = [](float v) { return roundf(v * 100.0f) / 100.0f; };
  auto r3 = [](float v) { return roundf(v * 1000.0f) / 1000.0f; };
  auto r4 = [](float v) { return roundf(v * 10000.0f) / 10000.0f; };
  auto pct = [](float num, float den) -> float {
    return (fabsf(den) > 1e-6f) ? roundf(num / den * 10000.0f) / 100.0f : 0.0f;
  };

  StaticJsonDocument<1024> doc;
  doc["ts"] = latestMeter.ts;

  const char* ch[3] = {"R", "S", "T"};

  JsonObject bs = doc.createNestedObject("box_sec");
  JsonObject bm = doc.createNestedObject("box_min");
  JsonObject mt = doc.createNestedObject("meter");
  JsonObject dv = doc.createNestedObject("dev");

  for (int i = 0; i < 3; i++) {
    JsonObject bsi = bs.createNestedObject(ch[i]);
    bsi["v"]  = r2(latestSec.v[i]);
    bsi["a"]  = r3(latestSec.a[i]);
    bsi["w"]  = r2(latestSec.w[i]);
    bsi["pf"] = r3(latestSec.pf[i]);
    bsi["hz"] = r2(latestSec.hz[i]);

    JsonObject bmi = bm.createNestedObject(ch[i]);
    bmi["dkwh"] = r4(dKwh[i]);

    float vd  = latestSec.v[i]  - latestMeter.v;
    float ad  = latestSec.a[i]  - latestMeter.a;
    float wd  = latestSec.w[i]  - latestMeter.w;
    float pfd = latestSec.pf[i] - latestMeter.pf;
    float ed  = dKwh[i]         - dMeterKwh;

    JsonObject dvi = dv.createNestedObject(ch[i]);
    dvi["v_abs"]      = r3(vd);
    dvi["v_pct"]      = pct(vd,  latestMeter.v);
    dvi["a_abs"]      = r4(ad);
    dvi["a_pct"]      = pct(ad,  latestMeter.a);
    dvi["w_abs"]      = r2(wd);
    dvi["w_pct"]      = pct(wd,  latestMeter.w);
    dvi["pf_abs"]     = r4(pfd);
    dvi["pf_pct"]     = pct(pfd, latestMeter.pf);
    dvi["dkwh_abs"]   = r4(ed);
    dvi["dkwh_pct"]   = pct(ed,  dMeterKwh);
  }

  mt["v"]    = r2(latestMeter.v);
  mt["a"]    = r3(latestMeter.a);
  mt["w"]    = r2(latestMeter.w);
  mt["pf"]   = r3(latestMeter.pf);
  mt["hz"]   = r2(latestMeter.hz);
  mt["dkwh"] = r4(dMeterKwh);

  char buf[1024];
  serializeJson(doc, buf, sizeof(buf));
  String topic = String(cfg.mqtt_topic) + "/min";
  if (mqtt.publish(topic.c_str(), (const uint8_t*)buf, strlen(buf)))
    lastPublishedMinTs = latestMin.ts;
}

static void mqttPublishEvent(const char* event, uint32_t secN, uint32_t minN, bool retain = false) {
  if (!cfg.mqtt_en || !mqtt.connected()) return;
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"event\":\"%s\",\"sec\":%u,\"min\":%u,\"ts\":%lu}",
           event, (unsigned)secN, (unsigned)minN, (unsigned long)time(nullptr));
  String topic = String(cfg.mqtt_topic) + "/event";
  mqtt.publish(topic.c_str(), (const uint8_t*)buf, strlen(buf), retain);
}

void mqttFaultEvent(const char* level, const char* code, int ch_idx, const char* msg) {
  if (!cfg.mqtt_en || !mqtt.connected()) return;
  char buf[160];
  if (ch_idx >= 0) {
    snprintf(buf, sizeof(buf),
      "{\"level\":\"%s\",\"fault\":\"%s\",\"ch\":\"%s\",\"msg\":\"%s\",\"ts\":%lu}",
      level, code, FAULT_CH[ch_idx], msg, (unsigned long)time(nullptr));
  } else {
    snprintf(buf, sizeof(buf),
      "{\"level\":\"%s\",\"fault\":\"%s\",\"msg\":\"%s\",\"ts\":%lu}",
      level, code, msg, (unsigned long)time(nullptr));
  }
  String topic = String(cfg.mqtt_topic) + "/fault";
  mqtt.publish(topic.c_str(), (const uint8_t*)buf, strlen(buf));
}

static void mqttPublishFaults() {
  if (!cfg.mqtt_en || !mqtt.connected()) return;
  int active = (int)(faults.commLost && faultEnabled(BIT_COMM_LOST, cfg));
  for (int i = 0; i < 3; i++) {
    active += (int)((faults.voltState[i] == 1 && faultEnabled(BIT_VOLT_ZERO,  cfg)) ||
                    (faults.voltState[i] == 2 && faultEnabled(BIT_VOLT_UNDER, cfg)) ||
                    (faults.voltState[i] == 3 && faultEnabled(BIT_VOLT_OVER,  cfg)));
    active += (int)(faults.currOver[i] && faultEnabled(BIT_CURR_OVER, cfg));
    active += (int)(faults.currZero[i] && faultEnabled(BIT_CURR_ZERO, cfg));
    active += (int)(faults.pfLow[i]    && faultEnabled(BIT_PF_LOW,    cfg));
  }
  active += (int)(faults.freqFault && faultEnabled(BIT_FREQ, cfg));

  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"ts\":%lu,\"comm_lost\":%s,"
    "\"volt\":[%d,%d,%d],"
    "\"curr_over\":[%s,%s,%s],"
    "\"curr_zero\":[%s,%s,%s],"
    "\"pf_low\":[%s,%s,%s],"
    "\"freq\":%s,\"active\":%d}",
    (unsigned long)time(nullptr),
    faults.commLost ? "true" : "false",
    faults.voltState[0], faults.voltState[1], faults.voltState[2],
    faults.currOver[0]?"true":"false", faults.currOver[1]?"true":"false", faults.currOver[2]?"true":"false",
    faults.currZero[0]?"true":"false", faults.currZero[1]?"true":"false", faults.currZero[2]?"true":"false",
    faults.pfLow[0]?"true":"false",    faults.pfLow[1]?"true":"false",    faults.pfLow[2]?"true":"false",
    faults.freqFault ? "true" : "false", active);

  String topic = String(cfg.mqtt_topic) + "/faults";
  mqtt.publish(topic.c_str(), (const uint8_t*)buf, strlen(buf), true);
  lastFaultsMs  = millis();
  gFaultChanged = false;
}

static void mqttReplay() {
  if (!cfg.mqtt_en || !mqtt.connected()) return;
  if (lastPublishedSecTs == 0 && lastPublishedMinTs == 0) return;

  String secTopic = String(cfg.mqtt_topic) + "/sec";
  uint32_t secGap = 0;
  for (uint32_t age = secBuf.cnt; age-- > 0; ) {
    SecRecord r; if (!secBuf.get(age, r)) continue;
    if (r.ts > lastPublishedSecTs) secGap++;
  }
  if (secGap == 0) return;

  Serial.printf("[REPLAY] gap sec=%u\n", secGap);
  mqttPublishEvent("replay_start", secGap, 0);

  uint32_t secSent = 0;
  for (uint32_t age = secBuf.cnt; age-- > 0; ) {
    SecRecord r;
    if (!secBuf.get(age, r) || r.ts <= lastPublishedSecTs) continue;
    if (mqtt.publish(secTopic.c_str(), (const uint8_t*)&r, sizeof(SecRecord))) {
      lastPublishedSecTs = r.ts;
      secSent++;
      if (secSent % 50 == 0) mqtt.loop();
    }
  }
  mqttPublishEvent("replay_done", secSent, 0, true);
  Serial.printf("[REPLAY] sent sec=%u\n", secSent);
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  delay(100);
  String mac = WiFi.macAddress();
  String tail = mac.substring(9);
  tail.replace(":", "");
  tail.toUpperCase();
  apSSID = String("CalEnergy-") + tail;

  // Auto-set mqtt_topic from MAC if still factory default
  if (strcmp(cfg.mqtt_topic, "cal") == 0) {
    snprintf(cfg.mqtt_topic, sizeof(cfg.mqtt_topic), "cal_%s", tail.c_str());
    Preferences p;
    if (p.begin(CFG_NVS, false)) p.putString("mqtt_topic", cfg.mqtt_topic);
    Serial.printf("[CFG] mqtt_topic auto-set to %s\n", cfg.mqtt_topic);
  }

  IPAddress apIP(AP_IP_1, AP_IP_2, AP_IP_3, AP_IP_4);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSSID.c_str(), AP_PASS);
  Serial.printf("[WIFI] AP: %s  IP: %s\n", apSSID.c_str(), apIP.toString().c_str());

  if (strlen(cfg.ssid) > 0) {
    Serial.printf("[WIFI] Connecting to %s ...\n", cfg.ssid);
    WiFi.begin(cfg.ssid, cfg.pass);
    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      strncpy(cfg.sta_ip, ip.c_str(), sizeof(cfg.sta_ip) - 1);
      saveStaIp(cfg.sta_ip);
      Serial.printf("[WIFI] STA connected  IP: %s\n", ip.c_str());
    } else {
      Serial.println("[WIFI] STA failed — AP only.");
    }
  }
}

static void wifiReconnect() {
  if (strlen(cfg.ssid) == 0) { WiFi.disconnect(); return; }
  WiFi.disconnect(); delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("[WIFI] Reconnecting to %s ...\n", cfg.ssid);
}

// ── NTP ───────────────────────────────────────────────────────────────────────
static void ntpSetup() {
  errorLogSetTz(cfg.tz_offset);
  if (WiFi.status() != WL_CONNECTED) return;
  configTime((long)cfg.tz_offset * 3600L, 0, cfg.ntp_srv);
  Serial.printf("[NTP] Configured: %s  tz=%+d\n", cfg.ntp_srv, cfg.tz_offset);
}

static void ntpCheck() {
  struct tm ti;
  bool synced = getLocalTime(&ti, 0);
  if (synced) lastNtpTs = time(nullptr);
  ntpSynced = synced;
}

// ── setup / loop ──────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);

  led.begin();
  led.setBrightness(48);
  ledIdle();

  loadConfig(cfg);
  errorLogSetTz(cfg.tz_offset);

#if TEST_MODE
  Serial.println("[EnergyCalibrator] TEST_MODE — box input from USB CDC");
#else
  Serial1.setRxBufferSize(1024);
  Serial1.begin(BOX_BAUD, SERIAL_8N1, BOX_GPIO, -1);
  Serial.printf("[EnergyCalibrator] PRODUCTION — box Serial1 GPIO%d\n", BOX_GPIO);
#endif

  // SDM630 Modbus on UART2
  Serial2.begin(SDM_BAUD, SERIAL_8N1, SDM_RX_PIN, SDM_TX_PIN);
  meter.begin(cfg.sdm_addr, Serial2);
  Serial.printf("[SDM] Modbus on UART2 RX=%d TX=%d addr=%d baud=%d\n",
                SDM_RX_PIN, SDM_TX_PIN, cfg.sdm_addr, SDM_BAUD);

  bool secAllocOk, minAllocOk;
  {
    uint32_t secCap = (cfg.buf_mode == BUF_MODE_ADF) ? SEC_CAP_ADF : SEC_CAP_LTE;
    uint32_t minCap = (cfg.buf_mode == BUF_MODE_ADF) ? MIN_CAP_ADF : MIN_CAP_LTE;
    secAllocOk = secBuf.init(secCap);
    minAllocOk = minBuf.init(minCap);
    if (!secAllocOk) Serial.println("[BUF] SecBuf alloc failed");
    else Serial.printf("[BUF] SecBuf: cap=%u  %.1f KB\n", secCap, secCap * sizeof(SecRecord) / 1024.0f);
    if (!minAllocOk) Serial.println("[BUF] MinBuf alloc failed");
    else Serial.printf("[BUF] MinBuf: cap=%u  %.1f KB\n", minCap, minCap * sizeof(MinRecord) / 1024.0f);
  }

  if (PWR_ADC_PIN >= 0) analogSetPinAttenuation(PWR_ADC_PIN, ADC_11db);
  if (BAT_ADC_PIN >= 0) analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

  lfsOk = energyLogInit();
  errorLog("INFO", "Boot");
  if (!secAllocOk) errorLog("ERROR", "SecBuf PSRAM alloc failed");
  if (!minAllocOk) errorLog("ERROR", "MinBuf PSRAM alloc failed");

  snapshotLoad(secBuf, minBuf);

  MinRecord lastRec = {};
  if (energyLoadLast(lastRec)) {
    for (int i = 0; i < 3; i++) {
      cumKwh[i]   = lastRec.kwh[i];
      cumKvarh[i] = lastRec.kvarh[i];
    }
    Serial.printf("[ENERGY] Restored cum: R=%.2f S=%.2f T=%.2f kWh\n",
                  cumKwh[0], cumKwh[1], cumKwh[2]);
  }
  if (energyLoadPrevBox(prevBoxKwh, prevBoxKvarh)) {
    Serial.printf("[ENERGY] Restored prevBox: R=%.2f S=%.2f T=%.2f kWh\n",
                  prevBoxKwh[0], prevBoxKwh[1], prevBoxKwh[2]);
  }
  { Preferences p; if (p.begin(CFG_NVS, true)) energyStartTs = p.getUInt("nrg_start", 0); }

  lastDataMs = millis();
  wifiSetup();
  ntpSetup();
  setupWebRoutes();
  server.begin();
  mqttConnect();

  // Task watchdog: arm after the blocking WiFi/MQTT init so those can't trip it.
  // OTA upload feeds it per-chunk (see handleOtaUpload) since loop() is stalled then.
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms     = WDT_TIMEOUT_S * 1000;
  wdtCfg.idle_core_mask = 0;          // watch the loop task explicitly, not idle tasks
  wdtCfg.trigger_panic  = true;
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
  esp_task_wdt_add(NULL);             // subscribe the loop/setup task
  esp_task_wdt_reset();
  Serial.printf("[WDT] Task watchdog armed: %ds\n", WDT_TIMEOUT_S);

  Serial.printf("[BOOT] AP=%s  STA=%s  PASS=%s\n", apSSID.c_str(), cfg.sta_ip, AP_PASS);
  ledIdle();
}

void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  if (gWifiReconnect)  { gWifiReconnect = false;  wifiReconnect(); ntpSetup(); }
  if (gNtpResync)      { gNtpResync     = false;  ntpSetup(); }
  if (gMqttReconnect)  { gMqttReconnect = false;  mqtt.disconnect(); mqttConnect(); }
  if (gDemoChanged)    { gDemoChanged   = false; }
  if (gBufModeChanged) {
    gBufModeChanged = false;
    uint32_t secCap = (cfg.buf_mode == BUF_MODE_ADF) ? SEC_CAP_ADF : SEC_CAP_LTE;
    uint32_t minCap = (cfg.buf_mode == BUF_MODE_ADF) ? MIN_CAP_ADF : MIN_CAP_LTE;
    secBuf.reinit(secCap); minBuf.reinit(minCap);
  }

  static uint32_t lastIpMs = 0;
  if (millis() - lastIpMs > 5000) {
    lastIpMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      if (ip != String(cfg.sta_ip)) {
        strncpy(cfg.sta_ip, ip.c_str(), sizeof(cfg.sta_ip) - 1);
        saveStaIp(cfg.sta_ip);
        ntpSetup();
      }
    }
  }

  static uint32_t lastNtpMs = 0;
  if (millis() - lastNtpMs > 30000) { lastNtpMs = millis(); ntpCheck(); }

  if (energyStartTs == 0 && time(nullptr) > 1000000L) {
    energyStartTs = (uint32_t)time(nullptr);
    nvsEnergyStartSave();
  }

  if (cfg.mqtt_en) {
    if (!mqtt.connected()) {
      static uint32_t lastMqttRetry = 0;
      if (millis() - lastMqttRetry > 10000) { lastMqttRetry = millis(); mqttConnect(); }
    } else {
      mqtt.loop();
    }
  }

  if (!bootGraceDone && millis() >= BOOT_GRACE_MS) {
    bootGraceDone = true;
    lastDataMs = millis();
    Serial.println("[BOOT] Grace period ended — fault monitoring active");
    if (mqtt.connected()) mqttPublishFaults();
  }

  if (mqttJustConnected) {
    mqttJustConnected = false;
    mqttPublishEvent("online", 0, 0);
    if (bootGraceDone) mqttPublishFaults();
    mqttReplay();
  }

  if (bootGraceDone && (gFaultChanged || millis() - lastFaultsMs >= 30000UL)) {
    mqttPublishFaults();
    ledIdle();
  }

  static uint32_t lastErrIdleMs = 0;
  if (millis() - lastErrIdleMs >= 300000UL) { lastErrIdleMs = millis(); errorLogIdle(); }

  static uint32_t lastSnapMs = 0;
  if (millis() - lastSnapMs >= SNAP_INTERVAL_S * 1000UL) { lastSnapMs = millis(); snapshotSave(secBuf, minBuf); }

  if (PWR_ADC_PIN >= 0) {
    static uint32_t lastPwrMs = 0; static uint8_t pwrLowCnt = 0; static bool pwrLostFired = false;
    if (millis() - lastPwrMs >= 100) {
      lastPwrMs = millis();
      bool ok = (analogRead(PWR_ADC_PIN) >= PWR_LOSS_ADC);
      if (!ok) {
        if (++pwrLowCnt >= 3 && !pwrLostFired) {
          pwrLostFired = true; gPwrOk = false;
          errorLog("WARN", "Power loss — emergency snapshot");
          mqttPublishEvent("power_loss", 0, 0);
          snapshotSave(secBuf, minBuf);
        }
      } else {
        if (!gPwrOk) { gPwrOk = true; errorLog("INFO", "Power restored"); mqttPublishEvent("power_restored", 0, 0); }
        pwrLowCnt = 0; pwrLostFired = false;
      }
    }
  }

  if (BAT_ADC_PIN >= 0) {
    static uint32_t lastBatMs = 0;
    if (millis() - lastBatMs >= 10000) {
      lastBatMs = millis();
      gBatPct = (int16_t)constrain(
        (analogRead(BAT_ADC_PIN) - BAT_ADC_EMPTY) * 100 / (BAT_ADC_FULL - BAT_ADC_EMPTY), 0, 100);
    }
  }

  if (bootGraceDone) { faultCheckComm(lastDataMs, cfg); faultRepeatCheck(cfg); }

  if (!read_line()) return;
  lastDataMs = millis();

  static bool chanSeen[3] = {false, false, false};
  static float dKwhThisMin[3] = {0, 0, 0};        // per-publish box delta sent to MQTT
  static float prevCumKwhAtMin[3] = {-1, -1, -1}; // cumKwh at last publish (-1 = not yet baselined). See globals.

  int ft = classify(linebuf);
  switch (ft) {
    case FT_SEC: {
      int ch = (linebuf[0] == 'R') ? 0 : (linebuf[0] == 'S') ? 1 : 2;
      chanSeen[ch] = true;
      parse_sec(linebuf, ch);
      Serial.printf("[SEC] %s\n", linebuf);
      if (ch == 2) {
        if (bootGraceDone) faultCheckCycle(chanSeen, cfg);
        secBuf.push(latestSec);
        mqttPublishSec();
      }
      led_flash(0, 32, 0);
      break;
    }
    case FT_MIN: {
      int ch = (linebuf[0] == 'U') ? 0 : (linebuf[0] == 'V') ? 1 : 2;
      parse_min(linebuf, ch);
      Serial.printf("[MIN] %s\n", linebuf);
      if (ch == 2) {
        minBuf.push(latestMin);
        energyLogAppend(latestMin);

        // Poll SDM630 immediately after box minute frame
        bool meterOk = sdm630Poll();
        led_flash(32, 24, 0);  // amber

        if (meterOk) {
          // Compute per-minute box deltas only on a successful poll, and advance
          // the baseline only here. On a failed poll the baseline is left
          // unadvanced so this minute's box energy folds into the next published
          // row — symmetric with the SDM side (prevMeterKwh also only advances on
          // success), keeping box and meter dkWh over the same interval (fixes F1).
          for (int i = 0; i < 3; i++) {
            dKwhThisMin[i] = (prevCumKwhAtMin[i] >= 0.0f)
                           ? cumKwh[i] - prevCumKwhAtMin[i]
                           : 0.0f;
            if (dKwhThisMin[i] < 0.0f) dKwhThisMin[i] = 0.0f;
            prevCumKwhAtMin[i] = cumKwh[i];
          }
          mqttPublishPaired(dKwhThisMin);
          led_flash(0, 16, 32);  // teal = paired publish
        } else {
          // Meter poll failed: emit a box-only marker (collector skips it) and
          // leave prevCumKwhAtMin unadvanced so this minute merges into the next.
          if (cfg.mqtt_en && mqtt.connected()) {
            String topic = String(cfg.mqtt_topic) + "/min";
            char buf[32];
            snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"meter\":null}", (unsigned long)latestMin.ts);
            mqtt.publish(topic.c_str(), (const uint8_t*)buf, strlen(buf));
          }
        }
      }
      break;
    }
    default:
      Serial.printf("[???] %s\n", linebuf);
      led_flash(32, 12, 0);
      break;
  }
  ledIdle();
}
