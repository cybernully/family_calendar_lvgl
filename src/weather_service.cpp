#include "weather_service.h"

#include "app_config.h"
#include "app_secrets.h"
#include "network_service.h"
#include "time_service.h"
#include "web_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

namespace {

struct WeatherUnits {
    char temperature[8];
    char wind[16];
    char precip[8];
    char pressure[12];
};

WeatherWorkerWakeCallback g_wake_callback = nullptr;
portMUX_TYPE g_weather_mux = portMUX_INITIALIZER_UNLOCKED;

WeatherCurrent g_current = {};
WeatherHourForecast g_hourly[WEATHER_MAX_HOURLY_POINTS] = {};
WeatherDayForecast g_daily[WEATHER_MAX_DAILY_POINTS] = {};
size_t g_hourly_count = 0;
size_t g_daily_count = 0;

bool g_has_data = false;
bool g_request_pending = false;
bool g_force_request_pending = false;
bool g_in_progress = false;
bool g_publish_pending = false;
bool g_source_test_pending = false;
bool g_source_test_in_progress = false;
WeatherSourceDiagnostics g_source_diagnostics = {};

uint32_t g_last_attempt_ms = 0;
uint32_t g_last_success_ms = 0;
uint32_t g_next_refresh_ms = 0;
time_t g_last_updated_epoch = 0;
time_t g_expires_epoch = 0;
char g_last_modified[64] = {};
char g_status[128] = "Weather idle";

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

void set_statusf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    portENTER_CRITICAL(&g_weather_mux);
    snprintf(g_status, sizeof(g_status), "%s", buffer);
    portEXIT_CRITICAL(&g_weather_mux);
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool parse_iso8601(const char *text, time_t &epoch) {
    epoch = 0;
    if (!text || strlen(text) < 10) return false;

    int year = 0;
    int month = 0;
    int day = 0;
    if (sscanf(text, "%d-%d-%d", &year, &month, &day) != 3) return false;

    int hour = 0;
    int minute = 0;
    int second = 0;
    if (strlen(text) >= 19) {
        char separator = 0;
        if (sscanf(text, "%d-%d-%d%c%d:%d:%d", &year, &month, &day,
                   &separator, &hour, &minute, &second) != 7 ||
            (separator != 'T' && separator != ' ')) {
            return false;
        }
    }

    const char *zone = text + ((strlen(text) >= 19) ? 19 : 10);
    while (*zone && *zone != 'Z' && *zone != 'z' && *zone != '+' && *zone != '-') ++zone;

    if (*zone == 'Z' || *zone == 'z' || *zone == '+' || *zone == '-') {
        int offset_seconds = 0;
        if (*zone == '+' || *zone == '-') {
            const int sign = (*zone == '+') ? 1 : -1;
            int zh = 0;
            int zm = 0;
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

WeatherCondition map_ha_condition(const char *condition) {
    if (!condition || !condition[0]) return WeatherCondition::Unknown;
    if (strcmp(condition, "sunny") == 0 || strcmp(condition, "clear-night") == 0) {
        return WeatherCondition::Clear;
    }
    if (strcmp(condition, "partlycloudy") == 0 || strcmp(condition, "windy-variant") == 0) {
        return WeatherCondition::PartlyCloudy;
    }
    if (strcmp(condition, "cloudy") == 0 || strcmp(condition, "windy") == 0) {
        return WeatherCondition::Cloudy;
    }
    if (strcmp(condition, "fog") == 0) return WeatherCondition::Fog;
    if (strcmp(condition, "lightning") == 0 || strcmp(condition, "lightning-rainy") == 0) {
        return WeatherCondition::Thunderstorm;
    }
    if (strcmp(condition, "snowy") == 0 || strcmp(condition, "snowy-rainy") == 0 || strcmp(condition, "hail") == 0) {
        return WeatherCondition::Snow;
    }
    if (strcmp(condition, "rainy") == 0 || strcmp(condition, "pouring") == 0) {
        return WeatherCondition::Rain;
    }
    if (strcmp(condition, "exceptional") == 0) return WeatherCondition::Showers;
    return WeatherCondition::Unknown;
}

bool same_local_day(time_t left, time_t right) {
    struct tm left_tm = {};
    struct tm right_tm = {};
    localtime_r(&left, &left_tm);
    localtime_r(&right, &right_tm);
    return left_tm.tm_year == right_tm.tm_year && left_tm.tm_yday == right_tm.tm_yday;
}

float temperature_to_c(float value, const char *unit) {
    if (!unit || !unit[0]) return value;

    // Home Assistant normally reports Fahrenheit as "°F", so checking only
    // unit[0] misses it because the first bytes are the UTF-8 degree symbol.
    // Accept both plain "F" and strings such as "°F" / "fahrenheit".
    if (strchr(unit, 'F') || strchr(unit, 'f')) {
        return (value - 32.0f) * 5.0f / 9.0f;
    }

    return value;
}

float wind_to_mps(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "mph") == 0) return value * 0.44704f;
    if (strcasecmp(unit, "km/h") == 0 || strcasecmp(unit, "kph") == 0) return value / 3.6f;
    if (strcasecmp(unit, "kn") == 0 || strcasecmp(unit, "knot") == 0 || strcasecmp(unit, "knots") == 0) {
        return value * 0.514444f;
    }
    return value;
}

float precip_to_mm(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "in") == 0 || strcasecmp(unit, "inch") == 0 || strcasecmp(unit, "inches") == 0) {
        return value * 25.4f;
    }
    if (strcasecmp(unit, "cm") == 0) return value * 10.0f;
    return value;
}

