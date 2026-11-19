#include "obd.h"
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <WebServer.h>
#include <DNSServer.h>

// ---- Bluetooth LE adapter (Vgate, Vlink, Veepeak...): the ELM327 talks over a notify + a write
// characteristic. Tried before Wi-Fi; Wi-Fi stays free for the home clock.
static BLEClient* bc = nullptr;
static BLERemoteCharacteristic *bRx = nullptr, *bTx = nullptr;
static bool useBle = false;
static char bleName[24] = "";
static uint8_t rb[512]; static volatile uint16_t rh = 0, rt = 0;   // bytes the adapter sent
static void onNotify(BLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) { uint16_t n = (rh + 1) % sizeof(rb); if (n != rt) { rb[rh] = data[i]; rh = n; } }
}
static bool looksLikeObd(BLEAdvertisedDevice& a) {
  String n = a.getName(); n.toUpperCase();
  const char* names[] = {"OBD", "VLINK", "V-LINK", "VGATE", "ELM", "ICAR", "VEEPEAK", "KONNWEI", "CARISTA"};
  for (auto s : names) if (n.indexOf(s) >= 0) return true;
  for (int i = 0; i < a.getServiceUUIDCount(); i++) {
    String u = a.getServiceUUID(i).toString().c_str(); u.toLowerCase();
    if (u.indexOf("fff0") >= 0 || u.indexOf("ffe0") >= 0 || u.indexOf("18f0") >= 0) return true;
  }
  return false;
}
static void ble_drop() { if (bc && bc->isConnected()) bc->disconnect(); bRx = bTx = nullptr; useBle = false; }
static bool ble_connect() {
  if (!BLEDevice::getInitialized()) return false;
  BLEScan* sc = BLEDevice::getScan();
  sc->setActiveScan(true);
  BLEScanResults* res = sc->start(4, false);
  BLEAdvertisedDevice dev; bool found = false;
  for (int i = 0; res && i < res->getCount(); i++) { BLEAdvertisedDevice a = res->getDevice(i); if (looksLikeObd(a)) { dev = a; found = true; break; } }
  sc->clearResults();
  if (!found) return false;
  if (!bc) bc = BLEDevice::createClient();
  if (!bc->connect(&dev)) return false;
  // any service with a notify characteristic (replies) and a write characteristic (commands)
  for (auto& s : *bc->getServices()) {
    String u = s.second->getUUID().toString().c_str();
    if (u.startsWith("00001800") || u.startsWith("00001801") || u.startsWith("0000180a")) continue;   // generic services
    BLERemoteCharacteristic *rx = nullptr, *tx = nullptr;
    for (auto& c : *s.second->getCharacteristics()) {
      if (!rx && (c.second->canNotify() || c.second->canIndicate())) rx = c.second;
      if (!tx && (c.second->canWrite() || c.second->canWriteNoResponse())) tx = c.second;
    }
    if (rx && tx) { bRx = rx; bTx = tx; break; }
  }
  if (!bRx) { bc->disconnect(); return false; }
  rh = rt = 0;
  bRx->registerForNotify(onNotify);
  strlcpy(bleName, dev.getName().c_str(), sizeof(bleName));
  useBle = true;
  return true;
}

static ObdData d = {false, false, -1, -1, -1000, -1, -1, false, {}, 0, -1, -1, 0, 0, 0, 0};
static char ssid[33] = "WiFi_OBDII", pass[65] = "", host[16] = "192.168.0.10";
static int port = 35000;
static volatile bool wantClear = false, restart = false;
static WiFiClient cli;
// home Wi-Fi, only used to read the clock from the internet (the board has no clock battery)
static char hssid[33] = "", hpass[65] = "";
static volatile bool timeNew = false;
static struct tm timeT;
static uint32_t ntpOk = 0, ntpTry = 0;

