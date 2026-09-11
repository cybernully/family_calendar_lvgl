#include "chore_service.h"
#include "app_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

Preferences g_preferences;
bool g_preferences_ready = false;
char g_entity_id[96] = {};
ChoreItem g_items[HA_MAX_CHORES] = {};
size_t g_item_count = 0;

void persist_entity() {
    if (!g_preferences_ready) return;
    g_preferences.putString("entity", g_entity_id);
}

} // namespace

void chore_service_begin() {
    g_preferences_ready = g_preferences.begin("famcal_chore", false);

    String saved;
    if (g_preferences_ready) saved = g_preferences.getString("entity", "");

    if (saved.length() > 0) {
        snprintf(g_entity_id, sizeof(g_entity_id), "%s", saved.c_str());
    } else if (HA_CHORE_TODO_ENTITY[0]) {
        snprintf(g_entity_id, sizeof(g_entity_id), "%s", HA_CHORE_TODO_ENTITY);
        persist_entity();
    }

    g_item_count = 0;
    memset(g_items, 0, sizeof(g_items));
}

bool chore_service_loop() {
    return false;
}

bool chore_service_configured() {
    return g_entity_id[0] != '\0';
}

const char *chore_service_entity() {
    return g_entity_id;
}

void chore_service_set_entity(const char *entity_id) {
    const char *value = entity_id ? entity_id : "";
    if (strncmp(g_entity_id, value, sizeof(g_entity_id)) == 0) return;
    snprintf(g_entity_id, sizeof(g_entity_id), "%s", value);
    chore_service_clear();
    persist_entity();
}

void chore_service_clear() {
    g_item_count = 0;
    memset(g_items, 0, sizeof(g_items));
}

size_t chore_service_count() {
    return g_item_count;
}

const ChoreItem *chore_service_item(size_t index) {
    return index < g_item_count ? &g_items[index] : nullptr;
}

const char *chore_service_name(size_t index) {
    const ChoreItem *item = chore_service_item(index);
    return item ? item->summary : "";
}

const char *chore_service_uid(size_t index) {
    const ChoreItem *item = chore_service_item(index);
    return item ? item->uid : "";
}

bool chore_service_done(size_t index) {
    const ChoreItem *item = chore_service_item(index);
    return item ? item->done : false;
}

bool chore_service_set_done(size_t index, bool done) {
    if (index >= g_item_count) return false;
    g_items[index].done = done;
    return true;
}

size_t chore_service_completed_count() {
    size_t completed = 0;
    for (size_t i = 0; i < g_item_count; ++i) {
        if (g_items[i].done) ++completed;
    }
    return completed;
}

void chore_service_replace(const ChoreItem *items, size_t count) {
    if (!items) count = 0;
    if (count > HA_MAX_CHORES) count = HA_MAX_CHORES;
    if (count) memcpy(g_items, items, count * sizeof(ChoreItem));
    if (count < HA_MAX_CHORES) {
        memset(g_items + count, 0, (HA_MAX_CHORES - count) * sizeof(ChoreItem));
    }
    g_item_count = count;
}
