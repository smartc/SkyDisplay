#include "web.h"
#include "settings.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ElegantOTA.h>
#include <WiFiManager.h>
#include "board.h"
#include "ui.h"

static WebServer server(80);
static WebHooks hooks;
static volatile bool updating = false;  // firmware upload in progress: the screen shows a message instead of redrawing

static String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    switch (c) { case '&': o += "&amp;"; break; case '<': o += "&lt;"; break; case '>': o += "&gt;"; break;
                 case '"': o += "&quot;"; break; default: o += c; }
  }
  return o;
}

static bool authOk() {
  if (!cfg.webPassword.length()) return true;
  if (server.authenticate("admin", cfg.webPassword.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "SkyDisplay");
  return false;
}

static const char CSS[] PROGMEM = R"CSS(
:root{color-scheme:dark;--bg:#070a12;--panel:#0e1320;--line:#232c44;--fg:#e2e8f2;--muted:#94a0b8;--accent:#6fb6ff;--warn:#ffb43d}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;padding:20px max(16px,env(safe-area-inset-left))}
main{max-width:720px;margin:0 auto;display:grid;gap:18px}h1{font-size:1.5rem;margin:0}h2{font-size:1.05rem;margin:0 0 10px}
.status{color:var(--muted);font-size:.9rem}section{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px 18px}
label{display:grid;gap:4px;align-content:start;font-size:.88rem;color:var(--muted)}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:14px 16px;align-items:start}
input,select{font:inherit;color:var(--fg);background:#0a0f1a;border:1px solid var(--line);border-radius:8px;padding:0 10px;width:100%;height:40px;margin:0}
input[type=number]{-moz-appearance:textfield}input::-webkit-inner-spin-button{opacity:.5}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:-1px}.hint{font-size:.8rem;color:var(--muted)}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}button,.btn{font:inherit;background:#1b2742;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:8px 14px;cursor:pointer;text-decoration:none}
button.primary{background:var(--accent);color:#04101f;border-color:var(--accent);font-weight:600}button.warn{border-color:var(--warn);color:var(--warn)}
.check{display:flex;gap:8px;align-items:center;color:var(--fg);min-height:40px}.check input{width:18px;height:18px}table{border-collapse:collapse;font-size:.9rem}td{padding:2px 14px 2px 0}td:first-child{color:var(--muted)}
)CSS";

static String opt(int v, int cur, const char* text) {
  return "<option value='" + String(v) + "'" + (v == cur ? " selected" : "") + ">" + text + "</option>";
}

static void handleRoot() {
  if (!authOk()) return;
  String h; h.reserve(9000);
  h += F("<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>SkyDisplay settings</title><style>");
  h += FPSTR(CSS);
  h += F("</style></head><body><main><header><h1>SkyDisplay</h1><div class='status'>");
  h += esc(hooks.statusLine());
  h += F("</div></header>");
  if (server.hasArg("saved")) h += F("<section><b>Saved.</b> Changes are applied now.</section>");

  h += F("<form method='post' action='/save'><section><h2>Clear Sky Chart</h2><div class='grid'>"
         "<label style='grid-column:1/-1'>Chart ID or chart page URL<input name='chartUrl' autocapitalize='none' autocorrect='off' autocomplete='off' spellcheck='false' value='");
  h += esc(cfg.chartUrl);
  h += F("'><span class='hint'>The ID from your Dark Dragons chart link, e.g. abcde from https://cds.darkdragonsastro.com/g/abcde.html. A full link works too.</span></label>"
         "<label>Check every (minutes)<input type='number' min='10' max='240' name='poll' value='");
  h += String(cfg.pollMinutes);
  h += F("'></label></div></section>");

  // Coming back from the HTTPS location page: ?lat=&lon=&acc= pre-fills the fields (saved only on Save).
  bool fromPhone = false; double pLat = 0, pLon = 0; int pAcc = -1;
  if (server.hasArg("lat") && server.hasArg("lon")) {
    pLat = server.arg("lat").toDouble(); pLon = server.arg("lon").toDouble();
    pAcc = server.hasArg("acc") ? server.arg("acc").toInt() : -1;
    fromPhone = pLat >= -90 && pLat <= 90 && pLon >= -180 && pLon <= 180 && !(pLat == 0 && pLon == 0);
  }
  h += F("<section><h2>Location and time</h2>");
  if (fromPhone) {
    h += "<p class='hint' style='color:var(--warn);margin:0 0 10px'>Filled in from your phone";
    if (pAcc >= 0) h += " (accurate to about " + String(pAcc) + " m)";
    h += F(". Press Save settings to keep it.</p>");
  }
  h += F("<div class='grid'><label>Latitude<input name='lat' inputmode='decimal' value='");
  h += fromPhone ? String(pLat, 5) : (cfg.lat != 0 ? String(cfg.lat, 5) : "");
  h += F("'></label><label>Longitude<input name='lon' inputmode='decimal' value='");
  h += fromPhone ? String(pLon, 5) : (cfg.lon != 0 ? String(cfg.lon, 5) : "");
  h += F("'><span class='hint'>Leave both blank to use the chart's location.</span></label>"
         "<div class='row' style='grid-column:1/-1'><a class='btn' id='locBtn' href='/locate'>Use my location</a>"
         "<span class='hint'>Opens a secure page that asks your phone for its location. Expect a one-time"
         " &quot;not private&quot; warning (the display's own certificate); continue past it.</span></div>"
         "<script>document.getElementById('locBtn').href='https://'+location.hostname+'/locate';</script>"
         "<label>Time zone (POSIX)<input name='tz' list='tzs' value='");
  h += esc(cfg.tz);
  h += F("'><datalist id='tzs'><option value='&lt;-06&gt;6'>UTC-6 all year, no DST</option><option value='UTC0'>UTC</option>"
         "<option value='MST7MDT,M3.2.0,M11.1.0'>Mountain, with DST</option><option value='MST7'>Mountain standard, no DST</option>"
         "<option value='CST6CDT,M3.2.0,M11.1.0'>Central, with DST</option><option value='PST8PDT,M3.2.0,M11.1.0'>Pacific, with DST</option>"
         "<option value='EST5EDT,M3.2.0,M11.1.0'>Eastern, with DST</option></datalist></label>"
         "<label>NTP server<input name='ntp1' value='");
  h += esc(cfg.ntp1);
  h += F("'></label><label>NTP fallback (optional)<input name='ntp2' value='");
  h += esc(cfg.ntp2);
  h += F("'></label><label>Re-sync clock every (hours)<input type='number' min='1' max='24' name='ntpHours' value='");
  h += String(cfg.ntpHours);
  h += F("'><span class='hint'>The status screen turns amber if no sync succeeds for 12 hours.</span></label></div></section>");

  h += F("<section><h2>Display</h2><div class='grid'><label>Brightness (1-255)<input type='number' min='1' max='255' name='bright' value='");
  h += String(cfg.brightness);
  h += F("'></label><label>Dim brightness (1-255)<input type='number' min='1' max='255' name='dimBright' value='");
  h += String(cfg.dimBrightness);
  h += F("'><span class='hint'>Used when idle, at night, and when you tap the screen.</span></label><label>When idle<select name='idleMode'>");
  h += opt(0, cfg.idleMode, "Stay on") + opt(1, cfg.idleMode, "Dim") + opt(2, cfg.idleMode, "Turn screen off");
  h += F("</select></label><label>Idle after (minutes)<input type='number' min='1' max='600' name='idleMin' value='");
  h += String(cfg.idleMinutes);
  h += F("'><span class='hint'>Touch the screen to wake it.</span></label>"
         "<label class='check' style='grid-column:1/-1'><input type='checkbox' name='nightDim' value='1'");
  if (cfg.nightDim) h += F(" checked");
  h += F("> Dim automatically at night</label><label>Night starts (hour)<input type='number' min='0' max='23' name='nightFrom' value='");
  h += String(cfg.nightFrom);
  h += F("'></label><label>Night ends (hour)<input type='number' min='0' max='23' name='nightTo' value='");
  h += String(cfg.nightTo);
  h += F("'></label><label>Footer rotation (seconds)<input type='number' min='2' max='30' name='footSec' value='");
  h += String(cfg.footerSeconds);
  h += F("'></label><label>Start-up screen<select name='startScr'>");
  h += opt(0, cfg.startScreen, "Forecast") + opt(1, cfg.startScreen, "Weather") + opt(2, cfg.startScreen, "Status");
  h += F("</select><span class='hint'>Also where the display goes back to after 2 minutes untouched.</span></label></div></section>");

  h += F("<section><h2>Scoring</h2><div class='grid'><label>Good hour: cloud up to (%)<input type='number' min='0' max='60' name='goodCloud' value='");
  h += String(cfg.goodMaxCloud);
  h += F("'></label><label>Good hour: transparency at least<select name='goodTrans'>");
  h += opt(2, cfg.goodMinTrans, "Below average") + opt(3, cfg.goodMinTrans, "Average") + opt(4, cfg.goodMinTrans, "Above average") + opt(5, cfg.goodMinTrans, "Transparent");
  h += F("</select></label><label>GO needs this many good hours in a row<input type='number' min='1' max='12' name='goHours' value='");
  h += String(cfg.goMinHours);
  h += F("'></label></div></section>");

  h += F("<section><h2>Weather Underground</h2><div class='grid'><label>Station ID<input name='wuStation' autocapitalize='characters' value='");
  h += esc(cfg.wuStation);
  h += F("'><span class='hint'>Your personal weather station ID, e.g. KCASANFR123.</span></label>"
         "<label>API key<input type='password' name='wuKey' autocomplete='off' placeholder='");
  if (cfg.wuKey.length() >= 4) h += "(set, ends in " + esc(cfg.wuKey.substring(cfg.wuKey.length() - 4)) + ")";
  else h += F("(not set)");
  h += F("'><span class='hint'>From your Weather Underground account, under API Keys. Leave blank to keep the current key.</span></label>"
         "<label class='check'><input type='checkbox' name='clearWuKey' value='1'> Remove the API key</label>"
         "</div></section>");

  h += F("<section><h2>Device</h2><div class='grid'><label>Network name (hostname)<input name='host' value='");
  h += esc(cfg.hostname);
  h += F("'><span class='hint'>Settings page at http://NAME/ or http://NAME.local after a restart.</span></label>"
         "<label>Settings page password<input type='password' name='webPass' autocomplete='new-password' placeholder='");
  h += cfg.webPassword.length() ? "(unchanged)" : "(none)";
  h += F("'><span class='hint'>User name is admin. Leave blank to keep the current one.</span></label>"
         "<label class='check'><input type='checkbox' name='clearPass' value='1'> Remove the password</label>"
         "</div></section><div class='row'><button class='primary' type='submit'>Save settings</button></div></form>");

  h += F("<section><h2>Wi-Fi</h2><table><tr><td>Network</td><td>");
  h += esc(WiFi.SSID());
  h += F("</td></tr><tr><td>Signal</td><td>");
  h += String(WiFi.RSSI()) + " dBm";
  h += F("</td></tr><tr><td>IP address</td><td>");
  h += WiFi.localIP().toString();
  h += F("</td></tr><tr><td>MAC</td><td>");
  h += WiFi.macAddress();
  h += F("</td></tr></table><form method='post' action='/wifi' style='margin-top:12px'><div class='grid'>"
         "<label>New network name<input name='ssid' required></label>"
         "<label>Password<input type='password' name='pass' autocomplete='off'></label></div>"
         "<div class='row' style='margin-top:10px'><button type='submit'>Connect to this network</button></div>"
         "<p class='hint'>If it can't connect, the display opens its setup network (" SETUP_AP_NAME ") so you can try again.</p></form>"
         "<form method='post' action='/forget'><button class='warn' type='submit'>Forget Wi-Fi and open setup network</button></form></section>");

  h += F("<section><h2>Maintenance</h2><div class='row'>"
         "<form method='post' action='/check'><button type='submit'>Check chart and weather now</button></form>"
         "<a class='btn' href='/firmware'>Update firmware</a>"
         "<form method='post' action='/restart'><button type='submit'>Restart</button></form></div>"
         "<p class='hint'>Firmware " FIRMWARE_VERSION " for the " BOARD_NAME ".</p></section>"
         "</main></body></html>");
  server.send(200, "text/html", h);
}

// Firmware update: our page layout, framing ElegantOTA's own page (/update) switched to its dark theme.
// /update, /ota/start and /ota/upload still work on their own for scripts.
static void handleFirmware() {
  if (!authOk()) return;
  String h; h.reserve(3500);
  h += F("<!doctype html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>SkyDisplay firmware</title><style>");
  h += FPSTR(CSS);
  h += F("iframe{width:100%;height:480px;border:0;background:transparent;display:block}code{color:var(--fg)}</style></head><body><main>"
         "<header><h1>Firmware update</h1><div class='status'>Running " FIRMWARE_VERSION " on the " BOARD_NAME ". <a href='/' style='color:var(--accent)'>Back to settings</a></div></header>"
         "<section><p class='hint' style='margin:0 0 8px'>Upload a <code>.bin</code> built for the " BOARD_NAME
         " (Arduino IDE: Sketch &gt; Export Compiled Binary, the file without &quot;merged&quot; in its name). Settings are kept. The display restarts when the upload finishes.</p>"
         "<iframe id='ota' src='/update' title='Firmware upload' onload='otaLoaded()'></iframe></section></main><script>"
         "try{localStorage.setItem('theme','dark')}catch(e){}"  // ElegantOTA reads this for its dark mode
         // color-scheme must match ours, or Chromium paints an opaque white backdrop behind the frame
         "function otaLoaded(){var f=document.getElementById('ota');try{var d=f.contentDocument;d.documentElement.classList.add('dark');"
         "var st=d.createElement('style');st.textContent=':root{color-scheme:dark}html,body,body>div{background:transparent!important}';d.head.appendChild(st);}catch(e){}}"
         "setInterval(function(){var f=document.getElementById('ota');try{var h=f.contentDocument.documentElement.scrollHeight;"
         "if(h>100&&Math.abs(h-f.offsetHeight)>4)f.style.height=h+'px';}catch(e){}},500);"
         "</script></body></html>");
  server.send(200, "text/html", h);
}

static String arg(const char* n) { String v = server.arg(n); v.trim(); return v; }
static int argInt(const char* n, int lo, int hi, int def) {
  if (!server.hasArg(n) || !server.arg(n).length()) return def;
  return constrain(server.arg(n).toInt(), lo, hi);
}
static void redirect(const char* to) { server.sendHeader("Location", to); server.send(303, "text/plain", ""); }

static void handleSave() {
  if (!authOk()) return;
  String oldUrl = cfg.chartUrl, oldTz = cfg.tz, oldN1 = cfg.ntp1, oldN2 = cfg.ntp2;
  int oldNtpHours = cfg.ntpHours;
  double oldLat = cfg.lat, oldLon = cfg.lon;
  if (arg("chartUrl").length()) {
    cfg.chartUrl = arg("chartUrl");
    if (cfg.chartUrl.indexOf("://") < 0) cfg.chartUrl.toLowerCase();  // bare chart IDs are lower case
  }
  cfg.pollMinutes = argInt("poll", 10, 240, cfg.pollMinutes);
  String la = arg("lat"), lo = arg("lon");
  if (la.length() && lo.length()) { cfg.lat = constrain(la.toDouble(), -90.0, 90.0); cfg.lon = constrain(lo.toDouble(), -180.0, 180.0); }
  else { cfg.lat = 0; cfg.lon = 0; }
  if (arg("tz").length()) cfg.tz = arg("tz");
  if (arg("ntp1").length()) cfg.ntp1 = arg("ntp1");
  cfg.ntp2 = arg("ntp2");
  cfg.ntpHours = argInt("ntpHours", 1, 24, cfg.ntpHours);
  cfg.brightness = argInt("bright", 1, 255, cfg.brightness);
  cfg.dimBrightness = argInt("dimBright", 1, 255, cfg.dimBrightness);
  cfg.idleMode = argInt("idleMode", 0, 2, cfg.idleMode);
  cfg.idleMinutes = argInt("idleMin", 1, 600, cfg.idleMinutes);
  cfg.nightDim = server.hasArg("nightDim");
  cfg.nightFrom = argInt("nightFrom", 0, 23, cfg.nightFrom);
  cfg.nightTo = argInt("nightTo", 0, 23, cfg.nightTo);
  cfg.footerSeconds = argInt("footSec", 2, 30, cfg.footerSeconds);
  cfg.startScreen = argInt("startScr", 0, 2, cfg.startScreen);
  cfg.goodMaxCloud = argInt("goodCloud", 0, 60, cfg.goodMaxCloud);
  cfg.goodMinTrans = argInt("goodTrans", 2, 5, cfg.goodMinTrans);
  cfg.goMinHours = argInt("goHours", 1, 12, cfg.goMinHours);
  if (arg("wuStation").length()) { cfg.wuStation = arg("wuStation"); cfg.wuStation.toUpperCase(); }
  if (server.hasArg("clearWuKey")) cfg.wuKey = "";
  else if (arg("wuKey").length()) cfg.wuKey = arg("wuKey");
  if (arg("host").length()) cfg.hostname = arg("host");
  if (server.hasArg("clearPass")) cfg.webPassword = "";
  else if (server.arg("webPass").length()) cfg.webPassword = server.arg("webPass");
  settingsSave();
  bool chartChanged = cfg.chartUrl != oldUrl;
  bool timeChanged = cfg.tz != oldTz || cfg.ntp1 != oldN1 || cfg.ntp2 != oldN2 || cfg.ntpHours != oldNtpHours
                  || cfg.lat != oldLat || cfg.lon != oldLon;
  hooks.applySettings(chartChanged, timeChanged);
  redirect("/?saved=1");
}

static void handleWifi() {
  if (!authOk()) return;
  String ssid = arg("ssid"), pass = server.arg("pass");
  if (!ssid.length()) { redirect("/"); return; }
  server.send(200, "text/html", "<!doctype html><meta name='viewport' content='width=device-width'><body style='font-family:system-ui;background:#070a12;color:#e2e8f2;padding:20px'>"
              "Switching to <b>" + esc(ssid) + "</b>. The display restarts; if it can't connect, join the <b>" SETUP_AP_NAME "</b> network to set Wi-Fi again.</body>");
  delay(500);
  WiFi.persistent(true);
  WiFi.disconnect(false, false);
  WiFi.begin(ssid.c_str(), pass.c_str());  // saves the new credentials
  delay(1500);
  ESP.restart();
}

static void handleForget() {
  if (!authOk()) return;
  server.send(200, "text/plain", "Wi-Fi forgotten. Join the " SETUP_AP_NAME " network to set it up again.");
  delay(500);
  WiFiManager wm; wm.resetSettings();
  ESP.restart();
}

void webBegin(const WebHooks& h) {
  hooks = h;
  if (MDNS.begin(cfg.hostname.c_str())) MDNS.addService("http", "tcp", 80);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/check", HTTP_POST, [] { if (!authOk()) return; hooks.checkNow(); redirect("/"); });
  server.on("/restart", HTTP_POST, [] { if (!authOk()) return; server.send(200, "text/plain", "Restarting..."); delay(500); ESP.restart(); });
  // Firmware upload (ElegantOTA at /update, framed by /firmware). Same password as the settings page (user admin), if set.
  server.on("/firmware", HTTP_GET, handleFirmware);
  ElegantOTA.begin(&server);
  if (cfg.webPassword.length()) ElegantOTA.setAuth("admin", cfg.webPassword.c_str());
  ElegantOTA.onStart([] {
    updating = true;
    Serial.println("[ota] update started");
    uiMessage("Updating firmware", "Receiving the new firmware...", "Keep the display plugged in.");
#if BLANK_DURING_FLASH_WRITES
    uiSetBacklight(0);  // the picture tears while flash is written; the browser shows the progress instead
#endif
  });
  ElegantOTA.onProgress([](size_t done, size_t) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 2000) { lastLog = millis(); Serial.printf("[ota] %u KB\n", (unsigned)(done / 1024)); }
  });
  ElegantOTA.onEnd([](bool ok) {
    Serial.printf("[ota] update %s\n", ok ? "finished, restarting" : "FAILED");
#if BLANK_DURING_FLASH_WRITES
    // The panel's refresh lost its timing during the flash writes and only a restart puts it right,
    // so restart either way (after a failure, the current firmware simply starts again).
    if (!ok) ESP.restart();
#endif
    if (ok) uiMessage("Update finished", "Restarting...");
    else { uiMessage("Update failed", "The current firmware is still installed."); updating = false; }
  });
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.printf("[web] settings at http://%s.local/ and http://%s/\n", cfg.hostname.c_str(), WiFi.localIP().toString().c_str());
}

void webLoop() { server.handleClient(); ElegantOTA.loop(); }
bool webUpdating() { return updating; }