float pressure_to_hpa(float value, const char *unit) {
    if (!unit || !unit[0]) return value;
    if (strcasecmp(unit, "hPa") == 0 || strcasecmp(unit, "mbar") == 0) return value;
    if (strcasecmp(unit, "Pa") == 0) return value / 100.0f;
    if (strcasecmp(unit, "inHg") == 0) return value * 33.8639f;
    if (strcasecmp(unit, "psi") == 0) return value * 68.9476f;
    return value;
}

float wind_bearing_to_degrees(JsonVariantConst value) {
    if (value.isNull()) return NAN;
    if (value.is<float>() || value.is<double>() || value.is<int>() || value.is<long>()) {
        return value.as<float>();
    }

    const char *text = value.as<const char *>();
    if (!text || !text[0]) return NAN;
    char *end = nullptr;
    const float numeric = strtof(text, &end);
    if (end && end != text && *end == '\0') return numeric;

    struct Cardinal { const char *name; float degrees; };
    static const Cardinal directions[] = {
        {"N", 0.0f}, {"NNE", 22.5f}, {"NE", 45.0f}, {"ENE", 67.5f},
        {"E", 90.0f}, {"ESE", 112.5f}, {"SE", 135.0f}, {"SSE", 157.5f},
        {"S", 180.0f}, {"SSW", 202.5f}, {"SW", 225.0f}, {"WSW", 247.5f},
        {"W", 270.0f}, {"WNW", 292.5f}, {"NW", 315.0f}, {"NNW", 337.5f},
    };
    for (const Cardinal &direction : directions) {
        if (strcasecmp(text, direction.name) == 0) return direction.degrees;
    }
    return NAN;
}

int http_request(const char *method, const String &path, const String *body, String &payload) {
    payload = "";
    const String url = clean_base_url() + path;

    HTTPClient http;
    http.setConnectTimeout(WEATHER_HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(WEATHER_HTTP_TIMEOUT_MS);
    http.setUserAgent(String("FamilyCalendar/") + APP_VERSION);

    const String auth = String("Bearer ") + HA_ACCESS_TOKEN;
    int code = -1;

    auto execute = [&](auto &client) -> int {
        if (!http.begin(client, url)) return -21;
        http.addHeader("Authorization", auth);
        http.addHeader("Accept", "application/json");
        if (body) http.addHeader("Content-Type", "application/json");

        int result = -1;
        if (strcmp(method, "POST") == 0 && body) {
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
#else
        return -20;
#endif
#if defined(HA_TLS_CA_CERT_PEM) || HA_TLS_ALLOW_INSECURE
        code = execute(client);
#endif
    } else {
        WiFiClient client;
        code = execute(client);
    }

    return code;
}

int http_post(const String &path, const String &body, String &payload) {
    return http_request("POST", path, &body, payload);
}

JsonObjectConst weather_snapshot_object(const JsonDocument &doc) {
    JsonObjectConst service_response = doc["service_response"].as<JsonObjectConst>();
    if (!service_response.isNull()) return service_response;
    return doc.as<JsonObjectConst>();
}

bool json_bool(JsonVariantConst value, bool fallback = false) {
    if (value.isNull()) return fallback;
    if (value.is<bool>()) return value.as<bool>();
    if (value.is<int>() || value.is<long>()) return value.as<long>() != 0;
    const char *text = value.as<const char *>();
    if (!text) return fallback;
    if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0 || strcasecmp(text, "yes") == 0) return true;
    if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0 || strcasecmp(text, "no") == 0) return false;
    return fallback;
}

