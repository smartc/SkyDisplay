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
#include "obs.h"
#include "po.h"
#include "sound.h"

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
.rigrow{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.4fr) minmax(0,1.4fr) auto;gap:8px 10px;align-items:center;margin-bottom:8px}
.righead{font-size:.88rem;color:var(--muted);margin-bottom:4px}
@media(max-width:620px){.rigrow{grid-template-columns:minmax(0,1fr) auto}.rigrow input:nth-child(1),.rigrow input:nth-child(2){grid-column:1/-1}.righead{display:none}.rigrow{padding-bottom:8px;border-bottom:1px solid var(--line)}}
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
  if (server.hasArg("po")) {  // result of linking or unlinking the notifications screen
    String m = server.arg("po");
    bool ok = m == "linked" || m == "unlinked" || m == "cleared";
    h += "<section" + String(ok ? "" : " style='border-color:var(--warn)'") + "><b>Notifications:</b> ";
    if (m == "linked") h += F("this display is now a device on your Pushover account. Turn on the screen below if it isn't.");
    else if (m == "unlinked") h += F("unlinked. The device stays listed in your Pushover account until you delete it there.");
    else if (m == "cleared") h += F("stored messages cleared.");
    else if (m == "2fa") h += F("your account uses two-factor login. Enter the code from your authenticator app with your email and password, then link again.");
    else h += esc(m);
    h += F("</section>");
  }

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
  h += opt(0, cfg.startScreen, "Clear Dark Sky") + opt(1, cfg.startScreen, "Weather") + opt(3, cfg.startScreen, "Observatory") + opt(4, cfg.startScreen, "Notifications") + opt(2, cfg.startScreen, "Status");
  h += F("</select></label><label>Return to it after (minutes)<input type='number' min='0' max='120' name='returnMin' value='");
  h += String(cfg.returnMinutes);
  h += F("'><span class='hint'>Other screens go back to the start-up screen after this long untouched. 0 = stay put."
         " An unacknowledged emergency notification always stays on screen.</span></label></div></section>");

  h += F("<section><h2>Scoring</h2><div class='grid'><label>Good hour: cloud up to (%)<input type='number' min='0' max='60' name='goodCloud' value='");
  h += String(cfg.goodMaxCloud);
  h += F("'></label><label>Good hour: transparency at least<select name='goodTrans'>");
  h += opt(2, cfg.goodMinTrans, "Below average") + opt(3, cfg.goodMinTrans, "Average") + opt(4, cfg.goodMinTrans, "Above average") + opt(5, cfg.goodMinTrans, "Transparent");
  h += F("</select></label><label>GO needs this many good hours in a row<input type='number' min='1' max='12' name='goHours' value='");
  h += String(cfg.goMinHours);
  h += F("'></label></div></section>");

  h += F("<section><h2>Weather</h2><div class='grid'>"
         "<label style='grid-column:1/-1'>EcoWitt gateway (optional)<input name='ecowitt' autocapitalize='none' autocorrect='off' spellcheck='false' value='");
  h += esc(cfg.ecowittHost);
  h += F("'><span class='hint'>Address of an EcoWitt gateway (e.g. GW1100, GW2000, GW3000) on this network. When set, current conditions"
         " come from it every minute, with no Weather Underground calls; the forecast still comes from Weather Underground.</span></label>"
         "<label>Weather Underground station ID<input name='wuStation' autocapitalize='characters' value='");
  h += esc(cfg.wuStation);
  h += F("'><span class='hint'>Your personal weather station ID, e.g. KCASANFR123. Used for current conditions when there's no EcoWitt"
         " gateway, and once at start-up to fill in the 24-hour trace.</span></label>"
         "<label>API key<input type='password' name='wuKey' autocomplete='off' placeholder='");
  if (cfg.wuKey.length() >= 4) h += "(set, ends in " + esc(cfg.wuKey.substring(cfg.wuKey.length() - 4)) + ")";
  else h += F("(not set)");
  h += F("'><span class='hint'>From your Weather Underground account, under API Keys; needed for the forecast. Leave blank to keep the current key."
         " Each display uses about 24 calls a day with EcoWitt, about 170 without (the free limit is 1,500 a day per key).</span></label>"
         "<label class='check'><input type='checkbox' name='clearWuKey' value='1'> Remove the API key</label>"
         "</div></section>");

  // ---- observatory ----
  h += F("<section><h2>Observatory</h2><div class='grid'><label class='check' style='grid-column:1/-1'><input type='checkbox' name='obsEn' value='1'");
  if (cfg.obsEnabled) h += F(" checked");
  h += F("> Show the observatory screen</label>"
         "<p class='hint' style='grid-column:1/-1;margin:0'>ASCOM Alpaca devices: write <code>host</code>, <code>host:port</code> or <code>host:port/device</code>"
         " (port 11111 and device 0 if left out; https:// for a device behind a proxy). Leave a field blank to leave it off the screen.</p>");
  struct { const char* name; const char* label; const String* val; const char* hint; } dev[] = {
    {"oRoof", "Roof (Alpaca Dome)", &cfg.obsRoof, ""},
    {"oSafe", "Safety verdict (Alpaca SafetyMonitor)", &cfg.obsSafety, "When it reports unsafe, Llama Watch's reason is shown too."},
    {"oRain", "Rain sensor", &cfg.obsRain, "An Alpaca SafetyMonitor, or udp to use a sensor's UDP safety broadcast (udp:name picks one)."},
    {"oSky", "Sky conditions (Alpaca ObservingConditions)", &cfg.obsSky, "Cloud cover, SQM, sky brightness and sky temperature."}};
  for (auto& d : dev) {
    h += "<label>" + String(d.label) + "<input name='" + d.name + "' id='" + d.name + "' autocapitalize='none' autocorrect='off' spellcheck='false' value='" + esc(*d.val) + "'>";
    if (*d.hint) h += "<span class='hint'>" + String(d.hint) + "</span>";
    h += "</label>";
  }
  h += F("<div style='grid-column:1/-1'><div class='row'><button type='button' id='findBtn'>Find Alpaca devices</button>"
         "<span class='hint' id='findMsg'>Searches this network (takes a few seconds).</span></div><div id='found' style='margin-top:8px'></div></div>"
         "<p class='hint' style='grid-column:1/-1;margin:0'>Mount park sensors and safety sensors that broadcast on UDP port 23435 (Dark Dragons format) appear by themselves."
         " Rigs use NINA's Advanced API plugin: <code>host</code> or <code>host:port</code> (port 1888 if left out)."
         " A rig's park sensor is optional: the name of the sensor on its pier, as heard on the UDP broadcast (suggested as you type"
         " once heard), or an Alpaca Switch address (<code>host:port</code> or <code>host:port/device</code>; add <code>#N</code>"
         " for a switch other than 0; on means parked).</p>");
  // Rigs: one row each (name, NINA address, Remove), plus Add rig. Rows are renumbered rN0/rH0... before saving.
  h += F("<div style='grid-column:1/-1'><h3 style='font-size:.95rem;margin:4px 0 8px'>Rigs</h3>"
         "<div class='rigrow righead'><span>Name</span><span>NINA address</span><span>Park sensor (optional)</span><span></span></div><div id='rigs'>");
  const char* ROW_A = "<div class='rigrow'><input aria-label='Rig name' placeholder='e.g. East pier' value='";
  const char* ROW_B = "'><input aria-label='NINA address' placeholder='host or host:port' autocapitalize='none' autocorrect='off' spellcheck='false' value='";
  const char* ROW_P = "'><input aria-label='Park sensor' list='parkList' placeholder='Sensor name' autocapitalize='none' autocorrect='off' spellcheck='false' value='";
  const char* ROW_C = "'><button type='button' class='rm'>Remove</button></div>";
  int shown = 0;
  for (int i = 0; i < Settings::MAX_RIGS; i++) {
    if (!cfg.rigHost[i].length() && !cfg.rigName[i].length()) continue;
    h += ROW_A + esc(cfg.rigName[i]) + ROW_B + esc(cfg.rigHost[i]) + ROW_P + esc(cfg.rigPark[i]) + ROW_C; shown++;
  }
  if (!shown) h += String(ROW_A) + ROW_B + ROW_P + ROW_C;  // start with one empty row
  h += F("</div><div class='row'><button type='button' id='addRig'>Add rig</button><span class='hint' id='rigMax'></span></div></div>");
  h += "<template id='rigTpl'>" + String(ROW_A) + ROW_B + ROW_P + ROW_C + "</template>";
  {
    String heard[OBS_MAX_PARK]; int nh = obsHeardParkSensors(heard, OBS_MAX_PARK);
    h += "<datalist id='parkList'>";
    for (int i = 0; i < nh; i++) h += "<option value='" + esc(heard[i]) + "'>";
    h += "</datalist>";
  }
  h += "<script>(()=>{const MAX=" + String(Settings::MAX_RIGS) + ",list=document.getElementById('rigs'),add=document.getElementById('addRig'),note=document.getElementById('rigMax');"
       "const sync=()=>{[...list.children].forEach((r,i)=>{const [n,a,p]=r.querySelectorAll('input');n.name='rN'+i;a.name='rH'+i;p.name='rP'+i;});"
       "const full=list.children.length>=MAX;add.disabled=full;note.textContent=full?'Up to '+MAX+' rigs.':'';};"
       "list.addEventListener('click',e=>{if(e.target.classList.contains('rm')){e.target.parentElement.remove();sync();}});"
       "add.addEventListener('click',()=>{list.appendChild(document.getElementById('rigTpl').content.cloneNode(true));sync();list.lastElementChild.querySelector('input').focus();});"
       "sync();})();</script>";
  h += F("<label>Show a finished night's run until (hour)<input type='number' min='0' max='23' name='oHold' value='");
  h += String(cfg.obsHoldHour);
  h += F("'><span class='hint'>NINA only runs while imaging. After a run the rig shows Finished (or Lost, if NINA stopped mid-sequence) until this hour, then Inactive.</span></label></div></section>"
         "<script>document.getElementById('findBtn').addEventListener('click',async()=>{const m=document.getElementById('findMsg'),f=document.getElementById('found');"
         "m.textContent='Searching...';f.innerHTML='';try{const r=await fetch('/alpaca/discover');const l=await r.json();"
         "m.textContent=l.length?l.length+' found. Pick what each one is for, then Save settings.':'None found. Devices on another network or VLAN have to be typed in.';"
         "const use={Dome:[['oRoof','Use as roof']],SafetyMonitor:[['oSafe','Use as safety'],['oRain','Use as rain sensor']],ObservingConditions:[['oSky','Use as sky conditions']]};"
         "for(const d of l){const row=document.createElement('div');row.className='row';row.style.margin='4px 0';"
         "const t=document.createElement('span');t.textContent=d.name+' ('+d.type+') at '+d.addr;row.appendChild(t);"
         "for(const [id,lab] of (use[d.type]||[])){const b=document.createElement('button');b.type='button';b.textContent=lab;"
         "b.onclick=()=>{document.getElementById(id).value=d.addr;b.textContent='Set';};row.appendChild(b);}f.appendChild(row);}}"
         "catch(e){m.textContent='Search failed: '+e;}});</script>");

  // ---- notifications (unofficial Pushover Open Client) ----
  // The link fields belong to a separate form (#poLink, after the main form) through their form= attribute.
  bool poLinked = cfg.poSecret.length() && cfg.poDevice.length();
  h += F("<section><h2>Notifications</h2><div class='grid'><label class='check' style='grid-column:1/-1'><input type='checkbox' name='poEn' value='1'");
  if (cfg.poEnabled) h += F(" checked");
  h += F("> Show the notifications screen</label><label class='check' style='grid-column:1/-1'><input type='checkbox' name='poListen' value='1'");
  if (cfg.poListen) h += F(" checked");
  h += F("> Monitoring on <span class='hint'>(off: disconnected from Pushover and silent; messages wait there and arrive"
         " quietly when it's back on. Also switched on the notifications screen.)</span></label>"
         "<label>When a message arrives<select name='poWake'>");
  h += opt(0, cfg.poWake, "Count it, don't wake the screen") + opt(1, cfg.poWake, "High priority: wake and show it") + opt(2, cfg.poWake, "Always wake and show it");
  h += F("</select><span class='hint'>Emergency messages stay until you tap to acknowledge them.</span></label><div><span class='hint'>Status</span><div>");
  {
    DataGuard g;
    h += poLinked ? "Linked as <b>" + esc(cfg.poDeviceName) + "</b>, " + esc(poStateText()) : String("Not linked");
  }
  h += F("</div></div>");
