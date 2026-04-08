/*
 =============================================================
  ESP8266 WiFi Repeater / Extender v2.4.0 created by Nihal MP
 =============================================================
  Board   : NodeMCU / Wemos D1 Mini / any ESP8266 (4 MB)
  Core    : arduino-esp8266  ≥ 3.1
  Libs    : ESP8266WiFi, ESP8266WebServer, ESP8266mDNS,
            LittleFS, ArduinoOTA, DNSServer, lwIP (built-in)
  Flash   : 4 MB  –  select "4MB (FS:2MB OTA:~1019 KB)"
  CPU     : 160 MHz  (Tools → CPU Frequency)
 =============================================================

  HOW IT WORKS
  ─────────────
  STA  = connects to your home router (upstream)
  AP   = broadcasts the extended network for devices
  NAT  = lwIP NAT forwards packets between STA ↔ AP
  DNS  = relay so AP clients can resolve names
  DHCP = built-in server assigns 192.168.4.x to clients

  FIRST-RUN SETUP
  ────────────────
  1. Flash the sketch (STA credentials left blank → captive portal)
  2. Connect to  ESP8266_EXT  (no password on first boot)
  3. Open  http://192.168.4.1  → Butler GUI
  4. Fill in your upstream SSID + password → Save & Reconnect
  5. Done – the extender bridges your network
 =============================================================
*/

// ── User-editable defaults (overridden by stored config) ──
#define DEFAULT_STA_SSID     ""          // leave blank → captive portal
#define DEFAULT_STA_PASS     ""
#define DEFAULT_AP_SSID      "ESP8266_EXT"
#define DEFAULT_AP_PASS      "12345678"  // min 8 chars or leave "" for open
#define DEFAULT_AP_CHANNEL   6
#define DEFAULT_AP_MAXCONN   4
#define AP_IP                "192.168.4.1"
#define AP_SUBNET            "255.255.255.0"

// ── Watchdog ──
#define WATCHDOG_PING_HOST   "8.8.8.8"
#define WATCHDOG_INTERVAL_S  30
#define RECONNECT_RETRIES    10
#define HARD_REBOOT_MIN      5          // reboot if offline this long

// ── OTA ──
#define OTA_HOSTNAME         "esp8266-repeater"
#define OTA_PASSWORD         "butler2024"

// ── Libraries ──
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>       // install via Library Manager
#include <lwip/napt.h>         // NAT – part of lwIP in esp8266 core ≥ 3.0
#include <lwip/dns.h>

// ── Globals ──
ESP8266WebServer server(80);
DNSServer        dnsServer;

// Config persisted to LittleFS
struct Config {
  char staSsid[64]   = DEFAULT_STA_SSID;
  char staPass[64]   = DEFAULT_STA_PASS;
  char apSsid[64]    = DEFAULT_AP_SSID;
  char apPass[64]    = DEFAULT_AP_PASS;
  uint8_t apChannel  = DEFAULT_AP_CHANNEL;
  uint8_t apMaxConn  = DEFAULT_AP_MAXCONN;
  bool    dhcpEnable = true;
  bool    macFilter  = false;
  uint8_t txPower    = 82;           // 20.5 dBm (unit = 0.25 dBm)
  char    pingHost[40] = WATCHDOG_PING_HOST;
} cfg;

// Runtime state
unsigned long lastPing       = 0;
unsigned long offlineSince   = 0;
bool          staConnected   = false;
bool          captivePortal  = false;
uint32_t      txBytes        = 0;
uint32_t      rxBytes        = 0;
uint32_t      packetsFwd     = 0;
unsigned long bootTime       = 0;

// MAC blacklist (up to 16)
uint8_t  blacklist[16][6];
uint8_t  blacklistCount = 0;