bool parse_weather_snapshot(const String &payload,
                            WeatherCurrent &current,
                            WeatherUnits &units,
                            WeatherHourForecast *hourly,
                            size_t &hourly_count,
                            WeatherDayForecast *daily,
                            size_t &daily_count,
                            time_t &updated_epoch,
                            WeatherSourceDiagnostics *source_diag) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonObject>()) {
        ESP_LOGW("FamilyCalendar", "[Weather] Snapshot JSON parse failed: %s",
                 error ? error.c_str() : "not object");
        return false;
    }

    const JsonObjectConst snapshot = weather_snapshot_object(doc);
    if (snapshot.isNull()) {
        ESP_LOGW("FamilyCalendar", "[Weather] Snapshot response missing service_response");
        return false;
    }

    const JsonObjectConst current_json = snapshot["current"].as<JsonObjectConst>();
    const JsonObjectConst units_json = snapshot["units"].as<JsonObjectConst>();
    const JsonArrayConst daily_json = snapshot["daily"].as<JsonArrayConst>();
    const JsonArrayConst hourly_json = snapshot["hourly"].as<JsonArrayConst>();

    if (current_json.isNull() || units_json.isNull() || daily_json.isNull() || hourly_json.isNull()) {
        ESP_LOGW("FamilyCalendar", "[Weather] Snapshot missing current/units/daily/hourly data");
        return false;
    }

    if (source_diag) {
        memset(source_diag, 0, sizeof(*source_diag));
        const JsonObjectConst sources = snapshot["sources"].as<JsonObjectConst>();
        if (!sources.isNull()) {
            source_diag->valid = true;
            source_diag->current_ok = json_bool(sources["current_ok"]);
            source_diag->daily_ok = json_bool(sources["daily_ok"]);
            source_diag->hourly_ok = json_bool(sources["hourly_ok"]);
            snprintf(source_diag->current_daily_entity, sizeof(source_diag->current_daily_entity), "%s",
                     sources["current_daily_entity"] | "");
            snprintf(source_diag->hourly_entity, sizeof(source_diag->hourly_entity), "%s",
                     sources["hourly_entity"] | "");
            snprintf(source_diag->message, sizeof(source_diag->message), "%s",
                     sources["message"] | "");
        }
    }

    memset(&current, 0, sizeof(current));
    memset(hourly, 0, WEATHER_MAX_HOURLY_POINTS * sizeof(WeatherHourForecast));
    memset(daily, 0, WEATHER_MAX_DAILY_POINTS * sizeof(WeatherDayForecast));
    hourly_count = 0;
    daily_count = 0;

    current.temperature_c = NAN;
    current.today_high_c = NAN;
    current.today_low_c = NAN;
    current.humidity_pct = -1;
    current.pressure_hpa = NAN;
    current.wind_mps = NAN;
    current.wind_direction_deg = NAN;
    current.precipitation_mm = 0.0f;
    current.precipitation_probability_pct = -1;

    snprintf(units.temperature, sizeof(units.temperature), "%s", units_json["temperature"] | "C");
    snprintf(units.wind, sizeof(units.wind), "%s", units_json["wind"] | "m/s");
    snprintf(units.precip, sizeof(units.precip), "%s", units_json["precipitation"] | "mm");
    snprintf(units.pressure, sizeof(units.pressure), "%s", units_json["pressure"] | "hPa");

    const char *condition_text = current_json["condition"] | "";
    current.condition = map_ha_condition(condition_text);
    snprintf(current.symbol_code, sizeof(current.symbol_code), "%s", condition_text);

    if (!current_json["temperature"].isNull()) {
        current.temperature_c = temperature_to_c(current_json["temperature"].as<float>(), units.temperature);
    }
    if (!current_json["today_high"].isNull()) {
        current.today_high_c = temperature_to_c(current_json["today_high"].as<float>(), units.temperature);
    }
    if (!current_json["today_low"].isNull()) {
        current.today_low_c = temperature_to_c(current_json["today_low"].as<float>(), units.temperature);
    }
    if (!current_json["humidity"].isNull()) {
        current.humidity_pct = current_json["humidity"].as<int>();
    }
    if (!current_json["pressure"].isNull()) {
        current.pressure_hpa = pressure_to_hpa(current_json["pressure"].as<float>(), units.pressure);
    }
    if (!current_json["wind_speed"].isNull()) {
        current.wind_mps = wind_to_mps(current_json["wind_speed"].as<float>(), units.wind);
    }
    if (!current_json["wind_bearing"].isNull()) {
        current.wind_direction_deg = wind_bearing_to_degrees(current_json["wind_bearing"]);
    }
    if (!current_json["precipitation"].isNull()) {
        current.precipitation_mm = precip_to_mm(current_json["precipitation"].as<float>(), units.precip);
    }
    if (!current_json["precipitation_probability"].isNull()) {
        current.precipitation_probability_pct = current_json["precipitation_probability"].as<int>();
    }

    const char *updated = current_json["last_updated"] | "";
    if (!parse_iso8601(updated, updated_epoch)) updated_epoch = time_service_now();
    current.epoch = updated_epoch;

    const time_t now_epoch = time_service_now();

    for (JsonObjectConst point : daily_json) {
        if (daily_count >= WEATHER_MAX_DAILY_POINTS) break;

        const char *datetime = point["datetime"] | point["time"] | "";
        time_t epoch = 0;
        if (!parse_iso8601(datetime, epoch)) continue;
        if (epoch + 86400 < now_epoch) continue;

        WeatherDayForecast &out = daily[daily_count++];
        out.valid = true;
        out.local_day_epoch = epoch;

        const char *condition = point["condition"] | "";
        out.condition = map_ha_condition(condition);
        snprintf(out.symbol_code, sizeof(out.symbol_code), "%s", condition);

        float high = NAN;
        if (!point["temperature"].isNull()) high = point["temperature"].as<float>();
        if (isnan(high) && !point["temperature_max"].isNull()) high = point["temperature_max"].as<float>();
        if (isnan(high) && !point["temp"].isNull()) high = point["temp"].as<float>();

        float low = NAN;
        if (!point["templow"].isNull()) low = point["templow"].as<float>();
        if (isnan(low) && !point["temperature_min"].isNull()) low = point["temperature_min"].as<float>();

        out.high_c = isnan(high) ? NAN : temperature_to_c(high, units.temperature);
        out.low_c = isnan(low) ? out.high_c : temperature_to_c(low, units.temperature);
        out.precipitation_mm = point["precipitation"].isNull()
                                   ? 0.0f
                                   : precip_to_mm(point["precipitation"].as<float>(), units.precip);
        out.precipitation_probability_pct = point["precipitation_probability"].isNull()
                                                ? -1
                                                : static_cast<int>(lroundf(point["precipitation_probability"].as<float>()));
        out.wind_max_mps = point["wind_speed"].isNull()
                               ? 0.0f
                               : wind_to_mps(point["wind_speed"].as<float>(), units.wind);

        struct tm local_tm = {};
        localtime_r(&out.local_day_epoch, &local_tm);
        local_tm.tm_hour = 0;
        local_tm.tm_min = 0;
        local_tm.tm_sec = 0;
        local_tm.tm_isdst = -1;
        out.local_day_epoch = mktime(&local_tm);
    }

    for (JsonObjectConst point : hourly_json) {
        if (hourly_count >= WEATHER_MAX_HOURLY_POINTS) break;

        const char *datetime = point["datetime"] | point["time"] | "";
        time_t epoch = 0;
        if (!parse_iso8601(datetime, epoch)) continue;
        if (epoch < now_epoch - 300) continue;

        WeatherHourForecast &out = hourly[hourly_count++];
        out.valid = true;
        out.epoch = epoch;

        const char *condition = point["condition"] | "";
        out.condition = map_ha_condition(condition);
        snprintf(out.symbol_code, sizeof(out.symbol_code), "%s", condition);

        const float temperature = point["temperature"].isNull() ? NAN : point["temperature"].as<float>();
        out.temperature_c = isnan(temperature) ? NAN : temperature_to_c(temperature, units.temperature);
        out.precipitation_mm = point["precipitation"].isNull()
                                   ? 0.0f
                                   : precip_to_mm(point["precipitation"].as<float>(), units.precip);
        out.precipitation_probability_pct = point["precipitation_probability"].isNull()
                                                ? -1
                                                : static_cast<int>(lroundf(point["precipitation_probability"].as<float>()));
        out.wind_mps = point["wind_speed"].isNull()
                           ? 0.0f
                           : wind_to_mps(point["wind_speed"].as<float>(), units.wind);
    }

    if ((!isfinite(current.temperature_c) || current.condition == WeatherCondition::Unknown) && hourly_count > 0) {
        if (isfinite(hourly[0].temperature_c)) current.temperature_c = hourly[0].temperature_c;
        if (current.condition == WeatherCondition::Unknown) {
            current.condition = hourly[0].condition;
            snprintf(current.symbol_code, sizeof(current.symbol_code), "%s", hourly[0].symbol_code);
        }
    }

    if ((!isfinite(current.today_high_c) || !isfinite(current.today_low_c)) && daily_count > 0) {
        size_t today_index = 0;
        for (size_t i = 0; i < daily_count; ++i) {
            if (same_local_day(daily[i].local_day_epoch, now_epoch)) {
                today_index = i;
                break;
            }
        }
        if (!isfinite(current.today_high_c)) current.today_high_c = daily[today_index].high_c;
        if (!isfinite(current.today_low_c)) current.today_low_c = daily[today_index].low_c;
    }

    if (!isfinite(current.today_high_c)) current.today_high_c = current.temperature_c;
    if (!isfinite(current.today_low_c)) current.today_low_c = current.temperature_c;

    current.valid = isfinite(current.temperature_c) || current.condition != WeatherCondition::Unknown;
    if (source_diag) {
        source_diag->daily_count = daily_count;
        source_diag->hourly_count = hourly_count;
        if (!source_diag->valid) {
            source_diag->valid = true;
            source_diag->current_ok = current.valid;
            source_diag->daily_ok = daily_count > 0;
            source_diag->hourly_ok = hourly_count > 0;
        }
        if (!source_diag->current_daily_entity[0] || !source_diag->hourly_entity[0]) {
            char current_daily_entity[96] = {};
            char hourly_entity[96] = {};
            web_manager_get_weather_sources(current_daily_entity, sizeof(current_daily_entity),
                                            hourly_entity, sizeof(hourly_entity));
            if (!source_diag->current_daily_entity[0]) {
                snprintf(source_diag->current_daily_entity, sizeof(source_diag->current_daily_entity),
                         "%s", current_daily_entity);
            }
            if (!source_diag->hourly_entity[0]) {
                snprintf(source_diag->hourly_entity, sizeof(source_diag->hourly_entity),
                         "%s", hourly_entity);
            }
        }
        if (!source_diag->message[0]) {
            snprintf(source_diag->message, sizeof(source_diag->message),
                     "current=%s, daily=%u, hourly=%u",
                     source_diag->current_ok ? "ok" : "unavailable",
                     static_cast<unsigned>(daily_count), static_cast<unsigned>(hourly_count));
        }
    }
    return current.valid && (daily_count > 0 || hourly_count > 0);
}

