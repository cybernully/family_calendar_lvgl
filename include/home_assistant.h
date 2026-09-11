#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr uint32_t HA_CHANGE_NONE = 0;
constexpr uint32_t HA_CHANGE_CALENDAR = 1U << 0;
constexpr uint32_t HA_CHANGE_CHORES = 1U << 1;
constexpr uint32_t HA_CHANGE_TODO_LISTS = 1U << 2;
constexpr uint32_t HA_CHANGE_ALARM = 1U << 3;
constexpr uint32_t HA_CHANGE_ALARM_PANELS = 1U << 4;
constexpr uint32_t HA_CHANGE_WEATHER = 1U << 5;

void home_assistant_begin();
void home_assistant_loop(int week_offset);
void home_assistant_request_sync();
void home_assistant_request_calendar_window(int week_offset_center);
bool home_assistant_calendar_window_needs_refresh(int week_offset_center, uint32_t max_age_ms);
uint32_t home_assistant_take_changes();

bool home_assistant_configured();
bool home_assistant_authenticated();
const char *home_assistant_status();

/* Active-dashboard refresh coordination.  Automatic refreshes are scheduled by
 * the UI, while these helpers expose whether the single HA worker is currently
 * safe to wake and when each data domain was last attempted. */
bool home_assistant_ready_for_auto_refresh();
uint32_t home_assistant_last_calendar_request_ms();
uint32_t home_assistant_last_chore_request_ms();
uint32_t home_assistant_last_alarm_request_ms();

/* Home Assistant To-do list / chore support. */
void home_assistant_request_todo_discovery();
bool home_assistant_todo_lists_ready();
size_t home_assistant_todo_list_count();
const char *home_assistant_todo_list_entity(size_t index);
const char *home_assistant_todo_list_name(size_t index);
void home_assistant_request_chore_sync();
bool home_assistant_queue_chore_status(const char *uid, bool completed);

/* Alarmo alarm_control_panel discovery, state sync, and controls. */
void home_assistant_request_alarm_discovery();
bool home_assistant_alarm_panels_ready();
size_t home_assistant_alarm_panel_count();
const char *home_assistant_alarm_panel_entity(size_t index);
const char *home_assistant_alarm_panel_name(size_t index);
void home_assistant_request_alarm_sync();
bool home_assistant_queue_alarm_arm(const char *mode, const char *code);
bool home_assistant_queue_alarm_disarm(const char *code);
bool home_assistant_queue_alarm_skip_delay();
