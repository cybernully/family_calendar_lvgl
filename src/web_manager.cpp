#include "web_manager.h"

#include "app_config.h"
#include "board_lvgl.h"
#include "alarm_service.h"
#include "chore_service.h"
#include "home_assistant.h"
#include "network_service.h"
#include "weather_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>

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
uint32_t g_reboot_at_ms = 0;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Family Hub Management</title>
<style>
:root{color-scheme:light dark;font-family:system-ui,-apple-system,sans-serif}body{margin:0;background:#0f172a;color:#e5e7eb}.wrap{max-width:980px;margin:auto;padding:24px}.card{background:#172033;border:1px solid #334155;border-radius:14px;padding:18px;margin:0 0 16px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}.stat{background:#111827;border-radius:10px;padding:12px}.k{color:#94a3b8;font-size:.82rem}.v{font-size:1.05rem;font-weight:650;margin-top:3px}h1{margin:.2rem 0 1rem}h2{margin:.1rem 0 1rem;font-size:1.15rem}label{display:block;margin:.7rem 0 .3rem;color:#cbd5e1}input,select,button{box-sizing:border-box;font:inherit;border-radius:8px;border:1px solid #475569;padding:10px;background:#0f172a;color:#f8fafc}input,select{width:100%}button{cursor:pointer;background:#2563eb;border:0;padding:10px 16px;font-weight:650}button.secondary{background:#334155}button.danger{background:#b91c1c}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.muted{color:#94a3b8;font-size:.9rem}.warn{color:#fbbf24}.ok{color:#4ade80}.bad{color:#f87171}progress{width:100%;height:18px}code{background:#111827;padding:2px 5px;border-radius:5px}@media(max-width:600px){.wrap{padding:12px}}
</style></head><body><div class="wrap">
<h1>Family Hub <span id="version" class="muted"></span></h1>
<div class="card"><h2>Device status</h2><div class="grid" id="stats"></div><p id="defaultPw" class="warn" hidden>Default web password is still in use. Change it below.</p></div>
<div class="card"><h2>Basic configuration</h2><form id="cfg">
<div class="grid"><div><label>mDNS hostname</label><input id="hostname" name="hostname" maxlength="31"><div class="muted">Browse to <code>http://hostname.local</code> after reboot.</div></div><div><label>Admin username</label><input id="username" name="username" maxlength="31"></div><div><label>New admin password</label><input id="password" name="password" type="password" maxlength="63" placeholder="Leave blank to keep current"></div><div><label>Backlight</label><input id="backlight" name="backlight" type="number" min="10" max="100"></div><div><label>Theme</label><select id="dark" name="dark"><option value="0">Light</option><option value="1">Dark</option></select></div><div><label>Screen timeout (seconds)</label><select id="timeout" name="timeout"><option value="0">Off</option><option value="30">30</option><option value="60">60</option><option value="120">120</option><option value="300">300</option><option value="600">600</option></select></div><div><label>Chores todo entity</label><input id="chore_entity" name="chore_entity" maxlength="95" placeholder="todo.family_chores"></div><div><label>Alarmo entity</label><input id="alarm_entity" name="alarm_entity" maxlength="95" placeholder="alarm_control_panel.alarmo"></div><div><label>Weather current + daily entity</label><input id="weather_current_entity" name="weather_current_entity" maxlength="95" placeholder="weather.kwineena131"><div class="muted">Current conditions and daily forecast, e.g. WUnderground PWS.</div></div><div><label>Weather hourly entity</label><input id="weather_hourly_entity" name="weather_hourly_entity" maxlength="95" placeholder="weather.forecast_home"><div class="muted">Provider that supports <code>weather.get_forecasts</code> type <code>hourly</code>.</div></div></div>
<p class="muted">Saved settings are stored in NVS and survive firmware updates. Weather-source changes apply immediately and queue a fresh snapshot; other UI settings take effect after reboot.</p><div class="row"><button type="submit">Save configuration</button><button type="button" class="secondary" onclick="refreshWeather()">Refresh weather now</button><button type="button" class="secondary" onclick="reboot()">Reboot device</button></div><p id="cfgMsg" class="muted"></p></form></div>
<div class="card"><h2>OTA firmware update</h2><p class="muted">Upload the PlatformIO <code>firmware.bin</code>. The current UI stays responsive while Home Assistant work drains; flashing starts only when the HA worker is idle.</p><input id="fw" type="file" accept=".bin,application/octet-stream"><div style="height:10px"></div><button id="otaBtn" onclick="ota()">Upload firmware</button><div style="height:10px"></div><progress id="prog" max="100" value="0"></progress><p id="otaMsg" class="muted"></p></div>
<div class="card"><div class="row"><button class="secondary" onclick="maintenance(false)">Cancel maintenance mode</button></div><p class="muted">Management uses HTTP Basic authentication on the local network. Do not expose port 80 to the Internet.</p></div>
</div><script>
const $=id=>document.getElementById(id); const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function json(url,opt){const r=await fetch(url,opt);let j={};try{j=await r.json()}catch(e){}if(!r.ok)throw new Error(j.error||('HTTP '+r.status));return j}
function esc(s){return String(s??'').replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function refresh(){try{const s=await json('/api/status');$('version').textContent='v'+s.version;$('defaultPw').hidden=!s.default_password;const pairs=[['IP',s.ip],['mDNS',s.mdns],['Wi-Fi RSSI',s.rssi+' dBm'],['Uptime',s.uptime],['Free heap',s.free_heap],['Free PSRAM',s.free_psram],['HA',s.ha_status],['Weather',s.weather_status],['OTA slot',s.ota_partition]];$('stats').innerHTML=pairs.map(p=>`<div class="stat"><div class="k">${esc(p[0])}</div><div class="v">${esc(p[1])}</div></div>`).join('')}catch(e){}}
async function loadCfg(){const c=await json('/api/config');$('hostname').value=c.hostname;$('username').value=c.username;$('backlight').value=c.backlight;$('dark').value=c.dark?'1':'0';$('timeout').value=String(c.timeout);$('chore_entity').value=c.chore_entity||'';$('alarm_entity').value=c.alarm_entity||'';$('weather_current_entity').value=c.weather_current_entity||'';$('weather_hourly_entity').value=c.weather_hourly_entity||''}
$('cfg').addEventListener('submit',async e=>{e.preventDefault();$('cfgMsg').textContent='Saving…';const p=new URLSearchParams();for(const id of ['hostname','username','password','backlight','dark','timeout','chore_entity','alarm_entity','weather_current_entity','weather_hourly_entity'])p.set(id,$(id).value);try{const r=await json('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});$('cfgMsg').textContent=r.weather_sources_changed?(r.weather_refresh_queued?'Saved. New weather sources are refreshing now. Reboot to apply UI/admin changes.':'Saved. Weather sources changed; use Refresh weather now after current work finishes. Reboot to apply UI/admin changes.'):'Saved. Reboot to apply UI/admin changes.';$('password').value=''}catch(e){$('cfgMsg').textContent=e.message}});
async function refreshWeather(){try{const r=await json('/api/weather/refresh',{method:'POST'});$('cfgMsg').textContent=r.queued?'Weather refresh queued.':'Weather refresh is already pending/in progress.'}catch(e){$('cfgMsg').textContent=e.message}}
async function maintenance(on){try{return await json(on?'/api/maintenance/start':'/api/maintenance/cancel',{method:'POST'})}catch(e){$('otaMsg').textContent=e.message;throw e}}
async function ota(){const f=$('fw').files[0];if(!f){$('otaMsg').textContent='Choose firmware.bin first.';return}if(!f.name.toLowerCase().endsWith('.bin')){$('otaMsg').textContent='Firmware file must end in .bin';return}$('otaBtn').disabled=true;$('otaMsg').textContent='Entering maintenance mode…';$('prog').value=0;try{await maintenance(true);let ready=false;for(let i=0;i<120;i++){const s=await json('/api/status');if(s.ota_ready){ready=true;break}$('otaMsg').textContent='Waiting for Home Assistant worker to become idle…';await sleep(250)}if(!ready)throw new Error('Timed out waiting for Home Assistant worker to become idle');$('otaMsg').textContent='Uploading firmware…';await new Promise((resolve,reject)=>{const x=new XMLHttpRequest();x.open('POST','/update');x.upload.onprogress=e=>{if(e.lengthComputable)$('prog').value=Math.round(e.loaded*100/e.total)};x.onload=()=>x.status>=200&&x.status<300?resolve():reject(new Error(x.responseText||('HTTP '+x.status)));x.onerror=()=>reject(new Error('Upload failed'));const d=new FormData();d.append('firmware',f,f.name);x.send(d)});$('prog').value=100;$('otaMsg').textContent='Update complete. Device is rebooting…'}catch(e){$('otaMsg').textContent=e.message;try{await maintenance(false)}catch(_){}$('otaBtn').disabled=false}}
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
    doc["free_heap"] = ESP.getFreeHeap();
    doc["free_psram"] = ESP.getFreePsram();
    doc["ha_status"] = home_assistant_status();
    doc["weather_status"] = weather_service_status();
    doc["maintenance"] = g_maintenance;
    doc["ota_in_progress"] = g_ota_in_progress;
    doc["ota_ready"] = ota_ready();
    doc["mdns"] = String("http://") + g_hostname + ".local";
    doc["default_password"] = (strcmp(WEB_MANAGER_DEFAULT_PASSWORD, "familyhub") == 0 &&
                               g_admin_user == WEB_MANAGER_DEFAULT_USER &&
                               g_admin_password == WEB_MANAGER_DEFAULT_PASSWORD);
    const esp_partition_t *running = esp_ota_get_running_partition();
    doc["ota_partition"] = running ? running->label : "unknown";
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

    /* These services already own their NVS entity selection.  Reuse their
     * setters so web and touchscreen configuration stay in sync. */
    chore_service_set_entity(chore_entity.c_str());
    alarm_service_set_entity(alarm_entity.c_str());

    const bool weather_queued = weather_sources_changed && !g_maintenance
                                    ? weather_service_request_refresh(true, "web weather source changed")
                                    : false;

    JsonDocument doc;
    doc["ok"] = true;
    doc["reboot_required"] = true;
    doc["weather_sources_changed"] = weather_sources_changed;
    doc["weather_refresh_queued"] = weather_queued;
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

        if (!g_server.authenticate(g_admin_user.c_str(), g_admin_password.c_str())) {
            g_ota_error = "Authentication required";
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
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            g_ota_error = String("Update.begin failed, error ") + Update.getError();
            return;
        }

        g_ota_in_progress = true;
        g_ota_accepting = true;
        if (board_display_awake()) {
            g_ota_restore_backlight = saved_backlight();
            const uint8_t ota_backlight = g_ota_restore_backlight > 35 ? 35 : g_ota_restore_backlight;
            board_set_backlight(ota_backlight);
            g_ota_dimmed = true;
        }
        ESP_LOGI("FamilyCalendar", "OTA upload started: %s", upload.filename.c_str());
        return;
    }

    if (!g_ota_accepting) return;

    if (upload.status == UPLOAD_FILE_WRITE) {
        const size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            g_ota_error = String("OTA write failed, error ") + Update.getError();
            g_ota_accepting = false;
            Update.abort();
            g_ota_in_progress = false;
            restore_backlight_after_failed_ota();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            g_ota_succeeded = true;
            g_reboot_at_ms = millis() + 3000;  // fail-safe if the final HTTP response is interrupted
            /* Keep g_ota_in_progress asserted until reboot so the main loop
             * cannot restart Home Assistant network work after the image has
             * been committed but before the reboot response is delivered. */
            ESP_LOGI("FamilyCalendar", "OTA image accepted: %u bytes", static_cast<unsigned>(upload.totalSize));
        } else {
            g_ota_error = String("OTA finalize failed, error ") + Update.getError();
            g_ota_in_progress = false;
            restore_backlight_after_failed_ota();
        }
        g_ota_accepting = false;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        g_ota_error = "OTA upload aborted";
        g_ota_accepting = false;
        g_ota_in_progress = false;
        restore_backlight_after_failed_ota();
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
    send_json(doc);
    g_reboot_at_ms = millis() + 1200;
}

void handle_reboot() {
    if (!ensure_auth()) return;
    JsonDocument doc;
    doc["ok"] = true;
    doc["rebooting"] = true;
    send_json(doc);
    g_reboot_at_ms = millis() + 700;
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
        Serial0.flush();
        delay(50);
        ESP.restart();
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