static void load() {
  Preferences p;
  if (!p.begin("obd", true)) return;
  p.getString("ssid", ssid, sizeof(ssid)) || strcpy(ssid, "WiFi_OBDII");
  p.getString("pass", pass, sizeof(pass));
  p.getString("host", host, sizeof(host)) || strcpy(host, "192.168.0.10");
  port = p.getInt("port", 35000);
  p.getString("hssid", hssid, sizeof(hssid));
  p.getString("hpass", hpass, sizeof(hpass));
  p.end();
}

void obd_set_wifi(const char* s, const char* pw, const char* ip, int pt) {
  Preferences p;
  p.begin("obd", false);
  if (s)  p.putString("ssid", s);
  p.putString("pass", pw ? pw : "");
  if (ip) p.putString("host", ip);
  if (pt > 0) p.putInt("port", pt);
  p.end();
  load();
  restart = true;
}

void obd_set_home_wifi(const char* s, const char* pw) {
  Preferences p;
  p.begin("obd", false);
  p.putString("hssid", s ? s : "");
  p.putString("hpass", pw ? pw : "");
  p.end();
  load();
  ntpOk = ntpTry = 0;
  restart = true;
}
bool obd_take_time(struct tm* t) { if (!timeNew) return false; *t = timeT; timeNew = false; return true; }

// The car adapter is not around: if a home network is set, read the time (Iran, +03:30, no DST).
static void home_clock() {
  if (!hssid[0]) return;
  uint32_t now = millis();
  if (ntpOk && now - ntpOk < 6UL * 3600 * 1000) return;           // every 6 hours is plenty
  if (ntpTry && now - ntpTry < 10UL * 60 * 1000) return;          // failed: try again in 10 min
  ntpTry = now;
  WiFi.disconnect();
  WiFi.begin(hssid, hpass[0] ? hpass : NULL);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime("<+0330>-3:30", "ir.pool.ntp.org", "pool.ntp.org", "time.google.com");
    struct tm t;
    if (getLocalTime(&t, 10000) && t.tm_year + 1900 >= 2025) { timeT = t; timeNew = true; ntpOk = millis(); }
  }
  WiFi.disconnect();
}

