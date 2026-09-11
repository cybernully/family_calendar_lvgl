#pragma once

#include <Arduino.h>
#include <lvgl.h>

bool board_lvgl_begin(uint16_t rotation);
void board_lvgl_loop();
void board_set_backlight(uint8_t percent);
int16_t board_width();
int16_t board_height();

/* Screen power/activity helpers.  The touch controller stays active while the
 * backlight is off, allowing the first touch to wake the display without also
 * activating the control underneath it. */
void board_set_display_awake(bool awake, uint8_t restore_percent);
bool board_display_awake();
bool board_take_touch_activity();

/* Latest mapped touch state in logical display coordinates.  This is a
 * lightweight snapshot of the same touch data fed to LVGL and is used by the
 * calendar for reliable distance-based swipe detection.  The last mapped x/y
 * are retained on release so callers can measure the full drag distance. */
bool board_get_touch_state(int16_t &x, int16_t &y, bool &pressed);
