#include "calendar_ui.h"
#include "calendar_model.h"
#include "chore_service.h"
#include "alarm_service.h"
#include "camera_service.h"
#include "time_service.h"
#include "board_lvgl.h"
#include "app_config.h"
#include "home_assistant.h"
#include "network_service.h"
#include "weather_service.h"
#include "ui_fonts.h"
#include "ui_symbols.h"

#include <lvgl.h>
#include <Preferences.h>
#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {

constexpr int SCREEN_W = 1280;
constexpr int SCREEN_H = 800;
constexpr int HEADER_H = 84;
constexpr int FOOTER_H = 64;
constexpr int FOOTER_Y = SCREEN_H - FOOTER_H;
constexpr int PAGE_Y = HEADER_H + 8;
constexpr int PAGE_H = FOOTER_Y - PAGE_Y - 8;

constexpr int CAL_TOOLBAR_H = 62;
constexpr int CAL_MAIN_Y = HEADER_H + CAL_TOOLBAR_H + 10;
constexpr int CAL_MAIN_H = FOOTER_Y - CAL_MAIN_Y - 8;
constexpr int CAL_SIDE_W = 210;
constexpr int CAL_X = CAL_SIDE_W + 12;
constexpr int CAL_W = SCREEN_W - CAL_X - 8;
constexpr int DAY_GAP = 4;
constexpr int DAY_W = (CAL_W - DAY_GAP * 6) / 7;
constexpr int EVENT_CARD_H = 76;
constexpr int EVENT_CARD_STEP = 80;
constexpr int MAX_VISIBLE_EVENTS_PER_DAY = 6;

enum class Dashboard : uint8_t {
    Calendar = 0,
    Chores,
    Meals,
    Weather,
    Alarm,
    Settings,
};

struct ThemeColors {
    uint32_t bg;
    uint32_t panel;
    uint32_t panel_alt;
    uint32_t text;
    uint32_t muted;
    uint32_t border;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t today;
    uint32_t button;
    uint32_t button_pressed;
    uint32_t overlay;
    uint32_t success;
};

const ThemeColors LIGHT = {
    0xF3F4F6, 0xFFFFFF, 0xF9FAFB, 0x111827, 0x6B7280, 0xE5E7EB,
    0x2563EB, 0xDBEAFE, 0xEFF6FF, 0xF9FAFB, 0xE5E7EB, 0x111827, 0x16A34A
};

const ThemeColors DARK = {
    0x0F172A, 0x172033, 0x111827, 0xF8FAFC, 0x94A3B8, 0x334155,
    0x60A5FA, 0x1E3A5F, 0x172554, 0x1E293B, 0x334155, 0x020617, 0x4ADE80
};

lv_obj_t *g_screen = nullptr;
lv_obj_t *g_header_date = nullptr;
lv_obj_t *g_header_time = nullptr;
lv_obj_t *g_wifi_state = nullptr;
lv_obj_t *g_wifi_bars[4] = {};
lv_obj_t *g_status_label = nullptr;
lv_obj_t *g_summary_label = nullptr;
lv_obj_t *g_week_label = nullptr;
lv_obj_t *g_day_columns[7] = {};
lv_obj_t *g_filter_buttons[4] = {};
lv_obj_t *g_overlay = nullptr;
bool g_wake_sensor_picker_open = false;

lv_obj_t *g_home_network = nullptr;
lv_obj_t *g_home_ha = nullptr;
lv_obj_t *g_home_calendar = nullptr;
lv_obj_t *g_home_system = nullptr;

Dashboard g_dashboard = Dashboard::Calendar;
bool g_person_visible[4] = {true, true, true, true};
int g_week_offset = 0;
uint8_t g_backlight = APP_DEFAULT_BACKLIGHT;
bool g_dark_mode = APP_DEFAULT_DARK_MODE != 0;
uint32_t g_screen_timeout_seconds = APP_SCREEN_TIMEOUT_SECONDS;
uint32_t g_last_user_activity_ms = 0;
bool g_rebuild_pending = false;
bool g_chore_choose_list = false;
bool g_alarm_choose_panel = false;

enum class AlarmUiAction : uint8_t { None = 0, Disarm, ArmHome, ArmAway, ArmNight, ArmVacation, SkipDelay };
AlarmUiAction g_alarm_pending_action = AlarmUiAction::None;
char g_alarm_pin[32] = {};
lv_obj_t *g_alarm_code_input = nullptr;

uint32_t g_last_clock_update = 0;
uint32_t g_last_status_update = 0;
uint32_t g_dashboard_entered_ms = 0;
uint32_t g_last_calendar_auto_request_ms = 0;
uint32_t g_last_chore_auto_request_ms = 0;
uint32_t g_last_alarm_auto_request_ms = 0;
uint32_t g_last_weather_auto_request_ms = 0;

uint32_t g_week_nav_changed_ms = 0;
bool g_week_nav_refresh_pending = false;
int g_week_nav_refresh_offset = 0;

/* Calendar swipe tracking deliberately does not use LV_EVENT_GESTURE.  LVGL's
 * simple gesture recognizer requires both a minimum per-sample velocity and a
 * minimum distance; on this touch controller a comfortable finger drag can
 * miss the velocity threshold.  Track the mapped touch position directly and
 * decide on release using total horizontal distance instead. */
bool g_calendar_swipe_tracking = false;
bool g_calendar_swipe_prev_pressed = false;
int16_t g_calendar_swipe_start_x = 0;
int16_t g_calendar_swipe_start_y = 0;
int16_t g_calendar_swipe_last_x = 0;
int16_t g_calendar_swipe_last_y = 0;
constexpr int CALENDAR_SWIPE_MIN_X = 70;
constexpr int CALENDAR_SWIPE_MAX_Y = 120;

Preferences g_ui_preferences;
bool g_ui_preferences_ready = false;

WeatherSnapshot g_weather_snapshot = {};

const char *screen_timeout_text(uint32_t seconds);
const char *selected_wake_sensor_name();

const ThemeColors &theme() {
    return g_dark_mode ? DARK : LIGHT;
}

const char *dashboard_name(Dashboard dashboard) {
    switch (dashboard) {
        case Dashboard::Calendar: return "Calendar";
        case Dashboard::Chores: return "Chores";
        case Dashboard::Meals: return "Meals";
        case Dashboard::Weather: return "Weather";
        case Dashboard::Alarm: return "Alarm";
        case Dashboard::Settings: return "Settings";
    }
    return "Calendar";
}

const char *dashboard_symbol(Dashboard dashboard) {
    switch (dashboard) {
        case Dashboard::Calendar: return SYMBOL_CALENDAR;
        case Dashboard::Chores: return SYMBOL_CHORES;
        case Dashboard::Meals: return SYMBOL_MEAL;
        case Dashboard::Weather: return SYMBOL_WEATHER;
        case Dashboard::Alarm: return SYMBOL_ALARM;
        case Dashboard::Settings: return SYMBOL_SETTINGS;
    }
    return SYMBOL_CALENDAR;
}