bool fetch_weather_snapshot(WeatherCurrent &current,
                            WeatherUnits &units,
                            WeatherHourForecast *hourly,
                            size_t &hourly_count,
                            WeatherDayForecast *daily,
                            size_t &daily_count,
                            time_t &updated_epoch,
                            WeatherSourceDiagnostics *source_diag) {
    static const char *kWeatherSnapshotPath =
        "/api/services/script/family_calendar_weather_snapshot?return_response";

    char current_daily_entity[96] = {};
    char hourly_entity[96] = {};
    web_manager_get_weather_sources(current_daily_entity, sizeof(current_daily_entity),
                                    hourly_entity, sizeof(hourly_entity));

    JsonDocument request;
    request["current_daily_entity"] = current_daily_entity;
    request["hourly_entity"] = hourly_entity;
    String body;
    serializeJson(request, body);

    ESP_LOGI("FamilyCalendar",
             "[Weather] Hybrid snapshot sources: current/daily=%s hourly=%s",
             current_daily_entity, hourly_entity);

    String payload;
    const int code = http_post(kWeatherSnapshotPath, body, payload);
    if (code != 200) {
        ESP_LOGW("FamilyCalendar", "[Weather] Snapshot HTTP %d", code);
        if (code == 400 || code == 422) {
            ESP_LOGW("FamilyCalendar",
                     "[Weather] Verify the v2.2.2 family_calendar_weather_snapshot script is installed with current_daily_entity and hourly_entity fields");
        }
        return false;
    }

    ESP_LOGD("FamilyCalendar", "[Weather] Snapshot payload %u bytes",
             static_cast<unsigned>(payload.length()));
    return parse_weather_snapshot(payload,
                                  current,
                                  units,
                                  hourly,
                                  hourly_count,
                                  daily,
                                  daily_count,
                                  updated_epoch,
                                  source_diag);
}