// ---- Settings panel: Laboo makes its own Wi-Fi "Laboo". Any phone or PC that opens the page
// sends its own clock (the page's script does it), and the home / adapter Wi-Fi can be set there.
static const char* PANEL_SSID = "Laboo", *PANEL_PASS = "laboo1234";
static volatile uint32_t panelUntil = 0;
static WebServer srv(80);
static DNSServer dns;
static bool srvInit = false;
static String esc(const char* s) { String r = s; r.replace("&", "&amp;"); r.replace("\"", "&quot;"); r.replace("<", "&lt;"); return r; }
static void page_main() {
  String h = F("<!doctype html><html dir=rtl><meta charset=utf-8><meta name=viewport content='width=device-width'><title>لبو</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;max-width:420px;margin:auto;padding:16px}"
    "input,button{width:100%;padding:10px;margin:4px 0 12px;font-size:16px;box-sizing:border-box;border-radius:8px;border:0}"
    "button{background:#f80;color:#000;font-weight:bold}h3{color:#f80}#t{background:#222;padding:10px;border-radius:8px}</style>"
    "<h2>تنظیمات لبو</h2><p id=t>در حال تنظیم ساعت…</p>"
    "<form action=/home method=post><h3>وای‌فای خانه (ساعت از اینترنت)</h3>نام:<input name=s value=\"");
  h += esc(hssid);
  h += F("\">رمز:<input name=p type=password><button>ذخیره</button></form>"
    "<form action=/obd method=post><h3>وای‌فای دیاگ ماشین</h3>نام:<input name=s value=\"");
  h += esc(ssid);
  h += F("\">رمز:<input name=p type=password placeholder='خالی = بدون رمز'>IP:<input name=i value=\"");
  h += esc(host);
  h += F("\">پورت:<input name=o type=number value=\"");
  h += port;
  h += F("\"><button>ذخیره</button></form>"
    "<script>var d=new Date();fetch('/time?v='+[d.getFullYear(),d.getMonth()+1,d.getDate(),d.getHours(),d.getMinutes(),d.getSeconds()].join(','))"
    ".then(r=>r.text()).then(x=>document.getElementById('t').textContent=x)</script></html>");
  srv.send(200, "text/html; charset=utf-8", h);
}
static void page_done(const char* msg) {
  String h = F("<!doctype html><html dir=rtl><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<body style='font-family:sans-serif;background:#111;color:#eee;padding:16px'><h2>");
  h += msg; h += F("</h2></html>");
  srv.send(200, "text/html; charset=utf-8", h);
}
static void panel_routes() {
  srv.on("/", page_main);
  srv.on("/time", []() {
    int v[6] = {0};
    if (sscanf(srv.arg("v").c_str(), "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6 && v[0] >= 2025 && v[0] < 2100) {
      struct tm t = {};
      t.tm_year = v[0] - 1900; t.tm_mon = v[1] - 1; t.tm_mday = v[2]; t.tm_hour = v[3]; t.tm_min = v[4]; t.tm_sec = v[5];
      timeT = t; timeNew = true;
      srv.send(200, "text/plain; charset=utf-8", "ساعت لبو تنظیم شد ✓");
    } else srv.send(400, "text/plain; charset=utf-8", "ساعت نامعتبر");
  });
  srv.on("/home", HTTP_POST, []() {
    page_done("وای‌فای خانه ذخیره شد. لبو ساعت را از اینترنت می‌گیرد.");
    delay(300);
    obd_set_home_wifi(srv.arg("s").c_str(), srv.arg("p").c_str());
  });
  srv.on("/obd", HTTP_POST, []() {
    page_done("وای‌فای دیاگ ذخیره شد.");
    delay(300);
    int pt = srv.arg("o").toInt();
    obd_set_wifi(srv.arg("s").c_str(), srv.arg("p").c_str(), srv.arg("i").length() ? srv.arg("i").c_str() : NULL, pt);
  });
  // phones probe a page to detect a sign-in Wi-Fi: send them all to the panel so it pops up by itself
  srv.onNotFound([]() { srv.sendHeader("Location", "http://192.168.4.1/"); srv.send(302, "text/plain", ""); });
}
void obd_panel(uint32_t minutes) { panelUntil = millis() + minutes * 60000UL; if (!panelUntil) panelUntil = 1; restart = true; }
bool obd_panel_on() { return panelUntil && (int32_t)(panelUntil - millis()) > 0; }
static void panel_run() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PANEL_SSID, PANEL_PASS);
  delay(100);
  dns.start(53, "*", WiFi.softAPIP());
  if (!srvInit) { panel_routes(); srvInit = true; }
  srv.begin();
  restart = false;
  while (obd_panel_on() && !restart) { dns.processNextRequest(); srv.handleClient(); delay(5); }
  if (restart) { uint32_t t0 = millis(); while (millis() - t0 < 1500) { srv.handleClient(); delay(5); } }   // let the reply page go out
  srv.stop(); dns.stop();
  panelUntil = 0;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

// Sends one command and reads until the ELM327 prompt '>'. Returns the reply without spaces/echo.
static int  tAvail() { return useBle ? (rh + sizeof(rb) - rt) % sizeof(rb) : cli.available(); }
static int  tRead()  { if (!useBle) return cli.read(); if (rh == rt) return -1; int c = rb[rt]; rt = (rt + 1) % sizeof(rb); return c; }
static bool tUp()    { return useBle ? (bc && bc->isConnected()) : cli.connected(); }
static bool cmd(const char* c, char* out, size_t n, uint32_t timeout = 1500) {
  while (tAvail()) tRead();
  if (useBle) { String s = String(c) + "\r"; bTx->writeValue((uint8_t*)s.c_str(), s.length(), !bTx->canWriteNoResponse()); }
  else { cli.print(c); cli.print("\r"); }
  size_t k = 0; uint32_t t0 = millis();
  while (millis() - t0 < timeout) {
    while (tAvail()) {
      char ch = tRead();
      if (ch == '>') { out[k] = 0; return true; }
      if (ch == ' ' || ch == '\r' || ch == '\n' || ch == 0) { if (ch != ' ' && k && out[k - 1] != '|') out[k++] = '|'; }
      else if (k < n - 1) out[k++] = toupper(ch);
      if (k >= n - 1) k = n - 2;
    }
    if (!tUp()) return false;
    delay(2);
  }
  out[k] = 0;
  return false;
}