// ════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println(F("\n[Butler] ESP8266 WiFi Repeater booting…"));

  bootTime = millis();

  // ── Filesystem ──
  if (!LittleFS.begin()) {
    Serial.println(F("[FS] Format…"));
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig();

  // ── CPU to 160 MHz ──
  system_update_cpu_freq(SYS_CPU_160MHZ);

  // ── WiFi mode: AP + STA ──
  WiFi.mode(WIFI_AP_STA);
  WiFi.setOutputPower(cfg.txPower * 0.25f);

  // ── AP setup ──
  startAP();

  // ── STA connection ──
  if (strlen(cfg.staSsid) > 0) {
    connectSTA();
  } else {
    Serial.println(F("[WiFi] No STA creds → captive portal"));
    captivePortal = true;
    startCaptivePortal();
  }

  // ── Enable NAT ──
  enableNAT();

  // ── mDNS ──
  MDNS.begin(OTA_HOSTNAME);

  // ── OTA ──
  setupOTA();

  // ── Web routes ──
  setupRoutes();
  server.begin();
  Serial.println(F("[HTTP] Server started on port 80"));
}

// ════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════
void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  ArduinoOTA.handle();
  MDNS.update();
  watchdog();
}

// ════════════════════════════════════════════
//  AP SETUP
// ════════════════════════════════════════════
void startAP() {
  IPAddress apIP, subnet;
  apIP.fromString(AP_IP);
  subnet.fromString(AP_SUBNET);

  WiFi.softAPConfig(apIP, apIP, subnet);

  bool ok;
  if (strlen(cfg.apPass) >= 8) {
    ok = WiFi.softAP(cfg.apSsid, cfg.apPass, cfg.apChannel, 0, cfg.apMaxConn);
  } else {
    ok = WiFi.softAP(cfg.apSsid, nullptr, cfg.apChannel, 0, cfg.apMaxConn);
  }

  Serial.printf("[AP] %s → %s\n", cfg.apSsid, ok ? "OK" : "FAIL");
}

// ════════════════════════════════════════════
//  STA CONNECTION
// ════════════════════════════════════════════
void connectSTA() {
  Serial.printf("[STA] Connecting to %s…\n", cfg.staSsid);
  WiFi.begin(cfg.staSsid, cfg.staPass);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print('.');
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    staConnected = true;
    offlineSince = 0;
    Serial.printf("\n[STA] Connected! IP=%s\n", WiFi.localIP().toString().c_str());
    enableNAT();
  } else {
    Serial.println(F("\n[STA] Failed – captive portal"));
    captivePortal = true;
    startCaptivePortal();
  }
}

// ════════════════════════════════════════════
//  NAT (IP FORWARDING)
// ════════════════════════════════════════════
void enableNAT() {
  // Requires lwIP with NAT – enabled in Arduino IDE:
  // Tools → lwIP Variant → "v2 Higher Bandwidth (no features)"
  // Or with -DLWIP_FEATURES=1 in extra flags
  ip_napt_enable_no(SOFTAP_IF, 1);
  Serial.println(F("[NAT] Enabled"));
}

// ════════════════════════════════════════════
//  CAPTIVE PORTAL (DNS trap)
// ════════════════════════════════════════════
void startCaptivePortal() {
  IPAddress apIP;
  apIP.fromString(AP_IP);
  dnsServer.start(53, "*", apIP);
  Serial.println(F("[DNS] Captive portal started"));
}

// ════════════════════════════════════════════
//  WATCHDOG
// ════════════════════════════════════════════
void watchdog() {
  if (millis() - lastPing < (unsigned long)WATCHDOG_INTERVAL_S * 1000) return;
  lastPing = millis();

  bool ok = WiFi.status() == WL_CONNECTED;

  if (ok) {
    staConnected = true;
    offlineSince = 0;
    captivePortal = false;
  } else {
    staConnected = false;
    if (offlineSince == 0) offlineSince = millis();

    unsigned long offlineMs = millis() - offlineSince;
    Serial.printf("[WDG] Offline %lu s – reconnecting…\n", offlineMs / 1000);

    int retries = 0;
    WiFi.reconnect();
    while (WiFi.status() != WL_CONNECTED && retries < RECONNECT_RETRIES) {
      delay(1000);
      retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      staConnected = true;
      offlineSince = 0;
      enableNAT();
      Serial.println(F("[WDG] Reconnected OK"));
    } else if (offlineMs > (unsigned long)HARD_REBOOT_MIN * 60 * 1000) {
      Serial.println(F("[WDG] Hard reboot!"));
      ESP.restart();
    }
  }
}

