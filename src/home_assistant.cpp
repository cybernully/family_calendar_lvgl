#include "home_assistant.h"
#include "app_config.h"
#include "app_secrets.h"
#include "network_service.h"
#include "calendar_model.h"
#include "chore_service.h"
#include "alarm_service.h"
#include "time_service.h"
#include "weather_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <limits.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>

namespace {

constexpr int PREFETCH_SLOT_COUNT = HA_PREFETCH_RADIUS_WEEKS * 2 + 1;
static_assert(PREFETCH_SLOT_COUNT == 3, "Calendar prefetch expects a +/-1 week window");

struct TodoListInfo {
    char entity_id[96];
    char name[80];
};

struct PendingChoreAction {
    char entity_id[96];
    char uid[96];
    bool completed;
};

struct AlarmPanelInfo {
    char entity_id[96];
    char name[80];
};

enum class AlarmActionType : uint8_t {
    Arm = 0,
    Disarm,
    SkipDelay,
};

struct PendingAlarmAction {
    AlarmActionType type;
    char entity_id[96];
    char mode[20];
    char code[32];
};

char g_status[176] = "Home Assistant starting";
volatile bool g_authenticated = false;
volatile bool g_discovery_done = false;
TaskHandle_t g_worker_task = nullptr;
volatile bool g_boot_calendar_pending = true;
uint32_t g_connected_since_ms = 0;

/* Calendar worker state. */
volatile bool g_force_calendar_sync = true;
volatile bool g_calendar_sync_in_progress = false;
volatile bool g_calendar_pending_ready = false;
volatile int g_requested_center_offset = 0;
volatile int g_current_week_offset = 0;
volatile uint32_t g_last_calendar_sync_ms = 0;
volatile uint32_t g_last_calendar_attempt_ms = 0;
time_t g_active_week_start = 0;
volatile uint32_t g_last_week_change_ms = 0;

CalendarEvent *g_sync_week_events = nullptr;
size_t g_sync_week_counts[PREFETCH_SLOT_COUNT] = {};
time_t g_sync_week_starts[PREFETCH_SLOT_COUNT] = {};
uint32_t g_sync_completed_ms = 0;

/* Chore worker state. */
volatile bool g_force_chore_sync = true;
volatile bool g_chore_sync_in_progress = false;
volatile bool g_chore_pending_ready = false;
volatile uint32_t g_last_chore_sync_ms = 0;
volatile uint32_t g_last_chore_attempt_ms = 0;
ChoreItem g_sync_chores[HA_MAX_CHORES] = {};
size_t g_sync_chore_count = 0;
char g_pending_chore_entity[96] = {};

/* To-do entity discovery is only requested when the Chores UI needs it. */
volatile bool g_todo_discovery_requested = false;
volatile bool g_todo_discovery_in_progress = false;
volatile bool g_todo_lists_ready = false;
volatile bool g_todo_lists_publish_pending = false;
TodoListInfo g_todo_lists[HA_MAX_TODO_LISTS] = {};
size_t g_todo_list_count = 0;

/* Main-thread change notifications. */
uint32_t g_change_flags = HA_CHANGE_NONE;

/* Chore updates are queued from LVGL and executed by the HA worker. */
PendingChoreAction g_chore_actions[HA_CHORE_ACTION_QUEUE_SIZE] = {};
size_t g_chore_action_head = 0;
size_t g_chore_action_tail = 0;
size_t g_chore_action_count = 0;
portMUX_TYPE g_chore_action_mux = portMUX_INITIALIZER_UNLOCKED;

/* Alarmo worker state. */
volatile bool g_alarm_discovery_requested = false;
volatile bool g_alarm_discovery_in_progress = false;
volatile bool g_alarm_panels_ready = false;
volatile bool g_alarm_panels_publish_pending = false;
AlarmPanelInfo g_alarm_panels[HA_MAX_ALARM_PANELS] = {};
size_t g_alarm_panel_count = 0;

volatile bool g_force_alarm_sync = true;
volatile bool g_alarm_sync_in_progress = false;
volatile bool g_alarm_pending_ready = false;
volatile uint32_t g_last_alarm_sync_ms = 0;
volatile uint32_t g_last_alarm_attempt_ms = 0;
AlarmoSnapshot g_sync_alarm = {};

PendingAlarmAction g_alarm_actions[HA_ALARM_ACTION_QUEUE_SIZE] = {};
size_t g_alarm_action_head = 0;
size_t g_alarm_action_tail = 0;
size_t g_alarm_action_count = 0;
portMUX_TYPE g_alarm_action_mux = portMUX_INITIALIZER_UNLOCKED;

const char *CALENDAR_IDS[4] = {
    HA_CALENDAR_1,
    HA_CALENDAR_2,
    HA_CALENDAR_3,
    HA_CALENDAR_4,
};

CalendarEvent *sync_slot_events(int slot) {
    if (!g_sync_week_events || slot < 0 || slot >= PREFETCH_SLOT_COUNT) return nullptr;
    return g_sync_week_events + static_cast<size_t>(slot) * HA_MAX_EVENTS;
}

bool usable_value(const char *value, const char *placeholder) {
    if (!value || !value[0]) return false;
    if (placeholder && strcmp(value, placeholder) == 0) return false;
    return true;
}

String clean_base_url() {
    String base(HA_BASE_URL);
    while (base.endsWith("/")) base.remove(base.length() - 1);
    return base;
}

String url_encode(const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    String out;
    if (!value) return out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

int http_request(const char *method, const String &path, const String *body, String &payload) {
    payload = "";
    const String url = clean_base_url() + path;
    HTTPClient http;
    http.setConnectTimeout(HA_HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HA_HTTP_TIMEOUT_MS);
    http.setUserAgent(String("FamilyCalendar/") + APP_VERSION);

    const String auth = String("Bearer ") + HA_ACCESS_TOKEN;
    int code = -1;

    auto execute = [&](auto &client) -> int {
        if (!http.begin(client, url)) return -21;
        http.addHeader("Authorization", auth);
        http.addHeader("Accept", "application/json");
        if (body) http.addHeader("Content-Type", "application/json");

        int result = -1;
        if (strcmp(method, "GET") == 0) {
            result = http.GET();
        } else if (strcmp(method, "POST") == 0 && body) {
            result = http.POST(*body);
        }
        if (result > 0) payload = http.getString();
        http.end();
        return result;
    };

    if (url.startsWith("https://")) {
        WiFiClientSecure client;
#if defined(HA_TLS_CA_CERT_PEM)
        client.setCACert(HA_TLS_CA_CERT_PEM);
#elif HA_TLS_ALLOW_INSECURE
        client.setInsecure();
        static bool tls_warning_logged = false;
        if (!tls_warning_logged) {
            ESP_LOGW("FamilyCalendar",
                     "HA HTTPS: TLS encrypted; server certificate verification disabled");
            tls_warning_logged = true;
        }
#else
        ESP_LOGE("FamilyCalendar",
                 "HA HTTPS requested but no CA certificate is configured and insecure TLS is disabled");
        code = -20;
#endif
#if defined(HA_TLS_CA_CERT_PEM) || HA_TLS_ALLOW_INSECURE
        code = execute(client);
#endif
    } else {
        WiFiClient client;
        code = execute(client);
    }
    if (code >= 200 && code < 300) g_authenticated = true;
    return code;
}

int http_get(const String &path, String &payload) {
    return http_request("GET", path, nullptr, payload);
}

int http_post(const String &path, const String &body, String &payload) {
    return http_request("POST", path, &body, payload);
}

/* Days since Unix epoch. Avoids depending on libc timegm() for ISO timestamps. */
int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_iso8601(const char *text, time_t &epoch, bool &all_day) {
    epoch = 0;
    all_day = false;
    if (!text || strlen(text) < 10) return false;

    int year = 0, month = 0, day = 0;
    if (sscanf(text, "%d-%d-%d", &year, &month, &day) != 3) return false;

    if (strlen(text) == 10) {
        struct tm local_tm = {};
        local_tm.tm_year = year - 1900;
        local_tm.tm_mon = month - 1;
        local_tm.tm_mday = day;
        local_tm.tm_isdst = -1;
        epoch = mktime(&local_tm);
        all_day = true;
        return epoch > 0;
    }

    int hour = 0, minute = 0, second = 0;
    char separator = 0;
    if (sscanf(text, "%d-%d-%d%c%d:%d:%d", &year, &month, &day,
               &separator, &hour, &minute, &second) != 7 ||
        (separator != 'T' && separator != ' ')) {
        return false;
    }

    const char *zone = text + 19;
    while (*zone && *zone != 'Z' && *zone != 'z' && *zone != '+' && *zone != '-') ++zone;

    if (*zone == 'Z' || *zone == 'z' || *zone == '+' || *zone == '-') {
        int offset_seconds = 0;
        if (*zone == '+' || *zone == '-') {
            const int sign = (*zone == '+') ? 1 : -1;
            int zh = 0, zm = 0;
            if (sscanf(zone + 1, "%d:%d", &zh, &zm) < 1) return false;
            offset_seconds = sign * (zh * 3600 + zm * 60);
        }
        const int64_t seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400LL +
                                hour * 3600LL + minute * 60LL + second - offset_seconds;
        epoch = static_cast<time_t>(seconds);
        return true;
    }

    struct tm local_tm = {};
    local_tm.tm_year = year - 1900;
    local_tm.tm_mon = month - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = second;
    local_tm.tm_isdst = -1;
    epoch = mktime(&local_tm);
    return epoch > 0;
}

size_t configured_calendar_count() {
    size_t count = 0;
    for (const char *id : CALENDAR_IDS) if (id && id[0]) ++count;
    return count;
}

time_t week_start_for_offset(int offset) {
    time_t start = time_service_start_of_week_midnight(time_service_now());
    return time_service_add_days(start, offset * 7);
}

bool discover_calendars() {
    String payload;
    const int code = http_get("/api/calendars", payload);
    if (code == 401) {
        snprintf(g_status, sizeof(g_status), "HA authentication failed (401)");
        g_authenticated = false;
        return false;
    }
    if (code != 200) {
        snprintf(g_status, sizeof(g_status), "HA connection error HTTP %d", code);
        ESP_LOGE("FamilyCalendar", "GET /api/calendars failed: HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        snprintf(g_status, sizeof(g_status), "HA calendar discovery JSON error");
        ESP_LOGE("FamilyCalendar", "Could not parse /api/calendars: %s", error.c_str());
        return false;
    }

    g_authenticated = true;
    g_discovery_done = true;
    Serial0.println("\n[HA] Available Home Assistant calendars:");
    for (JsonObject item : doc.as<JsonArray>()) {
        Serial0.printf("[HA]   %-42s  %s\n",
                       item["entity_id"] | "",
                       item["name"] | "");
    }
    Serial0.println();

    snprintf(g_status, sizeof(g_status), configured_calendar_count() == 0
             ? "HA connected | configure calendar IDs"
             : "HA connected | calendars discovered");
    return true;
}

const char *calendar_short_name(const char *entity_id) {
    static const char prefix[] = "calendar.";
    if (!entity_id) return "calendar";
    if (strncmp(entity_id, prefix, sizeof(prefix) - 1) == 0) return entity_id + sizeof(prefix) - 1;
    return entity_id;
}

bool event_time_text(JsonObject item, const char *key, const char *&text) {
    text = "";
    JsonVariant value = item[key];
    if (value.is<JsonObject>()) {
        JsonObject time_obj = value.as<JsonObject>();
        const char *date_time = time_obj["dateTime"] | "";
        if (date_time[0]) {
            text = date_time;
            return true;
        }
        const char *date = time_obj["date"] | "";
        if (date[0]) {
            text = date;
            return true;
        }
        return false;
    }

    const char *legacy = value | "";
    if (legacy[0]) {
        text = legacy;
        return true;
    }
    return false;
}

bool event_overlaps_week(const CalendarEvent &event, time_t week_start) {
    const time_t week_end = time_service_add_days(week_start, 7);
    const time_t event_end = event.end_epoch > event.start_epoch
                                 ? event.end_epoch
                                 : event.start_epoch + 1;
    return event.start_epoch < week_end && event_end > week_start;
}

bool append_calendar_payload_to_prefetch(const String &payload, uint8_t person, const char *source_id) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonArray>()) {
        ESP_LOGE("FamilyCalendar", "HA JSON parse/type failed for %s: %s",
                 source_id, error ? error.c_str() : "not an array");
        return false;
    }

    size_t accepted = 0;
    size_t skipped = 0;
    for (JsonObject item : doc.as<JsonArray>()) {
        const char *start_text = "";
        const char *end_text = "";
        if (!event_time_text(item, "start", start_text)) {
            ++skipped;
            continue;
        }
        event_time_text(item, "end", end_text);

        time_t start_epoch = 0;
        time_t end_epoch = 0;
        bool start_all_day = false;
        bool end_all_day = false;
        if (!parse_iso8601(start_text, start_epoch, start_all_day)) {
            ++skipped;
            continue;
        }
        if (!parse_iso8601(end_text, end_epoch, end_all_day) || end_epoch <= start_epoch) {
            end_epoch = start_epoch + (start_all_day ? 86400 : 3600);
        }

        CalendarEvent parsed = {};
        parsed.start_epoch = start_epoch;
        parsed.end_epoch = end_epoch;
        parsed.all_day = start_all_day;
        parsed.person = person;
        snprintf(parsed.title, sizeof(parsed.title), "%s", item["summary"] | "(Untitled event)");
        snprintf(parsed.location, sizeof(parsed.location), "%s", item["location"] | "");
        snprintf(parsed.description, sizeof(parsed.description), "%s", item["description"] | "");
        snprintf(parsed.status, sizeof(parsed.status), "%s", item["status"] | "");
        snprintf(parsed.source, sizeof(parsed.source), "%s", source_id);

        bool stored_somewhere = false;
        for (int slot = 0; slot < PREFETCH_SLOT_COUNT; ++slot) {
            if (!event_overlaps_week(parsed, g_sync_week_starts[slot])) continue;
            if (g_sync_week_counts[slot] >= HA_MAX_EVENTS) {
                ESP_LOGW("FamilyCalendar", "Week event limit reached for slot %d", slot);
                continue;
            }
            sync_slot_events(slot)[g_sync_week_counts[slot]++] = parsed;
            stored_somewhere = true;
        }
        if (stored_somewhere) ++accepted;
    }

    ESP_LOGI("FamilyCalendar", "HA %s: %u event(s) accepted into 3-week cache, %u skipped",
             source_id, static_cast<unsigned>(accepted), static_cast<unsigned>(skipped));
    return true;
}

bool sync_calendar_window(int center_offset) {
    if (configured_calendar_count() == 0 || !g_sync_week_events) return true;

    const time_t center_start = week_start_for_offset(center_offset);
    for (int slot = 0; slot < PREFETCH_SLOT_COUNT; ++slot) {
        const int delta = slot - HA_PREFETCH_RADIUS_WEEKS;
        g_sync_week_starts[slot] = time_service_add_days(center_start, delta * 7);
        g_sync_week_counts[slot] = 0;
        memset(sync_slot_events(slot), 0, HA_MAX_EVENTS * sizeof(CalendarEvent));
    }

    const time_t range_start = g_sync_week_starts[0];
    const time_t range_end = time_service_add_days(g_sync_week_starts[PREFETCH_SLOT_COUNT - 1], 7);
    char start_iso[32];
    char end_iso[32];
    time_service_format_utc_iso(range_start, start_iso, sizeof(start_iso));
    time_service_format_utc_iso(range_end, end_iso, sizeof(end_iso));

    snprintf(g_status, sizeof(g_status), "HA loading selected 3-week window...");
    ESP_LOGI("FamilyCalendar", "Syncing 3-week window %s -> %s with one calendar.get_events request",
             start_iso, end_iso);

    /*
     * Home Assistant's calendar.get_events action accepts multiple calendar
     * targets and returns a response keyed by entity id.  Using one request
     * replaces the four sequential /api/calendars/<id> calls and substantially
     * reduces the amount of ESP-Hosted/TLS traffic on this board.
     */
    JsonDocument request;
    JsonArray entities = request["entity_id"].to<JsonArray>();
    for (const char *entity_id : CALENDAR_IDS) {
        if (entity_id && entity_id[0]) entities.add(entity_id);
    }
    request["start_date_time"] = start_iso;
    request["end_date_time"] = end_iso;
    String body;
    serializeJson(request, body);

    String payload;
    ESP_LOGI("FamilyCalendar", "HA request start: calendar.get_events (%u calendar(s))",
             static_cast<unsigned>(configured_calendar_count()));
    const uint32_t request_started = millis();
    const int code = http_post("/api/services/calendar/get_events?return_response", body, payload);
    ESP_LOGI("FamilyCalendar", "HA request end: calendar.get_events HTTP %d in %lums",
             code, static_cast<unsigned long>(millis() - request_started));

    if (code == 401) {
        g_authenticated = false;
        snprintf(g_status, sizeof(g_status), "HA authentication failed (401)");
        return false;
    }
    if (code != 200) {
        snprintf(g_status, sizeof(g_status), "HA calendar fetch failed HTTP %d", code);
        ESP_LOGE("FamilyCalendar", "calendar.get_events failed: HTTP %d payload=%.120s",
                 code, payload.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        snprintf(g_status, sizeof(g_status), "HA calendar response JSON error");
        ESP_LOGE("FamilyCalendar", "calendar.get_events JSON error: %s", error.c_str());
        return false;
    }

    JsonVariant response = doc["service_response"];
    if (response.isNull()) response = doc.as<JsonVariant>();

    size_t successful = 0;
    size_t failed = 0;
    for (uint8_t person = 0; person < 4; ++person) {
        const char *entity_id = CALENDAR_IDS[person];
        if (!entity_id || !entity_id[0]) continue;

        JsonVariant events_variant = response[entity_id]["events"];
        if (!events_variant.is<JsonArray>()) {
            ++failed;
            ESP_LOGW("FamilyCalendar", "calendar.get_events response missing events for %s", entity_id);
            continue;
        }

        String calendar_payload;
        serializeJson(events_variant, calendar_payload);
        if (!append_calendar_payload_to_prefetch(calendar_payload, person, entity_id)) {
            ++failed;
            continue;
        }
        ++successful;
        vTaskDelay(1);
    }

    if (successful > 0) {
        for (int slot = 0; slot < PREFETCH_SLOT_COUNT; ++slot) {
            calendar_model_sort(sync_slot_events(slot), g_sync_week_counts[slot]);
        }
        g_sync_completed_ms = millis();
        __sync_synchronize();
        g_calendar_pending_ready = true;
    }

    if (failed > 0) {
        snprintf(g_status, sizeof(g_status), "HA partial calendar load | %u/%u calendars",
                 static_cast<unsigned>(successful),
                 static_cast<unsigned>(configured_calendar_count()));
        ESP_LOGW("FamilyCalendar", "HA partial 3-week sync: %u/%u calendars",
                 static_cast<unsigned>(successful),
                 static_cast<unsigned>(configured_calendar_count()));
        return false;
    }

    snprintf(g_status, sizeof(g_status), "HA synced | 3 weeks cached | v%s", APP_VERSION);
    ESP_LOGI("FamilyCalendar", "HA 3-week sync complete: prev=%u current=%u next=%u events",
             static_cast<unsigned>(g_sync_week_counts[0]),
             static_cast<unsigned>(g_sync_week_counts[1]),
             static_cast<unsigned>(g_sync_week_counts[2]));
    return true;
}

bool discover_todo_lists() {
    static const char *kTodoTemplate = R"HA(
{% set ns = namespace(items=[]) %}
{% for s in states.todo %}
  {% set ns.items = ns.items + [{'entity_id': s.entity_id, 'name': s.name}] %}
{% endfor %}
{{ {'items': ns.items} | to_json }}
)HA";

    JsonDocument request;
    request["template"] = kTodoTemplate;
    String body;
    serializeJson(request, body);

    ESP_LOGI("FamilyCalendar", "HA request start: compact to-do discovery");
    String payload;
    const int code = http_post("/api/template", body, payload);
    if (code != 200) {
        ESP_LOGE("FamilyCalendar", "HA to-do discovery failed: HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc["items"].is<JsonArray>()) {
        ESP_LOGE("FamilyCalendar", "HA to-do discovery JSON failed: %s",
                 error ? error.c_str() : "items array missing");
        return false;
    }

    TodoListInfo found[HA_MAX_TODO_LISTS] = {};
    size_t count = 0;
    for (JsonObject state : doc["items"].as<JsonArray>()) {
        if (count >= HA_MAX_TODO_LISTS) break;
        const char *entity_id = state["entity_id"] | "";
        if (!entity_id[0]) continue;
        snprintf(found[count].entity_id, sizeof(found[count].entity_id), "%s", entity_id);
        snprintf(found[count].name, sizeof(found[count].name), "%s", state["name"] | entity_id);
        ++count;
    }

    memcpy(g_todo_lists, found, sizeof(found));
    g_todo_list_count = count;
    __sync_synchronize();
    g_todo_lists_ready = true;
    g_todo_lists_publish_pending = true;

    ESP_LOGI("FamilyCalendar", "Discovered %u Home Assistant to-do list(s)", static_cast<unsigned>(count));
    return true;
}

bool fetch_chores() {
    char entity_id[96];
    snprintf(entity_id, sizeof(entity_id), "%s", chore_service_entity());
    if (!entity_id[0]) return false;

    JsonDocument request;
    request["entity_id"] = entity_id;
    JsonArray statuses = request["status"].to<JsonArray>();
    statuses.add("needs_action");
    statuses.add("completed");
    String body;
    serializeJson(request, body);

    String payload;
    const int code = http_post("/api/services/todo/get_items?return_response", body, payload);
    if (code != 200) {
        snprintf(g_status, sizeof(g_status), "HA chores fetch failed HTTP %d", code);
        ESP_LOGE("FamilyCalendar", "todo.get_items for %s failed: HTTP %d payload=%.120s",
                 entity_id, code, payload.c_str());
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        snprintf(g_status, sizeof(g_status), "HA chores JSON error");
        ESP_LOGE("FamilyCalendar", "todo.get_items JSON error: %s", error.c_str());
        return false;
    }

    JsonVariant service_response = doc["service_response"];
    if (service_response.isNull()) service_response = doc.as<JsonVariant>();
    JsonVariant list_response = service_response[entity_id];
    JsonArray items = list_response["items"].as<JsonArray>();
    if (items.isNull()) {
        snprintf(g_status, sizeof(g_status), "HA chores response missing items");
        ESP_LOGE("FamilyCalendar", "todo.get_items response for %s did not contain items", entity_id);
        return false;
    }

    g_sync_chore_count = 0;
    memset(g_sync_chores, 0, sizeof(g_sync_chores));
    for (JsonObject item : items) {
        if (g_sync_chore_count >= HA_MAX_CHORES) break;
        ChoreItem &out = g_sync_chores[g_sync_chore_count++];
        const char *uid = item["uid"] | "";
        if (!uid[0]) uid = item["summary"] | "";
        snprintf(out.uid, sizeof(out.uid), "%s", uid);
        snprintf(out.summary, sizeof(out.summary), "%s", item["summary"] | "(Untitled chore)");
        snprintf(out.description, sizeof(out.description), "%s", item["description"] | "");
        snprintf(out.due, sizeof(out.due), "%s", item["due"] | "");
        const char *status = item["status"] | "needs_action";
        out.done = strcmp(status, "completed") == 0;
    }

    snprintf(g_pending_chore_entity, sizeof(g_pending_chore_entity), "%s", entity_id);
    __sync_synchronize();
    g_chore_pending_ready = true;
    g_last_chore_sync_ms = millis();
    ESP_LOGI("FamilyCalendar", "HA chores synced: %u item(s) from %s",
             static_cast<unsigned>(g_sync_chore_count), entity_id);
    return true;
}

bool update_chore_status(const PendingChoreAction &action) {
    JsonDocument request;
    request["entity_id"] = action.entity_id;
    request["item"] = action.uid;
    request["status"] = action.completed ? "completed" : "needs_action";
    String body;
    serializeJson(request, body);

    String payload;
    const int code = http_post("/api/services/todo/update_item", body, payload);
    if (code != 200) {
        ESP_LOGE("FamilyCalendar", "todo.update_item failed for %s: HTTP %d payload=%.120s",
                 action.uid, code, payload.c_str());
        snprintf(g_status, sizeof(g_status), "HA chore update failed HTTP %d", code);
        return false;
    }
    return true;
}

bool pop_chore_action(PendingChoreAction &out) {
    bool available = false;
    portENTER_CRITICAL(&g_chore_action_mux);
    if (g_chore_action_count > 0) {
        out = g_chore_actions[g_chore_action_head];
        g_chore_action_head = (g_chore_action_head + 1) % HA_CHORE_ACTION_QUEUE_SIZE;
        --g_chore_action_count;
        available = true;
    }
    portEXIT_CRITICAL(&g_chore_action_mux);
    return available;
}

void humanize_entity_id(const char *entity_id, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!entity_id) return;
    const char *name = strchr(entity_id, '.');
    name = name ? name + 1 : entity_id;
    size_t j = 0;
    bool capitalize = true;
    for (size_t i = 0; name[i] && j + 1 < out_len; ++i) {
        char c = name[i];
        if (c == '_') {
            c = ' ';
            capitalize = true;
        } else if (capitalize && c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
            capitalize = false;
        } else {
            capitalize = false;
        }
        out[j++] = c;
    }
    out[j] = '\0';
}

bool looks_like_alarmo(JsonObjectConst state) {
    const char *entity_id = state["entity_id"] | "";
    if (strncmp(entity_id, "alarm_control_panel.", 20) != 0) return false;
    JsonObjectConst attrs = state["attributes"].as<JsonObjectConst>();
    if (attrs.isNull()) return false;
    /* Alarmo exposes these custom state attributes on its alarm_control_panel entities. */
    return !attrs["next_state"].isNull() &&
           !attrs["open_sensors"].isNull() &&
           !attrs["last_triggered"].isNull();
}

bool discover_alarm_panels() {
    static const char *kAlarmTemplate = R"HA(
{% set ns = namespace(items=[]) %}
{% for s in states.alarm_control_panel %}
  {% set ns.items = ns.items + [{'entity_id': s.entity_id, 'name': s.name}] %}
{% endfor %}
{{ {'items': ns.items} | to_json }}
)HA";

