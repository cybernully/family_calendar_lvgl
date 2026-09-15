#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Runtime household configuration backed by NVS.  app_local.h remains the
 * compile-time/default source; values saved from Web Management override those
 * defaults on subsequent boots.
 */
void runtime_config_begin();

const char *runtime_config_person_name(size_t index);
uint32_t runtime_config_person_color(size_t index);
const char *runtime_config_calendar_entity(size_t index);

bool runtime_config_set_person(size_t index,
                               const char *name,
                               uint32_t color,
                               const char *calendar_entity);

/* Number of non-empty calendar entity mappings. */
size_t runtime_config_calendar_count();
