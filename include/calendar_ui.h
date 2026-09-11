#pragma once

#include <stdint.h>

void calendar_ui_init();
void calendar_ui_loop();
void calendar_ui_refresh(uint32_t change_flags);
int calendar_ui_week_offset();

void calendar_ui_wake_refresh(uint32_t change_flags);