    JsonDocument request;
    request["template"] = kAlarmTemplate;
    String body;
    serializeJson(request, body);

    ESP_LOGI("FamilyCalendar", "HA request start: compact alarm-panel discovery");
    String payload;
    const int code = http_post("/api/template", body, payload);
    if (code != 200) {
        ESP_LOGE("FamilyCalendar", "HA Alarmo discovery failed: HTTP %d", code);
        return false;
    }

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc["items"].is<JsonArray>()) {
        ESP_LOGE("FamilyCalendar", "HA Alarmo discovery JSON failed: %s",
                 error ? error.c_str() : "items array missing");
        return false;
    }

    AlarmPanelInfo found[HA_MAX_ALARM_PANELS] = {};
    size_t count = 0;
    for (JsonObjectConst state : doc["items"].as<JsonArrayConst>()) {
        if (count >= HA_MAX_ALARM_PANELS) break;
        const char *entity_id = state["entity_id"] | "";
        if (!entity_id[0]) continue;
        snprintf(found[count].entity_id, sizeof(found[count].entity_id), "%s", entity_id);
        snprintf(found[count].name, sizeof(found[count].name), "%s", state["name"] | entity_id);
        ++count;
    }

    memcpy(g_alarm_panels, found, sizeof(found));
    g_alarm_panel_count = count;
    __sync_synchronize();
    g_alarm_panels_ready = true;
    g_alarm_panels_publish_pending = true;

    ESP_LOGI("FamilyCalendar", "Discovered %u alarm panel(s)", static_cast<unsigned>(count));
    return true;
}