void log_next_refresh(uint32_t now_ms, uint32_t next_refresh_ms) {
    uint32_t remaining_ms = 0;
    if (static_cast<int32_t>(next_refresh_ms - now_ms) > 0) {
        remaining_ms = next_refresh_ms - now_ms;
    }
    const unsigned long minutes = remaining_ms / 60000UL;
    const unsigned long seconds = (remaining_ms % 60000UL) / 1000UL;
    ESP_LOGI("FamilyCalendar", "[Weather] Next refresh in %lum %lus", minutes, seconds);
}

} // namespace

void weather_service_begin() {
    if (!weather_service_configured()) {
        set_statusf("Weather not configured");
        return;
    }
    set_statusf("Weather ready (HA hybrid snapshot)");
}

void weather_service_set_worker_wake_callback(WeatherWorkerWakeCallback callback) {
    g_wake_callback = callback;
}

bool weather_service_configured() {
    char current_daily_entity[96] = {};
    char hourly_entity[96] = {};
    web_manager_get_weather_sources(current_daily_entity, sizeof(current_daily_entity),
                                    hourly_entity, sizeof(hourly_entity));
    return usable_value(HA_BASE_URL, nullptr) &&
           usable_value(HA_ACCESS_TOKEN, "YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN") &&
           usable_value(current_daily_entity, "") &&
           usable_value(hourly_entity, "");
}