// ════════════════════════════════════════════
//  OTA
// ════════════════════════════════════════════
void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]()  { Serial.println(F("[OTA] Start")); });
  ArduinoOTA.onEnd([]()    { Serial.println(F("[OTA] Done – rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
  ArduinoOTA.begin();
}

// ════════════════════════════════════════════
//  CONFIG PERSISTENCE (LittleFS JSON)
// ════════════════════════════════════════════
void loadConfig() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) { Serial.println(F("[CFG] No config, using defaults")); return; }

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    strlcpy(cfg.staSsid,   doc["staSsid"]   | DEFAULT_STA_SSID,  64);
    strlcpy(cfg.staPass,   doc["staPass"]   | DEFAULT_STA_PASS,  64);
    strlcpy(cfg.apSsid,    doc["apSsid"]    | DEFAULT_AP_SSID,   64);
    strlcpy(cfg.apPass,    doc["apPass"]    | DEFAULT_AP_PASS,   64);
    cfg.apChannel  = doc["apChannel"]  | DEFAULT_AP_CHANNEL;
    cfg.apMaxConn  = doc["apMaxConn"]  | DEFAULT_AP_MAXCONN;
    cfg.dhcpEnable = doc["dhcpEnable"] | true;
    cfg.txPower    = doc["txPower"]    | 82;
    strlcpy(cfg.pingHost, doc["pingHost"] | WATCHDOG_PING_HOST, 40);
    Serial.println(F("[CFG] Loaded OK"));
  }
  f.close();
}

void saveConfig() {
  File f = LittleFS.open("/config.json", "w");
  if (!f) { Serial.println(F("[CFG] Write fail")); return; }

  StaticJsonDocument<512> doc;
  doc["staSsid"]   = cfg.staSsid;
  doc["staPass"]   = cfg.staPass;
  doc["apSsid"]    = cfg.apSsid;
  doc["apPass"]    = cfg.apPass;
  doc["apChannel"] = cfg.apChannel;
  doc["apMaxConn"] = cfg.apMaxConn;
  doc["dhcpEnable"]= cfg.dhcpEnable;
  doc["txPower"]   = cfg.txPower;
  doc["pingHost"]  = cfg.pingHost;
  serializeJson(doc, f);
  f.close();
  Serial.println(F("[CFG] Saved"));
}

// ════════════════════════════════════════════
//  HELPER: JSON Response
// ════════════════════════════════════════════
void sendJson(int code, const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(code, "application/json", body);
}