void append_open_sensor(AlarmoSnapshot &out, const char *entity_id) {
    if (!entity_id || !entity_id[0]) return;
    ++out.open_sensor_count;
    if (out.open_sensor_count > 5) return;

    char friendly[64];
    humanize_entity_id(entity_id, friendly, sizeof(friendly));
    if (out.open_sensors[0]) strlcat(out.open_sensors, ", ", sizeof(out.open_sensors));
    strlcat(out.open_sensors, friendly, sizeof(out.open_sensors));
}

bool parse_alarm_snapshot(const String &payload, AlarmoSnapshot &out) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonObject>()) {
        ESP_LOGE("FamilyCalendar", "Alarmo state JSON error: %s",
                 error ? error.c_str() : "not an object");
        return false;
    }

    JsonObjectConst root = doc.as<JsonObjectConst>();
    const char *entity_id = root["entity_id"] | "";
    JsonObjectConst attrs = root["attributes"].as<JsonObjectConst>();

    memset(&out, 0, sizeof(out));
    snprintf(out.entity_id, sizeof(out.entity_id), "%s", entity_id);
    snprintf(out.friendly_name, sizeof(out.friendly_name), "%s",
             attrs["friendly_name"] | entity_id);
    snprintf(out.state, sizeof(out.state), "%s", root["state"] | "unknown");
    snprintf(out.next_state, sizeof(out.next_state), "%s", attrs["next_state"] | "");
    snprintf(out.arm_mode, sizeof(out.arm_mode), "%s", attrs["arm_mode"] | "");
    snprintf(out.last_triggered, sizeof(out.last_triggered), "%s", attrs["last_triggered"] | "");
    snprintf(out.code_format, sizeof(out.code_format), "%s", attrs["code_format"] | "");
    out.supported_features = attrs["supported_features"] | 0U;
    out.delay_seconds = attrs["delay"].is<int>() ? attrs["delay"].as<int>() : 0;

    JsonVariantConst open = attrs["open_sensors"];
    if (open.is<JsonObjectConst>()) {
        for (JsonPairConst kv : open.as<JsonObjectConst>()) append_open_sensor(out, kv.key().c_str());
    } else if (open.is<JsonArrayConst>()) {
        for (JsonVariantConst value : open.as<JsonArrayConst>()) append_open_sensor(out, value.as<const char *>());
    }
    if (out.open_sensor_count > 5) {
        char extra[32];
        snprintf(extra, sizeof(extra), " +%u more", static_cast<unsigned>(out.open_sensor_count - 5));
        strlcat(out.open_sensors, extra, sizeof(out.open_sensors));
    }

    out.valid = entity_id[0] != '\0';
    return out.valid;
}