bool weather_service_has_data() {
    bool has_data = false;
    portENTER_CRITICAL(&g_weather_mux);
    has_data = g_has_data;
    portEXIT_CRITICAL(&g_weather_mux);
    return has_data;
}

const char *weather_service_status() {
    return g_status;
}

uint32_t weather_service_last_attempt_ms() {
    uint32_t last_attempt = 0;
    portENTER_CRITICAL(&g_weather_mux);
    last_attempt = g_last_attempt_ms;
    portEXIT_CRITICAL(&g_weather_mux);
    return last_attempt;
}

uint32_t weather_service_last_success_ms() {
    uint32_t value = 0;
    portENTER_CRITICAL(&g_weather_mux);
    value = g_last_success_ms;
    portEXIT_CRITICAL(&g_weather_mux);
    return value;
}

bool weather_service_should_refresh(uint32_t now_ms) {
    if (!weather_service_configured()) return false;

    bool has_data = false;
    bool in_progress = false;
    uint32_t next_refresh_ms = 0;

    portENTER_CRITICAL(&g_weather_mux);
    has_data = g_has_data;
    in_progress = g_in_progress;
    next_refresh_ms = g_next_refresh_ms;
    portEXIT_CRITICAL(&g_weather_mux);

    if (in_progress) return false;
    if (!has_data) return true;
    if (next_refresh_ms == 0) return true;
    return static_cast<int32_t>(now_ms - next_refresh_ms) >= 0;
}

bool weather_service_request_refresh(bool force, const char *reason) {
    if (!weather_service_configured()) {
        set_statusf("Weather not configured");
        return false;
    }

    const uint32_t now_ms = millis();
    if (!force && !weather_service_should_refresh(now_ms)) {
        ESP_LOGI("FamilyCalendar", "[Weather] Cache fresh; network request skipped (%s)",
                 reason ? reason : "no reason");
        return false;
    }

    bool queued = false;
    portENTER_CRITICAL(&g_weather_mux);
    if (!g_request_pending) {
        if (!g_in_progress || force) {
            g_request_pending = true;
            g_force_request_pending = force;
            queued = true;
            snprintf(g_status, sizeof(g_status),
                     g_in_progress ? "Weather follow-up refresh queued" : "Weather refresh queued");
        }
    } else if (force && !g_force_request_pending) {
        /* Upgrade an already queued normal refresh to a forced refresh. */
        g_force_request_pending = true;
        queued = true;
    }
    portEXIT_CRITICAL(&g_weather_mux);

    if (!queued) return false;

    ESP_LOGI("FamilyCalendar", "[Weather] Snapshot fetch queued (%s)",
             reason ? reason : "no reason");
    if (g_wake_callback) g_wake_callback();
    return true;
}

bool weather_service_worker_has_pending() {
    bool pending = false;
    portENTER_CRITICAL(&g_weather_mux);
    pending = g_request_pending || g_source_test_pending;
    portEXIT_CRITICAL(&g_weather_mux);
    return pending;
}

bool weather_service_worker_busy() {
    bool busy = false;
    portENTER_CRITICAL(&g_weather_mux);
    busy = g_request_pending || g_in_progress || g_source_test_pending || g_source_test_in_progress;
    portEXIT_CRITICAL(&g_weather_mux);
    return busy;
}