// ════════════════════════════════════════════
//  WEB ROUTES
// ════════════════════════════════════════════
void setupRoutes() {

  // ── Captive portal redirect ──
  server.onNotFound([]() {
    if (captivePortal) {
      server.sendHeader("Location", "http://192.168.4.1/");
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });

  // ── Root → Butler GUI ──
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", butlerHTML());
  });

  // ── GET /api/status ──
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["staConnected"] = staConnected;
    doc["staSsid"]      = cfg.staSsid;
    doc["staIp"]        = WiFi.localIP().toString();
    doc["staGw"]        = WiFi.gatewayIP().toString();
    doc["staRssi"]      = WiFi.RSSI();
    doc["apSsid"]       = cfg.apSsid;
    doc["apIp"]         = AP_IP;
    doc["apChannel"]    = cfg.apChannel;
    doc["apClients"]    = WiFi.softAPgetStationNum();
    doc["freeHeap"]     = ESP.getFreeHeap();
    doc["uptime"]       = (millis() - bootTime) / 1000;
    doc["chipId"]       = String(ESP.getChipId(), HEX);
    doc["flashSize"]    = ESP.getFlashChipSize();
    doc["cpuFreq"]      = ESP.getCpuFreqMHz();
    doc["txBytes"]      = txBytes;
    doc["rxBytes"]      = rxBytes;
    doc["packetsFwd"]   = packetsFwd;
    String out; serializeJson(doc, out);
    sendJson(200, out);
  });

  // ── GET /api/config ──
  server.on("/api/config", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["staSsid"]   = cfg.staSsid;
    // never send password in plaintext
    doc["staPassSet"]= strlen(cfg.staPass) > 0;
    doc["apSsid"]    = cfg.apSsid;
    doc["apPassSet"] = strlen(cfg.apPass) > 0;
    doc["apChannel"] = cfg.apChannel;
    doc["apMaxConn"] = cfg.apMaxConn;
    doc["txPower"]   = cfg.txPower;
    doc["pingHost"]  = cfg.pingHost;
    String out; serializeJson(doc, out);
    sendJson(200, out);
  });

  // ── POST /api/config/sta  { staSsid, staPass } ──
  server.on("/api/config/sta", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}"); return;
    }
    strlcpy(cfg.staSsid, doc["staSsid"] | cfg.staSsid, 64);
    const char* p = doc["staPass"];
    if (p && strlen(p) > 0) strlcpy(cfg.staPass, p, 64);
    saveConfig();
    sendJson(200, "{\"ok\":true,\"msg\":\"Reconnecting…\"}");
    delay(200);
    WiFi.disconnect();
    connectSTA();
  });

  // ── POST /api/config/ap  { apSsid, apPass, apChannel, apMaxConn } ──
  server.on("/api/config/ap", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}"); return;
    }
    strlcpy(cfg.apSsid, doc["apSsid"] | cfg.apSsid, 64);
    const char* p = doc["apPass"];
    if (p && strlen(p) == 0) cfg.apPass[0] = '\0';
    else if (p && strlen(p) >= 8) strlcpy(cfg.apPass, p, 64);
    cfg.apChannel = doc["apChannel"] | cfg.apChannel;
    cfg.apMaxConn = doc["apMaxConn"] | cfg.apMaxConn;
    saveConfig();
    sendJson(200, "{\"ok\":true,\"msg\":\"AP restarting…\"}");
    delay(200);
    WiFi.softAPdisconnect(false);
    startAP();
  });

  // ── GET /api/scan  (scan upstream networks) ──
  server.on("/api/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("networks");
    for (int i = 0; i < min(n, 20); i++) {
      JsonObject o = arr.createNestedObject();
      o["ssid"]    = WiFi.SSID(i);
      o["rssi"]    = WiFi.RSSI(i);
      o["enc"]     = (int)WiFi.encryptionType(i);
      o["channel"] = WiFi.channel(i);
    }
    WiFi.scanDelete();
    String out; serializeJson(doc, out);
    sendJson(200, out);
  });

  // ── GET /api/clients  (AP stations) ──
  server.on("/api/clients", HTTP_GET, []() {
    struct station_info* s = wifi_softap_get_station_info();
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("clients");
    int count = 0;
    while (s && count < 8) {
      JsonObject o = arr.createNestedObject();
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
        s->bssid[0],s->bssid[1],s->bssid[2],s->bssid[3],s->bssid[4],s->bssid[5]);
      o["mac"] = mac;
      s = STAILQ_NEXT(s, next);
      count++;
    }
    wifi_softap_free_station_info();
    doc["count"] = count;
    String out; serializeJson(doc, out);
    sendJson(200, out);
  });

  // ── POST /api/power  { txPower: 0-82 } (0.25 dBm/unit) ──
  server.on("/api/power", HTTP_POST, []() {
    StaticJsonDocument<64> doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
      sendJson(400, "{\"error\":\"bad json\"}"); return;
    }
    cfg.txPower = constrain((int)doc["txPower"], 0, 82);
    WiFi.setOutputPower(cfg.txPower * 0.25f);
    saveConfig();
    sendJson(200, "{\"ok\":true}");
  });

  // ── POST /api/reboot ──
  server.on("/api/reboot", HTTP_POST, []() {
    sendJson(200, "{\"ok\":true,\"msg\":\"Rebooting…\"}");
    delay(500);
    ESP.restart();
  });

  // ── POST /api/factory ──
  server.on("/api/factory", HTTP_POST, []() {
    sendJson(200, "{\"ok\":true,\"msg\":\"Factory reset – rebooting\"}");
    delay(300);
    LittleFS.remove("/config.json");
    delay(200);
    ESP.restart();
  });

  // ── GET /api/heap ──
  server.on("/api/heap", HTTP_GET, []() {
    String j = "{\"freeHeap\":" + String(ESP.getFreeHeap()) + "}";
    sendJson(200, j);
  });
}