bool fetch_alarm_state() {
    char entity_id[96];
    snprintf(entity_id, sizeof(entity_id), "%s", alarm_service_entity());
    if (!entity_id[0]) return false;

    String payload;
    const int code = http_get(String("/api/states/") + entity_id, payload);
    if (code != 200) {
        snprintf(g_status, sizeof(g_status), "HA Alarmo state failed HTTP %d", code);
        ESP_LOGE("FamilyCalendar", "Alarmo state fetch for %s failed: HTTP %d payload=%.120s",
                 entity_id, code, payload.c_str());
        return false;
    }

    AlarmoSnapshot snapshot = {};
    if (!parse_alarm_snapshot(payload, snapshot)) return false;
    g_sync_alarm = snapshot;
    __sync_synchronize();
    g_alarm_pending_ready = true;
    g_last_alarm_sync_ms = millis();
    return true;
}

bool process_alarm_action(const PendingAlarmAction &action) {
    JsonDocument request;
    request["entity_id"] = action.entity_id;

    String path;
    if (action.type == AlarmActionType::Arm) {
        path = "/api/services/alarmo/arm";
        request["mode"] = action.mode[0] ? action.mode : "away";
        request["code"] = action.code;
        request["skip_delay"] = false;
        request["force"] = false;
    } else if (action.type == AlarmActionType::Disarm) {
        path = "/api/services/alarmo/disarm";
        request["code"] = action.code;
    } else {
        path = "/api/services/alarmo/skip_delay";
    }

    String body;
    serializeJson(request, body);
    String payload;
    const int code = http_post(path, body, payload);
    if (code < 200 || code >= 300) {
        snprintf(g_status, sizeof(g_status), "Alarmo command failed HTTP %d", code);
        ESP_LOGE("FamilyCalendar", "Alarmo command %s failed HTTP %d payload=%.160s",
                 path.c_str(), code, payload.c_str());
        return false;
    }

    ESP_LOGI("FamilyCalendar", "Alarmo command sent: %s %s", path.c_str(), action.mode);
    g_force_alarm_sync = true;
    return true;
}