void style_box(lv_obj_t *obj, uint32_t bg, int radius = 12, int border = 0) {
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, border, LV_PART_MAIN);
    if (border > 0) {
        lv_obj_set_style_border_color(obj, lv_color_hex(theme().border), LV_PART_MAIN);
    }
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    /*
     * LVGL's default theme gives generic objects/buttons internal padding.
     * All coordinates in this UI are intentionally pixel-positioned, so
     * leaving that theme padding in place shifts child content and can clip
     * second-line labels at container boundaries. Start every styled box
     * from a zero-padding content area; callers that need padding add it
     * explicitly after style_box().
     */
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *label(lv_obj_t *parent,
                const char *text,
                const lv_font_t *font,
                uint32_t color,
                int width = LV_SIZE_CONTENT) {
    if (!parent) return nullptr;
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return nullptr;
    lv_obj_set_style_text_font(obj, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(obj, 2, LV_PART_MAIN);
    /*
     * Keep labels padding-free when using LV_SIZE_CONTENT. Padding on the
     * label itself was contributing to the half-clipped subtitle/time text
     * visible on the physical panel.
     */
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    if (width != LV_SIZE_CONTENT) lv_obj_set_width(obj, width);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_label_set_text(obj, text ? text : "");
    return obj;
}

lv_obj_t *button(lv_obj_t *parent,
                 const char *text,
                 int w,
                 int h,
                 bool active = false,
                 const lv_font_t *font = &lv_font_montserrat_16) {
    if (!parent) return nullptr;
    lv_obj_t *btn = lv_button_create(parent);
    if (!btn) return nullptr;
    lv_obj_set_size(btn, w, h);
    style_box(btn, active ? theme().accent : theme().button, 10, active ? 0 : 1);
    lv_obj_set_style_bg_color(
        btn,
        lv_color_hex(active ? theme().accent : theme().button_pressed),
        static_cast<lv_style_selector_t>(
            static_cast<uint32_t>(LV_PART_MAIN) |
            static_cast<uint32_t>(LV_STATE_PRESSED)));
    lv_obj_t *txt = label(btn, text, font, active ? 0xFFFFFF : theme().text);
    if (txt) lv_obj_center(txt);
    return btn;
}

void set_label_text(lv_obj_t *obj, const char *text) {
    if (obj) lv_label_set_text(obj, text ? text : "");
}

void set_pos_if(lv_obj_t *obj, int x, int y) {
    if (obj) lv_obj_set_pos(obj, x, y);
}

void set_height_if(lv_obj_t *obj, int h) {
    if (obj) lv_obj_set_height(obj, h);
}

void style_icon_piece(lv_obj_t *obj, uint32_t color, int radius = 0) {
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *icon_piece(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    if (!parent) return nullptr;
    lv_obj_t *piece = lv_obj_create(parent);
    if (!piece) return nullptr;
    lv_obj_set_size(piece, w, h);
    lv_obj_set_pos(piece, x, y);
    style_icon_piece(piece, color, radius);
    return piece;
}

void create_theme_icon(lv_obj_t *button_obj) {
    if (!button_obj) return;
    const uint32_t icon_color = theme().text;

    if (g_dark_mode) {
        /* Crescent moon: draw one circle, then mask its upper-right side with
         * a second circle that matches the button background. Using LVGL
         * primitives avoids relying on Unicode glyph coverage. */
        icon_piece(button_obj, 17, 12, 22, 22, icon_color, 11);
        icon_piece(button_obj, 25, 8, 20, 20, theme().button, 10);
        return;
    }

    /* Sun: center disc plus four rays and four corner glints. */
    icon_piece(button_obj, 21, 15, 14, 14, icon_color, 7);
    icon_piece(button_obj, 26, 5, 4, 7, icon_color, 2);
    icon_piece(button_obj, 26, 32, 4, 7, icon_color, 2);
    icon_piece(button_obj, 11, 20, 7, 4, icon_color, 2);
    icon_piece(button_obj, 38, 20, 7, 4, icon_color, 2);
    icon_piece(button_obj, 15, 9, 4, 4, icon_color, 2);
    icon_piece(button_obj, 37, 9, 4, 4, icon_color, 2);
    icon_piece(button_obj, 15, 31, 4, 4, icon_color, 2);
    icon_piece(button_obj, 37, 31, 4, 4, icon_color, 2);
}

static lv_obj_t *create_nav_button(lv_obj_t *parent,
                                    const char *symbol,
                                    const char *text,
                                    int w,
                                    int h,
                                    bool selected) {
    if (!parent) return nullptr;

    lv_obj_t *btn = lv_button_create(parent);
    if (!btn) return nullptr;

    lv_obj_set_size(btn, w, h);

    // Match the visual behavior of the existing button() helper.
    style_box(btn, selected ? theme().accent : theme().button, 10, selected ? 0 : 1);
    lv_obj_set_style_bg_color(
        btn,
        lv_color_hex(selected ? theme().accent : theme().button_pressed),
        static_cast<lv_style_selector_t>(
            static_cast<uint32_t>(LV_PART_MAIN) |
            static_cast<uint32_t>(LV_STATE_PRESSED)));

    // The footer is only 64 px tall, so keep the 24 px icon and caption
    // on one horizontal row rather than stacking them vertically.
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        btn,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 10, LV_PART_MAIN);

    const uint32_t text_color = selected ? 0xFFFFFF : theme().text;

    lv_obj_t *icon = label(btn, symbol, &ui_font_icons_24, text_color);
    if (icon) {
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *caption = label(btn, text, &lv_font_montserrat_14, text_color);
    if (caption) {
        lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);
    }

    return btn;
}

lv_obj_t *create_weather_icon(lv_obj_t *parent,
                              WeatherCondition condition,
                              int size,
                              uint32_t primary,
                              uint32_t secondary) {
    if (!parent) return nullptr;
    if (size < 16) size = 16;

    lv_obj_t *icon = lv_obj_create(parent);
    if (!icon) return nullptr;
    lv_obj_set_size(icon, size, size);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    const int sun_r = size / 6;
    const int sun_d = sun_r * 2;
    const int cloud_h = size / 4;
    const int cloud_y = size / 2;

    auto draw_cloud = [&](uint32_t color) {
        icon_piece(icon, size / 6, cloud_y - 2, size * 2 / 3, cloud_h + 3, color, cloud_h / 2);
        icon_piece(icon, size / 4, cloud_y - cloud_h / 2 - 1, cloud_h, cloud_h, color, cloud_h / 2);
        icon_piece(icon, size / 2 - cloud_h / 3, cloud_y - cloud_h / 2 - 4,
                   cloud_h + 2, cloud_h + 2, color, (cloud_h + 2) / 2);
    };

    switch (condition) {
        case WeatherCondition::Clear:
            icon_piece(icon, size / 2 - sun_r, size / 2 - sun_r, sun_d, sun_d, primary, sun_r);
            icon_piece(icon, size / 2 - 1, size / 8, 2, size / 5, primary, 1);
            icon_piece(icon, size / 2 - 1, size * 5 / 8, 2, size / 5, primary, 1);
            icon_piece(icon, size / 8, size / 2 - 1, size / 5, 2, primary, 1);
            icon_piece(icon, size * 5 / 8, size / 2 - 1, size / 5, 2, primary, 1);
            break;

        case WeatherCondition::PartlyCloudy:
            icon_piece(icon, size / 4, size / 6, sun_d, sun_d, secondary, sun_r);
            icon_piece(icon, size / 4 + sun_r - 1, size / 14, 2, size / 7, secondary, 1);
            icon_piece(icon, size / 8, size / 6 + sun_r - 1, size / 8, 2, secondary, 1);
            draw_cloud(primary);
            break;

        case WeatherCondition::Cloudy:
            draw_cloud(primary);
            icon_piece(icon, size / 3, cloud_y - cloud_h / 2 - 6, cloud_h, cloud_h, primary, cloud_h / 2);
            break;

        case WeatherCondition::Rain:
        case WeatherCondition::Showers:
            draw_cloud(primary);
            icon_piece(icon, size / 3, cloud_y + cloud_h + 2, 2, size / 5, secondary, 1);
            icon_piece(icon, size / 2, cloud_y + cloud_h + 4, 2, size / 5, secondary, 1);
            if (condition == WeatherCondition::Showers) {
                icon_piece(icon, size / 4, cloud_y + cloud_h + 5, 2, size / 6, secondary, 1);
            }
            break;

        case WeatherCondition::Thunderstorm:
            draw_cloud(primary);
            icon_piece(icon, size / 2 - 2, cloud_y + cloud_h + 2, size / 6, 3, secondary, 1);
            icon_piece(icon, size / 2 + size / 12 - 2, cloud_y + cloud_h + 4, 3, size / 6, secondary, 1);
            icon_piece(icon, size / 2 + size / 12 - 1, cloud_y + cloud_h + size / 6 + 2,
                       size / 6, 3, secondary, 1);
            break;

        case WeatherCondition::Snow:
            draw_cloud(primary);
            icon_piece(icon, size / 3, cloud_y + cloud_h + 4, 3, 3, secondary, 2);
            icon_piece(icon, size / 2, cloud_y + cloud_h + 6, 3, 3, secondary, 2);
            icon_piece(icon, size * 2 / 3 - 4, cloud_y + cloud_h + 4, 3, 3, secondary, 2);
            break;

        case WeatherCondition::Fog:
            draw_cloud(primary);
            icon_piece(icon, size / 5, size * 3 / 4, size * 3 / 5, 2, secondary, 1);
            icon_piece(icon, size / 4, size * 3 / 4 + 6, size / 2, 2, secondary, 1);
            break;

        case WeatherCondition::Unknown:
        default:
            icon_piece(icon, size / 2 - sun_r, size / 2 - sun_r, sun_d, sun_d, primary, sun_r);
            break;
    }

    return icon;
}

const char *wind_direction_text(float degrees) {
    static const char *dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    if (isnan(degrees)) return "-";
    float normalized = fmodf(degrees, 360.0f);
    if (normalized < 0.0f) normalized += 360.0f;
    const int index = static_cast<int>((normalized + 22.5f) / 45.0f) % 8;
    return dirs[index];
}

int rounded_display_temp(float celsius) {
    if (!isfinite(celsius)) return 0;
    return static_cast<int>(lroundf(weather_display_temperature(celsius)));
}

void update_wifi_header() {
    if (!g_wifi_state || !g_wifi_bars[0]) return;

    const bool connected = network_service_connected();
    int active_bars = 0;
    uint32_t state_color = g_dark_mode ? 0xF87171 : 0xDC2626;

    if (connected) {
        const int rssi = network_service_rssi();
        if (rssi >= -55) active_bars = 4;
        else if (rssi >= -65) active_bars = 3;
        else if (rssi >= -75) active_bars = 2;
        else active_bars = 1;
        state_color = theme().success;
        set_label_text(g_wifi_state, "ON");
    } else {
        set_label_text(g_wifi_state, "OFF");
    }

    lv_obj_set_style_text_color(g_wifi_state, lv_color_hex(state_color), LV_PART_MAIN);
    for (int i = 0; i < 4; ++i) {
        const bool active = connected && i < active_bars;
        const uint32_t color = active ? theme().success : theme().muted;
        lv_obj_set_style_bg_color(g_wifi_bars[i], lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(g_wifi_bars[i], active ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    }
}

void persist_ui_state() {
    if (!g_ui_preferences_ready) return;
    g_ui_preferences.putBool("dark", g_dark_mode);
    g_ui_preferences.putUChar("bright", g_backlight);
    g_ui_preferences.putUInt("timeout", g_screen_timeout_seconds);
}

void request_rebuild() {
    g_rebuild_pending = true;
}

void close_overlay() {
    g_wake_sensor_picker_open = false;
    if (!g_overlay) return;
    lv_obj_delete(g_overlay);
    g_overlay = nullptr;
    if (g_screen) lv_obj_invalidate(g_screen);
}

void close_overlay_cb(lv_event_t *) {
    close_overlay();
}

void show_info_overlay(const char *title, const char *body) {
    close_overlay();

    g_overlay = lv_obj_create(g_screen);
    lv_obj_set_size(g_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(theme().overlay), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_overlay);
    lv_obj_set_size(card, 620, 360);
    lv_obj_center(card);
    style_box(card, theme().panel, 18, 1);
    lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);

    lv_obj_t *title_label = label(card, title, &lv_font_montserrat_28, theme().text, 550);
    lv_obj_set_pos(title_label, 0, 0);

    lv_obj_t *body_label = label(card, body, &lv_font_montserrat_18, theme().muted, 550);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body_label, 205);
    lv_obj_set_pos(body_label, 0, 58);

    lv_obj_t *close = button(card, "Close", 120, 46, true);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, close_overlay_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_move_foreground(g_overlay);
    lv_obj_invalidate(g_screen);
}

void show_event_details_overlay(const CalendarEvent &event) {
    close_overlay();

    g_overlay = lv_obj_create(g_screen);
    lv_obj_set_size(g_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(theme().overlay), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_overlay);
    lv_obj_set_size(card, 840, 590);
    lv_obj_center(card);
    style_box(card, theme().panel, 18, 1);

    lv_obj_t *eyebrow = label(card, "CALENDAR EVENT", &lv_font_montserrat_12, theme().accent, 590);
    lv_obj_set_pos(eyebrow, 28, 22);

    lv_obj_t *title_label = label(card, event.title, &lv_font_montserrat_26, theme().text, 650);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(title_label, 64);
    lv_obj_set_pos(title_label, 28, 46);

    lv_obj_t *close = button(card, "Close", 112, 44, true, &lv_font_montserrat_14);
    lv_obj_set_pos(close, 700, 24);
    lv_obj_add_event_cb(close, close_overlay_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *details = lv_obj_create(card);
    lv_obj_set_size(details, 784, 438);
    lv_obj_set_pos(details, 28, 124);
    style_box(details, theme().panel_alt, 12, 1);
    lv_obj_set_style_pad_all(details, 18, LV_PART_MAIN);
    lv_obj_add_flag(details, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(details, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(details, LV_SCROLLBAR_MODE_AUTO);

    char start_date[64];
    char end_date[64];
    char range[64];
    time_service_format_date(event.start_epoch, start_date, sizeof(start_date));
    calendar_format_event_range(event, range, sizeof(range));

    time_t effective_end = event.end_epoch;
    if (event.all_day && effective_end > event.start_epoch) {
        // Home Assistant all-day event end dates are exclusive.
        effective_end -= 1;
    }
    time_service_format_date(effective_end, end_date, sizeof(end_date));

    struct tm start_tm = {};
    struct tm end_tm = {};
    localtime_r(&event.start_epoch, &start_tm);
    localtime_r(&effective_end, &end_tm);
    const bool same_day = start_tm.tm_year == end_tm.tm_year && start_tm.tm_yday == end_tm.tm_yday;

    char when[150];
    if (event.all_day) {
        if (same_day) snprintf(when, sizeof(when), "%s  |  All day", start_date);
        else snprintf(when, sizeof(when), "%s - %s  |  All day", start_date, end_date);
    } else if (same_day) {
        snprintf(when, sizeof(when), "%s  |  %s", start_date, range);
    } else {
        char start_time[32];
        char end_time[32];
        time_service_format_time(event.start_epoch, start_time, sizeof(start_time));
        time_service_format_time(event.end_epoch, end_time, sizeof(end_time));
        snprintf(when, sizeof(when), "%s %s - %s %s", start_date, start_time, end_date, end_time);
    }

    const char *calendar_name =
        event.person < 4 ? CALENDAR_PEOPLE[event.person].name : "Calendar";

    char body[1024];
    size_t used = 0;
    auto append_line = [&](const char *label_text, const char *value) {
        if (!value || !value[0] || used >= sizeof(body) - 1) return;
        const int written = snprintf(body + used, sizeof(body) - used,
                                     "%s%s: %s\n",
                                     used ? "\n" : "",
                                     label_text, value);
        if (written > 0) {
            const size_t available = sizeof(body) - used;
            used += static_cast<size_t>(written) < available
                        ? static_cast<size_t>(written)
                        : available - 1;
        }
    };

    append_line("When", when);
    append_line("Calendar", calendar_name);
    append_line("Location", event.location);
    append_line("Status", event.status);
    append_line("Home Assistant", event.source);

    if (event.description[0] && used < sizeof(body) - 1) {
        const int written = snprintf(body + used, sizeof(body) - used,
                                     "%sDescription:\n%s",
                                     used ? "\n" : "", event.description);
        if (written > 0) {
            const size_t available = sizeof(body) - used;
            used += static_cast<size_t>(written) < available
                        ? static_cast<size_t>(written)
                        : available - 1;
        }
    }
    body[sizeof(body) - 1] = '\0';

    lv_obj_t *body_label = label(details, body, &lv_font_montserrat_16, theme().text, 744);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body_label, LV_SIZE_CONTENT);
    lv_obj_set_pos(body_label, 0, 0);

    lv_obj_move_foreground(g_overlay);
    lv_obj_invalidate(g_screen);
}

void event_clicked_cb(lv_event_t *e) {
    const CalendarEvent *event = static_cast<const CalendarEvent *>(lv_event_get_user_data(e));
    if (!event) return;
    show_event_details_overlay(*event);
}

void dashboard_nav_cb(lv_event_t *e) {
    const intptr_t value = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (value < 0 || value > 5) return;
    const Dashboard next = static_cast<Dashboard>(value);
    if (next != g_dashboard) {
        g_dashboard = next;
        g_dashboard_entered_ms = millis();
    }
    close_overlay();
    request_rebuild();
}

void theme_toggle_cb(lv_event_t *) {
    g_dark_mode = !g_dark_mode;
    persist_ui_state();
    close_overlay();
    request_rebuild();
}

void brightness_cb(lv_event_t *e) {
    const intptr_t delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    int value = static_cast<int>(g_backlight) + static_cast<int>(delta);
    if (value < 10) value = 10;
    if (value > 100) value = 100;
    g_backlight = static_cast<uint8_t>(value);
    if (board_display_awake()) board_set_backlight(g_backlight);
    persist_ui_state();

    if (g_dashboard == Dashboard::Settings) request_rebuild();
}

void update_clock() {
    if (!g_header_date || !g_header_time) return;
    const time_t now = time_service_now();
    char date_buf[64];
    char time_buf[32];
    time_service_format_date(now, date_buf, sizeof(date_buf));
    time_service_format_time(now, time_buf, sizeof(time_buf));
    set_label_text(g_header_date, date_buf);
    set_label_text(g_header_time, time_buf);
}

void create_header() {
    lv_obj_t *header = lv_obj_create(g_screen);
    lv_obj_set_size(header, SCREEN_W, HEADER_H);
    lv_obj_set_pos(header, 0, 0);
    style_box(header, theme().panel, 0, 0);

    /*
     * Two deliberate rows with generous vertical clearance. The previous
     * 72 px header put row-two labels too close to the parent's clipped
     * content edge once LVGL theme padding was taken into account.
     */
    lv_obj_t *title = label(header, APP_NAME, &lv_font_montserrat_24, theme().text, 310);
    lv_obj_set_pos(title, 24, 10);

    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "%s dashboard  |  v%s", dashboard_name(g_dashboard), APP_VERSION);
    lv_obj_t *page = label(header, subtitle, &lv_font_montserrat_14, theme().muted, 330);
    lv_obj_set_pos(page, 24, 50);

    g_header_date = label(header, "", &lv_font_montserrat_18, theme().text, 360);
    lv_obj_set_pos(g_header_date, 390, 12);

    g_header_time = label(header, "", &lv_font_montserrat_16, theme().muted, 220);
    lv_obj_set_pos(g_header_time, 390, 49);

    /* Compact Wi-Fi status chip.  Four ascending bars represent RSSI while
     * ON/OFF makes the disconnected state explicit instead of relying on bar
     * color alone. */
    lv_obj_t *wifi = lv_obj_create(header);
    lv_obj_set_size(wifi, 142, 46);
    lv_obj_set_pos(wifi, SCREEN_W - 238, 19);
    style_box(wifi, theme().button, 10, 1);

    lv_obj_t *wifi_label = label(wifi, "WiFi", &lv_font_montserrat_14, theme().text, 40);
    lv_obj_set_pos(wifi_label, 10, 14);
    g_wifi_state = label(wifi, "OFF", &lv_font_montserrat_12, theme().muted, 30);
    lv_obj_set_pos(g_wifi_state, 49, 16);

    constexpr int WIFI_BAR_X = 86;
    constexpr int WIFI_BAR_BOTTOM = 34;
    constexpr int WIFI_BAR_WIDTH = 7;
    constexpr int WIFI_BAR_GAP = 3;
    constexpr int WIFI_BAR_HEIGHTS[4] = {7, 12, 17, 22};
    for (int i = 0; i < 4; ++i) {
        const int h = WIFI_BAR_HEIGHTS[i];
        g_wifi_bars[i] = icon_piece(
            wifi,
            WIFI_BAR_X + i * (WIFI_BAR_WIDTH + WIFI_BAR_GAP),
            WIFI_BAR_BOTTOM - h,
            WIFI_BAR_WIDTH,
            h,
            theme().muted,
            2);
    }

    lv_obj_t *theme_btn = lv_button_create(header);
    lv_obj_set_size(theme_btn, 56, 46);
    style_box(theme_btn, theme().button, 10, 1);
    lv_obj_set_style_bg_color(
        theme_btn,
        lv_color_hex(theme().button_pressed),
        static_cast<lv_style_selector_t>(
            static_cast<uint32_t>(LV_PART_MAIN) |
            static_cast<uint32_t>(LV_STATE_PRESSED)));
    lv_obj_set_pos(theme_btn, SCREEN_W - 80, 19);
    create_theme_icon(theme_btn);
    lv_obj_add_event_cb(theme_btn, theme_toggle_cb, LV_EVENT_CLICKED, nullptr);

    update_wifi_header();
}

void create_footer() {
    lv_obj_t *footer = lv_obj_create(g_screen);
    lv_obj_set_size(footer, SCREEN_W, FOOTER_H);
    lv_obj_set_pos(footer, 0, FOOTER_Y);
    style_box(footer, theme().panel, 0, 0);

    const Dashboard dashboards[] = {
        Dashboard::Calendar,
        Dashboard::Chores,
        Dashboard::Meals,
        Dashboard::Weather,
        Dashboard::Alarm,
        Dashboard::Settings,
    };

    constexpr int BUTTON_W = 202;
    constexpr int GAP = 8;
    constexpr int TOTAL_W = BUTTON_W * 6 + GAP * 5;
    int x = (SCREEN_W - TOTAL_W) / 2;

    for (Dashboard dashboard : dashboards) {
        const bool active = dashboard == g_dashboard;
        lv_obj_t *nav = create_nav_button(
            footer,
            dashboard_symbol(dashboard),
            dashboard_name(dashboard),
            BUTTON_W,
            46,
            active);
        lv_obj_set_pos(nav, x, 9);
        lv_obj_add_event_cb(
            nav,
            dashboard_nav_cb,
            LV_EVENT_CLICKED,
            reinterpret_cast<void *>(static_cast<intptr_t>(dashboard)));
        x += BUTTON_W + GAP;
    }
}

void add_event_card(lv_obj_t *column, const CalendarEvent *event, int slot) {
    lv_obj_t *card = lv_button_create(column);
    lv_obj_set_size(card, DAY_W - 12, EVENT_CARD_H);
    lv_obj_set_pos(card, 6, 66 + slot * EVENT_CARD_STEP);
    style_box(card, CALENDAR_PEOPLE[event->person].color, 9, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 7, LV_PART_MAIN);

    char time_text[22];
    calendar_format_event_time(*event, time_text, sizeof(time_text));

    lv_obj_t *time_label = label(card, time_text, &lv_font_montserrat_12, 0xFFFFFF, DAY_W - 28);
    lv_obj_set_pos(time_label, 0, -1);

    lv_obj_t *title_label = label(card, event->title, &lv_font_montserrat_14, 0xFFFFFF, DAY_W - 28);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(title_label, 44);
    lv_obj_set_pos(title_label, 0, 23);

    lv_obj_add_event_cb(card, event_clicked_cb, LV_EVENT_CLICKED, const_cast<CalendarEvent *>(event));
}

void render_week() {
    if (g_dashboard != Dashboard::Calendar || !g_week_label) return;

    const time_t now = time_service_now();
    time_t week_start = time_service_start_of_week(now);
    week_start = time_service_add_days(week_start, g_week_offset * 7);

    static const char *short_months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    char start_text[32];
    char end_text[32];
    struct tm tm_value = {};
    localtime_r(&week_start, &tm_value);
    snprintf(start_text, sizeof(start_text), "%s %d", short_months[tm_value.tm_mon], tm_value.tm_mday);
    time_t week_end = time_service_add_days(week_start, 6);
    localtime_r(&week_end, &tm_value);
    snprintf(end_text, sizeof(end_text), "%s %d, %d", short_months[tm_value.tm_mon], tm_value.tm_mday, tm_value.tm_year + 1900);

    char range[80];
    snprintf(range, sizeof(range), "%s - %s", start_text, end_text);
    set_label_text(g_week_label, range);

    struct tm now_tm = {};
    localtime_r(&now, &now_tm);

    for (int day = 0; day < 7; ++day) {
        lv_obj_t *column = g_day_columns[day];
        if (!column) continue;
        lv_obj_clean(column);

        const time_t day_time = time_service_add_days(week_start, day);
        struct tm day_tm = {};
        localtime_r(&day_time, &day_tm);

        const bool is_today = (g_week_offset == 0 &&
                               day_tm.tm_year == now_tm.tm_year &&
                               day_tm.tm_yday == now_tm.tm_yday);

        lv_obj_set_style_bg_color(column, lv_color_hex(is_today ? theme().today : theme().panel), LV_PART_MAIN);

        lv_obj_t *day_name = label(column, calendar_day_name(day), &lv_font_montserrat_14,
                                   is_today ? theme().accent : theme().muted);
        lv_obj_align(day_name, LV_ALIGN_TOP_MID, 0, 8);

        char number[8];
        snprintf(number, sizeof(number), "%d", day_tm.tm_mday);
        lv_obj_t *day_number = label(column, number, &lv_font_montserrat_24,
                                     is_today ? theme().accent : theme().text);
        lv_obj_align(day_number, LV_ALIGN_TOP_MID, 0, 32);

        WeatherDayForecast day_weather = {};
        if (weather_service_get_day_forecast(day_time, day_weather) && day_weather.valid) {
            lv_obj_t *weather_row = lv_obj_create(column);
            lv_obj_set_size(weather_row, 54, 18);
            lv_obj_set_pos(weather_row, DAY_W - 58, 42);
            lv_obj_set_style_bg_opa(weather_row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(weather_row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(weather_row, 0, LV_PART_MAIN);
            lv_obj_remove_flag(weather_row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(weather_row, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *icon = create_weather_icon(weather_row,
                                                 day_weather.condition,
                                                 14,
                                                 theme().muted,
                                                 theme().accent);
            set_pos_if(icon, 0, 2);

            char temps[24];
            snprintf(temps, sizeof(temps), "%d/%d",
                     rounded_display_temp(day_weather.high_c),
                     rounded_display_temp(day_weather.low_c));
            lv_obj_t *temp = label(weather_row, temps, &lv_font_montserrat_12, theme().muted, 36);
            set_pos_if(temp, 16, 2);
        }

        int slot = 0;
        int hidden = 0;
        for (size_t i = 0; i < CALENDAR_EVENT_COUNT; ++i) {
            const CalendarEvent &event = CALENDAR_EVENTS[i];
            if (!calendar_event_is_on_day(event, day_time)) continue;
            if (event.person >= 4 || !g_person_visible[event.person]) continue;
            if (slot < MAX_VISIBLE_EVENTS_PER_DAY) {
                add_event_card(column, &event, slot++);
            } else {
                ++hidden;
            }
        }

        if (slot == 0) {
            lv_obj_t *empty = label(column, "No events", &lv_font_montserrat_12, theme().muted);
            lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 94);
        } else if (hidden > 0) {
            char more[24];
            snprintf(more, sizeof(more), "+%d more", hidden);
            lv_obj_t *more_label = label(column, more, &lv_font_montserrat_12, theme().muted);
            lv_obj_align(more_label, LV_ALIGN_BOTTOM_MID, 0, -6);
        }
    }

    if (g_status_label) set_label_text(g_status_label, home_assistant_status());

    if (g_summary_label) {
        const size_t today_events = calendar_count_events_on_day(time_service_now());
        char summary[96];
        snprintf(summary, sizeof(summary), "%u event%s today\n%u total this week",
                 static_cast<unsigned>(today_events), today_events == 1 ? "" : "s",
                 static_cast<unsigned>(CALENDAR_EVENT_COUNT));
        set_label_text(g_summary_label, summary);
    }
}

void filter_clicked_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0 || index > 3) return;
    g_person_visible[index] = !g_person_visible[index];

    lv_obj_t *btn = g_filter_buttons[index];
    if (btn) {
        const uint32_t color = g_person_visible[index] ? CALENDAR_PEOPLE[index].color : theme().button;
        const uint32_t text_color = g_person_visible[index] ? 0xFFFFFF : theme().muted;
        lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_t *txt = lv_obj_get_child(btn, 0);
        if (txt) lv_obj_set_style_text_color(txt, lv_color_hex(text_color), LV_PART_MAIN);
    }
    render_week();
}

void set_week_offset(int new_offset, const char *reason) {
    if (new_offset == g_week_offset) return;

    g_week_offset = new_offset;
    g_week_nav_changed_ms = millis();
    g_week_nav_refresh_offset = new_offset;
    g_week_nav_refresh_pending = true;

    ESP_LOGI("FamilyCalendar", "[Calendar] Week changed to offset %d (%s)",
             g_week_offset, reason ? reason : "unknown");
    ESP_LOGI("FamilyCalendar", "[Calendar] Prefetch debounce started");
}

void service_calendar_prefetch_debounce(uint32_t now_ms) {
    if (!g_week_nav_refresh_pending) return;
    if (!board_display_awake()) return;
    if (HA_CALENDAR_NAV_DEBOUNCE_MS > 0 && now_ms - g_week_nav_changed_ms < HA_CALENDAR_NAV_DEBOUNCE_MS) {
        return;
    }

    const int settled_offset = g_week_nav_refresh_offset;
    g_week_nav_refresh_pending = false;
    ESP_LOGI("FamilyCalendar", "[Calendar] Prefetch selected +/-1: center offset %d", settled_offset);

    if (!home_assistant_calendar_window_needs_refresh(settled_offset,
                                                      HA_CALENDAR_NAV_REFRESH_MAX_AGE_MS)) {
        ESP_LOGI("FamilyCalendar", "[Calendar] Cache fresh; network request skipped");
        return;
    }

    home_assistant_request_calendar_window(settled_offset);
}

void shift_week_cb(lv_event_t *e) {
    const intptr_t delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    set_week_offset(g_week_offset + static_cast<int>(delta), "button");
}

void service_calendar_swipe() {
    int16_t x = 0;
    int16_t y = 0;
    bool pressed = false;
    const bool valid = board_get_touch_state(x, y, pressed);

    if (g_dashboard != Dashboard::Calendar || g_overlay || !board_display_awake()) {
        g_calendar_swipe_tracking = false;
        g_calendar_swipe_prev_pressed = pressed;
        return;
    }

    if (!valid) {
        g_calendar_swipe_prev_pressed = false;
        return;
    }

    /* Start only when the finger goes down inside the seven-day week region.
     * The swipe may finish outside the region; that feels natural at the edges. */
    if (pressed && !g_calendar_swipe_prev_pressed) {
        const bool inside_week =
            x >= CAL_X && x < (CAL_X + CAL_W) &&
            y >= CAL_MAIN_Y && y < (CAL_MAIN_Y + CAL_MAIN_H);

        g_calendar_swipe_tracking = inside_week;
        if (inside_week) {
            g_calendar_swipe_start_x = x;
            g_calendar_swipe_start_y = y;
            g_calendar_swipe_last_x = x;
            g_calendar_swipe_last_y = y;
        }
    } else if (pressed && g_calendar_swipe_tracking) {
        g_calendar_swipe_last_x = x;
        g_calendar_swipe_last_y = y;
    }

    if (!pressed && g_calendar_swipe_prev_pressed && g_calendar_swipe_tracking) {
        const int dx = static_cast<int>(g_calendar_swipe_last_x) -
                       static_cast<int>(g_calendar_swipe_start_x);
        const int dy = static_cast<int>(g_calendar_swipe_last_y) -
                       static_cast<int>(g_calendar_swipe_start_y);
        const int abs_dx = dx < 0 ? -dx : dx;
        const int abs_dy = dy < 0 ? -dy : dy;

        /* Require a meaningful horizontal drag and reject strongly vertical
         * motions.  No velocity requirement: slow deliberate swipes work too. */
        if (abs_dx >= CALENDAR_SWIPE_MIN_X &&
            abs_dy <= CALENDAR_SWIPE_MAX_Y &&
            abs_dx > abs_dy) {
            if (dx < 0) {
                ESP_LOGI("FamilyCalendar",
                         "Calendar distance swipe left accepted (dx=%d, dy=%d)",
                         dx, dy);
                set_week_offset(g_week_offset + 1, "swipe");
            } else {
                ESP_LOGI("FamilyCalendar",
                         "Calendar distance swipe right accepted (dx=%d, dy=%d)",
                         dx, dy);
                set_week_offset(g_week_offset - 1, "swipe");
            }
        }

        g_calendar_swipe_tracking = false;
    }

    g_calendar_swipe_prev_pressed = pressed;
}

void today_cb(lv_event_t *) {
    set_week_offset(0, "today");
}

void sync_now_cb(lv_event_t *) {
    home_assistant_request_sync();
    if (g_status_label) set_label_text(g_status_label, "HA refresh requested...");
}

void create_calendar_dashboard() {
    lv_obj_t *toolbar = lv_obj_create(g_screen);
    lv_obj_set_size(toolbar, SCREEN_W, CAL_TOOLBAR_H);
    lv_obj_set_pos(toolbar, 0, HEADER_H);
    style_box(toolbar, theme().panel, 0, 0);

    lv_obj_t *caption = label(toolbar, "SHOW", &lv_font_montserrat_12, theme().muted);
    lv_obj_set_pos(caption, 18, 23);

    int x = 70;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *chip = lv_button_create(toolbar);
        lv_obj_set_size(chip, 120, 40);
        lv_obj_set_pos(chip, x, 11);
        style_box(chip, g_person_visible[i] ? CALENDAR_PEOPLE[i].color : theme().button, 18, 0);
        lv_obj_t *txt = label(
            chip,
            CALENDAR_PEOPLE[i].name,
            &lv_font_montserrat_14,
            g_person_visible[i] ? 0xFFFFFF : theme().muted,
            106);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
        lv_obj_center(txt);
        lv_obj_add_event_cb(chip, filter_clicked_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        g_filter_buttons[i] = chip;
        x += 128;
    }

    g_week_label = label(toolbar, "", &lv_font_montserrat_16, theme().text, 220);
    lv_obj_set_pos(g_week_label, 600, 21);

    lv_obj_t *prev = button(toolbar, "<", 42, 38);
    lv_obj_set_pos(prev, 830, 12);
    lv_obj_add_event_cb(prev, shift_week_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(-1)));

    lv_obj_t *today = button(toolbar, "Today", 82, 38);
    lv_obj_set_pos(today, 882, 12);
    lv_obj_add_event_cb(today, today_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *next = button(toolbar, ">", 42, 38);
    lv_obj_set_pos(next, 972, 12);
    lv_obj_add_event_cb(next, shift_week_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(1)));

    lv_obj_t *side = lv_obj_create(g_screen);
    lv_obj_set_size(side, CAL_SIDE_W - 8, CAL_MAIN_H);
    lv_obj_set_pos(side, 8, CAL_MAIN_Y);
    style_box(side, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(side, 14, LV_PART_MAIN);

    lv_obj_t *today_title = label(side, "TODAY", &lv_font_montserrat_14, theme().muted);
    lv_obj_set_pos(today_title, 0, 0);
    g_summary_label = label(side, "Calendar starting...", &lv_font_montserrat_18, theme().text, 174);
    lv_label_set_long_mode(g_summary_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_summary_label, 0, 30);

    lv_obj_t *line = lv_obj_create(side);
    lv_obj_set_size(line, 174, 1);
    lv_obj_set_pos(line, 0, 122);
    lv_obj_set_style_bg_color(line, lv_color_hex(theme().border), LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);

    lv_obj_t *integration = label(side, "HOME ASSISTANT", &lv_font_montserrat_14, theme().muted);
    lv_obj_set_pos(integration, 0, 146);
    g_status_label = label(side, "", &lv_font_montserrat_12, theme().muted, 174);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_status_label, 126);
    lv_obj_set_pos(g_status_label, 0, 176);

    lv_obj_t *sync_side = button(side, "Refresh now", 174, 42, false, &lv_font_montserrat_14);
    lv_obj_set_pos(sync_side, 0, 318);
    lv_obj_add_event_cb(sync_side, sync_now_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *refresh_note = label(side, "Auto-refresh while visible: 5 min", &lv_font_montserrat_12, theme().muted, 190);
    lv_obj_set_pos(refresh_note, 0, 370);

    lv_obj_t *swipe_note = label(side, "Swipe week left/right", &lv_font_montserrat_12, theme().muted, 190);
    lv_obj_set_pos(swipe_note, 0, 394);

    for (int day = 0; day < 7; ++day) {
        lv_obj_t *column = lv_obj_create(g_screen);
        lv_obj_set_size(column, DAY_W, CAL_MAIN_H);
        lv_obj_set_pos(column, CAL_X + day * (DAY_W + DAY_GAP), CAL_MAIN_Y);
        style_box(column, theme().panel, 12, 1);
        g_day_columns[day] = column;
    }

    render_week();
}

void chore_toggle_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0 || static_cast<size_t>(index) >= chore_service_count()) return;

    const size_t i = static_cast<size_t>(index);
    const bool new_done = !chore_service_done(i);
    const char *uid = chore_service_uid(i);
    if (!home_assistant_queue_chore_status(uid, new_done)) {
        show_info_overlay("Home Assistant chores",
                          "The chore update could not be queued. Check the Home Assistant connection and try again.");
        return;
    }

    /* Optimistic RAM update: the card changes immediately while HA updates in the background. */
    chore_service_set_done(i, new_done);
    request_rebuild();
}

void chore_reset_cb(lv_event_t *) {
    size_t queued = 0;
    for (size_t i = 0; i < chore_service_count(); ++i) {
        if (!chore_service_done(i)) continue;
        if (home_assistant_queue_chore_status(chore_service_uid(i), false)) {
            chore_service_set_done(i, false);
            ++queued;
        }
    }
    if (queued > 0) request_rebuild();
}

void chore_refresh_cb(lv_event_t *) {
    home_assistant_request_chore_sync();
}

void chore_change_list_cb(lv_event_t *) {
    g_chore_choose_list = true;
    home_assistant_request_todo_discovery();
    request_rebuild();
}

void chore_cancel_list_cb(lv_event_t *) {
    g_chore_choose_list = false;
    request_rebuild();
}

void chore_select_list_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0 || static_cast<size_t>(index) >= home_assistant_todo_list_count()) return;
    const char *entity = home_assistant_todo_list_entity(static_cast<size_t>(index));
    if (!entity || !entity[0]) return;
    chore_service_set_entity(entity);
    g_chore_choose_list = false;
    home_assistant_request_chore_sync();
    request_rebuild();
}