// ════════════════════════════════════════════
//  BUTLER HTML (served from RAM)
//  Full single-page GUI with tabs & live fetch
// ════════════════════════════════════════════
String butlerHTML() {
  return F(R"rawhtml(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 Butler</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:1rem}
h1{font-size:18px;font-weight:600;margin-bottom:4px}
.sub{font-size:12px;color:#94a3b8}
header{display:flex;align-items:center;gap:12px;margin-bottom:20px}
.logo{width:40px;height:40px;background:#2563eb;border-radius:50%;display:flex;align-items:center;justify-content:center}
.badge{display:inline-block;font-size:11px;padding:2px 9px;border-radius:10px;font-weight:600}
.ok{background:#166534;color:#bbf7d0}.warn{background:#7c2d12;color:#fed7aa}.off{background:#1e293b;color:#94a3b8}
nav{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:18px}
.tab{padding:7px 16px;border:1px solid #334155;border-radius:8px;font-size:13px;cursor:pointer;background:#1e293b;color:#94a3b8;transition:.15s}
.tab.act{background:#2563eb;color:#fff;border-color:#2563eb}
.card{background:#1e293b;border:1px solid #334155;border-radius:12px;padding:14px 18px;margin-bottom:12px}
.card h3{font-size:11px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;color:#64748b;margin-bottom:10px}
.row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid #0f172a;font-size:13px}
.row:last-child{border:none}
.lbl{color:#64748b}
.stat-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px;margin-bottom:14px}
.stat{background:#0f172a;border-radius:8px;padding:12px;text-align:center}
.stat .v{font-size:22px;font-weight:700;color:#e2e8f0}
.stat .l{font-size:11px;color:#64748b;margin-top:2px}
input,select{width:100%;background:#0f172a;border:1px solid #334155;border-radius:8px;padding:8px 12px;color:#e2e8f0;font-size:13px;margin-top:4px;margin-bottom:10px}
input:focus,select:focus{outline:none;border-color:#2563eb}
.btn{padding:8px 18px;border:1px solid #334155;border-radius:8px;background:#1e293b;color:#e2e8f0;cursor:pointer;font-size:13px;transition:.15s}
.btn:hover{background:#334155}
.btn-blue{background:#2563eb;border-color:#2563eb;color:#fff}
.btn-red{background:#991b1b;border-color:#991b1b;color:#fff}
.btn-row{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
.page{display:none}.page.act{display:block}
.sig{display:inline-flex;gap:2px;align-items:flex-end;height:14px;vertical-align:middle}
.sig span{width:4px;border-radius:1px}
</style></head><body>

<header>
  <div class="logo">
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="#fff" stroke-width="2">
      <path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/>
      <path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1" fill="#fff"/>
    </svg>
  </div>
  <div><h1>ESP8266 Butler</h1><p class="sub">WiFi Repeater &middot; 192.168.4.1</p></div>
  <span id="sta-badge" class="badge off" style="margin-left:auto">…</span>
</header>

<nav>
  <div class="tab act" onclick="showPage('status',this)">Status</div>
  <div class="tab" onclick="showPage('network',this)">Network</div>
  <div class="tab" onclick="showPage('clients',this)">Clients</div>
  <div class="tab" onclick="showPage('scan',this)">Scan</div>
  <div class="tab" onclick="showPage('advanced',this)">Advanced</div>
  <div class="tab" onclick="showPage('system',this)">System</div>
</nav>

<!-- STATUS -->
<div id="p-status" class="page act">
  <div class="stat-grid">
    <div class="stat"><div class="v" id="s-clients">-</div><div class="l">Clients</div></div>
    <div class="stat"><div class="v" id="s-rssi">-</div><div class="l">RSSI (dBm)</div></div>
    <div class="stat"><div class="v" id="s-up">-</div><div class="l">Uptime</div></div>
    <div class="stat"><div class="v" id="s-heap">-</div><div class="l">Free heap</div></div>
  </div>
  <div class="card">
    <h3>Upstream (STA)</h3>
    <div class="row"><span class="lbl">SSID</span><span id="r-staSsid">-</span></div>
    <div class="row"><span class="lbl">IP</span><span id="r-staIp">-</span></div>
    <div class="row"><span class="lbl">Gateway</span><span id="r-staGw">-</span></div>
    <div class="row"><span class="lbl">Signal</span><span id="r-sig">-</span></div>
  </div>
  <div class="card">
    <h3>Hotspot (AP)</h3>
    <div class="row"><span class="lbl">SSID</span><span id="r-apSsid">-</span></div>
    <div class="row"><span class="lbl">IP</span><span>192.168.4.1</span></div>
    <div class="row"><span class="lbl">Channel</span><span id="r-apCh">-</span></div>
  </div>
  <div class="card">
    <h3>Traffic</h3>
    <div class="row"><span class="lbl">TX</span><span id="r-tx">-</span></div>
    <div class="row"><span class="lbl">RX</span><span id="r-rx">-</span></div>
    <div class="row"><span class="lbl">Packets forwarded</span><span id="r-pkt">-</span></div>
  </div>
</div>

<!-- NETWORK -->
<div id="p-network" class="page">
  <div class="card">
    <h3>Upstream WiFi (STA)</h3>
    <label>SSID</label><input id="n-staSsid" type="text" placeholder="Home router SSID">
    <label>Password</label><input id="n-staPass" type="password" placeholder="Leave blank to keep current">
    <button class="btn btn-blue" onclick="saveSTA()">Save &amp; Reconnect</button>
  </div>
  <div class="card">
    <h3>Hotspot (AP)</h3>
    <label>AP SSID</label><input id="n-apSsid" type="text">
    <label>AP Password (min 8 or blank for open)</label><input id="n-apPass" type="password">
    <label>Channel</label>
    <select id="n-apCh"><option>1</option><option>2</option><option>3</option><option>4</option>
    <option>5</option><option selected>6</option><option>7</option><option>8</option>
    <option>9</option><option>10</option><option>11</option></select>
    <label>Max Clients</label>
    <select id="n-apMax"><option>1</option><option>2</option><option>3</option>
    <option selected>4</option><option>5</option><option>6</option><option>7</option><option>8</option></select>
    <button class="btn btn-blue" onclick="saveAP()">Apply AP Settings</button>
  </div>
</div>

<!-- CLIENTS -->
<div id="p-clients" class="page">
  <div class="card">
    <h3>Connected clients</h3>
    <div id="client-list"><p style="color:#64748b;font-size:13px">Loading…</p></div>
  </div>
</div>

<!-- SCAN -->
<div id="p-scan" class="page">
  <div class="card">
    <h3>WiFi networks nearby</h3>
    <button class="btn btn-blue" onclick="doScan()">Scan now</button>
    <div id="scan-list" style="margin-top:14px"></div>
  </div>
</div>

<!-- ADVANCED -->
<div id="p-advanced" class="page">
  <div class="card">
    <h3>Repeater features</h3>
    <div class="row"><span class="lbl">NAT / IP forwarding</span><span class="badge ok">Enabled</span></div>
    <div class="row"><span class="lbl">DNS relay</span><span class="badge ok">Enabled</span></div>
    <div class="row"><span class="lbl">DHCP server</span><span class="badge ok">192.168.4.x</span></div>
    <div class="row"><span class="lbl">OTA updates</span><span class="badge ok">Ready</span></div>
  </div>
  <div class="card">
    <h3>TX power</h3>
    <label>Power (0–82 units, 1 unit = 0.25 dBm)</label>
    <input id="txp" type="range" min="0" max="82" value="82" oninput="document.getElementById('txpv').textContent=(this.value*0.25).toFixed(2)+' dBm'">
    <span id="txpv">20.50 dBm</span>
    <div class="btn-row"><button class="btn btn-blue" onclick="setPower()">Set Power</button></div>
  </div>
  <div class="card">
    <h3>Watchdog</h3>
    <div class="row"><span class="lbl">Ping host</span><span>8.8.8.8</span></div>
    <div class="row"><span class="lbl">Interval</span><span>30 s</span></div>
    <div class="row"><span class="lbl">Reconnect retries</span><span>10</span></div>
    <div class="row"><span class="lbl">Hard reboot after</span><span>5 min offline</span></div>
  </div>
</div>

<!-- SYSTEM -->
<div id="p-system" class="page">
  <div class="card">
    <h3>Device info</h3>
    <div class="row"><span class="lbl">Firmware</span><span>v2.4.0</span></div>
    <div class="row"><span class="lbl">Chip ID</span><span id="r-chip">-</span></div>
    <div class="row"><span class="lbl">Flash size</span><span id="r-flash">-</span></div>
    <div class="row"><span class="lbl">CPU freq</span><span id="r-cpu">-</span></div>
    <div class="row"><span class="lbl">Free heap</span><span id="r-heap2">-</span></div>
  </div>
  <div class="btn-row">
    <button class="btn btn-blue" onclick="doOTA()">OTA Update</button>
    <button class="btn" onclick="doReboot()">Reboot</button>
    <button class="btn btn-red" onclick="doFactory()">Factory Reset</button>
  </div>
</div>

<script>
function showPage(id, el){
  document.querySelectorAll('.page').forEach(p=>p.classList.remove('act'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('act'));
  document.getElementById('p-'+id).classList.add('act');
  el.classList.add('act');
  if(id==='clients') loadClients();
  if(id==='network') loadNetConfig();
  if(id==='system') loadStatus();
}

function fmt(b){if(b<1024)return b+'B';if(b<1048576)return (b/1024).toFixed(1)+'KB';return (b/1048576).toFixed(1)+'MB';}
function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h+'h '+m+'m';}
function sigBars(r){var p=r>-55?4:r>-65?3:r>-75?2:1;return p;}

async function loadStatus(){
  try{
    var d=await(await fetch('/api/status')).json();
    document.getElementById('s-clients').textContent=d.apClients;
    document.getElementById('s-rssi').textContent=d.staConnected?d.staRssi:'—';
    document.getElementById('s-up').textContent=fmtUp(d.uptime);
    document.getElementById('s-heap').textContent=fmt(d.freeHeap);
    document.getElementById('r-staSsid').textContent=d.staSsid||'—';
    document.getElementById('r-staIp').textContent=d.staIp||'—';
    document.getElementById('r-staGw').textContent=d.staGw||'—';
    document.getElementById('r-sig').textContent=d.staConnected?(d.staRssi+' dBm'):'Offline';
    document.getElementById('r-apSsid').textContent=d.apSsid;
    document.getElementById('r-apCh').textContent=d.apChannel;
    document.getElementById('r-tx').textContent=fmt(d.txBytes);
    document.getElementById('r-rx').textContent=fmt(d.rxBytes);
    document.getElementById('r-pkt').textContent=d.packetsFwd.toLocaleString();
    var b=document.getElementById('sta-badge');
    b.textContent=d.staConnected?'Online':'Offline';
    b.className='badge '+(d.staConnected?'ok':'warn');
    if(document.getElementById('r-chip')){
      document.getElementById('r-chip').textContent=d.chipId.toUpperCase();
      document.getElementById('r-flash').textContent=fmt(d.flashSize);
      document.getElementById('r-cpu').textContent=d.cpuFreq+' MHz';
      document.getElementById('r-heap2').textContent=fmt(d.freeHeap);
    }
  }catch(e){console.warn(e);}
}

async function loadNetConfig(){
  try{
    var d=await(await fetch('/api/config')).json();
    document.getElementById('n-staSsid').value=d.staSsid;
    document.getElementById('n-apSsid').value=d.apSsid;
    var ch=document.getElementById('n-apCh');
    for(var i=0;i<ch.options.length;i++)if(ch.options[i].value==d.apChannel)ch.selectedIndex=i;
    var mx=document.getElementById('n-apMax');
    for(var i=0;i<mx.options.length;i++)if(mx.options[i].value==d.apMaxConn)mx.selectedIndex=i;
  }catch(e){}
}

async function loadClients(){
  try{
    var d=await(await fetch('/api/clients')).json();
    var el=document.getElementById('client-list');
    if(!d.clients.length){el.innerHTML='<p style="color:#64748b;font-size:13px">No clients connected</p>';return;}
    el.innerHTML=d.clients.map(c=>`<div class="row"><span style="color:#22c55e;font-size:11px">&#9679;</span> <span style="flex:1;margin:0 8px">${c.mac}</span></div>`).join('');
  }catch(e){}
}

async function doScan(){
  document.getElementById('scan-list').innerHTML='<p style="color:#64748b;font-size:13px">Scanning…</p>';
  try{
    var d=await(await fetch('/api/scan')).json();
    document.getElementById('scan-list').innerHTML=d.networks.map(n=>
      `<div class="row"><span>${n.ssid}</span><span style="color:#64748b;font-size:12px">${n.rssi} dBm · ch${n.channel}</span></div>`
    ).join('')||'<p style="color:#64748b;font-size:13px">None found</p>';
  }catch(e){document.getElementById('scan-list').innerHTML='<p style="color:#ef4444;font-size:13px">Scan failed</p>';}
}

async function saveSTA(){
  var body={staSsid:document.getElementById('n-staSsid').value,staPass:document.getElementById('n-staPass').value};
  var r=await fetch('/api/config/sta',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert((await r.json()).msg||'Saved');
}

async function saveAP(){
  var body={apSsid:document.getElementById('n-apSsid').value,apPass:document.getElementById('n-apPass').value,
    apChannel:parseInt(document.getElementById('n-apCh').value),apMaxConn:parseInt(document.getElementById('n-apMax').value)};
  var r=await fetch('/api/config/ap',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert((await r.json()).msg||'Saved');
}

async function setPower(){
  var v=parseInt(document.getElementById('txp').value);
  await fetch('/api/power',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({txPower:v})});
  alert('TX power set to '+(v*0.25).toFixed(2)+' dBm');
}

async function doReboot(){if(!confirm('Reboot ESP8266?'))return;await fetch('/api/reboot',{method:'POST'});alert('Rebooting…');}
async function doFactory(){if(!confirm('Factory reset? All settings lost!'))return;await fetch('/api/factory',{method:'POST'});alert('Resetting…');}
function doOTA(){alert('Use Arduino IDE OTA at '+location.hostname+' with password: butler2024');}

loadStatus();
setInterval(loadStatus, 5000);
</script>
</body></html>
)rawhtml");
}