bool pop_alarm_action(PendingAlarmAction &out) {
    bool available = false;
    portENTER_CRITICAL(&g_alarm_action_mux);
    if (g_alarm_action_count > 0) {
        out = g_alarm_actions[g_alarm_action_head];
        memset(&g_alarm_actions[g_alarm_action_head], 0, sizeof(PendingAlarmAction));
        g_alarm_action_head = (g_alarm_action_head + 1) % HA_ALARM_ACTION_QUEUE_SIZE;
        --g_alarm_action_count;
        available = true;
    }
    portEXIT_CRITICAL(&g_alarm_action_mux);
    return available;
}

bool queue_alarm_action(AlarmActionType type, const char *mode, const char *code) {
    if (!alarm_service_configured()) return false;

    PendingAlarmAction action = {};
    action.type = type;
    snprintf(action.entity_id, sizeof(action.entity_id), "%s", alarm_service_entity());
    snprintf(action.mode, sizeof(action.mode), "%s", mode ? mode : "");
    snprintf(action.code, sizeof(action.code), "%s", code ? code : "");

    bool queued = false;
    portENTER_CRITICAL(&g_alarm_action_mux);
    if (g_alarm_action_count < HA_ALARM_ACTION_QUEUE_SIZE) {
        g_alarm_actions[g_alarm_action_tail] = action;
        g_alarm_action_tail = (g_alarm_action_tail + 1) % HA_ALARM_ACTION_QUEUE_SIZE;
        ++g_alarm_action_count;
        queued = true;
    }
    portEXIT_CRITICAL(&g_alarm_action_mux);

    /* Erase the temporary copy of the PIN as soon as it is queued. */
    memset(action.code, 0, sizeof(action.code));

    if (queued) {
        g_force_alarm_sync = true;
        if (g_worker_task) xTaskNotifyGive(g_worker_task);
    }
    return queued;
}

