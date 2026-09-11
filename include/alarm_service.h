#pragma once

#include <stddef.h>
#include <stdint.h>

struct AlarmoSnapshot {
    char entity_id[96];
    char friendly_name[80];
    char state[32];
    char next_state[32];
    char arm_mode[32];
    char open_sensors[320];
    char last_triggered[40];
    char code_format[20];
    uint32_t supported_features;
    int delay_seconds;
    uint16_t open_sensor_count;
    bool valid;
};

void alarm_service_begin();

bool alarm_service_configured();
const char *alarm_service_entity();
void alarm_service_set_entity(const char *entity_id);
void alarm_service_clear();

void alarm_service_replace(const AlarmoSnapshot &snapshot);
const AlarmoSnapshot *alarm_service_snapshot();

const char *alarm_service_state();
const char *alarm_service_friendly_name();
bool alarm_service_code_required();
bool alarm_service_transitioning();
