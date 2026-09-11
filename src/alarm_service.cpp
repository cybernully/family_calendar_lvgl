#include "alarm_service.h"
#include "app_config.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

namespace {

Preferences g_preferences;
bool g_preferences_ready = false;
char g_entity_id[96] = {};
AlarmoSnapshot g_snapshot = {};

bool usable(const char *value) {
    return value && value[0];
}

} // namespace

void alarm_service_begin() {
    g_preferences_ready = g_preferences.begin("famcal_alarm", false);

    if (usable(HA_ALARMO_ENTITY)) {
        snprintf(g_entity_id, sizeof(g_entity_id), "%s", HA_ALARMO_ENTITY);
    } else if (g_preferences_ready) {
        const String saved = g_preferences.getString("entity", "");
        snprintf(g_entity_id, sizeof(g_entity_id), "%s", saved.c_str());
    }
}

bool alarm_service_configured() {
    return g_entity_id[0] != '\0';
}

const char *alarm_service_entity() {
    return g_entity_id;
}

void alarm_service_set_entity(const char *entity_id) {
    if (!entity_id) entity_id = "";
    snprintf(g_entity_id, sizeof(g_entity_id), "%s", entity_id);
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    if (g_preferences_ready) g_preferences.putString("entity", g_entity_id);
}

void alarm_service_clear() {
    alarm_service_set_entity("");
}

void alarm_service_replace(const AlarmoSnapshot &snapshot) {
    if (snapshot.entity_id[0] && strcmp(snapshot.entity_id, g_entity_id) != 0) return;
    g_snapshot = snapshot;
}

const AlarmoSnapshot *alarm_service_snapshot() {
    return &g_snapshot;
}

const char *alarm_service_state() {
    return g_snapshot.valid ? g_snapshot.state : "unknown";
}

const char *alarm_service_friendly_name() {
    if (g_snapshot.valid && g_snapshot.friendly_name[0]) return g_snapshot.friendly_name;
    return g_entity_id[0] ? g_entity_id : "Alarmo";
}

bool alarm_service_code_required() {
    /* Home Assistant only exposes code_format while the current transition requires a code. */
    return g_snapshot.valid && g_snapshot.code_format[0] != '\0';
}

bool alarm_service_transitioning() {
    if (!g_snapshot.valid) return false;
    return strcmp(g_snapshot.state, "arming") == 0 ||
           strcmp(g_snapshot.state, "pending") == 0;
}