void wake_shared_worker() {
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

void ha_worker(void *) {
    ESP_LOGI("FamilyCalendar",
             "HA background task started at priority %d with %u-byte stack",
             HA_SYNC_TASK_PRIORITY, static_cast<unsigned>(HA_SYNC_TASK_STACK_BYTES));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!network_service_connected()) continue;

        if (g_alarm_discovery_requested && !g_alarm_discovery_in_progress) {
            g_alarm_discovery_in_progress = true;
            discover_alarm_panels();
            g_alarm_discovery_requested = false;
            g_alarm_discovery_in_progress = false;
        }

        PendingAlarmAction alarm_action;
        bool processed_alarm_action = false;
        while (pop_alarm_action(alarm_action)) {
            process_alarm_action(alarm_action);
            memset(alarm_action.code, 0, sizeof(alarm_action.code));
            processed_alarm_action = true;
        }
        if (processed_alarm_action) g_force_alarm_sync = true;

        if (g_force_alarm_sync && alarm_service_configured() && !g_alarm_pending_ready) {
            g_alarm_sync_in_progress = true;
            g_last_alarm_attempt_ms = millis();
            ESP_LOGI("FamilyCalendar", "HA request start: Alarmo state");
            fetch_alarm_state();
            g_force_alarm_sync = false;
            g_alarm_sync_in_progress = false;
        }

        if (g_todo_discovery_requested && !g_todo_discovery_in_progress) {
            g_todo_discovery_in_progress = true;
            discover_todo_lists();
            g_todo_discovery_requested = false;
            g_todo_discovery_in_progress = false;
        }

        PendingChoreAction action;
        bool processed_chore_action = false;
        while (pop_chore_action(action)) {
            update_chore_status(action);
            processed_chore_action = true;
        }
        if (processed_chore_action) g_force_chore_sync = true;

        if (g_force_chore_sync && chore_service_configured() && !g_chore_pending_ready) {
            g_chore_sync_in_progress = true;
            g_last_chore_attempt_ms = millis();
            ESP_LOGI("FamilyCalendar", "HA request start: chores");
            fetch_chores();
            g_force_chore_sync = false;
            g_chore_sync_in_progress = false;
            vTaskDelay(1);
        }

        if (g_force_calendar_sync && !g_calendar_pending_ready && configured_calendar_count() > 0) {
            g_calendar_sync_in_progress = true;
            const int center = g_requested_center_offset;
            g_last_calendar_attempt_ms = millis();
            ESP_LOGI("FamilyCalendar", "[Calendar] Consolidated calendar.get_events start (center offset %d)", center);
            sync_calendar_window(center);
            if (g_requested_center_offset != center) {
                g_force_calendar_sync = true;
                ESP_LOGI("FamilyCalendar",
                         "[Calendar] Consolidated fetch superseded by newer offset %d",
                         g_requested_center_offset);
            } else {
                g_force_calendar_sync = false;
            }
            ESP_LOGI("FamilyCalendar", "[Calendar] Consolidated calendar.get_events complete");
            if (!g_calendar_pending_ready) g_calendar_sync_in_progress = false;
            vTaskDelay(1);
        }

        if (weather_service_worker_has_pending()) {
            weather_service_worker_fetch();
            vTaskDelay(1);
        }
    }
}

} // namespace

bool home_assistant_configured() {
    return usable_value(HA_BASE_URL, nullptr) &&
           usable_value(HA_ACCESS_TOKEN, "YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN");
}