bool weather_service_worker_fetch() {
    if (!weather_service_configured()) {
        portENTER_CRITICAL(&g_weather_mux);
        g_request_pending = false;
        g_force_request_pending = false;
        portEXIT_CRITICAL(&g_weather_mux);
        return false;
    }

    if (!network_service_connected()) return false;

    const uint32_t now_ms = millis();
    bool forced = false;
    bool source_test = false;
    portENTER_CRITICAL(&g_weather_mux);
    source_test = g_source_test_pending;
    if (source_test && !g_in_progress) {
        g_source_test_pending = false;
        g_source_test_in_progress = true;
        g_request_pending = true;
        g_force_request_pending = true;
    }
    forced = g_force_request_pending;
    portEXIT_CRITICAL(&g_weather_mux);

    if (!forced && !weather_service_should_refresh(now_ms)) {
        portENTER_CRITICAL(&g_weather_mux);
        g_request_pending = false;
        g_force_request_pending = false;
        snprintf(g_status, sizeof(g_status), "Weather cache still fresh");
        portEXIT_CRITICAL(&g_weather_mux);
        ESP_LOGI("FamilyCalendar", "[Weather] Cache fresh; network request skipped");
        return false;
    }

    bool should_fetch = false;
    portENTER_CRITICAL(&g_weather_mux);
    if (g_request_pending && !g_in_progress) {
        should_fetch = true;
        forced = g_force_request_pending;
        g_request_pending = false;
        g_force_request_pending = false;
        g_in_progress = true;
        g_last_attempt_ms = now_ms;
        snprintf(g_status, sizeof(g_status), forced ? "Forced weather fetch in progress" : "Weather fetch in progress");
    }
    portEXIT_CRITICAL(&g_weather_mux);
    if (!should_fetch) return false;

    ESP_LOGI("FamilyCalendar", "[Weather] Fetch start (single HA snapshot request)");

    WeatherCurrent current = {};
    WeatherUnits units = {};
    WeatherHourForecast hourly[WEATHER_MAX_HOURLY_POINTS] = {};
    WeatherDayForecast daily[WEATHER_MAX_DAILY_POINTS] = {};
    size_t hourly_count = 0;
    size_t daily_count = 0;
    time_t updated_epoch = 0;
    WeatherSourceDiagnostics source_diag = {};

    const uint32_t started_ms = millis();
    const bool snapshot_ok = fetch_weather_snapshot(current,
                                                    units,
                                                    hourly,
                                                    hourly_count,
                                                    daily,
                                                    daily_count,
                                                    updated_epoch,
                                                    &source_diag);
    const uint32_t elapsed_ms = millis() - started_ms;

    if (!snapshot_ok) {
        ESP_LOGW("FamilyCalendar", "[Weather] HA snapshot fetch failed in %lums",
                 static_cast<unsigned long>(elapsed_ms));
        const uint32_t retry_due_ms = now_ms + WEATHER_RETRY_INTERVAL_MS;
        portENTER_CRITICAL(&g_weather_mux);
        g_next_refresh_ms = retry_due_ms;
        g_in_progress = false;
        if (g_source_test_in_progress) {
            memset(&g_source_diagnostics, 0, sizeof(g_source_diagnostics));
            g_source_diagnostics.valid = true;
            g_source_diagnostics.tested_ms = millis();
            g_source_diagnostics.latency_ms = elapsed_ms;
            web_manager_get_weather_sources(g_source_diagnostics.current_daily_entity,
                                            sizeof(g_source_diagnostics.current_daily_entity),
                                            g_source_diagnostics.hourly_entity,
                                            sizeof(g_source_diagnostics.hourly_entity));
            snprintf(g_source_diagnostics.message, sizeof(g_source_diagnostics.message),
                     "Hybrid weather request failed");
            g_source_test_in_progress = false;
        }
        snprintf(g_status, sizeof(g_status), "Weather snapshot fetch failed");
        portEXIT_CRITICAL(&g_weather_mux);
        log_next_refresh(now_ms, retry_due_ms);
        return true;
    }

    const uint32_t next_refresh_ms = now_ms + WEATHER_ACTIVE_TAB_REFRESH_MS;

    portENTER_CRITICAL(&g_weather_mux);
    g_current = current;
    memcpy(g_hourly, hourly, sizeof(g_hourly));
    memcpy(g_daily, daily, sizeof(g_daily));
    g_hourly_count = hourly_count;
    g_daily_count = daily_count;
    g_last_updated_epoch = updated_epoch;
    g_expires_epoch = 0;
    g_last_modified[0] = '\0';
    g_last_success_ms = now_ms;
    g_next_refresh_ms = next_refresh_ms;
    g_has_data = true;
    g_publish_pending = true;
    g_in_progress = false;
    source_diag.tested_ms = millis();
    source_diag.latency_ms = elapsed_ms;
    source_diag.pending = false;
    source_diag.in_progress = false;
    g_source_diagnostics = source_diag;
    g_source_test_in_progress = false;
    snprintf(g_status, sizeof(g_status), "Weather updated from HA snapshot");
    portEXIT_CRITICAL(&g_weather_mux);

    ESP_LOGI("FamilyCalendar", "[Weather] Snapshot HTTP 200 in %lums",
             static_cast<unsigned long>(elapsed_ms));
    ESP_LOGI("FamilyCalendar", "[Weather] Parsed %u hourly / %u daily entries",
             static_cast<unsigned>(hourly_count), static_cast<unsigned>(daily_count));
    log_next_refresh(now_ms, next_refresh_ms);
    return true;
}

bool weather_service_request_source_test() {
    if (!weather_service_configured()) return false;
    bool queued = false;
    portENTER_CRITICAL(&g_weather_mux);
    if (!g_source_test_pending && !g_source_test_in_progress) {
        g_source_test_pending = true;
        g_source_diagnostics.pending = true;
        g_source_diagnostics.in_progress = false;
        queued = true;
    }
    portEXIT_CRITICAL(&g_weather_mux);
    if (queued && g_wake_callback) g_wake_callback();
    return queued;
}