#if BOARD_HAS_SOUND
  h += F("<label>Alarm sound for<select name='alLevel'>");
  h += opt(1, cfg.alarmLevel, "Emergency messages") + opt(2, cfg.alarmLevel, "High priority and emergency") + opt(0, cfg.alarmLevel, "Nothing (silent)");
  h += F("</select><span class='hint'>Emergency: beep-beep every 3 s until you tap the screen (5 minutes at most). High priority: one short chime.</span></label>"
         "<label class='check'><input type='checkbox' name='armed' value='1'");
  if (cfg.soundArmed) h += F(" checked");
  h += F("> Sound on <span class='hint'>(also switched on the notifications screen)</span></label><div><span class='hint'>Buzzer</span><div>");
  h += esc(soundStatus());
  h += F("</div></div>");
#endif
  if (!poLinked) {
    String def; for (char c : cfg.hostname) if (isalnum((uint8_t)c) || c == '-' || c == '_') def += c;
    h += F("<p class='hint' style='grid-column:1/-1;margin:0'>The display shows messages sent to your <a href='https://pushover.net' style='color:var(--accent)'>Pushover</a>"
           " account by registering as a device on it. Pushover licenses this like a desktop client: free for 30 days, then a one-time"
           " Pushover for Desktop licence (from pushover.net) covers it. Your password is used once to log in and is not stored."
           " SkyDisplay is an unofficial Open Client, not released or supported by Pushover.</p>"
           "<label>Pushover email<input form='poLink' name='email' type='email' autocomplete='username' autocapitalize='none'></label>"
           "<label>Password<input form='poLink' name='pass' type='password' autocomplete='current-password'></label>"
           "<label>Two-factor code (if you use it)<input form='poLink' name='twofa' inputmode='numeric' autocomplete='one-time-code'></label>"
           "<label>Device name<input form='poLink' name='name' maxlength='25' pattern='[A-Za-z0-9_-]{1,25}' value='");
    h += esc(def.substring(0, 25));
    h += F("'><span class='hint'>How the display appears in Pushover; letters, numbers, - and _ only. Send to it by name.</span></label>"
           "<div class='row' style='grid-column:1/-1'><button type='submit' form='poLink'>Link this display</button>"
           "<span class='hint'>Takes a few seconds.</span></div>");
  } else {
    h += F("<div class='row' style='grid-column:1/-1'><button type='submit' form='poUnlink' class='warn'>Unlink</button>"
           "<button type='submit' form='poClear'>Clear stored messages</button></div>");
  }
  h += F("</div></section>");

  h += F("<section><h2>Device</h2><div class='grid'><label>Network name (hostname)<input name='host' value='");
  h += esc(cfg.hostname);
  h += F("'><span class='hint'>Settings page at http://NAME/ or http://NAME.local after a restart.</span></label>"
         "<label>Settings page password<input type='password' name='webPass' autocomplete='new-password' placeholder='");
  h += cfg.webPassword.length() ? "(unchanged)" : "(none)";
  h += F("'><span class='hint'>User name is admin. Leave blank to keep the current one.</span></label>"
         "<label class='check'><input type='checkbox' name='clearPass' value='1'> Remove the password</label>"
         "</div></section><div class='row'><button class='primary' type='submit'>Save settings</button></div></form>"
         "<form id='poLink' method='post' action='/pushover/link'></form><form id='poUnlink' method='post' action='/pushover/unlink'></form>"
         "<form id='poClear' method='post' action='/pushover/clear'></form>");

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
static String urlParam(const String& v) {  // percent-encode for a query string
  String o;
  for (char c : v) { if (isalnum((uint8_t)c) || c == '-' || c == '_' || c == '.') o += c; else { char b[4]; snprintf(b, sizeof b, "%%%02X", (uint8_t)c); o += b; } }
  return o;
}

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
  cfg.startScreen = argInt("startScr", 0, 4, cfg.startScreen);
  cfg.returnMinutes = argInt("returnMin", 0, 120, cfg.returnMinutes);
  bool poWas = cfg.poEnabled;
  cfg.poEnabled = server.hasArg("poEn");
  cfg.poWake = argInt("poWake", 0, 2, cfg.poWake);
  bool listenWas = cfg.poListen;
  cfg.poListen = server.hasArg("poListen");