void create_chore_list_picker(lv_obj_t *page) {
    home_assistant_request_todo_discovery();

    lv_obj_t *title = label(page, "Choose a Home Assistant chore list", &lv_font_montserrat_24,
                            theme().text, 620);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *description = label(
        page,
        "Select the Home Assistant todo.* entity that should back this dashboard. The selection is saved on the display.",
        &lv_font_montserrat_14,
        theme().muted,
        900);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(description, 0, 40);

    if (chore_service_configured()) {
        lv_obj_t *cancel = button(page, "Back", 100, 40, false, &lv_font_montserrat_14);
        lv_obj_set_pos(cancel, 1110, 8);
        lv_obj_add_event_cb(cancel, chore_cancel_list_cb, LV_EVENT_CLICKED, nullptr);
    }

    if (!home_assistant_todo_lists_ready()) {
        lv_obj_t *loading = label(page, "Discovering Home Assistant to-do lists in the background...",
                                  &lv_font_montserrat_18, theme().muted, 760);
        lv_obj_set_pos(loading, 0, 120);
        return;
    }

    const size_t count = home_assistant_todo_list_count();
    if (count == 0) {
        lv_obj_t *empty = label(
            page,
            "No todo.* entities were found. Create a To-do list in Home Assistant, then reboot or return here to discover it.",
            &lv_font_montserrat_18,
            theme().muted,
            980);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(empty, 0, 120);
        return;
    }

    constexpr int CARD_W = 580;
    constexpr int CARD_H = 84;
    constexpr int GAP_X = 28;
    constexpr int GAP_Y = 14;
    constexpr int START_Y = 104;

    for (size_t i = 0; i < count; ++i) {
        const int column = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        const int x = column * (CARD_W + GAP_X);
        const int y = START_Y + row * (CARD_H + GAP_Y);

        lv_obj_t *card = lv_button_create(page);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, x, y);
        style_box(card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);

        lv_obj_t *name = label(card, home_assistant_todo_list_name(i), &lv_font_montserrat_18,
                               theme().text, CARD_W - 28);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 0, 4);

        lv_obj_t *entity = label(card, home_assistant_todo_list_entity(i), &lv_font_montserrat_12,
                                 theme().muted, CARD_W - 28);
        lv_label_set_long_mode(entity, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(entity, 0, 42);

        lv_obj_add_event_cb(card, chore_select_list_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
}

void create_chores_dashboard() {
    lv_obj_t *page = lv_obj_create(g_screen);
    lv_obj_set_size(page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(page, 8, PAGE_Y);
    style_box(page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(page, 18, LV_PART_MAIN);

    if (!chore_service_configured() || g_chore_choose_list) {
        /* Picker can extend below one screen when many todo entities exist. */
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        create_chore_list_picker(page);
        return;
    }

    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    lv_obj_t *title = label(page, "Home Assistant chores", &lv_font_montserrat_24, theme().text, 420);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *source = label(page, chore_service_entity(), &lv_font_montserrat_12, theme().muted, 560);
    lv_label_set_long_mode(source, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(source, 0, 40);

    const size_t count = chore_service_count();
    const size_t completed = chore_service_completed_count();
    char progress_text[96];
    snprintf(progress_text, sizeof(progress_text), "%u of %u complete  |  auto-refreshes every 1 min while visible",
             static_cast<unsigned>(completed), static_cast<unsigned>(count));
    lv_obj_t *progress_label = label(page, progress_text, &lv_font_montserrat_14, theme().muted, 650);
    lv_obj_set_pos(progress_label, 0, 68);

    lv_obj_t *progress = lv_bar_create(page);
    lv_obj_set_size(progress, 500, 14);
    lv_obj_set_pos(progress, 0, 96);
    lv_bar_set_range(progress, 0, count > 0 ? static_cast<int32_t>(count) : 1);
    lv_bar_set_value(progress, static_cast<int32_t>(completed), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress, lv_color_hex(theme().button), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress, lv_color_hex(theme().accent), LV_PART_INDICATOR);

    lv_obj_t *reset = button(page, "Reset all", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(reset, 850, 18);
    lv_obj_add_event_cb(reset, chore_reset_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *refresh = button(page, "Refresh", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(refresh, 972, 18);
    lv_obj_add_event_cb(refresh, chore_refresh_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *change = button(page, "Change list", 132, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(change, 1094, 18);
    lv_obj_add_event_cb(change, chore_change_list_cb, LV_EVENT_CLICKED, nullptr);

    if (count == 0) {
        lv_obj_t *empty = label(
            page,
            "No items are loaded from this Home Assistant to-do list yet. Tap Refresh if you just selected it.",
            &lv_font_montserrat_18,
            theme().muted,
            950);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(empty, 0, 156);
        return;
    }

    constexpr int CARD_W = 580;
    constexpr int CARD_H = 96;
    constexpr int GAP_X = 28;
    constexpr int GAP_Y = 14;
    constexpr int START_Y = 136;

    for (size_t i = 0; i < count; ++i) {
        const int column = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        const int x = column * (CARD_W + GAP_X);
        const int y = START_Y + row * (CARD_H + GAP_Y);
        const bool done = chore_service_done(i);
        const ChoreItem *item = chore_service_item(i);

        lv_obj_t *card = lv_button_create(page);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, x, y);
        style_box(card, done ? theme().accent_soft : theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);

        char text[160];
        snprintf(text, sizeof(text), "%s  %s", done ? "[x]" : "[ ]", chore_service_name(i));
        lv_obj_t *task = label(card, text, &lv_font_montserrat_18,
                               done ? theme().accent : theme().text, CARD_W - 32);
        lv_label_set_long_mode(task, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(task, 0, 8);

        const char *detail = (item && item->description[0])
                                 ? item->description
                                 : (done ? "Tap to mark incomplete" : "Tap to complete");
        lv_obj_t *hint = label(card, detail, &lv_font_montserrat_12, theme().muted, CARD_W - 32);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(hint, 0, 52);

        lv_obj_add_event_cb(card, chore_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
}

bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    const size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower(static_cast<unsigned char>(p[i])) ==
                   tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

bool is_meal_event(const CalendarEvent &event) {
    static const char *keywords[] = {
        "breakfast", "brunch", "lunch", "dinner", "supper", "meal", "restaurant", "cookout"
    };
    for (const char *keyword : keywords) {
        if (contains_case_insensitive(event.title, keyword)) return true;
    }
    return false;
}

void create_meal_day_card(lv_obj_t *page, int day, time_t day_time, int x, int y, int w, int h) {
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    style_box(card, theme().panel_alt, 12, 1);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);

    struct tm day_tm = {};
    localtime_r(&day_time, &day_tm);
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    char heading[40];
    snprintf(heading, sizeof(heading), "%s  |  %s %d",
             calendar_day_name(static_cast<uint8_t>(day)), months[day_tm.tm_mon], day_tm.tm_mday);
    lv_obj_t *day_label = label(card, heading, &lv_font_montserrat_16, theme().text, w - 32);
    lv_obj_set_pos(day_label, 0, 0);

    int meal_count = 0;
    int text_y = 40;
    const CalendarEvent *first_event = nullptr;
    for (size_t i = 0; i < CALENDAR_EVENT_COUNT && meal_count < 3; ++i) {
        const CalendarEvent &event = CALENDAR_EVENTS[i];
        if (!calendar_event_is_on_day(event, day_time) || !is_meal_event(event)) continue;
        if (!first_event) first_event = &event;

        char time_text[22];
        calendar_format_event_time(event, time_text, sizeof(time_text));
        char line[120];
        snprintf(line, sizeof(line), "%s  %s", time_text, event.title);
        lv_obj_t *meal = label(card, line, &lv_font_montserrat_14, theme().text, w - 32);
        lv_label_set_long_mode(meal, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(meal, 48);
        lv_obj_set_pos(meal, 0, text_y);
        text_y += 52;
        ++meal_count;
    }

    if (meal_count == 0) {
        lv_obj_t *empty = label(card, "No meal event on the calendar", &lv_font_montserrat_14,
                                theme().muted, w - 32);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(empty, 0, 52);
    } else if (first_event) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, event_clicked_cb, LV_EVENT_CLICKED, const_cast<CalendarEvent *>(first_event));
        lv_obj_t *hint = label(card, "Tap for details", &lv_font_montserrat_12, theme().muted, w - 32);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

void create_meals_dashboard() {
    lv_obj_t *page = lv_obj_create(g_screen);
    lv_obj_set_size(page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(page, 8, PAGE_Y);
    style_box(page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(page, 16, LV_PART_MAIN);

    lv_obj_t *title = label(page, "Meals", &lv_font_montserrat_24, theme().text, 260);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *description = label(
        page,
        "Automatically finds meal-like events in the current Home Assistant calendar week.  Calendar data refreshes every 5 min while visible.",
        &lv_font_montserrat_14,
        theme().muted,
        760);
    lv_obj_set_pos(description, 0, 38);

    lv_obj_t *keywords = label(
        page,
        "Looks for breakfast, brunch, lunch, dinner, supper, meal, restaurant, or cookout.",
        &lv_font_montserrat_12,
        theme().muted,
        780);
    lv_obj_set_pos(keywords, 0, 64);

    time_t week_start = time_service_start_of_week(time_service_now());
    week_start = time_service_add_days(week_start, g_week_offset * 7);
    constexpr int GAP = 12;
    constexpr int CARD_W = 292;
    constexpr int CARD_H = 242;
    constexpr int ROW1_Y = 100;
    constexpr int ROW2_Y = ROW1_Y + CARD_H + GAP;

    for (int day = 0; day < 7; ++day) {
        int x = 0;
        int y = 0;
        if (day < 4) {
            x = day * (CARD_W + GAP);
            y = ROW1_Y;
        } else {
            constexpr int ROW2_W = CARD_W * 3 + GAP * 2;
            const int row2_start = (SCREEN_W - 48 - ROW2_W) / 2;
            x = row2_start + (day - 4) * (CARD_W + GAP);
            y = ROW2_Y;
        }
        create_meal_day_card(page, day, time_service_add_days(week_start, day), x, y, CARD_W, CARD_H);
    }
}

void weather_refresh_cb(lv_event_t *) {
    weather_service_request_refresh(false, "manual weather refresh");
    request_rebuild();
}

void create_weather_dashboard() {
    lv_obj_t *page = lv_obj_create(g_screen);
    if (!page) return;
    lv_obj_set_size(page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(page, 8, PAGE_Y);
    style_box(page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(page, 16, LV_PART_MAIN);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);

    weather_service_get_snapshot(g_weather_snapshot);
    const WeatherSnapshot &weather = g_weather_snapshot;

    lv_obj_t *title = label(page, "Weather", &lv_font_montserrat_24, theme().text, 300);
    set_pos_if(title, 0, 0);

    lv_obj_t *subtitle = label(
        page,
        "Forecast source: Home Assistant weather entity",
        &lv_font_montserrat_12,
        theme().muted,
        520);
    set_pos_if(subtitle, 0, 36);

    lv_obj_t *refresh = button(page, "Refresh", 108, 38, false, &lv_font_montserrat_14);
    set_pos_if(refresh, 1120, 6);
    if (refresh) lv_obj_add_event_cb(refresh, weather_refresh_cb, LV_EVENT_CLICKED, nullptr);

    if (!weather.configured) {
        lv_obj_t *card = lv_obj_create(page);
        if (!card) return;
        lv_obj_set_size(card, 1228, 220);
        lv_obj_set_pos(card, 0, 76);
        style_box(card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);

        lv_obj_t *heading = label(card, "Weather is not configured", &lv_font_montserrat_24, theme().text, 700);
        set_pos_if(heading, 0, 0);
        lv_obj_t *body = label(
            card,
            "Add HA_WEATHER_ENTITY to include/app_local.h and point it at a weather.* entity.\n"
            "That Home Assistant weather entity is the only weather data source used by this firmware.",
            &lv_font_montserrat_16,
            theme().muted,
            1160);
        if (body) lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        set_height_if(body, 120);
        set_pos_if(body, 0, 58);
        return;
    }

    if (!weather.has_data && !weather.in_progress) {
        weather_service_request_refresh(false, "weather tab open");
    }

    if (!weather.has_data) {
        lv_obj_t *loading = label(page,
                                  weather.in_progress ? "Loading forecast..." : "Forecast queued...",
                                  &lv_font_montserrat_20,
                                  theme().muted,
                                  420);
        set_pos_if(loading, 0, 110);
        return;
    }

    lv_obj_t *current = lv_obj_create(page);
    if (!current) return;
    lv_obj_set_size(current, 1228, 192);
    lv_obj_set_pos(current, 0, 72);
    style_box(current, theme().panel_alt, 14, 1);
    lv_obj_set_style_pad_all(current, 18, LV_PART_MAIN);

    lv_obj_t *current_icon = create_weather_icon(current,
                                                 weather.current.condition,
                                                 78,
                                                 theme().text,
                                                 theme().accent);
    set_pos_if(current_icon, 10, 24);

    char temp_text[24];
    snprintf(temp_text, sizeof(temp_text), "%d %s",
             rounded_display_temp(weather.current.temperature_c),
             weather_temperature_unit());
    lv_obj_t *current_temp = label(current, temp_text, &lv_font_montserrat_28, theme().text, 200);
    set_pos_if(current_temp, 110, 24);

    lv_obj_t *condition = label(current,
                                weather_condition_label(weather.current.condition),
                                &lv_font_montserrat_16,
                                theme().muted,
                                220);
    set_pos_if(condition, 112, 66);

    char hi_low[48];
    snprintf(hi_low, sizeof(hi_low), "Today %d/%d%s",
             rounded_display_temp(weather.current.today_high_c),
             rounded_display_temp(weather.current.today_low_c),
             weather_temperature_unit());
    lv_obj_t *daily_range = label(current, hi_low, &lv_font_montserrat_16, theme().text, 220);
    set_pos_if(daily_range, 112, 94);

    char humidity_text[48];
    if (weather.current.humidity_pct >= 0) {
        snprintf(humidity_text, sizeof(humidity_text), "Humidity %d%%", weather.current.humidity_pct);
    } else {
        snprintf(humidity_text, sizeof(humidity_text), "Humidity --");
    }
    lv_obj_t *humidity = label(current, humidity_text, &lv_font_montserrat_16, theme().text, 210);
    set_pos_if(humidity, 430, 26);

    char wind_text[64];
    snprintf(wind_text, sizeof(wind_text), "Wind %.1f %s %s",
             weather_display_wind(weather.current.wind_mps),
             weather_wind_unit(),
             wind_direction_text(weather.current.wind_direction_deg));
    lv_obj_t *wind = label(current, wind_text, &lv_font_montserrat_16, theme().text, 270);
    set_pos_if(wind, 430, 58);

    char precip_text[72];
    if (weather.current.precipitation_probability_pct >= 0) {
        snprintf(precip_text, sizeof(precip_text), "Precip %.2f %s (%d%%)",
                 weather_display_precip(weather.current.precipitation_mm),
                 weather_precip_unit(),
                 weather.current.precipitation_probability_pct);
    } else {
        snprintf(precip_text, sizeof(precip_text), "Precip %.2f %s",
                 weather_display_precip(weather.current.precipitation_mm),
                 weather_precip_unit());
    }
    lv_obj_t *precip = label(current, precip_text, &lv_font_montserrat_16, theme().text, 300);
    set_pos_if(precip, 430, 90);

    char pressure_text[48];
    if (!isnan(weather.current.pressure_hpa)) {
        snprintf(pressure_text, sizeof(pressure_text), "Pressure %.0f hPa", weather.current.pressure_hpa);
    } else {
        snprintf(pressure_text, sizeof(pressure_text), "Pressure --");
    }
    lv_obj_t *pressure = label(current, pressure_text, &lv_font_montserrat_16, theme().text, 210);
    set_pos_if(pressure, 430, 122);

    char updated_line[128];
    char updated_time[24] = "--";
    if (weather.last_updated_epoch > 0) {
        time_service_format_time(weather.last_updated_epoch, updated_time, sizeof(updated_time));
    }
    const uint32_t now_ms = millis();
    uint32_t remaining_ms = 0;
    if (static_cast<int32_t>(weather.next_refresh_ms - now_ms) > 0) {
        remaining_ms = weather.next_refresh_ms - now_ms;
    }
    snprintf(updated_line, sizeof(updated_line), "Updated %s  |  Next refresh ~%lum",
             updated_time, static_cast<unsigned long>(remaining_ms / 60000UL));
    lv_obj_t *updated = label(current, updated_line, &lv_font_montserrat_12, theme().muted, 420);
    set_pos_if(updated, 780, 132);

    lv_obj_t *daily_heading = label(page, "Daily forecast", &lv_font_montserrat_18, theme().text, 240);
    set_pos_if(daily_heading, 0, 280);

    lv_obj_t *daily_card = lv_obj_create(page);
    if (!daily_card) return;
    lv_obj_set_size(daily_card, 1228, 164);
    lv_obj_set_pos(daily_card, 0, 314);
    style_box(daily_card, theme().panel_alt, 10, 1);
    lv_obj_set_style_pad_all(daily_card, 12, LV_PART_MAIN);

    char daily_text[512];
    daily_text[0] = '\0';
    size_t daily_used = 0;
    const size_t daily_rows = weather.daily_count < 4 ? weather.daily_count : 4;
    static const char *short_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (size_t i = 0; i < daily_rows; ++i) {
        const WeatherDayForecast &day = weather.daily[i];
        char day_name[12] = "Day";
        struct tm day_tm = {};
        if (localtime_r(&day.local_day_epoch, &day_tm) && day_tm.tm_wday >= 0 && day_tm.tm_wday <= 6) {
            snprintf(day_name, sizeof(day_name), "%s", short_days[day_tm.tm_wday]);
        }

        char row[160];
        if (day.precipitation_probability_pct >= 0) {
            snprintf(row, sizeof(row), "%s  %d/%d%s  %s  %d%%  %.1f %s\n",
                     day_name,
                     rounded_display_temp(day.high_c),
                     rounded_display_temp(day.low_c),
                     weather_temperature_unit(),
                     weather_condition_label(day.condition),
                     day.precipitation_probability_pct,
                     weather_display_wind(day.wind_max_mps),
                     weather_wind_unit());
        } else {
            snprintf(row, sizeof(row), "%s  %d/%d%s  %s  %.1f %s\n",
                     day_name,
                     rounded_display_temp(day.high_c),
                     rounded_display_temp(day.low_c),
                     weather_temperature_unit(),
                     weather_condition_label(day.condition),
                     weather_display_wind(day.wind_max_mps),
                     weather_wind_unit());
        }

        const int written = snprintf(daily_text + daily_used,
                                     sizeof(daily_text) - daily_used,
                                     "%s",
                                     row);
        if (written <= 0) break;
        const size_t remaining = sizeof(daily_text) - daily_used;
        if (static_cast<size_t>(written) >= remaining) {
            daily_used = sizeof(daily_text) - 1;
            break;
        }
        daily_used += static_cast<size_t>(written);
    }
    if (daily_used == 0) {
        snprintf(daily_text, sizeof(daily_text), "No daily forecast data available yet.");
    }

    lv_obj_t *daily_label = label(daily_card, daily_text, &lv_font_montserrat_14, theme().text, 1200);
    if (daily_label) lv_label_set_long_mode(daily_label, LV_LABEL_LONG_WRAP);
    set_height_if(daily_label, 140);
    set_pos_if(daily_label, 0, 0);

    lv_obj_t *hourly_heading = label(page, "Hourly forecast", &lv_font_montserrat_18, theme().text, 220);
    set_pos_if(hourly_heading, 0, 494);

    lv_obj_t *hourly_card = lv_obj_create(page);
    if (!hourly_card) return;
    lv_obj_set_size(hourly_card, 1228, 196);
    lv_obj_set_pos(hourly_card, 0, 526);
    style_box(hourly_card, theme().panel_alt, 10, 1);
    lv_obj_set_style_pad_all(hourly_card, 12, LV_PART_MAIN);

    char hourly_text[640];
    hourly_text[0] = '\0';
    size_t hourly_used = 0;
    size_t hourly_rows = weather.hourly_count < WEATHER_MAX_HOURLY_POINTS
        ? weather.hourly_count
        : WEATHER_MAX_HOURLY_POINTS;
    if (hourly_rows > 8) hourly_rows = 8;
    for (size_t i = 0; i < hourly_rows; ++i) {
        const WeatherHourForecast &hour = weather.hourly[i];
        char hhmm[16];
        time_service_format_time(hour.epoch, hhmm, sizeof(hhmm));

        char row[140];
        if (hour.precipitation_probability_pct >= 0) {
            snprintf(row, sizeof(row), "%s  %d%s  %s  rain %d%%  wind %.1f %s\n",
                     hhmm,
                     rounded_display_temp(hour.temperature_c),
                     weather_temperature_unit(),
                     weather_condition_label(hour.condition),
                     hour.precipitation_probability_pct,
                     weather_display_wind(hour.wind_mps),
                     weather_wind_unit());
        } else {
            snprintf(row, sizeof(row), "%s  %d%s  %s  wind %.1f %s\n",
                     hhmm,
                     rounded_display_temp(hour.temperature_c),
                     weather_temperature_unit(),
                     weather_condition_label(hour.condition),
                     weather_display_wind(hour.wind_mps),
                     weather_wind_unit());
        }

        const int written = snprintf(hourly_text + hourly_used,
                                     sizeof(hourly_text) - hourly_used,
                                     "%s",
                                     row);
        if (written <= 0) break;
        const size_t remaining = sizeof(hourly_text) - hourly_used;
        if (static_cast<size_t>(written) >= remaining) {
            hourly_used = sizeof(hourly_text) - 1;
            break;
        }
        hourly_used += static_cast<size_t>(written);
    }
    if (hourly_used == 0) {
        snprintf(hourly_text, sizeof(hourly_text), "No hourly forecast data available yet.");
    }

    lv_obj_t *hourly_label = label(hourly_card, hourly_text, &lv_font_montserrat_14, theme().text, 1200);
    if (hourly_label) lv_label_set_long_mode(hourly_label, LV_LABEL_LONG_WRAP);
    set_height_if(hourly_label, 172);
    set_pos_if(hourly_label, 0, 0);

    lv_obj_t *attrib = label(page, "Data from Home Assistant", &lv_font_montserrat_12, theme().muted, 260);
    set_pos_if(attrib, 968, 728);

    lv_obj_t *status = label(page, weather.status, &lv_font_montserrat_12, theme().muted, 780);
    set_pos_if(status, 0, 728);
}


void home_sync_cb(lv_event_t *) {
    home_assistant_request_sync();
    show_info_overlay("Home Assistant", "A three-week refresh has been requested for the selected week.\n\nCalendar navigation switches to cache immediately, then a debounced selected +/-1 refresh is queued after week changes settle.");
}


const char *friendly_alarm_state(const char *state) {
    if (!state) return "Unknown";
    if (strcmp(state, "disarmed") == 0) return "Disarmed";
    if (strcmp(state, "armed_home") == 0) return "Armed Home";
    if (strcmp(state, "armed_away") == 0) return "Armed Away";
    if (strcmp(state, "armed_night") == 0) return "Armed Night";
    if (strcmp(state, "armed_vacation") == 0) return "Armed Vacation";
    if (strcmp(state, "armed_custom_bypass") == 0) return "Armed Custom";
    if (strcmp(state, "arming") == 0) return "Arming";
    if (strcmp(state, "pending") == 0) return "Entry Delay";
    if (strcmp(state, "triggered") == 0) return "TRIGGERED";
    if (strcmp(state, "disarming") == 0) return "Disarming";
    return state;
}

uint32_t alarm_state_color(const char *state) {
    if (!state) return theme().muted;
    if (strcmp(state, "disarmed") == 0) return theme().success;
    if (strcmp(state, "triggered") == 0) return g_dark_mode ? 0xF87171 : 0xDC2626;
    if (strcmp(state, "arming") == 0 || strcmp(state, "pending") == 0 || strcmp(state, "disarming") == 0) {
        return g_dark_mode ? 0xFBBF24 : 0xD97706;
    }
    if (strncmp(state, "armed_", 6) == 0) return theme().accent;
    return theme().muted;
}

const char *alarm_action_title(AlarmUiAction action) {
    switch (action) {
        case AlarmUiAction::Disarm: return "Disarm alarm?";
        case AlarmUiAction::ArmHome: return "Arm Home?";
        case AlarmUiAction::ArmAway: return "Arm Away?";
        case AlarmUiAction::ArmNight: return "Arm Night?";
        case AlarmUiAction::ArmVacation: return "Arm Vacation?";
        case AlarmUiAction::SkipDelay: return "Skip exit delay?";
        default: return "Alarm command";
    }
}

const char *alarm_action_mode(AlarmUiAction action) {
    switch (action) {
        case AlarmUiAction::ArmHome: return "home";
        case AlarmUiAction::ArmAway: return "away";
        case AlarmUiAction::ArmNight: return "night";
        case AlarmUiAction::ArmVacation: return "vacation";
        default: return "";
    }
}

void clear_alarm_code() {
    memset(g_alarm_pin, 0, sizeof(g_alarm_pin));
    g_alarm_code_input = nullptr;
}

void alarm_command_cancel_cb(lv_event_t *) {
    clear_alarm_code();
    g_alarm_pending_action = AlarmUiAction::None;
    close_overlay();
}

void alarm_command_confirm_cb(lv_event_t *) {
    if (g_alarm_pending_action == AlarmUiAction::None) return;

    const char *code = "";
    if (g_alarm_code_input) {
        const char *entered = lv_textarea_get_text(g_alarm_code_input);
        if (!entered || !entered[0]) {
            lv_textarea_set_placeholder_text(g_alarm_code_input, "Code required");
            lv_obj_set_style_border_color(g_alarm_code_input, lv_color_hex(0xDC2626), LV_PART_MAIN);
            return;
        }
        snprintf(g_alarm_pin, sizeof(g_alarm_pin), "%s", entered);
        code = g_alarm_pin;
    }

    bool queued = false;
    if (g_alarm_pending_action == AlarmUiAction::Disarm) {
        queued = home_assistant_queue_alarm_disarm(code);
    } else if (g_alarm_pending_action == AlarmUiAction::SkipDelay) {
        queued = home_assistant_queue_alarm_skip_delay();
    } else {
        queued = home_assistant_queue_alarm_arm(alarm_action_mode(g_alarm_pending_action), code);
    }

    clear_alarm_code();
    g_alarm_pending_action = AlarmUiAction::None;
    close_overlay();

    if (!queued) {
        show_info_overlay("Alarmo", "The alarm command could not be queued. Check the Home Assistant connection and try again.");
    } else {
        home_assistant_request_alarm_sync();
    }
}

void show_alarm_command_overlay(AlarmUiAction action) {
    close_overlay();
    clear_alarm_code();
    g_alarm_pending_action = action;

    g_overlay = lv_obj_create(g_screen);
    lv_obj_set_size(g_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(theme().overlay), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    const bool code_required = alarm_service_code_required() && action != AlarmUiAction::SkipDelay;
    const int card_h = code_required ? 650 : 330;
    lv_obj_t *card = lv_obj_create(g_overlay);
    lv_obj_set_size(card, 660, card_h);
    lv_obj_center(card);
    style_box(card, theme().panel, 18, 1);
    lv_obj_set_style_pad_all(card, 26, LV_PART_MAIN);

    lv_obj_t *title = label(card, alarm_action_title(action), &lv_font_montserrat_28, theme().text, 600);
    lv_obj_set_pos(title, 0, 0);

    const AlarmoSnapshot *snapshot = alarm_service_snapshot();
    char detail[360];
    detail[0] = '\0';
    if (action == AlarmUiAction::SkipDelay) {
        snprintf(detail, sizeof(detail),
                 "This ends the Alarmo exit delay immediately.  The alarm will enter its armed state now.");
    } else if (snapshot && snapshot->open_sensor_count > 0 && action != AlarmUiAction::Disarm) {
        snprintf(detail, sizeof(detail),
                 "%u sensor%s currently open: %s\n\nAlarmo will still enforce its configured arming rules.  This dashboard does not force-bypass open sensors.",
                 static_cast<unsigned>(snapshot->open_sensor_count),
                 snapshot->open_sensor_count == 1 ? " is" : "s are",
                 snapshot->open_sensors[0] ? snapshot->open_sensors : "see Home Assistant");
    } else {
        snprintf(detail, sizeof(detail), "Confirm this Alarmo command for %s.", alarm_service_friendly_name());
    }

    lv_obj_t *body = label(card, detail, &lv_font_montserrat_16, theme().muted, 600);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body, code_required ? 120 : 140);
    lv_obj_set_pos(body, 0, 58);

    if (code_required) {
        lv_obj_t *prompt = label(card, "Alarm code", &lv_font_montserrat_16, theme().text, 200);
        lv_obj_set_pos(prompt, 0, 184);

        g_alarm_code_input = lv_textarea_create(card);
        lv_obj_set_size(g_alarm_code_input, 600, 58);
        lv_obj_set_pos(g_alarm_code_input, 0, 214);
        lv_textarea_set_one_line(g_alarm_code_input, true);
        lv_textarea_set_password_mode(g_alarm_code_input, true);
        lv_textarea_set_placeholder_text(g_alarm_code_input, "Enter Alarmo code");
        lv_textarea_set_max_length(g_alarm_code_input, 24);
        lv_obj_set_style_text_font(g_alarm_code_input, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(g_alarm_code_input, lv_color_hex(theme().panel_alt), LV_PART_MAIN);
        lv_obj_set_style_text_color(g_alarm_code_input, lv_color_hex(theme().text), LV_PART_MAIN);
        lv_obj_set_style_border_color(g_alarm_code_input, lv_color_hex(theme().border), LV_PART_MAIN);

        lv_obj_t *keyboard = lv_keyboard_create(card);
        lv_obj_set_size(keyboard, 600, 270);
        lv_obj_set_pos(keyboard, 0, 286);
        lv_keyboard_set_textarea(keyboard, g_alarm_code_input);
        const bool numeric = !snapshot || !snapshot->code_format[0] || strcmp(snapshot->code_format, "text") != 0;
        lv_keyboard_set_mode(keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    }

    lv_obj_t *cancel = button(card, "Cancel", 130, 46, false, &lv_font_montserrat_16);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(cancel, alarm_command_cancel_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *confirm = button(card, "Confirm", 150, 46, true, &lv_font_montserrat_16);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(confirm, alarm_command_confirm_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_move_foreground(g_overlay);
}

void alarm_action_cb(lv_event_t *e) {
    const intptr_t raw = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const AlarmUiAction action = static_cast<AlarmUiAction>(raw);
    if (action == AlarmUiAction::None) return;
    show_alarm_command_overlay(action);
}

void alarm_refresh_cb(lv_event_t *) {
    home_assistant_request_alarm_sync();
}

void alarm_change_panel_cb(lv_event_t *) {
    g_alarm_choose_panel = true;
    home_assistant_request_alarm_discovery();
    request_rebuild();
}

void alarm_cancel_panel_cb(lv_event_t *) {
    g_alarm_choose_panel = false;
    request_rebuild();
}

void alarm_select_panel_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0 || static_cast<size_t>(index) >= home_assistant_alarm_panel_count()) return;
    const char *entity = home_assistant_alarm_panel_entity(static_cast<size_t>(index));
    if (!entity || !entity[0]) return;
    alarm_service_set_entity(entity);
    g_alarm_choose_panel = false;
    home_assistant_request_alarm_sync();
    request_rebuild();
}

void create_alarm_panel_picker(lv_obj_t *page) {
    home_assistant_request_alarm_discovery();

    lv_obj_t *title = label(page, "Choose an Alarmo panel", &lv_font_montserrat_24, theme().text, 620);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *description = label(
        page,
        "Select the Home Assistant alarm_control_panel entity managed by Alarmo.  The selection is saved on this display.",
        &lv_font_montserrat_14,
        theme().muted,
        940);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(description, 0, 40);

    if (alarm_service_configured()) {
        lv_obj_t *back = button(page, "Back", 100, 40, false, &lv_font_montserrat_14);
        lv_obj_set_pos(back, 1110, 8);
        lv_obj_add_event_cb(back, alarm_cancel_panel_cb, LV_EVENT_CLICKED, nullptr);
    }

    if (!home_assistant_alarm_panels_ready()) {
        lv_obj_t *loading = label(page, "Discovering Alarmo panels in Home Assistant...",
                                  &lv_font_montserrat_18, theme().muted, 760);
        lv_obj_set_pos(loading, 0, 120);
        return;
    }

    const size_t count = home_assistant_alarm_panel_count();
    if (count == 0) {
        lv_obj_t *empty = label(page,
            "No Alarmo alarm_control_panel entities were found.  Confirm Alarmo is loaded in Home Assistant, then tap this dashboard again.",
            &lv_font_montserrat_18, theme().muted, 980);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(empty, 0, 120);
        return;
    }

    constexpr int CARD_W = 580;
    constexpr int CARD_H = 84;
    constexpr int GAP_X = 28;
    constexpr int GAP_Y = 14;
    constexpr int START_Y = 104;
    for (size_t i = 0; i < count; ++i) {
        const int column = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        lv_obj_t *card = lv_button_create(page);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_pos(card, column * (CARD_W + GAP_X), START_Y + row * (CARD_H + GAP_Y));
        style_box(card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);

        lv_obj_t *name = label(card, home_assistant_alarm_panel_name(i), &lv_font_montserrat_18,
                               theme().text, CARD_W - 28);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, 0, 4);
        lv_obj_t *entity = label(card, home_assistant_alarm_panel_entity(i), &lv_font_montserrat_12,
                                 theme().muted, CARD_W - 28);
        lv_label_set_long_mode(entity, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(entity, 0, 42);
        lv_obj_add_event_cb(card, alarm_select_panel_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
}

void disable_button(lv_obj_t *btn) {
    if (!btn) return;
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_PART_MAIN);
}

void create_alarm_dashboard() {
    lv_obj_t *page = lv_obj_create(g_screen);
    lv_obj_set_size(page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(page, 8, PAGE_Y);
    style_box(page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(page, 18, LV_PART_MAIN);

    if (!alarm_service_configured() || g_alarm_choose_panel) {
        lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(page, LV_DIR_VER);
        create_alarm_panel_picker(page);
        return;
    }

    const AlarmoSnapshot *snapshot = alarm_service_snapshot();
    if (!snapshot || !snapshot->valid) {
        home_assistant_request_alarm_sync();
        lv_obj_t *title = label(page, "Alarmo", &lv_font_montserrat_24, theme().text, 300);
        lv_obj_set_pos(title, 0, 0);
        lv_obj_t *loading = label(page, "Loading alarm state in the background...", &lv_font_montserrat_18,
                                  theme().muted, 760);
        lv_obj_set_pos(loading, 0, 70);
        lv_obj_t *change = button(page, "Change panel", 140, 40, false, &lv_font_montserrat_14);
        lv_obj_set_pos(change, 1060, 6);
        lv_obj_add_event_cb(change, alarm_change_panel_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }

    lv_obj_t *title = label(page, "Alarmo", &lv_font_montserrat_24, theme().text, 260);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *source = label(page, snapshot->friendly_name, &lv_font_montserrat_14, theme().muted, 500);
    lv_obj_set_pos(source, 0, 38);
    lv_obj_t *auto_note = label(page, "Auto-refresh while visible: 15 sec", &lv_font_montserrat_12, theme().muted, 260);
    lv_obj_set_pos(auto_note, 520, 40);

    lv_obj_t *refresh = button(page, "Refresh", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(refresh, 958, 4);
    lv_obj_add_event_cb(refresh, alarm_refresh_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *change = button(page, "Change panel", 140, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(change, 1082, 4);
    lv_obj_add_event_cb(change, alarm_change_panel_cb, LV_EVENT_CLICKED, nullptr);

    constexpr int STATUS_W = 430;
    constexpr int CONTROLS_X = 450;
    constexpr int CONTROLS_W = 772;
    constexpr int CARD_Y = 78;
    constexpr int CARD_H = 512;

    lv_obj_t *status_card = lv_obj_create(page);
    lv_obj_set_size(status_card, STATUS_W, CARD_H);
    lv_obj_set_pos(status_card, 0, CARD_Y);
    style_box(status_card, theme().panel_alt, 16, 1);
    lv_obj_set_style_pad_all(status_card, 22, LV_PART_MAIN);

    lv_obj_t *state_caption = label(status_card, "STATUS", &lv_font_montserrat_14, theme().muted, STATUS_W - 44);
    lv_obj_set_pos(state_caption, 0, 0);
    lv_obj_t *state_label = label(status_card, friendly_alarm_state(snapshot->state), &lv_font_montserrat_28,
                                  alarm_state_color(snapshot->state), STATUS_W - 44);
    lv_label_set_long_mode(state_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(state_label, 0, 34);

    char transition[120];
    if ((strcmp(snapshot->state, "arming") == 0 || strcmp(snapshot->state, "pending") == 0) && snapshot->delay_seconds > 0) {
        snprintf(transition, sizeof(transition), "%s in about %d seconds",
                 snapshot->next_state[0] ? friendly_alarm_state(snapshot->next_state) : "Next state",
                 snapshot->delay_seconds);
    } else if (snapshot->next_state[0] && strcmp(snapshot->next_state, snapshot->state) != 0) {
        snprintf(transition, sizeof(transition), "Next: %s", friendly_alarm_state(snapshot->next_state));
    } else {
        snprintf(transition, sizeof(transition), "State synchronized with Home Assistant");
    }
    lv_obj_t *next = label(status_card, transition, &lv_font_montserrat_14, theme().muted, STATUS_W - 44);
    lv_label_set_long_mode(next, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(next, 48);
    lv_obj_set_pos(next, 0, 88);

    lv_obj_t *sensor_caption = label(status_card, "OPEN SENSORS", &lv_font_montserrat_14, theme().muted, STATUS_W - 44);
    lv_obj_set_pos(sensor_caption, 0, 158);
    char sensors[380];
    if (snapshot->open_sensor_count == 0) {
        snprintf(sensors, sizeof(sensors), "None reported");
    } else {
        snprintf(sensors, sizeof(sensors), "%u open\n%s",
                 static_cast<unsigned>(snapshot->open_sensor_count),
                 snapshot->open_sensors[0] ? snapshot->open_sensors : "See Home Assistant");
    }
    lv_obj_t *sensor_text = label(status_card, sensors, &lv_font_montserrat_16,
                                  snapshot->open_sensor_count ? (g_dark_mode ? 0xFBBF24 : 0xB45309) : theme().text,
                                  STATUS_W - 44);
    lv_label_set_long_mode(sensor_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(sensor_text, 120);
    lv_obj_set_pos(sensor_text, 0, 190);

    lv_obj_t *last_caption = label(status_card, "LAST TRIGGERED", &lv_font_montserrat_14, theme().muted, STATUS_W - 44);
    lv_obj_set_pos(last_caption, 0, 336);
    lv_obj_t *last = label(status_card, snapshot->last_triggered[0] ? snapshot->last_triggered : "No trigger recorded",
                           &lv_font_montserrat_16, theme().text, STATUS_W - 44);
    lv_obj_set_pos(last, 0, 368);

    const char *code_text = alarm_service_code_required()
        ? "PIN/code required for the next command"
        : "No code required for the next command";
    lv_obj_t *code_label = label(status_card, code_text, &lv_font_montserrat_12, theme().muted, STATUS_W - 44);
    lv_label_set_long_mode(code_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(code_label, 0, 430);

    lv_obj_t *controls = lv_obj_create(page);
    lv_obj_set_size(controls, CONTROLS_W, CARD_H);
    lv_obj_set_pos(controls, CONTROLS_X, CARD_Y);
    style_box(controls, theme().panel_alt, 16, 1);
    lv_obj_set_style_pad_all(controls, 22, LV_PART_MAIN);

    lv_obj_t *control_title = label(controls, "ALARM CONTROLS", &lv_font_montserrat_14, theme().muted, CONTROLS_W - 44);
    lv_obj_set_pos(control_title, 0, 0);
    lv_obj_t *hint = label(controls,
        "Commands are sent to Alarmo in the background.  Open sensors are never force-bypassed from this screen.",
        &lv_font_montserrat_14, theme().muted, CONTROLS_W - 44);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(hint, 48);
    lv_obj_set_pos(hint, 0, 30);

    constexpr int BW = 338;
    constexpr int BH = 74;
    constexpr int GX = 18;
    constexpr int BY = 104;
    constexpr int GY = 18;

    lv_obj_t *disarm = button(controls, "Disarm", BW, BH,
                              strcmp(snapshot->state, "disarmed") != 0, &lv_font_montserrat_20);
    lv_obj_set_pos(disarm, 0, BY);
    lv_obj_add_event_cb(disarm, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::Disarm)));
    if (strcmp(snapshot->state, "disarmed") == 0) disable_button(disarm);

    lv_obj_t *home = button(controls, "Arm Home", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(home, BW + GX, BY);
    lv_obj_add_event_cb(home, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmHome)));
    if (!(snapshot->supported_features & 1U)) disable_button(home);

    lv_obj_t *away = button(controls, "Arm Away", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(away, 0, BY + BH + GY);
    lv_obj_add_event_cb(away, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmAway)));
    if (!(snapshot->supported_features & 2U)) disable_button(away);

    lv_obj_t *night = button(controls, "Arm Night", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(night, BW + GX, BY + BH + GY);
    lv_obj_add_event_cb(night, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmNight)));
    if (!(snapshot->supported_features & 4U)) disable_button(night);

    lv_obj_t *vacation = button(controls, "Arm Vacation", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(vacation, 0, BY + (BH + GY) * 2);
    lv_obj_add_event_cb(vacation, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmVacation)));
    if (!(snapshot->supported_features & 32U)) disable_button(vacation);

    if (strcmp(snapshot->state, "arming") == 0) {
        lv_obj_t *skip = button(controls, "Skip exit delay", BW, BH, true, &lv_font_montserrat_18);
        lv_obj_set_pos(skip, BW + GX, BY + (BH + GY) * 2);
        lv_obj_add_event_cb(skip, alarm_action_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::SkipDelay)));
    } else {
        lv_obj_t *info = label(controls,
            strcmp(snapshot->state, "pending") == 0
                ? "Entry delay active - disarm to cancel the pending alarm."
                : "Exit-delay control appears here while Alarmo is arming.",
            &lv_font_montserrat_14, theme().muted, BW);
        lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(info, BH);
        lv_obj_set_pos(info, BW + GX, BY + (BH + GY) * 2 + 10);
    }

    lv_obj_t *entity = label(controls, alarm_service_entity(), &lv_font_montserrat_12, theme().muted, CONTROLS_W - 44);
    lv_label_set_long_mode(entity, LV_LABEL_LONG_DOT);
    lv_obj_align(entity, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

lv_obj_t *create_info_card(lv_obj_t *page,
                           const char *heading,
                           int x,
                           int y,
                           int w,
                           int h,
                           lv_obj_t **body_out) {
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    style_box(card, theme().panel_alt, 12, 1);
    lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);

    lv_obj_t *heading_label = label(card, heading, &lv_font_montserrat_16, theme().muted, w - 36);
    lv_obj_set_pos(heading_label, 0, 0);

    lv_obj_t *body = label(card, "", &lv_font_montserrat_18, theme().text, w - 36);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(body, h - 62);
    lv_obj_set_pos(body, 0, 36);
    if (body_out) *body_out = body;
    return card;
}

void update_home_dashboard() {
    if (g_dashboard != Dashboard::Settings) return;

    char buffer[420];
    if (network_service_connected()) {
        const String ip = network_service_ip();
        snprintf(buffer, sizeof(buffer), "Connected\n%s\nRSSI %d dBm",
                 ip.c_str(), network_service_rssi());
    } else {
        snprintf(buffer, sizeof(buffer), "%s", network_service_status());
    }
    set_label_text(g_home_network, buffer);

    snprintf(buffer, sizeof(buffer), "%s\nAlarmo: %s", home_assistant_status(),
             alarm_service_configured() ? friendly_alarm_state(alarm_service_state()) : "not selected");
    set_label_text(g_home_ha, buffer);

    const size_t today_events = calendar_count_events_on_day(time_service_now());
    snprintf(buffer, sizeof(buffer),
             "%u events in active week\n%u today\n%u/%u weeks cached in PSRAM\n"
             "Auto-refresh: active tab only; calendar nav settles then refreshes +/-1",
             static_cast<unsigned>(CALENDAR_EVENT_COUNT),
             static_cast<unsigned>(today_events),
             static_cast<unsigned>(calendar_cache_valid_weeks()),
             static_cast<unsigned>(calendar_cache_capacity_weeks()));
    set_label_text(g_home_calendar, buffer);

    const uint32_t uptime_s = millis() / 1000UL;
    const uint32_t hours = uptime_s / 3600UL;
    const uint32_t minutes = (uptime_s % 3600UL) / 60UL;
    snprintf(buffer, sizeof(buffer),
             "Brightness %u%%  |  %s theme\nScreen timeout: %s\nWake: touch only (HA polling disabled)\nTime: %s  |  Uptime %luh %lum",
             g_backlight,
             g_dark_mode ? "Dark" : "Light",
             screen_timeout_text(g_screen_timeout_seconds),
             time_service_has_network_time() ? "NTP" : "build fallback",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
    set_label_text(g_home_system, buffer);
}

const char *screen_timeout_text(uint32_t seconds) {
    switch (seconds) {
        case 0: return "Off";
        case 30: return "30 sec";
        case 60: return "1 min";
        case 120: return "2 min";
        case 300: return "5 min";
        case 600: return "10 min";
        default: return "Custom";
    }
}

void screen_timeout_cycle_cb(lv_event_t *) {
    static const uint32_t choices[] = {0, 30, 60, 120, 300, 600};
    size_t current = 0;
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        if (choices[i] == g_screen_timeout_seconds) {
            current = i;
            break;
        }
    }
    g_screen_timeout_seconds = choices[(current + 1) % (sizeof(choices) / sizeof(choices[0]))];
    g_last_user_activity_ms = millis();
    persist_ui_state();
    request_rebuild();
}

const char *selected_wake_sensor_name() {
    const char *selected = camera_service_selected_wake_sensor();
    if (!selected || !selected[0]) return "Off";
    for (size_t i = 0; i < camera_service_wake_sensor_count(); ++i) {
        if (strcmp(selected, camera_service_wake_sensor_entity(i)) == 0) {
            const char *name = camera_service_wake_sensor_name(i);
            return name && name[0] ? name : selected;
        }
    }
    return selected;
}

void wake_sensor_select_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0) {
        camera_service_select_wake_sensor("");
    } else if (static_cast<size_t>(index) < camera_service_wake_sensor_count()) {
        camera_service_select_wake_sensor(camera_service_wake_sensor_entity(static_cast<size_t>(index)));
    }
    g_last_user_activity_ms = millis();
    close_overlay();
    request_rebuild();
}

void show_wake_sensor_picker() {
    close_overlay();
    if (!camera_service_wake_sensors_ready()) camera_service_request_wake_sensor_discovery();

    g_overlay = lv_obj_create(g_screen);
    g_wake_sensor_picker_open = true;
    lv_obj_set_size(g_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_overlay, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(theme().overlay), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(g_overlay);
    lv_obj_set_size(card, 800, 620);
    lv_obj_center(card);
    style_box(card, theme().panel, 18, 1);
    lv_obj_set_style_pad_all(card, 26, LV_PART_MAIN);

    lv_obj_t *title = label(card, "Wake on approach", &lv_font_montserrat_24, theme().text, 590);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *description = label(
        card,
        "Choose a Home Assistant person, motion, occupancy, or presence binary sensor.  A camera person-detection sensor works well.",
        &lv_font_montserrat_14,
        theme().muted,
        680);
    lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(description, 48);
    lv_obj_set_pos(description, 0, 38);

    lv_obj_t *close = button(card, "Close", 100, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(close, 646, 0);
    lv_obj_add_event_cb(close, close_overlay_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *list = lv_obj_create(card);
    lv_obj_set_size(list, 748, 468);
    lv_obj_set_pos(list, 0, 94);
    style_box(list, theme().panel_alt, 12, 1);
    lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    int y = 0;
    const char *selected = camera_service_selected_wake_sensor();
    lv_obj_t *off = button(list, LV_SYMBOL_CLOSE "  Off", 700, 50,
                           !selected || !selected[0], &lv_font_montserrat_14);
    lv_obj_set_pos(off, 0, y);
    lv_obj_add_event_cb(off, wake_sensor_select_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
    y += 58;

    if (!camera_service_wake_sensors_ready()) {
        lv_obj_t *loading = label(list, "Discovering Home Assistant wake sensors...",
                                  &lv_font_montserrat_16, theme().muted, 680);
        lv_obj_set_pos(loading, 8, y + 12);
    } else if (camera_service_wake_sensor_count() == 0) {
        lv_obj_t *none = label(list,
            "No motion/occupancy/presence/person binary_sensor entities were found.",
            &lv_font_montserrat_16, theme().muted, 680);
        lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(none, 8, y + 12);
    } else {
        for (size_t i = 0; i < camera_service_wake_sensor_count(); ++i) {
            const char *entity = camera_service_wake_sensor_entity(i);
            const bool active = selected && selected[0] && strcmp(selected, entity) == 0;
            char text[180];
            snprintf(text, sizeof(text), "%s%s", active ? LV_SYMBOL_OK "  " : "", camera_service_wake_sensor_name(i));
            lv_obj_t *choice = button(list, text, 700, 50, active, &lv_font_montserrat_14);
            lv_obj_set_pos(choice, 0, y);
            lv_obj_add_event_cb(choice, wake_sensor_select_cb, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<intptr_t>(i)));
            y += 58;
        }
    }

    lv_obj_move_foreground(g_overlay);
    lv_obj_invalidate(g_screen);
}

void wake_sensor_picker_cb(lv_event_t *) {
    show_wake_sensor_picker();
}

void create_home_dashboard() {
    lv_obj_t *page = lv_obj_create(g_screen);
    lv_obj_set_size(page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(page, 8, PAGE_Y);
    style_box(page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(page, 16, LV_PART_MAIN);

    lv_obj_t *title = label(page, "Settings", &lv_font_montserrat_24, theme().text, 380);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *description = label(
        page,
        "Display, Home Assistant, and system settings.  Calendar remains the default dashboard at every boot.",
        &lv_font_montserrat_14,
        theme().muted,
        850);
    lv_obj_set_pos(description, 0, 38);

    constexpr int CARD_W = 592;
    constexpr int CARD_H = 238;
    constexpr int GAP = 16;
    constexpr int START_Y = 88;

    lv_obj_t *network_card = create_info_card(page, "NETWORK", 0, START_Y, CARD_W, CARD_H, &g_home_network);
    lv_obj_t *ha_card = create_info_card(page, "HOME ASSISTANT", CARD_W + GAP, START_Y, CARD_W, CARD_H, &g_home_ha);
    lv_obj_t *calendar_card = create_info_card(page, "CALENDAR", 0, START_Y + CARD_H + GAP, CARD_W, CARD_H, &g_home_calendar);
    lv_obj_t *system_card = create_info_card(page, "DISPLAY & SYSTEM", CARD_W + GAP, START_Y + CARD_H + GAP, CARD_W, CARD_H, &g_home_system);

    (void)network_card;

    lv_obj_t *sync = button(ha_card, "Sync now", 120, 38, true, &lv_font_montserrat_14);
    lv_obj_align(sync, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(sync, home_sync_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *calendar_default = label(calendar_card, "Default screen: Calendar", &lv_font_montserrat_14,
                                       theme().success, CARD_W - 36);
    lv_obj_align(calendar_default, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    char timeout_button[64];
    snprintf(timeout_button, sizeof(timeout_button), "Timeout: %s", screen_timeout_text(g_screen_timeout_seconds));
    lv_obj_t *timeout = button(system_card, timeout_button, 160, 38, false, &lv_font_montserrat_14);
    lv_obj_align(timeout, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(timeout, screen_timeout_cycle_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *wake_note = label(system_card, "Wake: touch only", &lv_font_montserrat_12, theme().muted, 150);
    lv_obj_align(wake_note, LV_ALIGN_BOTTOM_LEFT, 172, -10);

    lv_obj_t *dim = button(system_card, "-", 46, 38, false, &lv_font_montserrat_18);
    lv_obj_align(dim, LV_ALIGN_BOTTOM_RIGHT, -58, 0);
    lv_obj_add_event_cb(dim, brightness_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(-10)));

    lv_obj_t *bright = button(system_card, "+", 46, 38, false, &lv_font_montserrat_18);
    lv_obj_align(bright, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(bright, brightness_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(10)));

    update_home_dashboard();
}

void reset_ui_pointers() {
    g_header_date = nullptr;
    g_header_time = nullptr;
    g_wifi_state = nullptr;
    memset(g_wifi_bars, 0, sizeof(g_wifi_bars));
    g_status_label = nullptr;
    g_summary_label = nullptr;
    g_week_label = nullptr;
    g_overlay = nullptr;
    g_alarm_code_input = nullptr;
    g_home_network = nullptr;
    g_home_ha = nullptr;
    g_home_calendar = nullptr;
    g_home_system = nullptr;
    memset(g_day_columns, 0, sizeof(g_day_columns));
    memset(g_filter_buttons, 0, sizeof(g_filter_buttons));
}

bool auto_refresh_due(uint32_t now, uint32_t last_attempt_ms, uint32_t last_ui_request_ms, uint32_t interval_ms) {
    if (interval_ms == 0) return false;
    if (last_attempt_ms != 0 && now - last_attempt_ms < interval_ms) return false;
    if (last_ui_request_ms != 0 && now - last_ui_request_ms < interval_ms) return false;
    return true;
}

void service_active_dashboard_auto_refresh(uint32_t now) {
    /* The display being asleep is a strong signal that nobody is looking at
     * the dashboard, so do not spend ESP-Hosted/TLS traffic refreshing it. */
    if (!board_display_awake()) return;
    if (now - g_dashboard_entered_ms < HA_ACTIVE_TAB_SETTLE_MS) return;
    if (!home_assistant_ready_for_auto_refresh()) return;

    switch (g_dashboard) {
        case Dashboard::Calendar:
        case Dashboard::Meals:
            if (auto_refresh_due(now,
                                 home_assistant_last_calendar_request_ms(),
                                 g_last_calendar_auto_request_ms,
                                 HA_ACTIVE_TAB_CALENDAR_REFRESH_MS)) {
                g_last_calendar_auto_request_ms = now;
                ESP_LOGI("FamilyCalendar", "Active-tab auto refresh: %s calendar data", dashboard_name(g_dashboard));
                home_assistant_request_sync();
            }
            if (weather_service_should_refresh(now) &&
                auto_refresh_due(now,
                                 weather_service_last_attempt_ms(),
                                 g_last_weather_auto_request_ms,
                                 WEATHER_MIN_REQUEST_GAP_MS)) {
                g_last_weather_auto_request_ms = now;
                weather_service_request_refresh(false, "calendar indicator refresh");
            }
            break;

        case Dashboard::Weather:
            if (weather_service_should_refresh(now) &&
                auto_refresh_due(now,
                                 weather_service_last_attempt_ms(),
                                 g_last_weather_auto_request_ms,
                                 WEATHER_ACTIVE_TAB_REFRESH_MS)) {
                g_last_weather_auto_request_ms = now;
                ESP_LOGI("FamilyCalendar", "Active-tab auto refresh: Weather");
                weather_service_request_refresh(false, "weather active tab refresh");
            }
            break;

        case Dashboard::Chores:
            if (!chore_service_configured() || g_chore_choose_list) break;
            if (auto_refresh_due(now,
                                 home_assistant_last_chore_request_ms(),
                                 g_last_chore_auto_request_ms,
                                 HA_ACTIVE_TAB_CHORES_REFRESH_MS)) {
                g_last_chore_auto_request_ms = now;
                ESP_LOGI("FamilyCalendar", "Active-tab auto refresh: Chores");
                home_assistant_request_chore_sync();
            }
            break;

        case Dashboard::Alarm:
            if (!alarm_service_configured() || g_alarm_choose_panel) break;
            if (auto_refresh_due(now,
                                 home_assistant_last_alarm_request_ms(),
                                 g_last_alarm_auto_request_ms,
                                 HA_ACTIVE_TAB_ALARM_REFRESH_MS)) {
                g_last_alarm_auto_request_ms = now;
                ESP_LOGI("FamilyCalendar", "Active-tab auto refresh: Alarmo");
                home_assistant_request_alarm_sync();
            }
            break;

        case Dashboard::Settings:
            /* Settings is refreshed locally once per second by update_home_dashboard().
             * It intentionally does not create Home Assistant network traffic. */
            break;
    }
}

void rebuild_ui() {
    g_rebuild_pending = false;
    reset_ui_pointers();
    lv_obj_clean(g_screen);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(theme().bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, LV_PART_MAIN);

    create_header();
    create_footer();

    switch (g_dashboard) {
        case Dashboard::Calendar:
            create_calendar_dashboard();
            break;
        case Dashboard::Chores:
            create_chores_dashboard();
            break;
        case Dashboard::Meals:
            create_meals_dashboard();
            break;
        case Dashboard::Weather:
            create_weather_dashboard();
            break;
        case Dashboard::Alarm:
            create_alarm_dashboard();
            break;
        case Dashboard::Settings:
            create_home_dashboard();
            break;
    }

    update_clock();
    lv_obj_invalidate(g_screen);
}

} // namespace

void calendar_ui_init() {
    g_screen = lv_screen_active();
    lv_obj_remove_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);

    g_ui_preferences_ready = g_ui_preferences.begin("famcal_ui", false);
    if (g_ui_preferences_ready) {
        g_dark_mode = g_ui_preferences.getBool("dark", APP_DEFAULT_DARK_MODE != 0);
        g_backlight = g_ui_preferences.getUChar("bright", APP_DEFAULT_BACKLIGHT);
        if (g_backlight < 10 || g_backlight > 100) g_backlight = APP_DEFAULT_BACKLIGHT;
        g_screen_timeout_seconds = g_ui_preferences.getUInt("timeout", APP_SCREEN_TIMEOUT_SECONDS);
        if (g_screen_timeout_seconds != 0 &&
            (g_screen_timeout_seconds < APP_SCREEN_TIMEOUT_MIN_SECONDS ||
             g_screen_timeout_seconds > APP_SCREEN_TIMEOUT_MAX_SECONDS)) {
            g_screen_timeout_seconds = 120U;
        }
    }
    board_set_display_awake(true, g_backlight);
    g_last_user_activity_ms = millis();

    /* Calendar is deliberately not persisted: it is always the boot/default dashboard. */
    g_dashboard = Dashboard::Calendar;
    g_dashboard_entered_ms = millis();
    rebuild_ui();
}

void calendar_ui_loop() {
    const uint32_t now = millis();

    service_calendar_swipe();
    service_calendar_prefetch_debounce(now);

    const bool touch_activity = board_take_touch_activity();
    if (touch_activity) {
        g_last_user_activity_ms = now;
        if (!board_display_awake()) {
            board_set_display_awake(true, g_backlight);
            g_dashboard_entered_ms = now;
        }
    }

    /* A selected Home Assistant person/motion/presence sensor can wake
     * the panel before it is touched.  While the sensor remains active, keep
     * extending the inactivity timer so the display does not blank underneath
     * someone standing in front of it. */
    if (camera_service_presence_active()) {
        g_last_user_activity_ms = now;
        if (!board_display_awake()) {
            board_set_display_awake(true, g_backlight);
            g_dashboard_entered_ms = now;
        }
    }

    if (board_display_awake() && g_screen_timeout_seconds > 0 &&
        now - g_last_user_activity_ms >= g_screen_timeout_seconds * 1000UL) {
        board_set_display_awake(false, g_backlight);
    }

    if (g_rebuild_pending) {
        rebuild_ui();
    }

    if (chore_service_loop() && g_dashboard == Dashboard::Chores) {
        request_rebuild();
    }

    if (now - g_last_clock_update >= 1000UL) {
        g_last_clock_update = now;
        update_clock();
    }

    service_active_dashboard_auto_refresh(now);

    if (now - g_last_status_update >= 1000UL) {
        g_last_status_update = now;
        update_wifi_header();
        if (g_status_label) set_label_text(g_status_label, home_assistant_status());
        update_home_dashboard();
    }
}

void calendar_ui_refresh(uint32_t change_flags) {
    if (g_dashboard == Dashboard::Calendar &&
        (change_flags & (HA_CHANGE_CALENDAR | HA_CHANGE_WEATHER))) {
        render_week();
    }
    if (g_dashboard == Dashboard::Meals && (change_flags & HA_CHANGE_CALENDAR)) {
        request_rebuild();
    }
    if (g_dashboard == Dashboard::Weather && (change_flags & HA_CHANGE_WEATHER)) {
        request_rebuild();
    }
    if (g_dashboard == Dashboard::Chores &&
        (change_flags & (HA_CHANGE_CHORES | HA_CHANGE_TODO_LISTS))) {
        request_rebuild();
    }
    if (g_dashboard == Dashboard::Alarm &&
        (change_flags & (HA_CHANGE_ALARM | HA_CHANGE_ALARM_PANELS))) {
        request_rebuild();
    }
    if (g_dashboard == Dashboard::Settings && change_flags != HA_CHANGE_NONE) {
        update_home_dashboard();
    }
}

void calendar_ui_wake_refresh(uint32_t change_flags) {
    if (g_dashboard == Dashboard::Settings &&
        (change_flags & (CAMERA_CHANGE_WAKE_SENSORS | CAMERA_CHANGE_WAKE_STATE))) {
        if (g_wake_sensor_picker_open && (change_flags & CAMERA_CHANGE_WAKE_SENSORS)) {
            show_wake_sensor_picker();
        } else {
            update_home_dashboard();
        }
    }
}

int calendar_ui_week_offset() {
    return g_week_offset;
}