// Finds the reply bytes after "41 <pid>" (headers off, spaces off) -> number of data bytes.
static int pid_bytes(const char* r, const char* hdr, uint8_t* b, int max) {
  const char* p = strstr(r, hdr);
  if (!p) return 0;
  p += strlen(hdr);
  int n = 0;
  while (n < max && isxdigit(p[0]) && isxdigit(p[1])) {
    char h[3] = {p[0], p[1], 0};
    b[n++] = strtol(h, NULL, 16);
    p += 2;
  }
  return n;
}

static bool elm_setup() {
  char r[96];
  cmd("ATZ", r, sizeof(r), 3000);           // reset
  delay(300);
  const char* init[] = {"ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATSP0"};
  for (auto c : init) if (!cmd(c, r, sizeof(r))) return false;
  // first request lets the ELM327 search the car's protocol (can take a few seconds)
  cmd("0100", r, sizeof(r), 8000);
  return strstr(r, "4100") != NULL;
}

static void read_codes() {
  char r[160]; uint8_t b[8];
  if (cmd("0101", r, sizeof(r)) && pid_bytes(r, "4101", b, 4) >= 1) {
    d.checkEngine = b[0] & 0x80;
    d.dtcCount = b[0] & 0x7F;
  }
  if (d.dtcCount <= 0) { for (auto& s : d.dtc) s[0] = 0; return; }
  if (!cmd("03", r, sizeof(r), 3000)) return;
  // reply: 43 then pairs of bytes per code (CAN adds a count byte; skip "0000" fillers)
  const char* p = strstr(r, "43");
  int i = 0;
  if (!p) return;
  p += 2;
  if (strlen(p) % 4 == 2) p += 2;          // CAN: count byte
  static const char kind[] = {'P', 'C', 'B', 'U'};
  while (i < 8 && isxdigit(p[0]) && isxdigit(p[3])) {
    char h[5] = {p[0], p[1], p[2], p[3], 0};
    uint16_t v = strtol(h, NULL, 16);
    p += 4;
    if (*p == '|') p++;
    if (!v) continue;
    snprintf(d.dtc[i++], 6, "%c%04X", kind[v >> 14], v & 0x3FFF);
  }
  for (; i < 8; i++) d.dtc[i][0] = 0;
}

static void poll_once(int step) {
  char r[96]; uint8_t b[4];
  bool any = false;
  if (cmd("010D", r, sizeof(r)) && pid_bytes(r, "410D", b, 1) == 1) { d.kmh = b[0]; any = true; }
  if (cmd("010C", r, sizeof(r)) && pid_bytes(r, "410C", b, 2) == 2) { d.rpm = (b[0] * 256 + b[1]) / 4.0f; any = true; }
  if (step % 5 == 0) {
    if (cmd("0105", r, sizeof(r)) && pid_bytes(r, "4105", b, 1) == 1) d.coolant = b[0] - 40;
    if (cmd("ATRV", r, sizeof(r))) d.volts = atof(r);
  }
  if (step % 2 == 0 && cmd("0110", r, sizeof(r)) && pid_bytes(r, "4110", b, 2) == 2)
    d.lph = (b[0] * 256 + b[1]) / 100.0f * 3600 / (14.7f * 740);   // air g/s -> petrol L/h (14.7:1, 740 g/L)
  if (step % 25 == 1 && cmd("012F", r, sizeof(r)) && pid_bytes(r, "412F", b, 1) == 1) d.fuelPct = b[0] * 100 / 255.0f;
  if (step % 150 == 0) read_codes();         // about every 30 s
  if (any) {
    // trip: add up distance, time and fuel over the time since the last good reading
    uint32_t now = millis();
    float dt = d.link && now - d.lastData < 3000 ? (now - d.lastData) / 1000.0f : 0;
    static float secAcc = 0;
    secAcc += dt; while (secAcc >= 1) { d.tripSec++; secAcc -= 1; }
    if (d.kmh > 0) d.tripKm += d.kmh * dt / 3600;
    if (d.lph > 0) d.tripL += d.lph * dt / 3600;
    if (d.kmh > d.maxKmh) d.maxKmh = d.kmh;
    d.link = true; d.lastData = now;
  }
}

