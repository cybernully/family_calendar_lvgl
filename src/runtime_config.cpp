#include "runtime_config.h"

#include "app_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

constexpr size_t PERSON_COUNT = 4;
constexpr size_t NAME_LEN = 48;
constexpr size_t ENTITY_LEN = 96;

char g_names[PERSON_COUNT][NAME_LEN] = {};
char g_calendars[PERSON_COUNT][ENTITY_LEN] = {};
uint32_t g_colors[PERSON_COUNT] = {};
bool g_ready = false;

const char *k_default_names[PERSON_COUNT] = {
    PERSON_1_NAME, PERSON_2_NAME, PERSON_3_NAME, PERSON_4_NAME,
};

const char *k_default_calendars[PERSON_COUNT] = {
    HA_CALENDAR_1, HA_CALENDAR_2, HA_CALENDAR_3, HA_CALENDAR_4,
};

const uint32_t k_default_colors[PERSON_COUNT] = {
    PERSON_1_COLOR, PERSON_2_COLOR, PERSON_3_COLOR, PERSON_4_COLOR,
};

void load_defaults() {
    for (size_t i = 0; i < PERSON_COUNT; ++i) {
        snprintf(g_names[i], sizeof(g_names[i]), "%s", k_default_names[i] ? k_default_names[i] : "");
        snprintf(g_calendars[i], sizeof(g_calendars[i]), "%s", k_default_calendars[i] ? k_default_calendars[i] : "");
        g_colors[i] = k_default_colors[i];
    }
}

String key_for(const char *prefix, size_t index) {
    String key(prefix);
    key += static_cast<unsigned>(index + 1);
    return key;
}

uint32_t sanitize_color(uint32_t color, uint32_t fallback) {
    if (color > 0xFFFFFFUL) return fallback;
    return color;
}

} // namespace

void runtime_config_begin() {
    load_defaults();

    Preferences prefs;
    if (prefs.begin("famcal_people", true)) {
        for (size_t i = 0; i < PERSON_COUNT; ++i) {
            const String saved_name = prefs.getString(key_for("name", i).c_str(), g_names[i]);
            const String saved_calendar = prefs.getString(key_for("cal", i).c_str(), g_calendars[i]);
            const uint32_t saved_color = prefs.getUInt(key_for("color", i).c_str(), g_colors[i]);

            if (!saved_name.isEmpty()) {
                snprintf(g_names[i], sizeof(g_names[i]), "%s", saved_name.substring(0, NAME_LEN - 1).c_str());
            }
            snprintf(g_calendars[i], sizeof(g_calendars[i]), "%s",
                     saved_calendar.substring(0, ENTITY_LEN - 1).c_str());
            g_colors[i] = sanitize_color(saved_color, k_default_colors[i]);
        }
        prefs.end();
    }

    g_ready = true;
    Serial0.printf("[Config] Runtime household configuration loaded (%u calendars)\n",
                   static_cast<unsigned>(runtime_config_calendar_count()));
}

const char *runtime_config_person_name(size_t index) {
    if (!g_ready) load_defaults();
    return index < PERSON_COUNT ? g_names[index] : "";
}

uint32_t runtime_config_person_color(size_t index) {
    if (!g_ready) load_defaults();
    return index < PERSON_COUNT ? g_colors[index] : 0x64748B;
}

const char *runtime_config_calendar_entity(size_t index) {
    if (!g_ready) load_defaults();
    return index < PERSON_COUNT ? g_calendars[index] : "";
}

bool runtime_config_set_person(size_t index,
                               const char *name,
                               uint32_t color,
                               const char *calendar_entity) {
    if (index >= PERSON_COUNT) return false;
    if (!g_ready) runtime_config_begin();

    String clean_name = name ? name : "";
    String clean_calendar = calendar_entity ? calendar_entity : "";
    clean_name.trim();
    clean_calendar.trim();
    if (clean_name.isEmpty()) clean_name = k_default_names[index];
    if (clean_name.length() >= NAME_LEN) clean_name.remove(NAME_LEN - 1);
    if (clean_calendar.length() >= ENTITY_LEN) clean_calendar.remove(ENTITY_LEN - 1);
    if (!clean_calendar.isEmpty() && !clean_calendar.startsWith("calendar.")) return false;
    color = sanitize_color(color, k_default_colors[index]);

    Preferences prefs;
    if (!prefs.begin("famcal_people", false)) return false;
    const size_t a = prefs.putString(key_for("name", index).c_str(), clean_name);
    prefs.putUInt(key_for("color", index).c_str(), color);
    prefs.putString(key_for("cal", index).c_str(), clean_calendar);
    prefs.end();
    if (a == 0 && !clean_name.isEmpty()) return false;

    snprintf(g_names[index], sizeof(g_names[index]), "%s", clean_name.c_str());
    snprintf(g_calendars[index], sizeof(g_calendars[index]), "%s", clean_calendar.c_str());
    g_colors[index] = color;
    return true;
}

size_t runtime_config_calendar_count() {
    size_t count = 0;
    for (size_t i = 0; i < PERSON_COUNT; ++i) {
        if (runtime_config_calendar_entity(i)[0]) ++count;
    }
    return count;
}