#if BOARD_HAS_SOUND
  cfg.alarmLevel = argInt("alLevel", 0, 2, cfg.alarmLevel);
  cfg.soundArmed = server.hasArg("armed");
  if (!cfg.soundArmed) soundStop();
#endif
  cfg.obsEnabled = server.hasArg("obsEn");
  cfg.obsRoof = arg("oRoof"); cfg.obsSafety = arg("oSafe"); cfg.obsRain = arg("oRain"); cfg.obsSky = arg("oSky");
  {
    int n = 0;  // rows without an address are dropped; the rest move up so the list has no gaps
    for (int i = 0; i < Settings::MAX_RIGS; i++) {
      String name = arg(("rN" + String(i)).c_str()), host = arg(("rH" + String(i)).c_str()), park = arg(("rP" + String(i)).c_str());
      if (!host.length()) continue;
      cfg.rigName[n] = name; cfg.rigHost[n] = host; cfg.rigPark[n] = park; n++;
    }
    for (int i = n; i < Settings::MAX_RIGS; i++) { cfg.rigName[i] = ""; cfg.rigHost[i] = ""; cfg.rigPark[i] = ""; }
  }
  cfg.obsHoldHour = argInt("oHold", 0, 23, cfg.obsHoldHour);
  cfg.goodMaxCloud = argInt("goodCloud", 0, 60, cfg.goodMaxCloud);
  cfg.goodMinTrans = argInt("goodTrans", 2, 5, cfg.goodMinTrans);
  cfg.goMinHours = argInt("goHours", 1, 12, cfg.goMinHours);
  if (arg("wuStation").length()) { cfg.wuStation = arg("wuStation"); cfg.wuStation.toUpperCase(); }
  if (server.hasArg("ecowitt")) cfg.ecowittHost = arg("ecowitt");
  if (server.hasArg("clearWuKey")) cfg.wuKey = "";
  else if (arg("wuKey").length()) cfg.wuKey = arg("wuKey");
  if (arg("host").length()) cfg.hostname = arg("host");
  if (server.hasArg("clearPass")) cfg.webPassword = "";
  else if (server.arg("webPass").length()) cfg.webPassword = server.arg("webPass");
  settingsSave();
  if (cfg.obsEnabled) obsBegin();
  if (cfg.poEnabled != poWas || cfg.poListen != listenWas) poRestart();
  if (!cfg.poListen) soundStop();
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
  // Notifications: link this display to a Pushover account (the password is passed on once, never stored), unlink, clear.
  server.on("/pushover/link", HTTP_POST, [] {
    if (!authOk()) return;
    String name = server.arg("name"); name.trim();
    if (!name.length()) name = cfg.hostname;
    String r = poLink(server.arg("email"), server.arg("pass"), server.arg("twofa"), name.substring(0, 25));
    redirect(("/?po=" + urlParam(r.length() ? r : String("linked"))).c_str());
  });
  server.on("/pushover/unlink", HTTP_POST, [] { if (!authOk()) return; poUnlink(); redirect("/?po=unlinked"); });
  server.on("/pushover/clear", HTTP_POST, [] { if (!authOk()) return; poClear(); redirect("/?po=cleared"); });
  server.on("/restart", HTTP_POST, [] { if (!authOk()) return; server.send(200, "text/plain", "Restarting..."); delay(500); ESP.restart(); });
  // Firmware upload (ElegantOTA at /update, framed by /firmware). Same password as the settings page (user admin), if set.
  server.on("/firmware", HTTP_GET, handleFirmware);
  // Raw RGB565 dump of the screen (same format as the serial 's' command; tools/shot_to_png.py converts it).
  // ?screen=0..3 switches the display to that screen first.
  server.on("/screenshot", HTTP_GET, [] {
    if (!authOk()) return;
    if (server.hasArg("screen")) hooks.showScreen(server.arg("screen").toInt());
    WiFiClient cl = server.client();
    cl.print("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
    uiScreenshot(cl);
    cl.stop();
  });
  server.on("/alpaca/discover", HTTP_GET, [] { if (!authOk()) return; server.send(200, "application/json", obsDiscoverJson()); });
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