static void task(void*) {
  load();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);          // the adapter is a metre away; full power made car USB ports brown out
  for (;;) {
    if (obd_panel_on()) { panel_run(); WiFi.mode(WIFI_STA); WiFi.setSleep(false); WiFi.setTxPower(WIFI_POWER_8_5dBm); }
    restart = false;
    d.wifi = d.link = false;
    // Bluetooth adapter first
    if (ble_connect()) {
      if (elm_setup()) {
        read_codes();
        for (int step = 1; tUp() && !restart; step++) {
          if (wantClear) { char r[32]; cmd("04", r, sizeof(r), 3000); wantClear = false; read_codes(); }
          poll_once(step);
          delay(150);
        }
      }
      ble_drop(); d.link = false;
      delay(2000);
      continue;
    }
    WiFi.mode(WIFI_STA);                        // radio is switched off between searches
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.disconnect();
    WiFi.begin(ssid, pass[0] ? pass : NULL);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000 && !restart) delay(200);
    // not found: the radio's search pulls current and makes the screen jump, so after the
    // first 5 minutes (car USB powers us when the car starts) only look every 5 minutes
    if (WiFi.status() != WL_CONNECTED) { home_clock(); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      uint32_t wait = millis() < 300000 ? 20000 : 300000, w0 = millis();
      while (millis() - w0 < wait && !restart) delay(200);
      continue; }   // not found: look again in 20 s (less power)   // adapter off (car off): try again
    d.wifi = true;
    if (!cli.connect(host, port, 3000)) { delay(3000); continue; }
    cli.setNoDelay(true);
    if (!elm_setup()) { cli.stop(); delay(3000); continue; }
    read_codes();
    for (int step = 1; cli.connected() && WiFi.status() == WL_CONNECTED && !restart; step++) {
      if (wantClear) { char r[32]; cmd("04", r, sizeof(r), 3000); wantClear = false; read_codes(); }
      poll_once(step);
      delay(150);
    }
    cli.stop();
  }
}

void obd_begin() { xTaskCreatePinnedToCore(task, "obd", 6144, NULL, 3, NULL, 0); }
const ObdData& obd_data() { return d; }
bool obd_live() { return d.link && millis() - d.lastData < 3000; }
void obd_clear_codes() { wantClear = true; }

void obd_status(Print& out) {
  out.printf("fuel=%.0f%% lph=%.1f trip=%.1fkm %lus %.2fL max=%.0f ", d.fuelPct, d.lph, d.tripKm, (unsigned long)d.tripSec, d.tripL, d.maxKmh);
  out.printf("ble=%s(%s) ", useBle ? "up" : "down", bleName);
  out.printf("obd wifi=%s(%s) link=%s kmh=%.0f rpm=%.0f coolant=%d volts=%.1f mil=%d dtc=%d",
             d.wifi ? "up" : "down", ssid, obd_live() ? "live" : "none", d.kmh, d.rpm, d.coolant, d.volts,
             d.checkEngine, d.dtcCount);
  for (auto& s : d.dtc) if (s[0]) out.printf(" %s", s);
  out.println();
}
