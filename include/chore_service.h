#pragma once

#include <stddef.h>
#include <stdint.h>
#include "app_config.h"

struct ChoreItem {
    char uid[96];
    char summary[112];
    char description[160];
    char due[40];
    bool done;
};

void chore_service_begin();
/* Reserved for future time-based behavior; currently returns false. */
bool chore_service_loop();

bool chore_service_configured();
const char *chore_service_entity();
void chore_service_set_entity(const char *entity_id);
void chore_service_clear();

size_t chore_service_count();
const ChoreItem *chore_service_item(size_t index);
const char *chore_service_name(size_t index);
const char *chore_service_uid(size_t index);
bool chore_service_done(size_t index);
bool chore_service_set_done(size_t index, bool done);
size_t chore_service_completed_count();

void chore_service_replace(const ChoreItem *items, size_t count);