void home_assistant_begin() {
    if (!home_assistant_configured()) {
        snprintf(g_status, sizeof(g_status), "Home Assistant not configured");
        return;
    }

    g_active_week_start = week_start_for_offset(0);
    g_force_calendar_sync = false;
    g_force_chore_sync = false;
    g_force_alarm_sync = false;
    g_boot_calendar_pending = true;
    g_connected_since_ms = 0;
    /* Configured entity ids are authoritative; avoid an extra /api/calendars discovery request. */
    g_discovery_done = configured_calendar_count() > 0;

    if (!g_sync_week_events) {
        const size_t count = static_cast<size_t>(PREFETCH_SLOT_COUNT) * HA_MAX_EVENTS;
        const size_t bytes = count * sizeof(CalendarEvent);
        g_sync_week_events = static_cast<CalendarEvent *>(
            heap_caps_calloc(count, sizeof(CalendarEvent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!g_sync_week_events) {
            ESP_LOGE("FamilyCalendar", "Could not allocate %u-byte 3-week HA staging buffer in PSRAM",
                     static_cast<unsigned>(bytes));
            snprintf(g_status, sizeof(g_status), "HA calendar staging allocation failed");
            return;
        }
        ESP_LOGI("FamilyCalendar", "Allocated %u-byte 3-week HA staging buffer in PSRAM",
                 static_cast<unsigned>(bytes));
    }

    if (!g_worker_task) {
        const BaseType_t created = xTaskCreate(
            ha_worker,
            "ha_worker",
            HA_SYNC_TASK_STACK_BYTES,
            nullptr,
            HA_SYNC_TASK_PRIORITY,
            &g_worker_task);
        if (created != pdPASS) {
            g_worker_task = nullptr;
            snprintf(g_status, sizeof(g_status), "HA background task could not start");
            ESP_LOGE("FamilyCalendar", "Could not create Home Assistant background task");
            return;
        }
    }
    weather_service_set_worker_wake_callback(wake_shared_worker);
    snprintf(g_status, sizeof(g_status), "Waiting for Wi-Fi");
}

void home_assistant_loop(int week_offset) {
    if (!home_assistant_configured() || !g_worker_task) return;

    g_current_week_offset = week_offset;
    const time_t desired_week = week_start_for_offset(week_offset);

    /*
     * Switching weeks is now a RAM operation. If this week was seen before,
     * activate it immediately from the PSRAM LRU cache. No network wait.
     */
    if (desired_week != g_active_week_start) {
        /*
         * Always publish the newest requested center immediately.  The worker
         * uses this value to abandon a stale three-week fetch after its current
         * HTTP operation returns.
         */
        g_requested_center_offset = week_offset;
        g_last_week_change_ms = millis();

        if (calendar_cache_activate(desired_week)) {
            ESP_LOGI("FamilyCalendar", "Calendar week served immediately from RAM cache (offset %d)", week_offset);
        } else {
            calendar_model_clear();
            snprintf(g_status, sizeof(g_status),
                     "Week not cached | waiting for refresh");
            ESP_LOGI("FamilyCalendar",
                     "Calendar week cache miss (offset %d)",
                     week_offset);
        }
        g_active_week_start = desired_week;
        g_change_flags |= HA_CHANGE_CALENDAR;
    }

    if (!network_service_connected()) {
        g_connected_since_ms = 0;
        if (!g_calendar_sync_in_progress && !g_chore_sync_in_progress && !g_alarm_sync_in_progress) {
            snprintf(g_status, sizeof(g_status), "%s", network_service_status());
        }
        return;
    }

    const uint32_t now = millis();
    if (g_connected_since_ms == 0) {
        g_connected_since_ms = now;
        ESP_LOGI("FamilyCalendar", "Wi-Fi connected; holding HA boot sync for %lums stability window",
                 static_cast<unsigned long>(HA_BOOT_NETWORK_STABLE_MS));
    }

    if (g_boot_calendar_pending &&
        now - g_connected_since_ms >= HA_BOOT_NETWORK_STABLE_MS &&
        !g_calendar_sync_in_progress && !g_calendar_pending_ready) {
        g_boot_calendar_pending = false;
        g_force_calendar_sync = true;
        ESP_LOGI("FamilyCalendar", "Wi-Fi stability window satisfied; queuing one boot calendar load");
    }

    /*
     * 1.5.9 network-safe core policy:
     *   - one boot calendar load, delayed until Wi-Fi has been stable
     *   - calendar navigation is RAM-only
     *   - no hidden periodic polling inside the HA service
     *   - the UI may request a refresh only for the dashboard that is visible
     *
     * Keeping scheduling out of this worker makes every recurring request
     * intentional and tied to the active screen, while preserving the single
     * serialized Home Assistant worker used for ESP-Hosted stability.
     */
    const bool calendar_retry_ok = g_last_calendar_attempt_ms == 0 ||
                                   now - g_last_calendar_attempt_ms >= HA_RETRY_INTERVAL_MS;

    if (g_force_calendar_sync && !g_calendar_pending_ready &&
        !g_calendar_sync_in_progress && calendar_retry_ok) {
        g_requested_center_offset = week_offset;
        g_calendar_sync_in_progress = true;
        snprintf(g_status, sizeof(g_status),
                 "HA calendar refresh: selected week +/-1...");
        xTaskNotifyGive(g_worker_task);
    }

    if (g_todo_discovery_requested || g_force_chore_sync ||
        g_alarm_discovery_requested || g_force_alarm_sync ||
        weather_service_worker_has_pending()) {
        xTaskNotifyGive(g_worker_task);
    }
}

void home_assistant_request_sync() {
    home_assistant_request_calendar_window(g_current_week_offset);
}

void home_assistant_request_calendar_window(int week_offset_center) {
    if (!home_assistant_configured()) return;
    g_boot_calendar_pending = false;
    g_requested_center_offset = week_offset_center;
    g_force_calendar_sync = true;
    ESP_LOGI("FamilyCalendar", "[Calendar] Queued selected +/-1 refresh at offset %d", week_offset_center);
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

bool home_assistant_calendar_window_needs_refresh(int week_offset_center, uint32_t max_age_ms) {
    if (max_age_ms == 0 || calendar_cache_capacity_weeks() == 0) return true;

    const uint32_t now = millis();
    const time_t center = week_start_for_offset(week_offset_center);
    for (int delta = -HA_PREFETCH_RADIUS_WEEKS; delta <= HA_PREFETCH_RADIUS_WEEKS; ++delta) {
        const time_t week_start = time_service_add_days(center, delta * 7);
        if (!calendar_cache_has(week_start)) return true;
        if (!calendar_cache_is_fresh(week_start, now, max_age_ms)) return true;
    }
    return false;
}

uint32_t home_assistant_take_changes() {
    if (g_calendar_pending_ready) {
        __sync_synchronize();
        const uint32_t refreshed_ms = g_sync_completed_ms ? g_sync_completed_ms : millis();
        for (int slot = 0; slot < PREFETCH_SLOT_COUNT; ++slot) {
            calendar_cache_store(g_sync_week_starts[slot],
                                 sync_slot_events(slot),
                                 g_sync_week_counts[slot],
                                 refreshed_ms);
        }
        g_last_calendar_sync_ms = millis();
        g_calendar_pending_ready = false;
        __sync_synchronize();
        g_calendar_sync_in_progress = false;

        const time_t desired = week_start_for_offset(g_current_week_offset);
        if (calendar_cache_activate(desired)) {
            g_active_week_start = desired;
            g_change_flags |= HA_CHANGE_CALENDAR;
        }
    }

    if (g_chore_pending_ready) {
        __sync_synchronize();
        if (strcmp(g_pending_chore_entity, chore_service_entity()) == 0) {
            chore_service_replace(g_sync_chores, g_sync_chore_count);
            g_change_flags |= HA_CHANGE_CHORES;
        }
        g_chore_pending_ready = false;
    }

    if (g_todo_lists_publish_pending) {
        g_todo_lists_publish_pending = false;
        g_change_flags |= HA_CHANGE_TODO_LISTS;
    }

    if (g_alarm_pending_ready) {
        __sync_synchronize();
        if (strcmp(g_sync_alarm.entity_id, alarm_service_entity()) == 0) {
            alarm_service_replace(g_sync_alarm);
            g_change_flags |= HA_CHANGE_ALARM;
        }
        g_alarm_pending_ready = false;
    }

    if (g_alarm_panels_publish_pending) {
        g_alarm_panels_publish_pending = false;
        g_change_flags |= HA_CHANGE_ALARM_PANELS;
    }

    if (weather_service_take_publish_pending()) {
        g_change_flags |= HA_CHANGE_WEATHER;
    }

    const uint32_t changes = g_change_flags;
    g_change_flags = HA_CHANGE_NONE;
    return changes;
}

bool home_assistant_authenticated() {
    return g_authenticated;
}

const char *home_assistant_status() {
    return g_status;
}

bool home_assistant_ready_for_auto_refresh() {
    if (!home_assistant_configured() || !g_worker_task || !network_service_connected()) return false;
    if (g_connected_since_ms == 0) return false;
    const uint32_t now = millis();
    if (now - g_connected_since_ms < HA_BOOT_NETWORK_STABLE_MS) return false;
    if (g_boot_calendar_pending || g_force_calendar_sync || g_force_chore_sync || g_force_alarm_sync) return false;
    if (g_todo_discovery_requested || g_todo_discovery_in_progress ||
        g_alarm_discovery_requested || g_alarm_discovery_in_progress) return false;
    if (weather_service_worker_busy()) return false;
    if (g_chore_action_count > 0 || g_alarm_action_count > 0) return false;
    return !g_calendar_sync_in_progress && !g_chore_sync_in_progress && !g_alarm_sync_in_progress &&
           !g_calendar_pending_ready && !g_chore_pending_ready && !g_alarm_pending_ready;
}

uint32_t home_assistant_last_calendar_request_ms() {
    return g_last_calendar_attempt_ms;
}

uint32_t home_assistant_last_chore_request_ms() {
    return g_last_chore_attempt_ms;
}

uint32_t home_assistant_last_alarm_request_ms() {
    return g_last_alarm_attempt_ms;
}

void home_assistant_request_todo_discovery() {
    if (g_todo_lists_ready || g_todo_discovery_in_progress) return;
    g_todo_discovery_requested = true;
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

bool home_assistant_todo_lists_ready() {
    return g_todo_lists_ready;
}

size_t home_assistant_todo_list_count() {
    return g_todo_list_count;
}

const char *home_assistant_todo_list_entity(size_t index) {
    return index < g_todo_list_count ? g_todo_lists[index].entity_id : "";
}

const char *home_assistant_todo_list_name(size_t index) {
    return index < g_todo_list_count ? g_todo_lists[index].name : "";
}

void home_assistant_request_chore_sync() {
    if (!chore_service_configured()) return;
    g_force_chore_sync = true;
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

bool home_assistant_queue_chore_status(const char *uid, bool completed) {
    if (!uid || !uid[0] || !chore_service_configured()) return false;

    PendingChoreAction action = {};
    snprintf(action.entity_id, sizeof(action.entity_id), "%s", chore_service_entity());
    snprintf(action.uid, sizeof(action.uid), "%s", uid);
    action.completed = completed;

    bool queued = false;
    portENTER_CRITICAL(&g_chore_action_mux);
    if (g_chore_action_count < HA_CHORE_ACTION_QUEUE_SIZE) {
        g_chore_actions[g_chore_action_tail] = action;
        g_chore_action_tail = (g_chore_action_tail + 1) % HA_CHORE_ACTION_QUEUE_SIZE;
        ++g_chore_action_count;
        queued = true;
    }
    portEXIT_CRITICAL(&g_chore_action_mux);

    if (queued) {
        g_force_chore_sync = true;
        if (g_worker_task) xTaskNotifyGive(g_worker_task);
    } else {
        ESP_LOGW("FamilyCalendar", "Chore action queue full; update not queued");
    }
    return queued;
}

void home_assistant_request_alarm_discovery() {
    if (g_alarm_panels_ready || g_alarm_discovery_in_progress) return;
    g_alarm_discovery_requested = true;
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

bool home_assistant_alarm_panels_ready() {
    return g_alarm_panels_ready;
}

size_t home_assistant_alarm_panel_count() {
    return g_alarm_panel_count;
}

const char *home_assistant_alarm_panel_entity(size_t index) {
    return index < g_alarm_panel_count ? g_alarm_panels[index].entity_id : "";
}

const char *home_assistant_alarm_panel_name(size_t index) {
    return index < g_alarm_panel_count ? g_alarm_panels[index].name : "";
}

void home_assistant_request_alarm_sync() {
    if (!alarm_service_configured()) return;
    g_force_alarm_sync = true;
    if (g_worker_task) xTaskNotifyGive(g_worker_task);
}

bool home_assistant_queue_alarm_arm(const char *mode, const char *code) {
    if (!mode || !mode[0]) return false;
    return queue_alarm_action(AlarmActionType::Arm, mode, code);
}

bool home_assistant_queue_alarm_disarm(const char *code) {
    return queue_alarm_action(AlarmActionType::Disarm, "", code);
}

bool home_assistant_queue_alarm_skip_delay() {
    return queue_alarm_action(AlarmActionType::SkipDelay, "", "");
}