void weather_service_get_source_diagnostics(WeatherSourceDiagnostics &out) {
    memset(&out, 0, sizeof(out));
    portENTER_CRITICAL(&g_weather_mux);
    out = g_source_diagnostics;
    out.pending = g_source_test_pending;
    out.in_progress = g_source_test_in_progress;
    portEXIT_CRITICAL(&g_weather_mux);
}

bool weather_service_take_publish_pending() {
    bool changed = false;
    portENTER_CRITICAL(&g_weather_mux);
    changed = g_publish_pending;
    g_publish_pending = false;
    portEXIT_CRITICAL(&g_weather_mux);
    return changed;
}

void weather_service_get_snapshot(WeatherSnapshot &out) {
    memset(&out, 0, sizeof(out));
    portENTER_CRITICAL(&g_weather_mux);
    out.configured = weather_service_configured();
    out.has_data = g_has_data;
    out.in_progress = g_in_progress;
    out.request_pending = g_request_pending;
    out.last_attempt_ms = g_last_attempt_ms;
    out.last_success_ms = g_last_success_ms;
    out.next_refresh_ms = g_next_refresh_ms;
    out.last_updated_epoch = g_last_updated_epoch;
    out.expires_epoch = g_expires_epoch;
    snprintf(out.last_modified, sizeof(out.last_modified), "%s", g_last_modified);
    snprintf(out.status, sizeof(out.status), "%s", g_status);
    out.current = g_current;
    memcpy(out.hourly, g_hourly, sizeof(out.hourly));
    memcpy(out.daily, g_daily, sizeof(out.daily));
    out.hourly_count = g_hourly_count;
    out.daily_count = g_daily_count;
    portEXIT_CRITICAL(&g_weather_mux);
}

bool weather_service_get_day_forecast(time_t local_time, WeatherDayForecast &out) {
    memset(&out, 0, sizeof(out));
    WeatherDayForecast daily_copy[WEATHER_MAX_DAILY_POINTS] = {};
    size_t daily_count = 0;

    portENTER_CRITICAL(&g_weather_mux);
    daily_count = g_daily_count;
    if (daily_count > WEATHER_MAX_DAILY_POINTS) daily_count = WEATHER_MAX_DAILY_POINTS;
    memcpy(daily_copy, g_daily, daily_count * sizeof(WeatherDayForecast));
    portEXIT_CRITICAL(&g_weather_mux);

    for (size_t i = 0; i < daily_count; ++i) {
        if (!daily_copy[i].valid) continue;
        if (same_local_day(daily_copy[i].local_day_epoch, local_time)) {
            out = daily_copy[i];
            return true;
        }
    }
    return false;
}

const char *weather_condition_label(WeatherCondition condition) {
    switch (condition) {
        case WeatherCondition::Clear: return "Clear";
        case WeatherCondition::PartlyCloudy: return "Partly cloudy";
        case WeatherCondition::Cloudy: return "Cloudy";
        case WeatherCondition::Rain: return "Rain";
        case WeatherCondition::Showers: return "Showers";
        case WeatherCondition::Thunderstorm: return "Thunderstorms";
        case WeatherCondition::Snow: return "Snow";
        case WeatherCondition::Fog: return "Fog";
        case WeatherCondition::Unknown: return "Unknown";
    }
    return "Unknown";
}

float weather_display_temperature(float temperature_c) {
#if WEATHER_USE_IMPERIAL
    return temperature_c * 9.0f / 5.0f + 32.0f;
#else
    return temperature_c;
#endif
}

float weather_display_wind(float wind_mps) {
#if WEATHER_USE_IMPERIAL
    return wind_mps * 2.236936f;
#else
    return wind_mps;
#endif
}

float weather_display_precip(float precip_mm) {
#if WEATHER_USE_IMPERIAL
    return precip_mm / 25.4f;
#else
    return precip_mm;
#endif
}

float weather_display_pressure(float pressure_hpa) {
#if WEATHER_USE_IMPERIAL
    return pressure_hpa / 33.8639f;
#else
    return pressure_hpa;
#endif
}

const char *weather_temperature_unit() {
#if WEATHER_USE_IMPERIAL
    return "F";
#else
    return "C";
#endif
}

const char *weather_wind_unit() {
#if WEATHER_USE_IMPERIAL
    return "mph";
#else
    return "m/s";
#endif
}

const char *weather_precip_unit() {
#if WEATHER_USE_IMPERIAL
    return "in";
#else
    return "mm";
#endif
}
const char *weather_pressure_unit() {
#if WEATHER_USE_IMPERIAL
    return "inHg";
#else
    return "hPa";
#endif
}

