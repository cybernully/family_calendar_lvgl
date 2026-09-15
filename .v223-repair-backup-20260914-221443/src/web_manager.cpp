#include "web_manager.h"

#include "app_config.h"
#include "board_lvgl.h"
#include "alarm_service.h"
#include "chore_service.h"
#include "home_assistant.h"
#include "network_service.h"
#include "weather_service.h"
#include "runtime_config.h"
#include "battery_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SHA2Builder.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

namespace {

WebServer g_server(WEB_MANAGER_PORT);
String g_hostname = WEB_MANAGER_HOSTNAME;
String g_admin_user = WEB_MANAGER_DEFAULT_USER;
String g_admin_password = WEB_MANAGER_DEFAULT_PASSWORD;
char g_weather_current_daily_entity[96] = HA_WEATHER_CURRENT_DAILY_ENTITY;
char g_weather_hourly_entity[96] = HA_WEATHER_HOURLY_ENTITY;
portMUX_TYPE g_config_mux = portMUX_INITIALIZER_UNLOCKED;
bool g_routes_registered = false;
bool g_mdns_started = false;
uint32_t g_last_mdns_attempt_ms = 0;
bool g_maintenance = false;
bool g_ota_in_progress = false;
bool g_ota_accepting = false;
bool g_ota_succeeded = false;
bool g_ota_dimmed = false;
uint8_t g_ota_restore_backlight = APP_DEFAULT_BACKLIGHT;
String g_ota_error;
String g_ota_stream_sha256;
String g_ota_readback_sha256;
size_t g_ota_expected_size = 0;
size_t g_ota_written = 0;
uint8_t g_ota_last_logged_percent = 0;
const esp_partition_t *g_ota_target_partition = nullptr;
SHA256Builder g_ota_stream_hash;
uint32_t g_reboot_at_ms = 0;
String g_scheduled_reboot_source;
String g_reset_reason = "unknown";
String g_last_reboot_source = "none";
uint32_t g_boot_count = 0;
uint32_t g_brownout_count = 0;
String g_last_ota_result = "none";
String g_last_ota_from;
String g_last_ota_to;
String g_last_ota_sha256;
String g_last_ota_error;
uint32_t g_last_ota_checkpoint_expected = 0;
uint32_t g_last_ota_checkpoint_written = 0;
String g_last_ota_checkpoint_stage;
uint8_t g_ota_last_persisted_percent = 0;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Family Hub Management</title>
<style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,sans-serif}*{box-sizing:border-box}body{margin:0;background:#0f172a;color:#e5e7eb}.wrap{max-width:1100px;margin:auto;padding:24px}.card{background:#172033;border:1px solid #334155;border-radius:14px;padding:18px;margin:0 0 16px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}.stat{background:#111827;border-radius:10px;padding:12px}.k{color:#94a3b8;font-size:.82rem}.v{font-size:1.02rem;font-weight:650;margin-top:3px;word-break:break-word}h1{margin:.2rem 0 1rem}h2{margin:.1rem 0 1rem;font-size:1.15rem}h3{margin:.4rem 0 .8rem;font-size:1rem}label{display:block;margin:.7rem 0 .3rem;color:#cbd5e1}input,select,button{font:inherit;border-radius:8px;border:1px solid #475569;padding:10px;background:#0f172a;color:#f8fafc}input:not([type=color]),select{width:100%}input[type=color]{width:56px;height:42px;padding:3px}button{cursor:pointer;background:#2563eb;border:0;padding:10px 16px;font-weight:650}button.secondary{background:#334155}button.danger{background:#b91c1c}button:disabled{opacity:.55;cursor:not-allowed}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.person{display:grid;grid-template-columns:1fr 70px 2fr;gap:10px;align-items:end;margin-bottom:8px}.muted{color:#94a3b8;font-size:.9rem}.warn{color:#fbbf24}.ok{color:#4ade80}.bad{color:#f87171}.result{white-space:pre-wrap;background:#111827;border-radius:8px;padding:10px;margin-top:10px;min-height:38px}progress{width:100%;height:18px}code{background:#111827;padding:2px 5px;border-radius:5px}@media(max-width:700px){.wrap{padding:12px}.person{grid-template-columns:1fr 64px}.person .cal{grid-column:1/-1}}
</style></head><body><div class="wrap">
<h1>Family Hub <span id="version" class="muted"></span></h1>
<div class="card"><h2>Device status</h2><div class="grid" id="stats"></div><p id="defaultPw" class="warn" hidden>Default web password is still in use. Change it below.</p></div>

<div class="card"><h2>Operations & diagnostics</h2><div class="row"><button onclick="testHA()">Test Home Assistant</button><button class="secondary" onclick="testWeather()">Test weather sources</button><button class="secondary" onclick="forceRefresh('calendar')">Refresh calendar</button><button class="secondary" onclick="forceRefresh('chores')">Refresh chores</button><button class="secondary" onclick="forceRefresh('weather')">Refresh weather</button><button class="secondary" onclick="window.open('/api/diagnostics','_blank')">Open diagnostics JSON</button></div><div id="diagMsg" class="result muted">No diagnostic test run yet.</div></div>

<div class="card"><h2>Basic configuration</h2><form id="cfg">
<div class="grid"><div><label>mDNS hostname</label><input id="hostname" maxlength="31"><div class="muted">Browse to <code>http://hostname.local</code> after reboot.</div></div><div><label>Admin username</label><input id="username" maxlength="31"></div><div><label>New admin password</label><input id="password" type="password" maxlength="63" placeholder="Leave blank to keep current"></div><div><label>Backlight</label><input id="backlight" type="number" min="10" max="100"></div><div><label>Theme</label><select id="dark"><option value="0">Light</option><option value="1">Dark</option></select></div><div><label>Screen timeout (seconds)</label><select id="timeout"><option value="0">Off</option><option value="30">30</option><option value="60">60</option><option value="120">120</option><option value="300">300</option><option value="600">600</option></select></div><div><label>Chores todo entity</label><input id="chore_entity" maxlength="95" placeholder="todo.family_chores"></div><div><label>Alarmo entity</label><input id="alarm_entity" maxlength="95" placeholder="alarm_control_panel.alarmo"></div><div><label>Weather current + daily entity</label><input id="weather_current_entity" maxlength="95" placeholder="weather.kwineena131"><div class="muted">Current conditions and daily forecast.</div></div><div><label>Weather hourly entity</label><input id="weather_hourly_entity" maxlength="95" placeholder="weather.forecast_home"><div class="muted">Provider supporting hourly forecasts.</div></div></div>
<h3>Battery monitoring</h3>
<div class="grid"><div><label>Header percentage</label><select id="battery_show_percent"><option value="1">Show percentage</option><option value="0">Icon only</option></select><div class="muted">The icon is always shown; this controls the numeric percentage.</div></div><div><label>Voltage calibration multiplier</label><input id="battery_multiplier" type="number" min="1.400" max="2.000" step="0.001"><div class="muted">Default 1.680 from the board's 68k/100k divider. Adjust after comparing to a multimeter.</div></div><div><label>Critical threshold (%)</label><input id="battery_critical_percent" type="number" min="1" max="30"><div class="muted">Battery icon turns red at or below this estimate.</div></div></div>
<p class="muted">Battery percentage is estimated from GPIO52 voltage and is not a true fuel-gauge reading. USB/charging state is not available to firmware on this board.</p>
<h3>Family calendars</h3>
<div class="person"><div><label>Person 1 name</label><input id="p1_name" maxlength="47"></div><div><label>Color</label><input id="p1_color" type="color"></div><div class="cal"><label>Calendar entity</label><input id="p1_calendar" maxlength="95" placeholder="calendar.person_1"></div></div>
<div class="person"><div><label>Person 2 name</label><input id="p2_name" maxlength="47"></div><div><label>Color</label><input id="p2_color" type="color"></div><div class="cal"><label>Calendar entity</label><input id="p2_calendar" maxlength="95"></div></div>
<div class="person"><div><label>Person 3 name</label><input id="p3_name" maxlength="47"></div><div><label>Color</label><input id="p3_color" type="color"></div><div class="cal"><label>Calendar entity</label><input id="p3_calendar" maxlength="95"></div></div>
<div class="person"><div><label>Person 4 name</label><input id="p4_name" maxlength="47"></div><div><label>Color</label><input id="p4_color" type="color"></div><div class="cal"><label>Calendar entity</label><input id="p4_calendar" maxlength="95"></div></div>
<p class="muted">Family names, colors and calendar mappings are stored in NVS. Reboot after changing calendar mappings so the PSRAM week cache starts clean. Weather-source changes can refresh immediately.</p><div class="row"><button type="submit">Save configuration</button><button type="button" class="secondary" onclick="reboot()">Reboot device</button></div><p id="cfgMsg" class="muted"></p></form></div>

<div class="card"><h2>OTA firmware update</h2><div id="otaInfo" class="muted"></div><p class="muted">Upload PlatformIO <code>firmware.bin</code>. v2.2.3 validates image size, persists upload checkpoints, blocks unrelated software reboots during flash writes, verifies SHA-256 against the inactive partition, and only then reboots.</p><input id="fw" type="file" accept=".bin,application/octet-stream"><div style="height:10px"></div><button id="otaBtn" onclick="ota()">Upload firmware</button><div style="height:10px"></div><progress id="prog" max="100" value="0"></progress><p id="otaMsg" class="result muted">No OTA attempt this boot.</p></div>
<div class="card"><div class="row"><button class="secondary" onclick="maintenance(false)">Cancel maintenance mode</button></div><p class="muted">Management uses HTTP Basic authentication on the local network. Do not expose port 80 to the Internet.</p></div>
</div><script>
const $=id=>document.getElementById(id);const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function json(url,opt){const r=await fetch(url,opt);let j={};try{j=await r.json()}catch(e){}if(!r.ok)throw new Error(j.error||('HTTP '+r.status));return j}
function esc(s){return String(s??'').replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function hexColor(v){let n=Number(v||0).toString(16).padStart(6,'0');return '#'+n.slice(-6)}
function since(uptimeMs,stamp){if(!stamp)return 'never';const sec=Math.max(0,Math.floor((uptimeMs-stamp)/1000));if(sec<60)return sec+'s ago';if(sec<3600)return Math.floor(sec/60)+'m ago';return Math.floor(sec/3600)+'h ago'}
function batteryText(s){return s.battery_valid?`${s.battery_percent}% / ${Number(s.battery_voltage_v).toFixed(2)} V`:'unavailable'}
async function refresh(){try{const s=await json('/api/status');$('version').textContent='v'+s.version;$('defaultPw').hidden=!s.default_password;const pairs=[['IP',s.ip],['mDNS',s.mdns],['Wi-Fi RSSI',s.rssi+' dBm'],['Uptime',s.uptime],['Free heap',s.free_heap],['Free PSRAM',s.free_psram],['Battery',batteryText(s)],['Battery ADC',(s.battery_adc_mv||0)+' mV'],['Battery sample',since(s.uptime_ms,s.battery_sampled_ms)],['HA',s.ha_status],['HA last success',since(s.uptime_ms,s.ha_last_success_ms)],['Weather',s.weather_status],['Weather last success',since(s.uptime_ms,s.weather_last_success_ms)],['Current/daily source',s.weather_current_entity||'not configured'],['Hourly source',s.weather_hourly_entity||'not configured'],['Reset',s.reset_reason],['Reset source',s.last_reboot_source||'none'],['Boot count',s.boot_count],['Brownouts',s.brownout_count],['OTA slot',s.ota_partition],['OTA target',s.ota_target||'n/a'],['Last OTA',s.last_ota_result||'none']];$('stats').innerHTML=pairs.map(p=>`<div class="stat"><div class="k">${esc(p[0])}</div><div class="v">${esc(p[1])}</div></div>`).join('');$('otaInfo').textContent=`Running ${s.ota_partition||'?'}; next target ${s.ota_target||'?'} (${s.ota_target_size||0} bytes). Last OTA: ${s.last_ota_result||'none'}${s.ota_checkpoint_written?` | checkpoint ${s.ota_checkpoint_written}/${s.ota_checkpoint_expected||'?'} bytes (${s.ota_checkpoint_stage||'unknown'})`:''}`;}catch(e){}}
async function loadCfg(){const c=await json('/api/config');for(const id of ['hostname','username','backlight','timeout','chore_entity','alarm_entity','weather_current_entity','weather_hourly_entity'])$(id).value=c[id]??'';$('dark').value=c.dark?'1':'0';$('battery_show_percent').value=c.battery_show_percent?'1':'0';$('battery_multiplier').value=Number(c.battery_multiplier||1.68).toFixed(3);$('battery_critical_percent').value=c.battery_critical_percent??10;for(let i=1;i<=4;i++){const p=c.people[i-1];$(`p${i}_name`).value=p.name||'';$(`p${i}_color`).value=hexColor(p.color);$(`p${i}_calendar`).value=p.calendar||''}}
$('cfg').addEventListener('submit',async e=>{e.preventDefault();$('cfgMsg').textContent='Saving…';const p=new URLSearchParams();for(const id of ['hostname','username','password','backlight','dark','timeout','chore_entity','alarm_entity','weather_current_entity','weather_hourly_entity','battery_show_percent','battery_multiplier','battery_critical_percent'])p.set(id,$(id).value);for(let i=1;i<=4;i++){p.set(`p${i}_name`,$(`p${i}_name`).value);p.set(`p${i}_color`,$(`p${i}_color`).value);p.set(`p${i}_calendar`,$(`p${i}_calendar`).value)}try{const r=await json('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});$('cfgMsg').textContent=r.weather_sources_changed?(r.weather_refresh_queued?'Saved. Weather is refreshing. Reboot recommended for family/calendar changes.':'Saved. Weather sources changed but a refresh is already busy. Reboot recommended.'):'Saved. Reboot recommended after family/calendar or UI changes.';$('password').value=''}catch(e){$('cfgMsg').textContent=e.message}});
async function forceRefresh(domain){try{const r=await json('/api/refresh/'+domain,{method:'POST'});$('diagMsg').textContent=r.message||(`${domain} refresh queued.`)}catch(e){$('diagMsg').textContent=e.message}}
async function testHA(){try{const r=await json('/api/ha/test',{method:'POST'});$('diagMsg').textContent=r.queued?'Home Assistant connection test queued on shared worker…':'A Home Assistant test is already pending.';await pollDiag('ha')}catch(e){$('diagMsg').textContent=e.message}}
async function testWeather(){try{const r=await json('/api/weather/test',{method:'POST'});$('diagMsg').textContent=r.queued?'Weather-source test queued on shared worker…':'A weather test is already pending.';await pollDiag('weather')}catch(e){$('diagMsg').textContent=e.message}}
async function pollDiag(kind){for(let i=0;i<80;i++){await sleep(250);const d=await json('/api/diagnostics');if(kind==='ha'){const t=d.home_assistant_test;if(t&&!t.pending&&!t.in_progress&&t.valid){$('diagMsg').textContent=`Home Assistant: HTTP ${t.http_code}, ${t.latency_ms} ms, authenticated=${t.authenticated}. ${t.message}`;return}}else{const t=d.weather_sources;if(t&&!t.pending&&!t.in_progress&&t.valid){$('diagMsg').textContent=`Weather current ${t.current_ok?'✓':'✗'} | daily ${t.daily_ok?'✓':'✗'} (${t.daily_count}) | hourly ${t.hourly_ok?'✓':'✗'} (${t.hourly_count})\n${t.current_daily_entity} + ${t.hourly_entity}\n${t.message||''}`;return}}} $('diagMsg').textContent+='\nTimed out waiting for test result.'}
async function maintenance(on){try{return await json(on?'/api/maintenance/start':'/api/maintenance/cancel',{method:'POST'})}catch(e){$('otaMsg').textContent=e.message;throw e}}
async function ota(){const f=$('fw').files[0];if(!f){$('otaMsg').textContent='Choose firmware.bin first.';return}if(!f.name.toLowerCase().endsWith('.bin')){$('otaMsg').textContent='Firmware file must end in .bin';return}$('otaBtn').disabled=true;$('prog').value=0;try{const pre=await json('/api/ota/preflight?size='+encodeURIComponent(f.size),{method:'POST'});$('otaMsg').textContent=`Preflight OK: ${f.size} bytes → ${pre.target_partition} (${pre.available_bytes} bytes). Entering maintenance…`;await maintenance(true);let ready=false;for(let i=0;i<120;i++){const s=await json('/api/status');if(s.ota_ready){ready=true;break}$('otaMsg').textContent='Waiting for Home Assistant worker to become idle…';await sleep(250)}if(!ready)throw new Error('Timed out waiting for Home Assistant worker to become idle');$('otaMsg').textContent='Uploading firmware…';const result=await new Promise((resolve,reject)=>{const x=new XMLHttpRequest();x.open('POST','/update');x.upload.onprogress=e=>{if(e.lengthComputable)$('prog').value=Math.round(e.loaded*100/e.total)};x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch(_){};x.status>=200&&x.status<300?resolve(j):reject(new Error(j.error||x.responseText||('HTTP '+x.status)))};x.onerror=()=>reject(new Error('Upload failed'));const d=new FormData();d.append('firmware',f,f.name);x.send(d)});$('prog').value=100;$('otaMsg').textContent=`Verified SHA-256 ${result.sha256||'complete'}. Device is rebooting…`;}catch(e){$('otaMsg').textContent=e.message;try{await maintenance(false)}catch(_){}$('otaBtn').disabled=false}}
async function reboot(){if(!confirm('Reboot Family Hub now?'))return;try{await json('/api/reboot',{method:'POST'});alert('Rebooting…')}catch(e){alert(e.message)}}
refresh();loadCfg().catch(()=>{});setInterval(refresh,5000);
</script></body></html>
)HTML";

String sanitize_hostname(String value) {
    value.toLowerCase();
    String out;
    out.reserve(32);
    bool last_dash = false;
    for (size_t i = 0; i < value.length() && out.length() < 31; ++i) {
        const char c = value[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (alnum) {
            out += c;
            last_dash = false;
        } else if ((c == '-' || c == '_' || c == ' ') && !out.isEmpty() && !last_dash) {
            out += '-';
            last_dash = true;
        }
    }
    while (out.endsWith("-")) out.remove(out.length() - 1);
    if (out.isEmpty()) out = WEB_MANAGER_HOSTNAME;
    return out;
}

bool ensure_auth() {
    if (g_server.authenticate(g_admin_user.c_str(), g_admin_password.c_str())) return true;
    g_server.requestAuthentication(BASIC_AUTH, "Family Hub");
    return false;
}

bool ota_ready() {
    if (!g_maintenance || g_ota_in_progress) return false;
    if (!network_service_connected()) return true;
    if (!home_assistant_configured()) return true;
    return home_assistant_ready_for_auto_refresh();
}

uint8_t saved_backlight() {
    Preferences ui;
    uint8_t value = APP_DEFAULT_BACKLIGHT;
    if (ui.begin("famcal_ui", true)) {
        value = ui.getUChar("bright", value);
        ui.end();
    }
    if (value < 10 || value > 100) value = APP_DEFAULT_BACKLIGHT;
    return value;
}

void restore_backlight_after_failed_ota() {
    if (!g_ota_dimmed) return;
    board_set_backlight(g_ota_restore_backlight);
    g_ota_dimmed = false;
}

String uptime_text() {
    const uint64_t seconds = millis() / 1000ULL;
    const uint32_t days = static_cast<uint32_t>(seconds / 86400ULL);
    const uint32_t hours = static_cast<uint32_t>((seconds / 3600ULL) % 24ULL);
    const uint32_t minutes = static_cast<uint32_t>((seconds / 60ULL) % 60ULL);
    char buffer[40];
    if (days) snprintf(buffer, sizeof(buffer), "%ud %uh %um", days, hours, minutes);
    else if (hours) snprintf(buffer, sizeof(buffer), "%uh %um", hours, minutes);
    else snprintf(buffer, sizeof(buffer), "%um", minutes);
    return String(buffer);
}

void send_json(JsonDocument &doc, int status = 200) {
    String body;
    serializeJson(doc, body);
    g_server.send(status, "application/json", body);
}

void send_error(int status, const char *message) {
    JsonDocument doc;
    doc["error"] = message ? message : "Request failed";
    send_json(doc, status);
}


const char *reset_reason_text(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external reset";
        case ESP_RST_SW: return "software reset";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default: return "unknown";
    }
}

uint32_t parse_hex_color(String value, uint32_t fallback) {
    value.trim();
    if (value.startsWith("#")) value.remove(0, 1);
    if (value.length() != 6) return fallback;
    char *end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 16);
    if (!end || *end != '\0' || parsed > 0xFFFFFFUL) return fallback;
    return static_cast<uint32_t>(parsed);
}

void persist_reboot_source(const char *source) {
    Preferences diag;
    if (!diag.begin("famcal_diag", false)) return;
    diag.putString("reboot_src", source && source[0] ? source : "unknown");
    diag.end();
}

bool schedule_reboot(const char *source, uint32_t delay_ms, bool verified_ota_reboot = false) {
    const char *effective_source = source && source[0] ? source : "unknown";
    if (g_ota_in_progress && !(verified_ota_reboot && g_ota_succeeded)) {
        ESP_LOGE("FamilyCalendar",
                 "Reboot request blocked during active OTA write: source=%s written=%u/%u",
                 effective_source,
                 static_cast<unsigned>(g_ota_written),
                 static_cast<unsigned>(g_ota_expected_size));
        return false;
    }

    g_scheduled_reboot_source = effective_source;
    persist_reboot_source(effective_source);
    g_reboot_at_ms = millis() + delay_ms;
    ESP_LOGW("FamilyCalendar", "Software reboot scheduled: source=%s delay=%lums",
             effective_source, static_cast<unsigned long>(delay_ms));
    return true;
}

void persist_ota_upload_checkpoint(const char *stage, bool active) {
    g_last_ota_checkpoint_expected = static_cast<uint32_t>(g_ota_expected_size);
    g_last_ota_checkpoint_written = static_cast<uint32_t>(g_ota_written);
    g_last_ota_checkpoint_stage = stage ? stage : "unknown";

    Preferences ota;
    if (!ota.begin("famcal_ota", false)) return;
    ota.putBool("upload_active", active);
    ota.putString("upload_from", APP_VERSION);
    ota.putString("upload_target", g_ota_target_partition ? g_ota_target_partition->label : "");
    ota.putUInt("upload_expected", g_last_ota_checkpoint_expected);
    ota.putUInt("upload_written", g_last_ota_checkpoint_written);
    ota.putString("upload_stage", g_last_ota_checkpoint_stage);
    ota.end();
}

void load_boot_and_ota_diagnostics() {
    const esp_reset_reason_t reason = esp_reset_reason();
    g_reset_reason = reset_reason_text(reason);

    Preferences diag;
    if (diag.begin("famcal_diag", false)) {
        g_boot_count = diag.getUInt("boots", 0) + 1;
        g_brownout_count = diag.getUInt("brownouts", 0);
        if (reason == ESP_RST_BROWNOUT) ++g_brownout_count;

        const String scheduled_source = diag.getString("reboot_src", "");
        if (reason == ESP_RST_SW) {
            g_last_reboot_source = scheduled_source.isEmpty()
                                       ? "unexpected-software-reset"
                                       : scheduled_source;
        } else {
            g_last_reboot_source = "none";
        }

        diag.putUInt("boots", g_boot_count);
        diag.putUInt("brownouts", g_brownout_count);
        diag.putString("last_reset", g_reset_reason);
        diag.putString("last_reboot_src", g_last_reboot_source);
        diag.remove("reboot_src");
        diag.end();
    }

    Preferences ota;
    if (!ota.begin("famcal_ota", false)) return;
    const bool pending = ota.getBool("pending", false);
    const bool interrupted_upload = ota.getBool("upload_active", false);

    g_last_ota_checkpoint_expected = ota.getUInt("upload_expected", 0);
    g_last_ota_checkpoint_written = ota.getUInt("upload_written", 0);
    g_last_ota_checkpoint_stage = ota.getString("upload_stage", "");
    g_last_ota_error = ota.getString("last_error", "");

    if (pending) {
        const String from = ota.getString("pending_from", "unknown");
        const String target = ota.getString("pending_target", "");
        const String sha = ota.getString("pending_sha", "");
        const esp_partition_t *running = esp_ota_get_running_partition();
        const bool succeeded = running && target == running->label;
        g_last_ota_result = succeeded ? "success" : "rollback / target not running";
        g_last_ota_from = from;
        g_last_ota_to = APP_VERSION;
        g_last_ota_sha256 = sha;
        g_last_ota_error = succeeded ? "" : "Verified OTA target did not boot";
        ota.putString("last_result", g_last_ota_result);
        ota.putString("last_from", g_last_ota_from);
        ota.putString("last_to", g_last_ota_to);
        ota.putString("last_sha", g_last_ota_sha256);
        ota.putString("last_error", g_last_ota_error);
        ota.putBool("pending", false);
        ota.putBool("upload_active", false);
        ESP_LOGI("FamilyCalendar", "OTA boot verification: %s -> %s, target=%s running=%s, result=%s",
                 from.c_str(), APP_VERSION, target.c_str(), running ? running->label : "unknown",
                 g_last_ota_result.c_str());
    } else if (interrupted_upload) {
        const String from = ota.getString("upload_from", APP_VERSION);
        const String target = ota.getString("upload_target", "unknown");
        const uint32_t expected = ota.getUInt("upload_expected", 0);
        const uint32_t written = ota.getUInt("upload_written", 0);
        const String stage = ota.getString("upload_stage", "unknown");

        g_last_ota_from = from;
        g_last_ota_to = "";
        g_last_ota_sha256 = "";
        g_last_ota_result = String("interrupted: ") + String(written) + "/" + String(expected) +
                            " bytes at " + stage;
        g_last_ota_error = String("OTA upload interrupted; reset=") + g_reset_reason +
                           ", source=" + g_last_reboot_source + ", target=" + target;
        ota.putString("last_result", g_last_ota_result);
        ota.putString("last_from", g_last_ota_from);
        ota.putString("last_to", "");
        ota.putString("last_sha", "");
        ota.putString("last_error", g_last_ota_error);
        ota.putBool("upload_active", false);
        ESP_LOGE("FamilyCalendar",
                 "Recovered interrupted OTA: %u/%u bytes stage=%s reset=%s source=%s target=%s",
                 static_cast<unsigned>(written), static_cast<unsigned>(expected), stage.c_str(),
                 g_reset_reason.c_str(), g_last_reboot_source.c_str(), target.c_str());
    } else {
        g_last_ota_result = ota.getString("last_result", "none");
        g_last_ota_from = ota.getString("last_from", "");
        g_last_ota_to = ota.getString("last_to", "");
        g_last_ota_sha256 = ota.getString("last_sha", "");
    }
    ota.end();
}

void record_ota_failure(const String &reason) {
    g_last_ota_result = String("failed: ") + reason;
    g_last_ota_from = APP_VERSION;
    g_last_ota_to = "";
    g_last_ota_sha256 = g_ota_stream_sha256;
    g_last_ota_error = reason;
    g_last_ota_checkpoint_expected = static_cast<uint32_t>(g_ota_expected_size);
    g_last_ota_checkpoint_written = static_cast<uint32_t>(g_ota_written);
    if (g_last_ota_checkpoint_stage.isEmpty()) g_last_ota_checkpoint_stage = "failed";

    Preferences ota;
    if (ota.begin("famcal_ota", false)) {
        ota.putBool("pending", false);
        ota.putBool("upload_active", false);
        ota.putString("last_result", g_last_ota_result);
        ota.putString("last_from", g_last_ota_from);
        ota.putString("last_to", "");
        ota.putString("last_sha", g_last_ota_sha256);
        ota.putString("last_error", g_last_ota_error);
        ota.putUInt("upload_expected", g_last_ota_checkpoint_expected);
        ota.putUInt("upload_written", g_last_ota_checkpoint_written);
        ota.putString("upload_stage", g_last_ota_checkpoint_stage);
        ota.end();
    }
}

void record_ota_pending() {
    g_last_ota_checkpoint_expected = static_cast<uint32_t>(g_ota_expected_size);
    g_last_ota_checkpoint_written = static_cast<uint32_t>(g_ota_written);
    g_last_ota_checkpoint_stage = "verified";
    g_last_ota_error = "";

    Preferences ota;
    if (!ota.begin("famcal_ota", false)) return;
    ota.putBool("pending", true);
    ota.putBool("upload_active", false);
    ota.putString("pending_from", APP_VERSION);
    ota.putString("pending_target", g_ota_target_partition ? g_ota_target_partition->label : "");
    ota.putString("pending_sha", g_ota_readback_sha256);
    ota.putUInt("pending_size", static_cast<uint32_t>(g_ota_written));
    ota.putUInt("upload_expected", g_last_ota_checkpoint_expected);
    ota.putUInt("upload_written", g_last_ota_checkpoint_written);
    ota.putString("upload_stage", g_last_ota_checkpoint_stage);
    ota.putString("last_error", "");
    ota.end();
}

bool hash_partition_sha256(const esp_partition_t *partition, size_t length, String &hash_out) {
    hash_out = "";
    if (!partition || length == 0 || length > partition->size) return false;
    constexpr size_t CHUNK = 4096;
    uint8_t *buffer = static_cast<uint8_t *>(
        heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) buffer = static_cast<uint8_t *>(malloc(CHUNK));
    if (!buffer) return false;

    SHA256Builder hash;
    hash.begin();
    size_t offset = 0;
    bool ok = true;
    while (offset < length) {
        const size_t amount = min(CHUNK, length - offset);
        if (esp_partition_read(partition, offset, buffer, amount) != ESP_OK) {
            ok = false;
            break;
        }
        hash.add(buffer, amount);
        offset += amount;
        delay(0);
    }
    free(buffer);
    if (!ok) return false;
    hash.calculate();
    hash_out = hash.toString();
    return hash_out.length() == 64;
}

void copy_weather_sources(char *current_daily, size_t current_daily_len,
                          char *hourly, size_t hourly_len) {
    portENTER_CRITICAL(&g_config_mux);
    if (current_daily && current_daily_len) {
        snprintf(current_daily, current_daily_len, "%s", g_weather_current_daily_entity);
    }
    if (hourly && hourly_len) {
        const char *source = g_weather_hourly_entity[0] ? g_weather_hourly_entity : g_weather_current_daily_entity;
        snprintf(hourly, hourly_len, "%s", source);
    }
    portEXIT_CRITICAL(&g_config_mux);
}

void set_weather_sources(const String &current_daily, const String &hourly) {
    String current = current_daily;
    String hours = hourly;
    current.trim();
    hours.trim();
    if (current.length() > 95) current.remove(95);
    if (hours.length() > 95) hours.remove(95);

    portENTER_CRITICAL(&g_config_mux);
    snprintf(g_weather_current_daily_entity, sizeof(g_weather_current_daily_entity), "%s", current.c_str());
    snprintf(g_weather_hourly_entity, sizeof(g_weather_hourly_entity), "%s", hours.c_str());
    portEXIT_CRITICAL(&g_config_mux);
}

void load_web_preferences() {
    Preferences prefs;
    if (!prefs.begin("famcal_web", true)) return;
    g_hostname = sanitize_hostname(prefs.getString("host", WEB_MANAGER_HOSTNAME));
    g_admin_user = prefs.getString("user", WEB_MANAGER_DEFAULT_USER);
    g_admin_password = prefs.getString("pass", WEB_MANAGER_DEFAULT_PASSWORD);
    const String weather_current = prefs.getString("wx_current", HA_WEATHER_CURRENT_DAILY_ENTITY);
    const String weather_hourly = prefs.getString("wx_hourly", HA_WEATHER_HOURLY_ENTITY);
    prefs.end();
    set_weather_sources(weather_current, weather_hourly);
    if (g_admin_user.isEmpty()) g_admin_user = WEB_MANAGER_DEFAULT_USER;
    if (g_admin_password.isEmpty()) g_admin_password = WEB_MANAGER_DEFAULT_PASSWORD;
}

void handle_status() {
    if (!ensure_auth()) return;
    JsonDocument doc;
    doc["version"] = APP_VERSION;
    doc["ip"] = network_service_connected() ? WiFi.localIP().toString() : String("offline");
    doc["rssi"] = network_service_connected() ? WiFi.RSSI() : 0;
    doc["uptime"] = uptime_text();
    doc["uptime_ms"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["free_psram"] = ESP.getFreePsram();
    doc["ha_status"] = home_assistant_status();
    doc["ha_last_success_ms"] = home_assistant_last_success_ms();
    doc["weather_status"] = weather_service_status();
    doc["weather_last_success_ms"] = weather_service_last_success_ms();
    char weather_current[96] = {};
    char weather_hourly[96] = {};
    copy_weather_sources(weather_current, sizeof(weather_current), weather_hourly, sizeof(weather_hourly));
    doc["weather_current_entity"] = weather_current;
    doc["weather_hourly_entity"] = weather_hourly;
    BatteryStatus battery = {};
    const bool battery_valid = battery_service_get_status(battery);
    doc["battery_valid"] = battery_valid;
    doc["battery_adc_mv"] = battery.adc_mv;
    doc["battery_voltage_v"] = battery.voltage_v;
    doc["battery_percent"] = battery.percent;
    doc["battery_sampled_ms"] = battery.sampled_ms;
    doc["maintenance"] = g_maintenance;
    doc["ota_in_progress"] = g_ota_in_progress;
    doc["ota_ready"] = ota_ready();
    doc["mdns"] = String("http://") + g_hostname + ".local";
    doc["reset_reason"] = g_reset_reason;
    doc["last_reboot_source"] = g_last_reboot_source;
    doc["boot_count"] = g_boot_count;
    doc["brownout_count"] = g_brownout_count;
    doc["last_ota_result"] = g_last_ota_result;
    doc["last_ota_error"] = g_last_ota_error;
    doc["ota_checkpoint_expected"] = g_last_ota_checkpoint_expected;
    doc["ota_checkpoint_written"] = g_last_ota_checkpoint_written;
    doc["ota_checkpoint_stage"] = g_last_ota_checkpoint_stage;
    doc["default_password"] = (strcmp(WEB_MANAGER_DEFAULT_PASSWORD, "familyhub") == 0 &&
                               g_admin_user == WEB_MANAGER_DEFAULT_USER &&
                               g_admin_password == WEB_MANAGER_DEFAULT_PASSWORD);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    doc["ota_partition"] = running ? running->label : "unknown";
    doc["ota_target"] = target ? target->label : "unavailable";
    doc["ota_target_size"] = target ? target->size : 0;
    send_json(doc);
}

void handle_get_config() {
    if (!ensure_auth()) return;
    Preferences ui;
    bool dark = APP_DEFAULT_DARK_MODE != 0;
    uint8_t backlight = APP_DEFAULT_BACKLIGHT;
    uint32_t timeout = APP_SCREEN_TIMEOUT_SECONDS;
    if (ui.begin("famcal_ui", true)) {
        dark = ui.getBool("dark", dark);
        backlight = ui.getUChar("bright", backlight);
        timeout = ui.getUInt("timeout", timeout);
        ui.end();
    }

    JsonDocument doc;
    doc["hostname"] = g_hostname;
    doc["username"] = g_admin_user;
    doc["dark"] = dark;
    doc["backlight"] = backlight;
    doc["timeout"] = timeout;
    doc["chore_entity"] = chore_service_entity();
    doc["alarm_entity"] = alarm_service_entity();
    char weather_current[96] = {};
    char weather_hourly[96] = {};
    copy_weather_sources(weather_current, sizeof(weather_current), weather_hourly, sizeof(weather_hourly));
    doc["weather_current_entity"] = weather_current;
    doc["weather_hourly_entity"] = weather_hourly;
    doc["battery_show_percent"] = battery_service_show_percent();
    doc["battery_multiplier"] = battery_service_voltage_multiplier();
    doc["battery_critical_percent"] = battery_service_critical_percent();
    JsonArray people = doc["people"].to<JsonArray>();
    for (size_t i = 0; i < 4; ++i) {
        JsonObject person = people.add<JsonObject>();
        person["name"] = runtime_config_person_name(i);
        person["color"] = runtime_config_person_color(i);
        person["calendar"] = runtime_config_calendar_entity(i);
    }
    send_json(doc);
}

void handle_save_config() {
    if (!ensure_auth()) return;

    const String hostname = sanitize_hostname(g_server.arg("hostname"));
    String username = g_server.arg("username");
    username.trim();
    if (username.isEmpty()) username = WEB_MANAGER_DEFAULT_USER;
    if (username.length() > 31) username.remove(31);

    const String new_password = g_server.arg("password");
    const int backlight_arg = constrain(g_server.arg("backlight").toInt(), 10, 100);
    uint32_t timeout_arg = static_cast<uint32_t>(g_server.arg("timeout").toInt());
    if (timeout_arg != 0 && timeout_arg != 30 && timeout_arg != 60 && timeout_arg != 120 &&
        timeout_arg != 300 && timeout_arg != 600) {
        timeout_arg = APP_SCREEN_TIMEOUT_SECONDS;
    }
    const bool dark_arg = g_server.arg("dark") == "1";
    const bool battery_show_percent = g_server.arg("battery_show_percent") != "0";
    const float battery_multiplier = g_server.arg("battery_multiplier").toFloat();
    const int battery_critical_arg = g_server.arg("battery_critical_percent").toInt();
    if (!isfinite(battery_multiplier) || battery_multiplier < BATTERY_MULTIPLIER_MIN ||
        battery_multiplier > BATTERY_MULTIPLIER_MAX) {
        send_error(400, "Battery multiplier must be between 1.400 and 2.000");
        return;
    }
    if (battery_critical_arg < 1 || battery_critical_arg > 30) {
        send_error(400, "Battery critical threshold must be between 1 and 30 percent");
        return;
    }
    const String chore_entity = g_server.arg("chore_entity").substring(0, 95);
    const String alarm_entity = g_server.arg("alarm_entity").substring(0, 95);
    String weather_current_entity = g_server.arg("weather_current_entity").substring(0, 95);
    String weather_hourly_entity = g_server.arg("weather_hourly_entity").substring(0, 95);
    weather_current_entity.trim();
    weather_hourly_entity.trim();
    char previous_weather_current[96] = {};
    char previous_weather_hourly[96] = {};
    copy_weather_sources(previous_weather_current, sizeof(previous_weather_current),
                         previous_weather_hourly, sizeof(previous_weather_hourly));
    if (!weather_current_entity.isEmpty() && !weather_current_entity.startsWith("weather.")) {
        send_error(400, "Current/daily weather entity must start with weather.");
        return;
    }
    if (!weather_hourly_entity.isEmpty() && !weather_hourly_entity.startsWith("weather.")) {
        send_error(400, "Hourly weather entity must start with weather.");
        return;
    }

    String person_names[4];
    String person_calendars[4];
    uint32_t person_colors[4] = {};
    bool calendar_config_changed = false;
    for (size_t i = 0; i < 4; ++i) {
        const String number = String(static_cast<unsigned>(i + 1));
        person_names[i] = g_server.arg(String("p") + number + "_name").substring(0, 47);
        person_names[i].trim();
        person_calendars[i] = g_server.arg(String("p") + number + "_calendar").substring(0, 95);
        person_calendars[i].trim();
        if (!person_calendars[i].isEmpty() && !person_calendars[i].startsWith("calendar.")) {
            send_error(400, (String("Person ") + number + " calendar entity must start with calendar.").c_str());
            return;
        }
        person_colors[i] = parse_hex_color(g_server.arg(String("p") + number + "_color"),
                                           runtime_config_person_color(i));
        if (person_calendars[i] != runtime_config_calendar_entity(i)) calendar_config_changed = true;
    }

    /* Calendar entity buffers are shared read-only with the HA worker.  Only
     * swap them while the single worker is idle, then reboot before the next
     * calendar sync so the multi-week cache is rebuilt from the new mapping. */
    if (calendar_config_changed && network_service_connected() && home_assistant_configured() &&
        !home_assistant_ready_for_auto_refresh()) {
        send_error(409, "Home Assistant is busy. Retry Save configuration after the current request finishes.");
        return;
    }

    Preferences web;
    if (!web.begin("famcal_web", false)) {
        send_error(500, "Could not open web configuration storage");
        return;
    }
    web.putString("host", hostname);
    web.putString("user", username);
    if (!new_password.isEmpty()) web.putString("pass", new_password.substring(0, 63));
    web.putString("wx_current", weather_current_entity);
    web.putString("wx_hourly", weather_hourly_entity);
    web.end();
    set_weather_sources(weather_current_entity, weather_hourly_entity);
    const char *effective_hourly = weather_hourly_entity.isEmpty()
                                       ? weather_current_entity.c_str()
                                       : weather_hourly_entity.c_str();
    const bool weather_sources_changed =
        strcmp(previous_weather_current, weather_current_entity.c_str()) != 0 ||
        strcmp(previous_weather_hourly, effective_hourly) != 0;

    Preferences ui;
    if (!ui.begin("famcal_ui", false)) {
        send_error(500, "Could not open UI configuration storage");
        return;
    }
    ui.putBool("dark", dark_arg);
    ui.putUChar("bright", static_cast<uint8_t>(backlight_arg));
    ui.putUInt("timeout", timeout_arg);
    ui.end();

    if (!battery_service_set_config(battery_show_percent, battery_multiplier,
                                    static_cast<uint8_t>(battery_critical_arg))) {
        send_error(500, "Could not save battery monitoring configuration");
        return;
    }

    /* These services already own their NVS entity selection.  Reuse their
     * setters so web and touchscreen configuration stay in sync. */
    chore_service_set_entity(chore_entity.c_str());
    alarm_service_set_entity(alarm_entity.c_str());
    for (size_t i = 0; i < 4; ++i) {
        if (!runtime_config_set_person(i, person_names[i].c_str(), person_colors[i],
                                       person_calendars[i].c_str())) {
            send_error(500, "Could not save family/calendar configuration");
            return;
        }
    }

    const bool weather_queued = weather_sources_changed && !g_maintenance
                                    ? weather_service_request_refresh(true, "web weather source changed")
                                    : false;

    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot_required"] = true;
    doc["weather_sources_changed"] = weather_sources_changed;
    doc["weather_refresh_queued"] = weather_queued;
    doc["calendar_config_changed"] = calendar_config_changed;
    doc["battery_config_applied"] = true;
    send_json(doc);
}

void handle_weather_refresh() {
    if (!ensure_auth()) return;
    if (g_maintenance) {
        send_error(409, "Weather refresh is unavailable during OTA maintenance");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["queued"] = weather_service_request_refresh(true, "web manual refresh");
    send_json(doc);
}

void handle_weather_test() {
    if (!ensure_auth()) return;
    if (g_maintenance) {
        send_error(409, "Weather testing is unavailable during OTA maintenance");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["queued"] = weather_service_request_source_test();
    send_json(doc);
}

void handle_ha_test() {
    if (!ensure_auth()) return;
    if (g_maintenance) {
        send_error(409, "Home Assistant testing is unavailable during OTA maintenance");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["queued"] = home_assistant_request_connection_test();
    send_json(doc);
}

void handle_refresh_calendar() {
    if (!ensure_auth()) return;
    if (g_maintenance) { send_error(409, "Refresh unavailable during OTA maintenance"); return; }
    home_assistant_request_sync();
    JsonDocument doc; doc["ok"] = true; doc["message"] = "Calendar refresh queued on shared HA worker"; send_json(doc);
}

void handle_refresh_chores() {
    if (!ensure_auth()) return;
    if (g_maintenance) { send_error(409, "Refresh unavailable during OTA maintenance"); return; }
    if (!chore_service_configured()) { send_error(400, "Chores todo entity is not configured"); return; }
    home_assistant_request_chore_sync();
    JsonDocument doc; doc["ok"] = true; doc["message"] = "Chores refresh queued on shared HA worker"; send_json(doc);
}

void handle_diagnostics() {
    if (!ensure_auth()) return;
    JsonDocument doc;
    doc["app"] = APP_NAME;
    doc["version"] = APP_VERSION;
    doc["uptime_ms"] = millis();
    doc["ip"] = network_service_connected() ? WiFi.localIP().toString() : String("offline");
    doc["rssi_dbm"] = network_service_connected() ? WiFi.RSSI() : 0;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["free_psram"] = ESP.getFreePsram();
    doc["reset_reason"] = g_reset_reason;
    doc["last_reboot_source"] = g_last_reboot_source;
    doc["scheduled_reboot_source"] = g_scheduled_reboot_source;
    doc["boot_count"] = g_boot_count;
    doc["brownout_count"] = g_brownout_count;

    JsonObject configured = doc["configured"].to<JsonObject>();
    configured["home_assistant"] = home_assistant_configured();
    configured["calendars"] = runtime_config_calendar_count() > 0;
    configured["calendar_count"] = runtime_config_calendar_count();
    configured["chores"] = chore_service_configured();
    configured["alarm"] = alarm_service_configured();
    configured["weather"] = weather_service_configured();

    doc["ha_status"] = home_assistant_status();
    doc["ha_authenticated"] = home_assistant_authenticated();
    doc["ha_last_success_ms"] = home_assistant_last_success_ms();
    doc["weather_status"] = weather_service_status();
    doc["weather_last_success_ms"] = weather_service_last_success_ms();

    BatteryStatus battery = {};
    const bool battery_valid = battery_service_get_status(battery);
    JsonObject battery_json = doc["battery"].to<JsonObject>();
    battery_json["valid"] = battery_valid;
    battery_json["adc_pin"] = BATTERY_ADC_PIN;
    battery_json["adc_mv"] = battery.adc_mv;
    battery_json["adc_voltage_v"] = battery.adc_voltage_v;
    battery_json["voltage_v"] = battery.voltage_v;
    battery_json["percent"] = battery.percent;
    battery_json["sampled_ms"] = battery.sampled_ms;
    battery_json["show_percent"] = battery_service_show_percent();
    battery_json["multiplier"] = battery_service_voltage_multiplier();
    battery_json["critical_percent"] = battery_service_critical_percent();

    HomeAssistantConnectionTest ha_test = {};
    home_assistant_get_connection_test(ha_test);
    JsonObject ha = doc["home_assistant_test"].to<JsonObject>();
    ha["valid"] = ha_test.valid;
    ha["pending"] = ha_test.pending;
    ha["in_progress"] = ha_test.in_progress;
    ha["authenticated"] = ha_test.authenticated;
    ha["http_code"] = ha_test.http_code;
    ha["latency_ms"] = ha_test.latency_ms;
    ha["tested_ms"] = ha_test.tested_ms;
    ha["message"] = ha_test.message;

    WeatherSourceDiagnostics wx = {};
    weather_service_get_source_diagnostics(wx);
    JsonObject weather = doc["weather_sources"].to<JsonObject>();
    weather["valid"] = wx.valid;
    weather["pending"] = wx.pending;
    weather["in_progress"] = wx.in_progress;
    weather["current_ok"] = wx.current_ok;
    weather["daily_ok"] = wx.daily_ok;
    weather["hourly_ok"] = wx.hourly_ok;
    weather["daily_count"] = wx.daily_count;
    weather["hourly_count"] = wx.hourly_count;
    weather["latency_ms"] = wx.latency_ms;
    weather["tested_ms"] = wx.tested_ms;
    weather["current_daily_entity"] = wx.current_daily_entity;
    weather["hourly_entity"] = wx.hourly_entity;
    weather["message"] = wx.message;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    JsonObject ota = doc["ota"].to<JsonObject>();
    ota["running_partition"] = running ? running->label : "unknown";
    ota["target_partition"] = target ? target->label : "unavailable";
    ota["target_size"] = target ? target->size : 0;
    ota["maintenance"] = g_maintenance;
    ota["in_progress"] = g_ota_in_progress;
    ota["expected_size"] = g_ota_expected_size;
    ota["bytes_written"] = g_ota_written;
    ota["stream_sha256"] = g_ota_stream_sha256;
    ota["readback_sha256"] = g_ota_readback_sha256;
    ota["last_result"] = g_last_ota_result;
    ota["last_from"] = g_last_ota_from;
    ota["last_to"] = g_last_ota_to;
    ota["last_sha256"] = g_last_ota_sha256;
    ota["last_error"] = g_last_ota_error;
    ota["current_error"] = g_ota_error;
    ota["checkpoint_expected"] = g_last_ota_checkpoint_expected;
    ota["checkpoint_written"] = g_last_ota_checkpoint_written;
    ota["checkpoint_stage"] = g_last_ota_checkpoint_stage;
    ota["reboot_scheduled"] = g_reboot_at_ms != 0;
    ota["scheduled_reboot_source"] = g_scheduled_reboot_source;

    send_json(doc);
}

void handle_ota_preflight() {
    if (!ensure_auth()) return;
    if (g_ota_in_progress) { send_error(409, "OTA upload is already in progress"); return; }
    if (g_reboot_at_ms != 0) { send_error(409, "A software reboot is already scheduled"); return; }
    const size_t image_size = static_cast<size_t>(strtoull(g_server.arg("size").c_str(), nullptr, 10));
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target || target == running) {
        send_error(500, "No inactive OTA partition is available");
        return;
    }
    if (image_size == 0) { send_error(400, "Firmware image size is missing or zero"); return; }
    if (image_size > target->size) {
        send_error(413, (String("Firmware is too large for ") + target->label + ": " +
                         String(static_cast<unsigned long>(image_size)) + " > " +
                         String(static_cast<unsigned long>(target->size)) + " bytes").c_str());
        return;
    }

    g_ota_target_partition = target;
    g_ota_expected_size = image_size;
    g_ota_written = 0;
    g_ota_stream_sha256 = "";
    g_ota_readback_sha256 = "";
    g_ota_error = "";
    g_ota_last_logged_percent = 0;
    g_ota_last_persisted_percent = 0;
    g_last_ota_checkpoint_expected = static_cast<uint32_t>(image_size);
    g_last_ota_checkpoint_written = 0;
    g_last_ota_checkpoint_stage = "preflight";

    ESP_LOGI("FamilyCalendar", "OTA preflight: running=%s target=%s image=%u bytes available=%u bytes",
             running ? running->label : "unknown", target->label,
             static_cast<unsigned>(image_size), static_cast<unsigned>(target->size));
    JsonDocument doc;
    doc["ok"] = true;
    doc["running_partition"] = running ? running->label : "unknown";
    doc["target_partition"] = target->label;
    doc["image_size"] = image_size;
    doc["available_bytes"] = target->size;
    send_json(doc);
}

void handle_maintenance_start() {
    if (!ensure_auth()) return;
    if (g_ota_in_progress) {
        send_error(409, "OTA upload is already in progress");
        return;
    }
    g_maintenance = true;
    JsonDocument doc;
    doc["ok"] = true;
    doc["ota_ready"] = ota_ready();
    send_json(doc);
    ESP_LOGI("FamilyCalendar", "Web maintenance mode enabled; waiting for HA worker to drain");
}

void handle_maintenance_cancel() {
    if (!ensure_auth()) return;
    if (g_ota_in_progress) {
        send_error(409, "Cannot cancel maintenance during firmware upload");
        return;
    }
    g_maintenance = false;
    JsonDocument doc;
    doc["ok"] = true;
    send_json(doc);
    ESP_LOGI("FamilyCalendar", "Web maintenance mode cancelled");
}

void handle_update_upload() {
    HTTPUpload &upload = g_server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        g_ota_error = "";
        g_ota_succeeded = false;
        g_ota_accepting = false;
        g_ota_written = 0;
        g_ota_stream_sha256 = "";
        g_ota_readback_sha256 = "";
        g_ota_last_logged_percent = 0;
        g_ota_last_persisted_percent = 0;

        if (!g_server.authenticate(g_admin_user.c_str(), g_admin_password.c_str())) {
            g_ota_error = "Authentication required";
            return;
        }
        if (g_reboot_at_ms != 0) {
            g_ota_error = "A software reboot is already scheduled; OTA cannot start";
            return;
        }
        if (!g_maintenance || !ota_ready()) {
            g_ota_error = "Home Assistant worker is not idle; start maintenance and wait for OTA ready";
            return;
        }
        if (!upload.filename.endsWith(".bin")) {
            g_ota_error = "Firmware file must end in .bin";
            return;
        }
        if (!g_ota_target_partition || g_ota_expected_size == 0) {
            g_ota_error = "OTA preflight was not completed";
            return;
        }
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        if (!next || next != g_ota_target_partition) {
            g_ota_error = "OTA target partition changed after preflight";
            return;
        }
        if (g_ota_expected_size > g_ota_target_partition->size) {
            g_ota_error = "Firmware exceeds inactive OTA partition size";
            return;
        }

        /* Persist intent before touching the inactive app slot. If the device
         * resets anywhere after this point, the next boot can report exactly
         * where the upload stopped instead of losing all OTA state. */
        persist_ota_upload_checkpoint("starting", true);
        if (!Update.begin(g_ota_expected_size, U_FLASH)) {
            g_ota_error = String("Update.begin failed: ") + Update.errorString() +
                          " (code " + String(static_cast<unsigned>(Update.getError())) + ")";
            ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
            record_ota_failure(g_ota_error);
            return;
        }

        g_ota_stream_hash.begin();
        g_ota_in_progress = true;
        g_ota_accepting = true;
        persist_ota_upload_checkpoint("writing", true);
        if (board_display_awake()) {
            g_ota_restore_backlight = saved_backlight();
            const uint8_t ota_backlight = g_ota_restore_backlight > 35 ? 35 : g_ota_restore_backlight;
            board_set_backlight(ota_backlight);
            g_ota_dimmed = true;
        }
        ESP_LOGI("FamilyCalendar", "OTA upload started: %s, %u bytes -> %s (%u bytes)",
                 upload.filename.c_str(), static_cast<unsigned>(g_ota_expected_size),
                 g_ota_target_partition->label,
                 static_cast<unsigned>(g_ota_target_partition->size));
        return;
    }

    if (!g_ota_accepting) return;

    if (upload.status == UPLOAD_FILE_WRITE) {
        const size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            g_ota_error = String("OTA write failed after ") + String(static_cast<unsigned long>(g_ota_written)) + " bytes: " +
                          Update.errorString() + " (code " + String(static_cast<unsigned>(Update.getError())) + ")";
            ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
            g_ota_accepting = false;
            Update.abort();
            g_ota_in_progress = false;
            record_ota_failure(g_ota_error);
            restore_backlight_after_failed_ota();
            return;
        }

        g_ota_stream_hash.add(upload.buf, written);
        g_ota_written += written;
        const uint8_t percent = g_ota_expected_size
                                    ? static_cast<uint8_t>((g_ota_written * 100ULL) / g_ota_expected_size)
                                    : 0;
        if (percent >= g_ota_last_logged_percent + 10 || percent == 100) {
            g_ota_last_logged_percent = percent;
            ESP_LOGI("FamilyCalendar", "OTA write progress: %u%% (%u/%u bytes)",
                     percent, static_cast<unsigned>(g_ota_written),
                     static_cast<unsigned>(g_ota_expected_size));
        }
        if (percent >= g_ota_last_persisted_percent + 10 || percent == 100) {
            g_ota_last_persisted_percent = percent;
            persist_ota_upload_checkpoint("writing", true);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        persist_ota_upload_checkpoint("finalizing", true);
        g_ota_stream_hash.calculate();
        g_ota_stream_sha256 = g_ota_stream_hash.toString();

        if (g_ota_written != g_ota_expected_size) {
            g_ota_error = String("OTA size mismatch: received ") + String(static_cast<unsigned long>(g_ota_written)) +
                          " of " + String(static_cast<unsigned long>(g_ota_expected_size)) + " bytes";
            Update.abort();
            g_ota_in_progress = false;
            g_ota_accepting = false;
            record_ota_failure(g_ota_error);
            restore_backlight_after_failed_ota();
            ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
            return;
        }

        if (!Update.end(false)) {
            g_ota_error = String("OTA finalize failed: ") + Update.errorString() +
                          " (code " + String(static_cast<unsigned>(Update.getError())) + ")";
            g_ota_in_progress = false;
            g_ota_accepting = false;
            record_ota_failure(g_ota_error);
            restore_backlight_after_failed_ota();
            ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
            return;
        }

        ESP_LOGI("FamilyCalendar", "OTA stream SHA-256: %s", g_ota_stream_sha256.c_str());
        persist_ota_upload_checkpoint("verifying", true);
        if (!hash_partition_sha256(g_ota_target_partition, g_ota_expected_size, g_ota_readback_sha256)) {
            g_ota_error = "Could not SHA-256 verify the written OTA partition";
        } else if (!g_ota_readback_sha256.equalsIgnoreCase(g_ota_stream_sha256)) {
            g_ota_error = String("OTA SHA-256 mismatch: stream=") + g_ota_stream_sha256 +
                          " flash=" + g_ota_readback_sha256;
        }

        if (!g_ota_error.isEmpty()) {
            const esp_partition_t *running = esp_ota_get_running_partition();
            if (running) esp_ota_set_boot_partition(running);
            g_ota_in_progress = false;
            g_ota_accepting = false;
            record_ota_failure(g_ota_error);
            restore_backlight_after_failed_ota();
            ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
            return;
        }

        ESP_LOGI("FamilyCalendar", "OTA readback SHA-256 verified: %s", g_ota_readback_sha256.c_str());
        g_ota_succeeded = true;
        g_ota_accepting = false;
        record_ota_pending();
        schedule_reboot("ota-success", 3000, true);  // fail-safe if final HTTP response is interrupted
        /* Keep g_ota_in_progress asserted until reboot so no HA traffic resumes. */
        ESP_LOGI("FamilyCalendar", "OTA image accepted: %u bytes, target=%s",
                 static_cast<unsigned>(g_ota_written), g_ota_target_partition->label);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_ota_error = String("OTA upload aborted after ") + String(static_cast<unsigned long>(g_ota_written)) + " bytes";
        g_ota_accepting = false;
        g_ota_in_progress = false;
        record_ota_failure(g_ota_error);
        restore_backlight_after_failed_ota();
        ESP_LOGE("FamilyCalendar", "%s", g_ota_error.c_str());
    }
}

void handle_update_complete() {
    if (!ensure_auth()) return;
    if (!g_ota_succeeded) {
        g_maintenance = false;
        restore_backlight_after_failed_ota();
        send_error(500, g_ota_error.isEmpty() ? "OTA update failed" : g_ota_error.c_str());
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["rebooting"] = true;
    doc["bytes"] = g_ota_written;
    doc["sha256"] = g_ota_readback_sha256;
    doc["target_partition"] = g_ota_target_partition ? g_ota_target_partition->label : "unknown";
    send_json(doc);
    schedule_reboot("ota-success", 1200, true);
}

void handle_reboot() {
    if (!ensure_auth()) return;
    if (g_ota_in_progress || g_maintenance) {
        send_error(409, "Reboot is blocked while OTA maintenance/upload is active");
        return;
    }
    if (!schedule_reboot("web-reboot", 700)) {
        send_error(409, "Reboot request was blocked");
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["rebooting"] = true;
    doc["source"] = "web-reboot";
    send_json(doc);
}

void register_routes() {
    if (g_routes_registered) return;
    g_routes_registered = true;

    g_server.on("/", HTTP_GET, []() {
        if (!ensure_auth()) return;
        g_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    });
    g_server.on("/api/status", HTTP_GET, handle_status);
    g_server.on("/api/config", HTTP_GET, handle_get_config);
    g_server.on("/api/config", HTTP_POST, handle_save_config);
    g_server.on("/api/weather/refresh", HTTP_POST, handle_weather_refresh);
    g_server.on("/api/weather/test", HTTP_POST, handle_weather_test);
    g_server.on("/api/ha/test", HTTP_POST, handle_ha_test);
    g_server.on("/api/refresh/calendar", HTTP_POST, handle_refresh_calendar);
    g_server.on("/api/refresh/chores", HTTP_POST, handle_refresh_chores);
    g_server.on("/api/refresh/weather", HTTP_POST, handle_weather_refresh);
    g_server.on("/api/diagnostics", HTTP_GET, handle_diagnostics);
    g_server.on("/api/ota/preflight", HTTP_POST, handle_ota_preflight);
    g_server.on("/api/maintenance/start", HTTP_POST, handle_maintenance_start);
    g_server.on("/api/maintenance/cancel", HTTP_POST, handle_maintenance_cancel);
    g_server.on("/api/reboot", HTTP_POST, handle_reboot);
    g_server.on("/update", HTTP_POST, handle_update_complete, handle_update_upload);
    g_server.onNotFound([]() {
        if (!ensure_auth()) return;
        send_error(404, "Not found");
    });
}

} // namespace

void web_manager_begin() {
    load_boot_and_ota_diagnostics();
    load_web_preferences();
    register_routes();
    g_server.begin();
    ESP_LOGI("FamilyCalendar", "Web management server started on port %u", static_cast<unsigned>(WEB_MANAGER_PORT));
    if (strcmp(WEB_MANAGER_DEFAULT_PASSWORD, "familyhub") == 0 &&
        g_admin_user == WEB_MANAGER_DEFAULT_USER && g_admin_password == WEB_MANAGER_DEFAULT_PASSWORD) {
        ESP_LOGW("FamilyCalendar", "Web management is using the stock admin credentials; change them in the web UI");
    }
}

void web_manager_loop() {
    if (network_service_connected()) {
        const uint32_t now = millis();
        if (!g_mdns_started && (g_last_mdns_attempt_ms == 0 || now - g_last_mdns_attempt_ms >= 5000UL)) {
            g_last_mdns_attempt_ms = now;
            if (MDNS.begin(g_hostname.c_str())) {
                MDNS.addService("http", "tcp", WEB_MANAGER_PORT);
                g_mdns_started = true;
                ESP_LOGI("FamilyCalendar", "Web management: http://%s.local", g_hostname.c_str());
            }
        }
        g_server.handleClient();
    } else if (g_mdns_started) {
        MDNS.end();
        g_mdns_started = false;
        g_last_mdns_attempt_ms = 0;
    }

    if (g_reboot_at_ms != 0 && static_cast<int32_t>(millis() - g_reboot_at_ms) >= 0) {
        if (g_ota_in_progress && !g_ota_succeeded) {
            ESP_LOGE("FamilyCalendar",
                     "Blocked scheduled reboot during active OTA write: source=%s written=%u/%u",
                     g_scheduled_reboot_source.c_str(),
                     static_cast<unsigned>(g_ota_written),
                     static_cast<unsigned>(g_ota_expected_size));
            g_reboot_at_ms = 0;
            g_scheduled_reboot_source = "";
        } else {
            ESP_LOGW("FamilyCalendar", "Executing software reboot: source=%s",
                     g_scheduled_reboot_source.isEmpty() ? "unknown" : g_scheduled_reboot_source.c_str());
            Serial0.flush();
            delay(50);
            ESP.restart();
        }
    }
}

bool web_manager_maintenance_active() {
    return g_maintenance;
}

bool web_manager_ota_in_progress() {
    return g_ota_in_progress;
}

void web_manager_get_weather_sources(char *current_daily, size_t current_daily_len,
                                     char *hourly, size_t hourly_len) {
    copy_weather_sources(current_daily, current_daily_len, hourly, hourly_len);
}
