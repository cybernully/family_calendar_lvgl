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
#include "weather_icons.h"
#include "runtime_config.h"
#include "battery_service.h"
#include "web_manager.h"
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
constexpr int DAY_HEADER_H = 88;
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
lv_obj_t *g_root_screen = nullptr;
lv_obj_t *g_header_root = nullptr;
lv_obj_t *g_footer_root = nullptr;
lv_obj_t *g_header_page_label = nullptr;
lv_obj_t *g_header_date = nullptr;
lv_obj_t *g_header_time = nullptr;
lv_obj_t *g_wifi_state = nullptr;
lv_obj_t *g_wifi_bars[4] = {};
lv_obj_t *g_battery_chip = nullptr;
lv_obj_t *g_battery_body = nullptr;
lv_obj_t *g_battery_fill = nullptr;
lv_obj_t *g_battery_terminal = nullptr;
lv_obj_t *g_battery_text = nullptr;
lv_obj_t *g_status_label = nullptr;
lv_obj_t *g_summary_label = nullptr;
lv_obj_t *g_week_label = nullptr;
lv_obj_t *g_day_columns[7] = {};
lv_obj_t *g_filter_buttons[4] = {};
lv_obj_t *g_overlay = nullptr;
bool g_wake_sensor_picker_open = false;

constexpr size_t DASHBOARD_COUNT = 6;
lv_obj_t *g_page_roots[DASHBOARD_COUNT] = {};
bool g_page_built[DASHBOARD_COUNT] = {};
bool g_page_dirty[DASHBOARD_COUNT] = {true, true, true, true, true, true};
lv_obj_t *g_nav_buttons[DASHBOARD_COUNT] = {};
lv_obj_t *g_nav_icons[DASHBOARD_COUNT] = {};
lv_obj_t *g_nav_captions[DASHBOARD_COUNT] = {};

struct CalendarEventWidget {
    lv_obj_t *card = nullptr;
    lv_obj_t *time_label = nullptr;
    lv_obj_t *title_label = nullptr;
    const CalendarEvent *event = nullptr;
};

struct CalendarDayWidgets {
    lv_obj_t *day_name = nullptr;
    lv_obj_t *day_number = nullptr;
    lv_obj_t *weather_row = nullptr;
    lv_obj_t *weather_icon = nullptr;
    lv_obj_t *weather_temp = nullptr;
    lv_obj_t *empty_label = nullptr;
    lv_obj_t *more_label = nullptr;
    CalendarEventWidget events[MAX_VISIBLE_EVENTS_PER_DAY];
};

struct ChoreCardWidgets {
    lv_obj_t *card = nullptr;
    lv_obj_t *task = nullptr;
    lv_obj_t *hint = nullptr;
};

struct PickerCardWidgets {
    lv_obj_t *card = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *entity = nullptr;
};

struct MealDayWidgets {
    lv_obj_t *card = nullptr;
    lv_obj_t *heading = nullptr;
    lv_obj_t *meal_lines[3] = {};
    lv_obj_t *empty = nullptr;
    lv_obj_t *hint = nullptr;
    const CalendarEvent *first_event = nullptr;
};

struct WeatherMetricWidgets {
    lv_obj_t *value = nullptr;
};

struct WeatherDailyWidgets {
    lv_obj_t *card = nullptr;
    lv_obj_t *accent = nullptr;
    lv_obj_t *day = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *condition = nullptr;
    lv_obj_t *temps = nullptr;
};

struct WeatherHourlyWidgets {
    lv_obj_t *card = nullptr;
    lv_obj_t *accent = nullptr;
    lv_obj_t *time = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *temp = nullptr;
    lv_obj_t *precip = nullptr;
};

struct WeatherWidgets {
    lv_obj_t *page = nullptr;
    lv_obj_t *updated = nullptr;
    lv_obj_t *message = nullptr;
    lv_obj_t *content = nullptr;
    lv_obj_t *hero_accent = nullptr;
    lv_obj_t *hero_icon = nullptr;
    lv_obj_t *hero_temp = nullptr;
    lv_obj_t *hero_condition = nullptr;
    lv_obj_t *hero_range = nullptr;
    WeatherMetricWidgets metrics[4];
    lv_obj_t *daily_heading = nullptr;
    WeatherDailyWidgets daily[6];
    lv_obj_t *hourly_heading = nullptr;
    WeatherHourlyWidgets hourly[18];
};

struct AlarmWidgets {
    lv_obj_t *page = nullptr;
    lv_obj_t *picker = nullptr;
    lv_obj_t *picker_back = nullptr;
    lv_obj_t *picker_status = nullptr;
    PickerCardWidgets picker_cards[HA_MAX_ALARM_PANELS];
    lv_obj_t *loading = nullptr;
    lv_obj_t *main = nullptr;
    lv_obj_t *state = nullptr;
    lv_obj_t *next = nullptr;
    lv_obj_t *sensors = nullptr;
    lv_obj_t *last = nullptr;
    lv_obj_t *code = nullptr;
    lv_obj_t *entity = nullptr;
    lv_obj_t *disarm = nullptr;
    lv_obj_t *home = nullptr;
    lv_obj_t *away = nullptr;
    lv_obj_t *night = nullptr;
    lv_obj_t *vacation = nullptr;
    lv_obj_t *skip = nullptr;
    lv_obj_t *skip_info = nullptr;
};

CalendarDayWidgets g_calendar_days[7] = {};
ChoreCardWidgets g_chore_cards[HA_MAX_CHORES] = {};
PickerCardWidgets g_chore_picker_cards[HA_MAX_TODO_LISTS] = {};
MealDayWidgets g_meal_days[7] = {};
WeatherWidgets g_weather_widgets = {};
AlarmWidgets g_alarm_widgets = {};

lv_obj_t *g_chore_page = nullptr;
lv_obj_t *g_chore_main = nullptr;
lv_obj_t *g_chore_picker = nullptr;
lv_obj_t *g_chore_picker_status = nullptr;
lv_obj_t *g_chore_picker_back = nullptr;
lv_obj_t *g_chore_source = nullptr;
lv_obj_t *g_chore_progress_label = nullptr;
lv_obj_t *g_chore_progress = nullptr;
lv_obj_t *g_chore_empty = nullptr;

lv_obj_t *g_meals_page = nullptr;

lv_obj_t *g_home_timeout_button_label = nullptr;

lv_obj_t *g_home_network = nullptr;
lv_obj_t *g_home_ha = nullptr;
lv_obj_t *g_home_calendar = nullptr;
lv_obj_t *g_home_system = nullptr;
lv_obj_t *g_summary_title = nullptr;

Dashboard g_dashboard = Dashboard::Calendar;
bool g_person_visible[4] = {true, true, true, true};
int g_week_offset = 0;
uint8_t g_backlight = APP_DEFAULT_BACKLIGHT;
bool g_dark_mode = APP_DEFAULT_DARK_MODE != 0;
uint32_t g_screen_timeout_seconds = APP_SCREEN_TIMEOUT_SECONDS;
uint32_t g_last_user_activity_ms = 0;
bool g_rebuild_pending = false;
bool g_full_rebuild_pending = false;
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
void activate_dashboard(Dashboard dashboard);
void request_full_rebuild();
void update_chores_dashboard();
void update_meals_dashboard();
void update_weather_dashboard();
void update_alarm_dashboard();
void update_dashboard_page(Dashboard dashboard);

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

size_t dashboard_index(Dashboard dashboard) {
    return static_cast<size_t>(dashboard);
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

lv_obj_t *create_weather_image(lv_obj_t *parent, int size) {
    if (!parent) return nullptr;
    lv_obj_t *image = lv_image_create(parent);
    if (!image) return nullptr;
    lv_obj_set_size(image, size, size);
    lv_image_set_src(image, weather_icon_asset(WeatherCondition::Unknown));
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_antialias(image, true);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    return image;
}

void set_weather_image(lv_obj_t *image,
                       WeatherCondition condition,
                       const char *symbol_code = nullptr) {
    if (!image) return;
    lv_image_set_src(image, weather_icon_asset(condition, symbol_code));
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

void update_battery_header() {
    if (!g_battery_chip || !g_battery_body || !g_battery_fill ||
        !g_battery_terminal || !g_battery_text) return;

    BatteryStatus battery = {};
    const bool valid = battery_service_get_status(battery);
    const bool show_percent = battery_service_show_percent();

    /* Center the icon in the chip when the numeric percentage is disabled. */
    if (show_percent) {
        lv_obj_set_pos(g_battery_body, 10, 12);
        lv_obj_set_pos(g_battery_terminal, 46, 18);
        lv_obj_set_pos(g_battery_text, 56, 15);
        lv_obj_remove_flag(g_battery_text, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_pos(g_battery_body, 43, 12);
        lv_obj_set_pos(g_battery_terminal, 79, 18);
        lv_obj_add_flag(g_battery_text, LV_OBJ_FLAG_HIDDEN);
    }

    uint32_t color = theme().muted;
    if (valid) {
        if (battery.percent <= battery_service_critical_percent()) {
            color = g_dark_mode ? 0xF87171 : 0xDC2626;
        } else if (battery.percent <= 20) {
            color = 0xF59E0B;
        } else {
            color = theme().success;
        }
    }

    lv_obj_set_style_border_color(g_battery_body, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_battery_terminal, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(g_battery_text, lv_color_hex(color), LV_PART_MAIN);

    if (!valid || battery.percent == 0) {
        lv_obj_add_flag(g_battery_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        const int fill_width = 2 + (static_cast<int>(battery.percent) * 24) / 100;
        lv_obj_set_width(g_battery_fill, fill_width);
        lv_obj_set_style_bg_color(g_battery_fill, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_remove_flag(g_battery_fill, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_percent) {
        char text[16];
        if (valid) snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(battery.percent));
        else snprintf(text, sizeof(text), "--");
        set_label_text(g_battery_text, text);
    }
}

void persist_ui_state() {
    if (!g_ui_preferences_ready) return;
    g_ui_preferences.putBool("dark", g_dark_mode);
    g_ui_preferences.putUChar("bright", g_backlight);
    g_ui_preferences.putUInt("timeout", g_screen_timeout_seconds);
}

void mark_dashboard_dirty(Dashboard dashboard) {
    const size_t index = dashboard_index(dashboard);
    if (index < DASHBOARD_COUNT) g_page_dirty[index] = true;
}

void request_rebuild() {
    mark_dashboard_dirty(g_dashboard);
    g_rebuild_pending = true;
}

void request_full_rebuild() {
    g_full_rebuild_pending = true;
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
        event.person < 4 ? runtime_config_person_name(event.person) : "Calendar";

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
    // append_line("Home Assistant", event.source);

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
    close_overlay();
    activate_dashboard(next);
}

void theme_toggle_cb(lv_event_t *) {
    g_dark_mode = !g_dark_mode;
    persist_ui_state();
    close_overlay();
    /* Theme colors are baked into many widget styles.  A theme change is the
     * intentionally rare case where v2 rebuilds the persistent shell/pages. */
    request_full_rebuild();
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
    g_header_root = header;
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
    g_header_page_label = label(header, subtitle, &lv_font_montserrat_14, theme().muted, 330);
    lv_obj_set_pos(g_header_page_label, 24, 50);

    g_header_date = label(header, "", &lv_font_montserrat_18, theme().text, 360);
    lv_obj_set_pos(g_header_date, 390, 12);

    g_header_time = label(header, "", &lv_font_montserrat_16, theme().muted, 220);
    lv_obj_set_pos(g_header_time, 390, 49);

    /* JC8012P4A1 battery chip. The icon is drawn with LVGL primitives so it
     * does not depend on a battery glyph being present in the custom font. */
    lv_obj_t *battery_chip = lv_obj_create(header);
    g_battery_chip = battery_chip;
    lv_obj_set_size(battery_chip, 126, 46);
    lv_obj_set_pos(battery_chip, SCREEN_W - 380, 19);
    style_box(battery_chip, theme().button, 10, 1);

    g_battery_body = lv_obj_create(battery_chip);
    lv_obj_set_size(g_battery_body, 36, 22);
    lv_obj_set_pos(g_battery_body, 10, 12);
    lv_obj_set_style_bg_opa(g_battery_body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_battery_body, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(g_battery_body, lv_color_hex(theme().muted), LV_PART_MAIN);
    lv_obj_set_style_radius(g_battery_body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_battery_body, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(g_battery_body, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_battery_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g_battery_body, LV_OBJ_FLAG_CLICKABLE);

    g_battery_fill = icon_piece(g_battery_body, 3, 3, 26, 12, theme().muted, 2);
    g_battery_terminal = icon_piece(battery_chip, 46, 18, 4, 10, theme().muted, 2);
    g_battery_text = label(battery_chip, "--", &lv_font_montserrat_14, theme().muted, 60);
    lv_obj_set_pos(g_battery_text, 56, 15);

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
    update_battery_header();
}

void create_footer() {
    lv_obj_t *footer = lv_obj_create(g_screen);
    g_footer_root = footer;
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

        const size_t index = dashboard_index(dashboard);
        if (index < DASHBOARD_COUNT) {
            g_nav_buttons[index] = nav;
            g_nav_icons[index] = lv_obj_get_child(nav, 0);
            g_nav_captions[index] = lv_obj_get_child(nav, 1);
        }
        x += BUTTON_W + GAP;
    }
}

void calendar_event_slot_clicked_cb(lv_event_t *e) {
    auto *slot = static_cast<CalendarEventWidget *>(lv_event_get_user_data(e));
    if (!slot || !slot->event) return;
    show_event_details_overlay(*slot->event);
}

void create_calendar_day_widgets(lv_obj_t *column, int day) {
    if (!column || day < 0 || day >= 7) return;
    CalendarDayWidgets &widgets = g_calendar_days[day];

    widgets.day_name = label(column, calendar_day_name(day), &lv_font_montserrat_14,
                             theme().muted, DAY_W - 12);
    if (widgets.day_name) {
        lv_obj_set_style_text_align(widgets.day_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(widgets.day_name, 6, 5);
    }

    widgets.day_number = label(column, "--", &lv_font_montserrat_24, theme().text);
    if (widgets.day_number) lv_obj_align(widgets.day_number, LV_ALIGN_TOP_MID, 0, 24);

    constexpr int WEATHER_ROW_W = 100;
    constexpr int WEATHER_ROW_H = 34;
    widgets.weather_row = lv_obj_create(column);
    lv_obj_set_size(widgets.weather_row, WEATHER_ROW_W, WEATHER_ROW_H);
    lv_obj_align(widgets.weather_row, LV_ALIGN_TOP_MID, 0, 49);
    lv_obj_set_style_bg_opa(widgets.weather_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(widgets.weather_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(widgets.weather_row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(widgets.weather_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(widgets.weather_row, LV_OBJ_FLAG_CLICKABLE);

    widgets.weather_icon = create_weather_image(widgets.weather_row, 32);
    set_pos_if(widgets.weather_icon, 1, 1);
    widgets.weather_temp = label(widgets.weather_row, "--/--", &lv_font_montserrat_14,
                                 theme().text, 62);
    if (widgets.weather_temp) {
        lv_obj_set_style_text_align(widgets.weather_temp, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(widgets.weather_temp, 38, 8);
    }

    widgets.empty_label = label(column, "No events", &lv_font_montserrat_12, theme().muted);
    if (widgets.empty_label) lv_obj_align(widgets.empty_label, LV_ALIGN_TOP_MID, 0, DAY_HEADER_H + 10);

    widgets.more_label = label(column, "", &lv_font_montserrat_12, theme().muted);
    if (widgets.more_label) lv_obj_align(widgets.more_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    for (int slot_index = 0; slot_index < MAX_VISIBLE_EVENTS_PER_DAY; ++slot_index) {
        CalendarEventWidget &slot = widgets.events[slot_index];
        slot.card = lv_button_create(column);
        lv_obj_set_size(slot.card, DAY_W - 12, EVENT_CARD_H);
        lv_obj_set_pos(slot.card, 6, DAY_HEADER_H + slot_index * EVENT_CARD_STEP);
        style_box(slot.card, theme().accent, 9, 0);
        lv_obj_set_style_bg_opa(slot.card, LV_OPA_90, LV_PART_MAIN);
        lv_obj_set_style_pad_all(slot.card, 7, LV_PART_MAIN);

        slot.time_label = label(slot.card, "", &lv_font_montserrat_12, 0xFFFFFF, DAY_W - 28);
        lv_obj_set_pos(slot.time_label, 0, -1);
        slot.title_label = label(slot.card, "", &lv_font_montserrat_14, 0xFFFFFF, DAY_W - 28);
        lv_label_set_long_mode(slot.title_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(slot.title_label, 44);
        lv_obj_set_pos(slot.title_label, 0, 23);
        lv_obj_add_event_cb(slot.card, calendar_event_slot_clicked_cb, LV_EVENT_CLICKED, &slot);
        lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
    }
}

void render_week() {
    if (!g_week_label) return;

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
        CalendarDayWidgets &widgets = g_calendar_days[day];

        const time_t day_time = time_service_add_days(week_start, day);
        struct tm day_tm = {};
        localtime_r(&day_time, &day_tm);

        const bool is_today = (g_week_offset == 0 &&
                               day_tm.tm_year == now_tm.tm_year &&
                               day_tm.tm_yday == now_tm.tm_yday);

        lv_obj_set_style_bg_color(column, lv_color_hex(is_today ? theme().today : theme().panel), LV_PART_MAIN);
        set_label_text(widgets.day_name, calendar_day_name(day));
        if (widgets.day_name) {
            lv_obj_set_style_text_color(widgets.day_name,
                                        lv_color_hex(is_today ? theme().accent : theme().muted),
                                        LV_PART_MAIN);
        }

        char number[8];
        snprintf(number, sizeof(number), "%d", day_tm.tm_mday);
        set_label_text(widgets.day_number, number);
        if (widgets.day_number) {
            lv_obj_set_style_text_color(widgets.day_number,
                                        lv_color_hex(is_today ? theme().accent : theme().text),
                                        LV_PART_MAIN);
        }

        WeatherDayForecast day_weather = {};
        if (weather_service_get_day_forecast(day_time, day_weather) && day_weather.valid) {
            lv_obj_remove_flag(widgets.weather_row, LV_OBJ_FLAG_HIDDEN);
            set_weather_image(widgets.weather_icon, day_weather.condition, day_weather.symbol_code);
            char temps[24];
            snprintf(temps, sizeof(temps), "%d/%d",
                     rounded_display_temp(day_weather.high_c),
                     rounded_display_temp(day_weather.low_c));
            set_label_text(widgets.weather_temp, temps);
            if (widgets.weather_temp) {
                lv_obj_set_style_text_color(widgets.weather_temp,
                                            lv_color_hex(is_today ? theme().accent : theme().text),
                                            LV_PART_MAIN);
            }
        } else {
            lv_obj_add_flag(widgets.weather_row, LV_OBJ_FLAG_HIDDEN);
        }

        int slot_count = 0;
        int hidden = 0;
        for (size_t i = 0; i < CALENDAR_EVENT_COUNT; ++i) {
            const CalendarEvent &event = CALENDAR_EVENTS[i];
            if (!calendar_event_is_on_day(event, day_time)) continue;
            if (event.person >= 4 || !g_person_visible[event.person]) continue;

            if (slot_count < MAX_VISIBLE_EVENTS_PER_DAY) {
                CalendarEventWidget &slot = widgets.events[slot_count++];
                slot.event = &event;
                char time_text[22];
                calendar_format_event_time(event, time_text, sizeof(time_text));
                set_label_text(slot.time_label, time_text);
                set_label_text(slot.title_label, event.title);
                lv_obj_set_style_bg_color(slot.card,
                                          lv_color_hex(runtime_config_person_color(event.person)),
                                          LV_PART_MAIN);
                lv_obj_remove_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            } else {
                ++hidden;
            }
        }

        for (int slot = slot_count; slot < MAX_VISIBLE_EVENTS_PER_DAY; ++slot) {
            widgets.events[slot].event = nullptr;
            lv_obj_add_flag(widgets.events[slot].card, LV_OBJ_FLAG_HIDDEN);
        }

        if (slot_count == 0) {
            lv_obj_remove_flag(widgets.empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(widgets.empty_label, LV_OBJ_FLAG_HIDDEN);
        }

        if (hidden > 0) {
            char more[24];
            snprintf(more, sizeof(more), "+%d more", hidden);
            set_label_text(widgets.more_label, more);
            lv_obj_remove_flag(widgets.more_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(widgets.more_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (g_status_label) set_label_text(g_status_label, home_assistant_status());

    if (g_summary_label) {
        char summary[96];
        if (g_week_offset == 0) {
            if (g_summary_title) set_label_text(g_summary_title, "TODAY");
            const size_t today_events = calendar_count_events_on_day(time_service_now());
            snprintf(summary, sizeof(summary), "%u event%s today\n%u total this week",
                     static_cast<unsigned>(today_events), today_events == 1 ? "" : "s",
                     static_cast<unsigned>(CALENDAR_EVENT_COUNT));
        } else {
            if (g_summary_title) set_label_text(g_summary_title, "SELECTED WEEK");
            snprintf(summary, sizeof(summary), "%u event%s",
                     static_cast<unsigned>(CALENDAR_EVENT_COUNT),
                     CALENDAR_EVENT_COUNT == 1 ? "" : "s");
        }
        set_label_text(g_summary_label, summary);
    }
}

void filter_clicked_cb(lv_event_t *e) {
    const intptr_t index = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (index < 0 || index > 3) return;
    g_person_visible[index] = !g_person_visible[index];

    lv_obj_t *btn = g_filter_buttons[index];
    if (btn) {
        const uint32_t color = g_person_visible[index] ? runtime_config_person_color(index) : theme().button;
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
    mark_dashboard_dirty(Dashboard::Meals);

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
        style_box(chip, g_person_visible[i] ? runtime_config_person_color(i) : theme().button, 18, 0);
        lv_obj_t *txt = label(
            chip,
            runtime_config_person_name(i),
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

    // Keep the sidebar intentionally sparse: today's summary at the top,
    // Home Assistant status below it, and the manual refresh action anchored
    // to the bottom.  Avoid instructional copy here so the calendar itself
    // remains the visual focus.
    g_summary_title = label(side, "TODAY", &lv_font_montserrat_14, theme().muted);
    lv_obj_set_pos(g_summary_title, 0, 0);

    g_summary_label = label(side, "Calendar starting...", &lv_font_montserrat_18, theme().text, 174);
    lv_label_set_long_mode(g_summary_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_summary_label, 0, 28);

    lv_obj_t *line = lv_obj_create(side);
    lv_obj_set_size(line, 174, 1);
    lv_obj_set_pos(line, 0, 92);
    lv_obj_set_style_bg_color(line, lv_color_hex(theme().border), LV_PART_MAIN);
    lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);

    lv_obj_t *integration = label(side, "HOME ASSISTANT", &lv_font_montserrat_14, theme().muted);
    lv_obj_set_pos(integration, 0, 116);

    g_status_label = label(side, "", &lv_font_montserrat_12, theme().muted, 174);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_status_label, 220);
    lv_obj_set_pos(g_status_label, 0, 146);

    lv_obj_t *sync_side = button(side, "Refresh now", 174, 42, false, &lv_font_montserrat_14);
    lv_obj_align(sync_side, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(sync_side, sync_now_cb, LV_EVENT_CLICKED, nullptr);

    for (int day = 0; day < 7; ++day) {
        lv_obj_t *column = lv_obj_create(g_screen);
        lv_obj_set_size(column, DAY_W, CAL_MAIN_H);
        lv_obj_set_pos(column, CAL_X + day * (DAY_W + DAY_GAP), CAL_MAIN_Y);
        style_box(column, theme().panel, 12, 1);
        g_day_columns[day] = column;
        create_calendar_day_widgets(column, day);
    }
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

void create_chores_dashboard() {
    g_chore_page = lv_obj_create(g_screen);
    lv_obj_set_size(g_chore_page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(g_chore_page, 8, PAGE_Y);
    style_box(g_chore_page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(g_chore_page, 18, LV_PART_MAIN);
    lv_obj_remove_flag(g_chore_page, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int CONTENT_W = 1226;
    constexpr int CONTENT_H = PAGE_H - 36;

    // Persistent picker -------------------------------------------------------
    g_chore_picker = lv_obj_create(g_chore_page);
    lv_obj_set_size(g_chore_picker, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(g_chore_picker, 0, 0);
    lv_obj_set_style_bg_opa(g_chore_picker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_chore_picker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_chore_picker, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_chore_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_chore_picker, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_chore_picker, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *picker_title = label(g_chore_picker, "Choose a Home Assistant chore list",
                                   &lv_font_montserrat_24, theme().text, 620);
    lv_obj_set_pos(picker_title, 0, 0);
    lv_obj_t *picker_desc = label(
        g_chore_picker,
        "Select the Home Assistant todo.* entity that should back this dashboard. The selection is saved on the display.",
        &lv_font_montserrat_14, theme().muted, 900);
    lv_label_set_long_mode(picker_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(picker_desc, 0, 40);

    g_chore_picker_back = button(g_chore_picker, "Back", 100, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(g_chore_picker_back, 1110, 8);
    lv_obj_add_event_cb(g_chore_picker_back, chore_cancel_list_cb, LV_EVENT_CLICKED, nullptr);

    g_chore_picker_status = label(g_chore_picker, "", &lv_font_montserrat_18,
                                  theme().muted, 980);
    lv_label_set_long_mode(g_chore_picker_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_chore_picker_status, 0, 120);

    constexpr int PICKER_CARD_W = 580;
    constexpr int PICKER_CARD_H = 84;
    constexpr int PICKER_GAP_X = 28;
    constexpr int PICKER_GAP_Y = 14;
    constexpr int PICKER_START_Y = 104;
    for (size_t i = 0; i < HA_MAX_TODO_LISTS; ++i) {
        PickerCardWidgets &slot = g_chore_picker_cards[i];
        const int column = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        slot.card = lv_button_create(g_chore_picker);
        lv_obj_set_size(slot.card, PICKER_CARD_W, PICKER_CARD_H);
        lv_obj_set_pos(slot.card, column * (PICKER_CARD_W + PICKER_GAP_X),
                       PICKER_START_Y + row * (PICKER_CARD_H + PICKER_GAP_Y));
        style_box(slot.card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(slot.card, 14, LV_PART_MAIN);
        slot.name = label(slot.card, "", &lv_font_montserrat_18, theme().text, PICKER_CARD_W - 28);
        lv_label_set_long_mode(slot.name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.name, 0, 4);
        slot.entity = label(slot.card, "", &lv_font_montserrat_12, theme().muted, PICKER_CARD_W - 28);
        lv_label_set_long_mode(slot.entity, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.entity, 0, 42);
        lv_obj_add_event_cb(slot.card, chore_select_list_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
    }

    // Persistent chore list ---------------------------------------------------
    g_chore_main = lv_obj_create(g_chore_page);
    lv_obj_set_size(g_chore_main, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(g_chore_main, 0, 0);
    lv_obj_set_style_bg_opa(g_chore_main, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_chore_main, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_chore_main, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_chore_main, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_chore_main, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_chore_main, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = label(g_chore_main, "Home Assistant chores", &lv_font_montserrat_24,
                            theme().text, 420);
    lv_obj_set_pos(title, 0, 0);
    g_chore_source = label(g_chore_main, "", &lv_font_montserrat_12, theme().muted, 560);
    lv_label_set_long_mode(g_chore_source, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_chore_source, 0, 40);
    g_chore_progress_label = label(g_chore_main, "", &lv_font_montserrat_14, theme().muted, 650);
    lv_obj_set_pos(g_chore_progress_label, 0, 68);

    g_chore_progress = lv_bar_create(g_chore_main);
    lv_obj_set_size(g_chore_progress, 500, 14);
    lv_obj_set_pos(g_chore_progress, 0, 96);
    lv_obj_set_style_bg_color(g_chore_progress, lv_color_hex(theme().button), LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_chore_progress, lv_color_hex(theme().accent), LV_PART_INDICATOR);

    lv_obj_t *reset = button(g_chore_main, "Reset all", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(reset, 850, 18);
    lv_obj_add_event_cb(reset, chore_reset_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *refresh = button(g_chore_main, "Refresh", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(refresh, 972, 18);
    lv_obj_add_event_cb(refresh, chore_refresh_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *change = button(g_chore_main, "Change list", 132, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(change, 1094, 18);
    lv_obj_add_event_cb(change, chore_change_list_cb, LV_EVENT_CLICKED, nullptr);

    g_chore_empty = label(g_chore_main,
                          "No items are loaded from this Home Assistant to-do list yet. Tap Refresh if you just selected it.",
                          &lv_font_montserrat_18, theme().muted, 950);
    lv_label_set_long_mode(g_chore_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_chore_empty, 0, 156);

    constexpr int CARD_W = 580;
    constexpr int CARD_H = 96;
    constexpr int GAP_X = 28;
    constexpr int GAP_Y = 14;
    constexpr int START_Y = 136;
    for (size_t i = 0; i < HA_MAX_CHORES; ++i) {
        ChoreCardWidgets &slot = g_chore_cards[i];
        const int column = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        slot.card = lv_button_create(g_chore_main);
        lv_obj_set_size(slot.card, CARD_W, CARD_H);
        lv_obj_set_pos(slot.card, column * (CARD_W + GAP_X), START_Y + row * (CARD_H + GAP_Y));
        style_box(slot.card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(slot.card, 16, LV_PART_MAIN);
        slot.task = label(slot.card, "", &lv_font_montserrat_18, theme().text, CARD_W - 32);
        lv_label_set_long_mode(slot.task, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.task, 0, 8);
        slot.hint = label(slot.card, "", &lv_font_montserrat_12, theme().muted, CARD_W - 32);
        lv_label_set_long_mode(slot.hint, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.hint, 0, 52);
        lv_obj_add_event_cb(slot.card, chore_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
    }
}

void update_chores_dashboard() {
    if (!g_chore_page) return;
    const bool picker_mode = !chore_service_configured() || g_chore_choose_list;

    if (picker_mode) {
        lv_obj_remove_flag(g_chore_picker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_chore_main, LV_OBJ_FLAG_HIDDEN);

        /* Rendering is deliberately network-free.  Discovery is scheduled by
         * service_active_dashboard_auto_refresh() only after this page has had
         * time to reach the display. */
        if (chore_service_configured()) lv_obj_remove_flag(g_chore_picker_back, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_chore_picker_back, LV_OBJ_FLAG_HIDDEN);

        size_t count = 0;
        if (!home_assistant_todo_lists_ready()) {
            set_label_text(g_chore_picker_status, "Discovering Home Assistant to-do lists in the background...");
            lv_obj_remove_flag(g_chore_picker_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            count = home_assistant_todo_list_count();
            if (count > HA_MAX_TODO_LISTS) count = HA_MAX_TODO_LISTS;
            if (count == 0) {
                set_label_text(g_chore_picker_status,
                               "No todo.* entities were found. Create a To-do list in Home Assistant, then return here to discover it.");
                lv_obj_remove_flag(g_chore_picker_status, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_chore_picker_status, LV_OBJ_FLAG_HIDDEN);
            }
        }

        for (size_t i = 0; i < HA_MAX_TODO_LISTS; ++i) {
            PickerCardWidgets &slot = g_chore_picker_cards[i];
            if (i < count) {
                set_label_text(slot.name, home_assistant_todo_list_name(i));
                set_label_text(slot.entity, home_assistant_todo_list_entity(i));
                lv_obj_remove_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    lv_obj_add_flag(g_chore_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_chore_main, LV_OBJ_FLAG_HIDDEN);
    set_label_text(g_chore_source, chore_service_entity());

    size_t count = chore_service_count();
    if (count > HA_MAX_CHORES) count = HA_MAX_CHORES;
    const size_t completed = chore_service_completed_count();
    char progress_text[96];
    snprintf(progress_text, sizeof(progress_text), "%u of %u complete  |  auto-refreshes every 1 min while visible",
             static_cast<unsigned>(completed), static_cast<unsigned>(count));
    set_label_text(g_chore_progress_label, progress_text);
    lv_bar_set_range(g_chore_progress, 0, count > 0 ? static_cast<int32_t>(count) : 1);
    lv_bar_set_value(g_chore_progress, static_cast<int32_t>(completed), LV_ANIM_OFF);

    if (count == 0) lv_obj_remove_flag(g_chore_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g_chore_empty, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < HA_MAX_CHORES; ++i) {
        ChoreCardWidgets &slot = g_chore_cards[i];
        if (i >= count) {
            lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const bool done = chore_service_done(i);
        const ChoreItem *item = chore_service_item(i);
        char text[160];
        snprintf(text, sizeof(text), "%s  %s", done ? "[x]" : "[ ]", chore_service_name(i));
        set_label_text(slot.task, text);
        lv_obj_set_style_text_color(slot.task, lv_color_hex(done ? theme().accent : theme().text), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slot.card, lv_color_hex(done ? theme().accent_soft : theme().panel_alt), LV_PART_MAIN);
        const char *detail = (item && item->description[0])
                                 ? item->description
                                 : (done ? "Tap to mark incomplete" : "Tap to complete");
        set_label_text(slot.hint, detail);
        lv_obj_remove_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
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

void meal_card_clicked_cb(lv_event_t *e) {
    auto *widgets = static_cast<MealDayWidgets *>(lv_event_get_user_data(e));
    if (!widgets || !widgets->first_event) return;
    show_event_details_overlay(*widgets->first_event);
}

void create_meal_day_card(lv_obj_t *page, int day, int x, int y, int w, int h) {
    MealDayWidgets &widgets = g_meal_days[day];
    widgets.card = lv_obj_create(page);
    lv_obj_set_size(widgets.card, w, h);
    lv_obj_set_pos(widgets.card, x, y);
    style_box(widgets.card, theme().panel_alt, 12, 1);
    lv_obj_set_style_pad_all(widgets.card, 16, LV_PART_MAIN);

    widgets.heading = label(widgets.card, "", &lv_font_montserrat_16, theme().text, w - 32);
    lv_obj_set_pos(widgets.heading, 0, 0);
    for (int i = 0; i < 3; ++i) {
        widgets.meal_lines[i] = label(widgets.card, "", &lv_font_montserrat_14, theme().text, w - 32);
        lv_label_set_long_mode(widgets.meal_lines[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_height(widgets.meal_lines[i], 48);
        lv_obj_set_pos(widgets.meal_lines[i], 0, 40 + i * 52);
        lv_obj_add_flag(widgets.meal_lines[i], LV_OBJ_FLAG_HIDDEN);
    }
    widgets.empty = label(widgets.card, "No meal event on the calendar", &lv_font_montserrat_14,
                          theme().muted, w - 32);
    lv_label_set_long_mode(widgets.empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(widgets.empty, 0, 52);
    widgets.hint = label(widgets.card, "Tap for details", &lv_font_montserrat_12,
                         theme().muted, w - 32);
    lv_obj_align(widgets.hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(widgets.hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(widgets.card, meal_card_clicked_cb, LV_EVENT_CLICKED, &widgets);
    lv_obj_remove_flag(widgets.card, LV_OBJ_FLAG_CLICKABLE);
}

void create_meals_dashboard() {
    g_meals_page = lv_obj_create(g_screen);
    lv_obj_set_size(g_meals_page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(g_meals_page, 8, PAGE_Y);
    style_box(g_meals_page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(g_meals_page, 16, LV_PART_MAIN);

    lv_obj_t *title = label(g_meals_page, "Meals", &lv_font_montserrat_24, theme().text, 260);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *description = label(
        g_meals_page,
        "Automatically finds meal-like events in the selected Home Assistant calendar week. Calendar data refreshes every 5 min while visible.",
        &lv_font_montserrat_14, theme().muted, 820);
    lv_obj_set_pos(description, 0, 38);
    lv_obj_t *keywords = label(
        g_meals_page,
        "Looks for breakfast, brunch, lunch, dinner, supper, meal, restaurant, or cookout.",
        &lv_font_montserrat_12, theme().muted, 780);
    lv_obj_set_pos(keywords, 0, 64);

    constexpr int GAP = 12;
    constexpr int CARD_W = 292;
    constexpr int CARD_H = 242;
    constexpr int ROW1_Y = 100;
    constexpr int ROW2_Y = ROW1_Y + CARD_H + GAP;
    for (int day = 0; day < 7; ++day) {
        int x;
        int y;
        if (day < 4) {
            x = day * (CARD_W + GAP);
            y = ROW1_Y;
        } else {
            constexpr int ROW2_W = CARD_W * 3 + GAP * 2;
            const int row2_start = (SCREEN_W - 48 - ROW2_W) / 2;
            x = row2_start + (day - 4) * (CARD_W + GAP);
            y = ROW2_Y;
        }
        create_meal_day_card(g_meals_page, day, x, y, CARD_W, CARD_H);
    }
}

void update_meals_dashboard() {
    if (!g_meals_page) return;
    time_t week_start = time_service_start_of_week(time_service_now());
    week_start = time_service_add_days(week_start, g_week_offset * 7);
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    for (int day = 0; day < 7; ++day) {
        MealDayWidgets &widgets = g_meal_days[day];
        const time_t day_time = time_service_add_days(week_start, day);
        struct tm day_tm = {};
        localtime_r(&day_time, &day_tm);
        char heading[40];
        snprintf(heading, sizeof(heading), "%s  |  %s %d",
                 calendar_day_name(static_cast<uint8_t>(day)), months[day_tm.tm_mon], day_tm.tm_mday);
        set_label_text(widgets.heading, heading);

        int meal_count = 0;
        widgets.first_event = nullptr;
        for (size_t i = 0; i < CALENDAR_EVENT_COUNT && meal_count < 3; ++i) {
            const CalendarEvent &event = CALENDAR_EVENTS[i];
            if (!calendar_event_is_on_day(event, day_time) || !is_meal_event(event)) continue;
            if (!widgets.first_event) widgets.first_event = &event;
            char time_text[22];
            calendar_format_event_time(event, time_text, sizeof(time_text));
            char line[160];
            snprintf(line, sizeof(line), "%s  %s", time_text, event.title);
            set_label_text(widgets.meal_lines[meal_count], line);
            lv_obj_remove_flag(widgets.meal_lines[meal_count], LV_OBJ_FLAG_HIDDEN);
            ++meal_count;
        }
        for (int i = meal_count; i < 3; ++i) {
            lv_obj_add_flag(widgets.meal_lines[i], LV_OBJ_FLAG_HIDDEN);
        }

        if (meal_count == 0) {
            lv_obj_remove_flag(widgets.empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(widgets.hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widgets.card, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(widgets.empty, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(widgets.hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(widgets.card, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void weather_refresh_cb(lv_event_t *) {
    weather_service_request_refresh(true, "manual weather refresh");
    update_weather_dashboard();
}

uint32_t weather_condition_accent(WeatherCondition condition) {
    switch (condition) {
        case WeatherCondition::Clear: return 0xF59E0B;
        case WeatherCondition::PartlyCloudy: return 0x60A5FA;
        case WeatherCondition::Cloudy: return 0x94A3B8;
        case WeatherCondition::Rain:
        case WeatherCondition::Showers: return 0x3B82F6;
        case WeatherCondition::Thunderstorm: return 0xFACC15;
        case WeatherCondition::Snow: return 0x38BDF8;
        case WeatherCondition::Fog: return 0xCBD5E1;
        case WeatherCondition::Unknown: return theme().accent;
    }
    return theme().accent;
}

lv_obj_t *weather_centered_label(lv_obj_t *parent,
                                 const char *text,
                                 const lv_font_t *font,
                                 uint32_t color,
                                 int parent_width,
                                 int width,
                                 int y) {
    lv_obj_t *obj = label(parent, text, font, color, width);
    if (!obj) return nullptr;
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(obj, (parent_width - width) / 2, y);
    return obj;
}

void create_weather_metric_card(lv_obj_t *parent,
                                int x,
                                int y,
                                int w,
                                int h,
                                const char *caption,
                                uint32_t accent,
                                WeatherMetricWidgets &widgets) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    style_box(card, theme().panel, 12, 1);
    icon_piece(card, 14, 14, 10, 10, accent, 5);
    lv_obj_t *cap = label(card, caption, &lv_font_montserrat_12, theme().muted, w - 46);
    set_pos_if(cap, 32, 10);
    widgets.value = label(card, "--", &lv_font_montserrat_18, theme().text, w - 28);
    if (widgets.value) lv_label_set_long_mode(widgets.value, LV_LABEL_LONG_DOT);
    set_pos_if(widgets.value, 14, 42);
}

void create_weather_daily_card(lv_obj_t *parent,
                               int x,
                               int y,
                               int w,
                               int h,
                               WeatherDailyWidgets &widgets) {
    widgets.card = lv_obj_create(parent);
    lv_obj_set_size(widgets.card, w, h);
    lv_obj_set_pos(widgets.card, x, y);
    style_box(widgets.card, theme().panel_alt, 12, 1);
    widgets.accent = icon_piece(widgets.card, 0, 0, w, 4, theme().accent, 2);
    widgets.day = weather_centered_label(widgets.card, "---", &lv_font_montserrat_20,
                                         theme().text, w, w - 16, 7);
    widgets.icon = create_weather_image(widgets.card, 64);
    if (widgets.icon) lv_obj_set_pos(widgets.icon, (w - 64) / 2, 30);
    widgets.condition = weather_centered_label(widgets.card, "--", &lv_font_montserrat_12,
                                               theme().muted, w, w - 16, 94);
    if (widgets.condition) lv_label_set_long_mode(widgets.condition, LV_LABEL_LONG_DOT);
    widgets.temps = weather_centered_label(widgets.card, "-- / --", &lv_font_montserrat_20,
                                           theme().accent, w, w - 12, 116);
}

void create_weather_hourly_card(lv_obj_t *parent,
                                int x,
                                int y,
                                int w,
                                int h,
                                WeatherHourlyWidgets &widgets) {
    widgets.card = lv_obj_create(parent);
    lv_obj_set_size(widgets.card, w, h);
    lv_obj_set_pos(widgets.card, x, y);
    style_box(widgets.card, theme().panel_alt, 10, 1);
    widgets.accent = icon_piece(widgets.card, 0, 0, w, 3, theme().accent, 1);
    widgets.time = label(widgets.card, "--", &lv_font_montserrat_12, theme().muted, w - 12);
    if (widgets.time) lv_obj_set_style_text_align(widgets.time, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    set_pos_if(widgets.time, 6, 6);
    widgets.icon = create_weather_image(widgets.card, 44);
    set_pos_if(widgets.icon, 7, 29);
    widgets.temp = label(widgets.card, "--", &lv_font_montserrat_18, theme().text, w - 58);
    set_pos_if(widgets.temp, 56, 29);
    widgets.precip = label(widgets.card, "", &lv_font_montserrat_12, 0x60A5FA, w - 58);
    set_pos_if(widgets.precip, 56, 56);
}

void create_weather_dashboard() {
    constexpr int CONTENT_W = 1228;
    constexpr int HERO_Y = 48;
    constexpr int HERO_H = 150;

    g_weather_widgets = {};
    g_weather_widgets.page = lv_obj_create(g_screen);
    lv_obj_set_size(g_weather_widgets.page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(g_weather_widgets.page, 8, PAGE_Y);
    style_box(g_weather_widgets.page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(g_weather_widgets.page, 12, LV_PART_MAIN);
    lv_obj_remove_flag(g_weather_widgets.page, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(g_weather_widgets.page, "Weather", &lv_font_montserrat_24,
                            theme().text, 240);
    set_pos_if(title, 0, 0);
    g_weather_widgets.updated = label(g_weather_widgets.page, "", &lv_font_montserrat_12,
                                      theme().muted, 500);
    set_pos_if(g_weather_widgets.updated, 250, 8);
    lv_obj_t *refresh = button(g_weather_widgets.page, "Refresh", 104, 36, false,
                               &lv_font_montserrat_14);
    set_pos_if(refresh, 1124, 0);
    lv_obj_add_event_cb(refresh, weather_refresh_cb, LV_EVENT_CLICKED, nullptr);

    g_weather_widgets.message = label(g_weather_widgets.page, "", &lv_font_montserrat_20,
                                      theme().muted, 1120);
    lv_label_set_long_mode(g_weather_widgets.message, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_weather_widgets.message, 140);
    set_pos_if(g_weather_widgets.message, 18, 88);

    g_weather_widgets.content = lv_obj_create(g_weather_widgets.page);
    lv_obj_set_size(g_weather_widgets.content, CONTENT_W, PAGE_H - 60);
    lv_obj_set_pos(g_weather_widgets.content, 0, 40);
    lv_obj_set_style_bg_opa(g_weather_widgets.content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_weather_widgets.content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_weather_widgets.content, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_weather_widgets.content, LV_OBJ_FLAG_SCROLLABLE);

    // Current conditions hero -------------------------------------------------
    lv_obj_t *hero = lv_obj_create(g_weather_widgets.content);
    lv_obj_set_size(hero, CONTENT_W, HERO_H);
    lv_obj_set_pos(hero, 0, HERO_Y - 40);
    style_box(hero, theme().panel_alt, 16, 1);
    g_weather_widgets.hero_accent = icon_piece(hero, 0, 0, CONTENT_W, 5, theme().accent, 2);
    g_weather_widgets.hero_icon = create_weather_image(hero, 96);
    set_pos_if(g_weather_widgets.hero_icon, 24, 27);
    g_weather_widgets.hero_temp = label(hero, "--", &lv_font_montserrat_28, theme().text, 210);
    set_pos_if(g_weather_widgets.hero_temp, 142, 25);
    g_weather_widgets.hero_condition = label(hero, "--", &lv_font_montserrat_18, theme().muted, 235);
    set_pos_if(g_weather_widgets.hero_condition, 144, 66);
    g_weather_widgets.hero_range = label(hero, "--", &lv_font_montserrat_16, theme().text, 235);
    set_pos_if(g_weather_widgets.hero_range, 144, 100);

    constexpr int METRIC_X = 398;
    constexpr int METRIC_Y = 23;
    constexpr int METRIC_W = 196;
    constexpr int METRIC_H = 104;
    constexpr int METRIC_GAP = 10;
    create_weather_metric_card(hero, METRIC_X + 0 * (METRIC_W + METRIC_GAP), METRIC_Y,
                               METRIC_W, METRIC_H, "HUMIDITY", 0x38BDF8, g_weather_widgets.metrics[0]);
    create_weather_metric_card(hero, METRIC_X + 1 * (METRIC_W + METRIC_GAP), METRIC_Y,
                               METRIC_W, METRIC_H, "WIND", 0x60A5FA, g_weather_widgets.metrics[1]);
    create_weather_metric_card(hero, METRIC_X + 2 * (METRIC_W + METRIC_GAP), METRIC_Y,
                               METRIC_W, METRIC_H, "PRECIP", 0x3B82F6, g_weather_widgets.metrics[2]);
    create_weather_metric_card(hero, METRIC_X + 3 * (METRIC_W + METRIC_GAP), METRIC_Y,
                               METRIC_W, METRIC_H, "PRESSURE", 0xA78BFA, g_weather_widgets.metrics[3]);

    // Six-day forecast ---------------------------------------------------------
    constexpr int DAILY_HEADING_Y = 166;
    constexpr int DAILY_Y = 188;
    constexpr int DAILY_H = 148;
    constexpr int DAILY_W = 198;
    constexpr int DAILY_GAP = 8;
    g_weather_widgets.daily_heading = label(g_weather_widgets.content, "Daily",
                                             &lv_font_montserrat_18, theme().text, 160);
    set_pos_if(g_weather_widgets.daily_heading, 0, DAILY_HEADING_Y);
    const int daily_row_w = 6 * DAILY_W + 5 * DAILY_GAP;
    const int daily_start_x = (CONTENT_W - daily_row_w) / 2;
    for (int i = 0; i < 6; ++i) {
        create_weather_daily_card(g_weather_widgets.content,
                                  daily_start_x + i * (DAILY_W + DAILY_GAP), DAILY_Y,
                                  DAILY_W, DAILY_H, g_weather_widgets.daily[i]);
    }

    // 18-hour forecast ---------------------------------------------------------
    constexpr int HOURLY_HEADING_Y = 348;
    constexpr int HOURLY_Y = 370;
    constexpr int HOURLY_W = 128;
    constexpr int HOURLY_H = 84;
    constexpr int HOURLY_GAP = 8;
    constexpr int HOURLY_ROW_GAP = 7;
    constexpr int HOURLY_PER_ROW = 9;
    g_weather_widgets.hourly_heading = label(g_weather_widgets.content, "Next 18 hours",
                                              &lv_font_montserrat_18, theme().text, 220);
    set_pos_if(g_weather_widgets.hourly_heading, 0, HOURLY_HEADING_Y);
    const int hourly_row_w = HOURLY_PER_ROW * HOURLY_W + (HOURLY_PER_ROW - 1) * HOURLY_GAP;
    const int hourly_start_x = (CONTENT_W - hourly_row_w) / 2;
    for (int i = 0; i < 18; ++i) {
        const int row = i / HOURLY_PER_ROW;
        const int col = i % HOURLY_PER_ROW;
        create_weather_hourly_card(g_weather_widgets.content,
                                   hourly_start_x + col * (HOURLY_W + HOURLY_GAP),
                                   HOURLY_Y + row * (HOURLY_H + HOURLY_ROW_GAP),
                                   HOURLY_W, HOURLY_H, g_weather_widgets.hourly[i]);
    }
}

void update_weather_dashboard() {
    if (!g_weather_widgets.page) return;
    weather_service_get_snapshot(g_weather_snapshot);
    const WeatherSnapshot &weather = g_weather_snapshot;

    char updated_text[128];
    if (weather.last_updated_epoch > 0) {
        char updated_time[24] = "--";
        time_service_format_time(weather.last_updated_epoch, updated_time, sizeof(updated_time));
        snprintf(updated_text, sizeof(updated_text), "Updated %s%s",
                 updated_time, weather.in_progress ? "  |  refreshing" : "");
    } else {
        snprintf(updated_text, sizeof(updated_text), "%s", weather.status);
    }
    set_label_text(g_weather_widgets.updated, updated_text);

    if (!weather.configured) {
        lv_obj_add_flag(g_weather_widgets.content, LV_OBJ_FLAG_HIDDEN);
        set_label_text(g_weather_widgets.message,
                       "Weather is not configured. Install the 2.2.1 Home Assistant weather snapshot script and set the current/daily and hourly weather entities in Web Management.");
        lv_obj_remove_flag(g_weather_widgets.message, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (!weather.has_data) {
        lv_obj_add_flag(g_weather_widgets.content, LV_OBJ_FLAG_HIDDEN);
        set_label_text(g_weather_widgets.message,
                       weather.in_progress ? "Loading weather snapshot..." : "Weather refresh will start after the page settles...");
        lv_obj_remove_flag(g_weather_widgets.message, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(g_weather_widgets.message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_weather_widgets.content, LV_OBJ_FLAG_HIDDEN);

    const uint32_t hero_accent = weather_condition_accent(weather.current.condition);
    if (g_weather_widgets.hero_accent) {
        lv_obj_set_style_bg_color(g_weather_widgets.hero_accent, lv_color_hex(hero_accent), LV_PART_MAIN);
    }
    set_weather_image(g_weather_widgets.hero_icon, weather.current.condition, weather.current.symbol_code);

    char buffer[96];
    if (isfinite(weather.current.temperature_c)) {
        snprintf(buffer, sizeof(buffer), "%d %s", rounded_display_temp(weather.current.temperature_c),
                 weather_temperature_unit());
    } else {
        snprintf(buffer, sizeof(buffer), "-- %s", weather_temperature_unit());
    }
    set_label_text(g_weather_widgets.hero_temp, buffer);
    set_label_text(g_weather_widgets.hero_condition, weather_condition_label(weather.current.condition));
    if (isfinite(weather.current.today_high_c) && isfinite(weather.current.today_low_c)) {
        snprintf(buffer, sizeof(buffer), "H %d   L %d %s",
                 rounded_display_temp(weather.current.today_high_c),
                 rounded_display_temp(weather.current.today_low_c), weather_temperature_unit());
    } else {
        snprintf(buffer, sizeof(buffer), "H --   L -- %s", weather_temperature_unit());
    }
    set_label_text(g_weather_widgets.hero_range, buffer);

    if (weather.current.humidity_pct >= 0) snprintf(buffer, sizeof(buffer), "%d%%", weather.current.humidity_pct);
    else snprintf(buffer, sizeof(buffer), "--");
    set_label_text(g_weather_widgets.metrics[0].value, buffer);

    if (isfinite(weather.current.wind_mps)) {
        snprintf(buffer, sizeof(buffer), "%.1f %s %s", weather_display_wind(weather.current.wind_mps),
                 weather_wind_unit(), wind_direction_text(weather.current.wind_direction_deg));
    } else snprintf(buffer, sizeof(buffer), "--");
    set_label_text(g_weather_widgets.metrics[1].value, buffer);

    if (weather.current.precipitation_probability_pct >= 0) {
        snprintf(buffer, sizeof(buffer), "%.2f %s  %d%%", weather_display_precip(weather.current.precipitation_mm),
                 weather_precip_unit(), weather.current.precipitation_probability_pct);
    } else {
        snprintf(buffer, sizeof(buffer), "%.2f %s", weather_display_precip(weather.current.precipitation_mm),
                 weather_precip_unit());
    }
    set_label_text(g_weather_widgets.metrics[2].value, buffer);

    if (isfinite(weather.current.pressure_hpa)) {
#if WEATHER_USE_IMPERIAL
        snprintf(buffer, sizeof(buffer), "%.2f %s", weather_display_pressure(weather.current.pressure_hpa),
                 weather_pressure_unit());
#else
        snprintf(buffer, sizeof(buffer), "%.0f %s", weather_display_pressure(weather.current.pressure_hpa),
                 weather_pressure_unit());
#endif
    } else snprintf(buffer, sizeof(buffer), "--");
    set_label_text(g_weather_widgets.metrics[3].value, buffer);

    static const char *short_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    size_t daily_count = weather.daily_count > 6 ? 6 : weather.daily_count;
    for (size_t i = 0; i < 6; ++i) {
        WeatherDailyWidgets &widgets = g_weather_widgets.daily[i];
        if (i >= daily_count || !weather.daily[i].valid) {
            lv_obj_add_flag(widgets.card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const WeatherDayForecast &day = weather.daily[i];
        struct tm day_tm = {};
        char day_name[12] = "Day";
        if (localtime_r(&day.local_day_epoch, &day_tm) && day_tm.tm_wday >= 0 && day_tm.tm_wday <= 6) {
            snprintf(day_name, sizeof(day_name), "%s", short_days[day_tm.tm_wday]);
        }
        set_label_text(widgets.day, day_name);
        set_weather_image(widgets.icon, day.condition, day.symbol_code);
        set_label_text(widgets.condition, weather_condition_label(day.condition));
        if (isfinite(day.high_c) && isfinite(day.low_c)) {
            snprintf(buffer, sizeof(buffer), "%d / %d %s", rounded_display_temp(day.high_c),
                     rounded_display_temp(day.low_c), weather_temperature_unit());
        } else snprintf(buffer, sizeof(buffer), "-- / -- %s", weather_temperature_unit());
        set_label_text(widgets.temps, buffer);
        const uint32_t accent = weather_condition_accent(day.condition);
        lv_obj_set_style_bg_color(widgets.accent, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_set_style_text_color(widgets.temps, lv_color_hex(accent), LV_PART_MAIN);
        lv_obj_remove_flag(widgets.card, LV_OBJ_FLAG_HIDDEN);
    }

    size_t hourly_count = weather.hourly_count > 18 ? 18 : weather.hourly_count;
    for (size_t i = 0; i < 18; ++i) {
        WeatherHourlyWidgets &widgets = g_weather_widgets.hourly[i];
        if (i >= hourly_count || !weather.hourly[i].valid) {
            lv_obj_add_flag(widgets.card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const WeatherHourForecast &hour = weather.hourly[i];
        char time_text[16];
        time_service_format_time(hour.epoch, time_text, sizeof(time_text));
        set_label_text(widgets.time, time_text);
        set_weather_image(widgets.icon, hour.condition, hour.symbol_code);
        if (isfinite(hour.temperature_c)) snprintf(buffer, sizeof(buffer), "%d %s",
                                                    rounded_display_temp(hour.temperature_c), weather_temperature_unit());
        else snprintf(buffer, sizeof(buffer), "-- %s", weather_temperature_unit());
        set_label_text(widgets.temp, buffer);
        if (hour.precipitation_probability_pct >= 0) {
            snprintf(buffer, sizeof(buffer), "%d%% rain", hour.precipitation_probability_pct);
        } else if (hour.precipitation_mm > 0.0f) {
            snprintf(buffer, sizeof(buffer), "%.2f %s", weather_display_precip(hour.precipitation_mm), weather_precip_unit());
        } else {
            buffer[0] = '\0';
        }
        set_label_text(widgets.precip, buffer);
        lv_obj_set_style_bg_color(widgets.accent,
                                  lv_color_hex(weather_condition_accent(hour.condition)), LV_PART_MAIN);
        lv_obj_remove_flag(widgets.card, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI("FamilyCalendar",
             "[WeatherUI] Updated in place: daily=%u hourly=%u free heap=%u free PSRAM=%u",
             static_cast<unsigned>(daily_count), static_cast<unsigned>(hourly_count),
             static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()));
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

void set_alarm_button_enabled(lv_obj_t *btn, bool enabled) {
    if (!btn) return;
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_40, LV_PART_MAIN);
    }
}

void create_alarm_dashboard() {
    g_alarm_widgets = {};
    g_alarm_widgets.page = lv_obj_create(g_screen);
    lv_obj_set_size(g_alarm_widgets.page, SCREEN_W - 16, PAGE_H);
    lv_obj_set_pos(g_alarm_widgets.page, 8, PAGE_Y);
    style_box(g_alarm_widgets.page, theme().panel, 14, 1);
    lv_obj_set_style_pad_all(g_alarm_widgets.page, 18, LV_PART_MAIN);
    lv_obj_remove_flag(g_alarm_widgets.page, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int CONTENT_W = 1226;
    constexpr int CONTENT_H = PAGE_H - 36;

    // Persistent panel picker -------------------------------------------------
    g_alarm_widgets.picker = lv_obj_create(g_alarm_widgets.page);
    lv_obj_set_size(g_alarm_widgets.picker, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(g_alarm_widgets.picker, 0, 0);
    lv_obj_set_style_bg_opa(g_alarm_widgets.picker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alarm_widgets.picker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alarm_widgets.picker, 0, LV_PART_MAIN);
    lv_obj_add_flag(g_alarm_widgets.picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_alarm_widgets.picker, LV_DIR_VER);

    lv_obj_t *picker_title = label(g_alarm_widgets.picker, "Choose an Alarmo panel",
                                   &lv_font_montserrat_24, theme().text, 620);
    lv_obj_set_pos(picker_title, 0, 0);
    lv_obj_t *picker_desc = label(
        g_alarm_widgets.picker,
        "Select the Home Assistant alarm_control_panel entity managed by Alarmo. The selection is saved on this display.",
        &lv_font_montserrat_14, theme().muted, 940);
    lv_label_set_long_mode(picker_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(picker_desc, 0, 40);
    g_alarm_widgets.picker_back = button(g_alarm_widgets.picker, "Back", 100, 40, false,
                                         &lv_font_montserrat_14);
    lv_obj_set_pos(g_alarm_widgets.picker_back, 1110, 8);
    lv_obj_add_event_cb(g_alarm_widgets.picker_back, alarm_cancel_panel_cb, LV_EVENT_CLICKED, nullptr);
    g_alarm_widgets.picker_status = label(g_alarm_widgets.picker, "", &lv_font_montserrat_18,
                                          theme().muted, 980);
    lv_label_set_long_mode(g_alarm_widgets.picker_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_alarm_widgets.picker_status, 0, 120);

    constexpr int PICK_W = 580;
    constexpr int PICK_H = 84;
    constexpr int PICK_GX = 28;
    constexpr int PICK_GY = 14;
    constexpr int PICK_Y = 104;
    for (size_t i = 0; i < HA_MAX_ALARM_PANELS; ++i) {
        PickerCardWidgets &slot = g_alarm_widgets.picker_cards[i];
        const int col = static_cast<int>(i % 2);
        const int row = static_cast<int>(i / 2);
        slot.card = lv_button_create(g_alarm_widgets.picker);
        lv_obj_set_size(slot.card, PICK_W, PICK_H);
        lv_obj_set_pos(slot.card, col * (PICK_W + PICK_GX), PICK_Y + row * (PICK_H + PICK_GY));
        style_box(slot.card, theme().panel_alt, 12, 1);
        lv_obj_set_style_pad_all(slot.card, 14, LV_PART_MAIN);
        slot.name = label(slot.card, "", &lv_font_montserrat_18, theme().text, PICK_W - 28);
        lv_label_set_long_mode(slot.name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.name, 0, 4);
        slot.entity = label(slot.card, "", &lv_font_montserrat_12, theme().muted, PICK_W - 28);
        lv_label_set_long_mode(slot.entity, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(slot.entity, 0, 42);
        lv_obj_add_event_cb(slot.card, alarm_select_panel_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
    }

    // Persistent loading state ------------------------------------------------
    g_alarm_widgets.loading = lv_obj_create(g_alarm_widgets.page);
    lv_obj_set_size(g_alarm_widgets.loading, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(g_alarm_widgets.loading, 0, 0);
    lv_obj_set_style_bg_opa(g_alarm_widgets.loading, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alarm_widgets.loading, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alarm_widgets.loading, 0, LV_PART_MAIN);
    lv_obj_t *loading_title = label(g_alarm_widgets.loading, "Alarm & Security",
                                    &lv_font_montserrat_24, theme().text, 300);
    lv_obj_set_pos(loading_title, 0, 0);
    lv_obj_t *loading_text = label(g_alarm_widgets.loading, "Loading alarm state in the background...",
                                   &lv_font_montserrat_18, theme().muted, 760);
    lv_obj_set_pos(loading_text, 0, 70);
    lv_obj_t *loading_change = button(g_alarm_widgets.loading, "Change panel", 140, 40,
                                      false, &lv_font_montserrat_14);
    lv_obj_set_pos(loading_change, 1060, 6);
    lv_obj_add_event_cb(loading_change, alarm_change_panel_cb, LV_EVENT_CLICKED, nullptr);

    // Persistent alarm dashboard ---------------------------------------------
    g_alarm_widgets.main = lv_obj_create(g_alarm_widgets.page);
    lv_obj_set_size(g_alarm_widgets.main, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(g_alarm_widgets.main, 0, 0);
    lv_obj_set_style_bg_opa(g_alarm_widgets.main, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_alarm_widgets.main, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_alarm_widgets.main, 0, LV_PART_MAIN);
    lv_obj_remove_flag(g_alarm_widgets.main, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(g_alarm_widgets.main, "Alarm & Security", &lv_font_montserrat_24,
                            theme().text, 260);
    lv_obj_set_pos(title, 0, 0);
    lv_obj_t *refresh = button(g_alarm_widgets.main, "Refresh", 110, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(refresh, 958, 4);
    lv_obj_add_event_cb(refresh, alarm_refresh_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *change = button(g_alarm_widgets.main, "Change panel", 140, 40, false, &lv_font_montserrat_14);
    lv_obj_set_pos(change, 1082, 4);
    lv_obj_add_event_cb(change, alarm_change_panel_cb, LV_EVENT_CLICKED, nullptr);

    constexpr int STATUS_W = 430;
    constexpr int CONTROLS_X = 450;
    constexpr int CONTROLS_W = 772;
    constexpr int CARD_Y = 78;
    constexpr int CARD_H = 512;

    lv_obj_t *status_card = lv_obj_create(g_alarm_widgets.main);
    lv_obj_set_size(status_card, STATUS_W, CARD_H);
    lv_obj_set_pos(status_card, 0, CARD_Y);
    style_box(status_card, theme().panel_alt, 16, 1);
    lv_obj_set_style_pad_all(status_card, 22, LV_PART_MAIN);
    lv_obj_t *state_caption = label(status_card, "STATUS", &lv_font_montserrat_14,
                                    theme().muted, STATUS_W - 44);
    lv_obj_set_pos(state_caption, 0, 0);
    g_alarm_widgets.state = label(status_card, "--", &lv_font_montserrat_28,
                                  theme().muted, STATUS_W - 44);
    lv_label_set_long_mode(g_alarm_widgets.state, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_alarm_widgets.state, 0, 34);
    g_alarm_widgets.next = label(status_card, "", &lv_font_montserrat_14,
                                 theme().muted, STATUS_W - 44);
    lv_label_set_long_mode(g_alarm_widgets.next, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_alarm_widgets.next, 48);
    lv_obj_set_pos(g_alarm_widgets.next, 0, 88);
    lv_obj_t *sensor_caption = label(status_card, "ACTIVE SENSORS", &lv_font_montserrat_14,
                                     theme().muted, STATUS_W - 44);
    lv_obj_set_pos(sensor_caption, 0, 128);
    g_alarm_widgets.sensors = label(status_card, "", &lv_font_montserrat_16,
                                    theme().success, STATUS_W - 44);
    lv_label_set_long_mode(g_alarm_widgets.sensors, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_alarm_widgets.sensors, 150);
    lv_obj_set_pos(g_alarm_widgets.sensors, 0, 160);
    lv_obj_t *last_caption = label(status_card, "LAST TRIGGERED", &lv_font_montserrat_14,
                                   theme().muted, STATUS_W - 44);
    lv_obj_set_pos(last_caption, 0, 336);
    g_alarm_widgets.last = label(status_card, "", &lv_font_montserrat_16, theme().text, STATUS_W - 44);
    lv_obj_set_pos(g_alarm_widgets.last, 0, 368);
    g_alarm_widgets.code = label(status_card, "", &lv_font_montserrat_12, theme().muted, STATUS_W - 44);
    lv_label_set_long_mode(g_alarm_widgets.code, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_alarm_widgets.code, 0, 430);

    lv_obj_t *controls = lv_obj_create(g_alarm_widgets.main);
    lv_obj_set_size(controls, CONTROLS_W, CARD_H);
    lv_obj_set_pos(controls, CONTROLS_X, CARD_Y);
    style_box(controls, theme().panel_alt, 16, 1);
    lv_obj_set_style_pad_all(controls, 22, LV_PART_MAIN);
    lv_obj_t *control_title = label(controls, "ALARM CONTROLS", &lv_font_montserrat_14,
                                    theme().muted, CONTROLS_W - 44);
    lv_obj_set_pos(control_title, 0, 0);
    lv_obj_t *hint = label(controls,
        "Commands are sent to Alarmo in the background. Open sensors are never force-bypassed from this screen.",
        &lv_font_montserrat_14, theme().muted, CONTROLS_W - 44);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(hint, 48);
    lv_obj_set_pos(hint, 0, 30);

    constexpr int BW = 338;
    constexpr int BH = 74;
    constexpr int GX = 18;
    constexpr int BY = 104;
    constexpr int GY = 18;
    g_alarm_widgets.disarm = button(controls, "Disarm", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(g_alarm_widgets.disarm, 0, BY);
    lv_obj_add_event_cb(g_alarm_widgets.disarm, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::Disarm)));
    g_alarm_widgets.home = button(controls, "Arm Home", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(g_alarm_widgets.home, BW + GX, BY);
    lv_obj_add_event_cb(g_alarm_widgets.home, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmHome)));
    g_alarm_widgets.away = button(controls, "Arm Away", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(g_alarm_widgets.away, 0, BY + BH + GY);
    lv_obj_add_event_cb(g_alarm_widgets.away, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmAway)));
    g_alarm_widgets.night = button(controls, "Arm Night", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(g_alarm_widgets.night, BW + GX, BY + BH + GY);
    lv_obj_add_event_cb(g_alarm_widgets.night, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmNight)));
    g_alarm_widgets.vacation = button(controls, "Arm Vacation", BW, BH, false, &lv_font_montserrat_20);
    lv_obj_set_pos(g_alarm_widgets.vacation, 0, BY + (BH + GY) * 2);
    lv_obj_add_event_cb(g_alarm_widgets.vacation, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::ArmVacation)));
    g_alarm_widgets.skip = button(controls, "Skip exit delay", BW, BH, true, &lv_font_montserrat_18);
    lv_obj_set_pos(g_alarm_widgets.skip, BW + GX, BY + (BH + GY) * 2);
    lv_obj_add_event_cb(g_alarm_widgets.skip, alarm_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(AlarmUiAction::SkipDelay)));
    g_alarm_widgets.skip_info = label(controls, "", &lv_font_montserrat_14, theme().muted, BW);
    lv_label_set_long_mode(g_alarm_widgets.skip_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(g_alarm_widgets.skip_info, BH);
    lv_obj_set_pos(g_alarm_widgets.skip_info, BW + GX, BY + (BH + GY) * 2 + 10);
    g_alarm_widgets.entity = label(controls, "", &lv_font_montserrat_12, theme().muted, CONTROLS_W - 44);
    lv_label_set_long_mode(g_alarm_widgets.entity, LV_LABEL_LONG_DOT);
    lv_obj_align(g_alarm_widgets.entity, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

void update_alarm_dashboard() {
    if (!g_alarm_widgets.page) return;
    const bool picker_mode = !alarm_service_configured() || g_alarm_choose_panel;

    if (picker_mode) {
        lv_obj_remove_flag(g_alarm_widgets.picker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_alarm_widgets.loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_alarm_widgets.main, LV_OBJ_FLAG_HIDDEN);

        /* Keep widget updates local.  Alarm-panel discovery is started only
         * after the rendered page has had the normal tab-settle interval. */
        if (alarm_service_configured()) lv_obj_remove_flag(g_alarm_widgets.picker_back, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_alarm_widgets.picker_back, LV_OBJ_FLAG_HIDDEN);

        size_t count = 0;
        if (!home_assistant_alarm_panels_ready()) {
            set_label_text(g_alarm_widgets.picker_status, "Discovering Alarmo panels in Home Assistant...");
            lv_obj_remove_flag(g_alarm_widgets.picker_status, LV_OBJ_FLAG_HIDDEN);
        } else {
            count = home_assistant_alarm_panel_count();
            if (count > HA_MAX_ALARM_PANELS) count = HA_MAX_ALARM_PANELS;
            if (count == 0) {
                set_label_text(g_alarm_widgets.picker_status,
                               "No alarm_control_panel entities were found. Confirm Alarmo is loaded in Home Assistant, then try again.");
                lv_obj_remove_flag(g_alarm_widgets.picker_status, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(g_alarm_widgets.picker_status, LV_OBJ_FLAG_HIDDEN);
            }
        }
        for (size_t i = 0; i < HA_MAX_ALARM_PANELS; ++i) {
            PickerCardWidgets &slot = g_alarm_widgets.picker_cards[i];
            if (i < count) {
                set_label_text(slot.name, home_assistant_alarm_panel_name(i));
                set_label_text(slot.entity, home_assistant_alarm_panel_entity(i));
                lv_obj_remove_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(slot.card, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    lv_obj_add_flag(g_alarm_widgets.picker, LV_OBJ_FLAG_HIDDEN);
    const AlarmoSnapshot *snapshot = alarm_service_snapshot();
    if (!snapshot || !snapshot->valid) {
        lv_obj_remove_flag(g_alarm_widgets.loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_alarm_widgets.main, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(g_alarm_widgets.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(g_alarm_widgets.main, LV_OBJ_FLAG_HIDDEN);
    set_label_text(g_alarm_widgets.state, friendly_alarm_state(snapshot->state));
    lv_obj_set_style_text_color(g_alarm_widgets.state, lv_color_hex(alarm_state_color(snapshot->state)), LV_PART_MAIN);

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
    set_label_text(g_alarm_widgets.next, transition);

    char sensors[380] = {};
    if (snapshot->active_sensor_count == 0) {
        snprintf(sensors, sizeof(sensors), "All sensors clear");
    } else {
        const unsigned total = snapshot->active_sensor_total > 0
                                   ? static_cast<unsigned>(snapshot->active_sensor_total)
                                   : static_cast<unsigned>(snapshot->active_sensor_count);
        snprintf(sensors, sizeof(sensors), "%u active", total);
        constexpr uint16_t MAX_DISPLAYED_SENSORS = 5;
        const uint16_t display_count = snapshot->active_sensor_count < MAX_DISPLAYED_SENSORS
                                           ? snapshot->active_sensor_count
                                           : MAX_DISPLAYED_SENSORS;
        for (uint16_t i = 0; i < display_count; ++i) {
            const AlarmActiveSensor &sensor = snapshot->active_sensors[i];
            const bool motion = strcmp(sensor.device_class, "motion") == 0;
            char line[128];
            snprintf(line, sizeof(line), "\n%s  -  %s",
                     sensor.name[0] ? sensor.name : sensor.entity_id, motion ? "MOTION" : "OPEN");
            strlcat(sensors, line, sizeof(sensors));
        }
        if (total > display_count) {
            char more[40];
            snprintf(more, sizeof(more), "\n+%u more", total - display_count);
            strlcat(sensors, more, sizeof(sensors));
        }
    }
    set_label_text(g_alarm_widgets.sensors, sensors);
    lv_obj_set_style_text_color(g_alarm_widgets.sensors,
                                lv_color_hex(snapshot->active_sensor_count > 0
                                                 ? (g_dark_mode ? 0xFBBF24 : 0xB45309)
                                                 : theme().success),
                                LV_PART_MAIN);
    set_label_text(g_alarm_widgets.last,
                   snapshot->last_triggered[0] ? snapshot->last_triggered : "No trigger recorded");
    set_label_text(g_alarm_widgets.code,
                   alarm_service_code_required() ? "PIN/code required for the next command"
                                                 : "No code required for the next command");
    set_label_text(g_alarm_widgets.entity, alarm_service_entity());

    const bool disarmed = strcmp(snapshot->state, "disarmed") == 0;
    set_alarm_button_enabled(g_alarm_widgets.disarm, !disarmed);
    lv_obj_set_style_bg_color(g_alarm_widgets.disarm,
                              lv_color_hex(!disarmed ? theme().accent : theme().button), LV_PART_MAIN);
    set_alarm_button_enabled(g_alarm_widgets.home, (snapshot->supported_features & 1U) != 0);
    set_alarm_button_enabled(g_alarm_widgets.away, (snapshot->supported_features & 2U) != 0);
    set_alarm_button_enabled(g_alarm_widgets.night, (snapshot->supported_features & 4U) != 0);
    set_alarm_button_enabled(g_alarm_widgets.vacation, (snapshot->supported_features & 32U) != 0);

    if (strcmp(snapshot->state, "arming") == 0) {
        lv_obj_remove_flag(g_alarm_widgets.skip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_alarm_widgets.skip_info, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_alarm_widgets.skip, LV_OBJ_FLAG_HIDDEN);
        set_label_text(g_alarm_widgets.skip_info,
                       strcmp(snapshot->state, "pending") == 0
                           ? "Entry delay active - disarm to cancel the pending alarm."
                           : "Exit-delay control appears here while Alarmo is arming.");
        lv_obj_remove_flag(g_alarm_widgets.skip_info, LV_OBJ_FLAG_HIDDEN);
    }
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
    if (!g_home_network || !g_home_ha || !g_home_calendar || !g_home_system) return;

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
    BatteryStatus battery = {};
    const bool battery_valid = battery_service_get_status(battery);
    char battery_text[64];
    if (battery_valid) {
        snprintf(battery_text, sizeof(battery_text), "%u%% / %.2f V",
                 static_cast<unsigned>(battery.percent),
                 static_cast<double>(battery.voltage_v));
    } else {
        snprintf(battery_text, sizeof(battery_text), "unavailable");
    }
    snprintf(buffer, sizeof(buffer),
             "Brightness %u%%  |  %s theme\nScreen timeout: %s\nBattery: %s\nWake: touch only (HA polling disabled)\nTime: %s  |  Uptime %luh %lum",
             g_backlight,
             g_dark_mode ? "Dark" : "Light",
             screen_timeout_text(g_screen_timeout_seconds),
             battery_text,
             time_service_has_network_time() ? "NTP" : "build fallback",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
    set_label_text(g_home_system, buffer);

    if (g_home_timeout_button_label) {
        char timeout_button[64];
        snprintf(timeout_button, sizeof(timeout_button), "Timeout: %s",
                 screen_timeout_text(g_screen_timeout_seconds));
        set_label_text(g_home_timeout_button_label, timeout_button);
    }
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
    g_home_timeout_button_label = timeout ? lv_obj_get_child(timeout, 0) : nullptr;
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
}

void reset_ui_pointers() {
    g_header_root = nullptr;
    g_footer_root = nullptr;
    g_header_page_label = nullptr;
    g_header_date = nullptr;
    g_header_time = nullptr;
    g_wifi_state = nullptr;
    memset(g_wifi_bars, 0, sizeof(g_wifi_bars));
    g_battery_chip = nullptr;
    g_battery_body = nullptr;
    g_battery_fill = nullptr;
    g_battery_terminal = nullptr;
    g_battery_text = nullptr;
    memset(g_nav_buttons, 0, sizeof(g_nav_buttons));
    memset(g_nav_icons, 0, sizeof(g_nav_icons));
    memset(g_nav_captions, 0, sizeof(g_nav_captions));
    memset(g_page_roots, 0, sizeof(g_page_roots));
    g_status_label = nullptr;
    g_summary_label = nullptr;
    g_summary_title = nullptr;
    g_week_label = nullptr;
    g_overlay = nullptr;
    g_alarm_code_input = nullptr;
    g_home_network = nullptr;
    g_home_ha = nullptr;
    g_home_calendar = nullptr;
    g_home_system = nullptr;
    g_home_timeout_button_label = nullptr;

    memset(g_calendar_days, 0, sizeof(g_calendar_days));
    memset(g_chore_cards, 0, sizeof(g_chore_cards));
    memset(g_chore_picker_cards, 0, sizeof(g_chore_picker_cards));
    memset(g_meal_days, 0, sizeof(g_meal_days));
    g_weather_widgets = {};
    g_alarm_widgets = {};

    g_chore_page = nullptr;
    g_chore_main = nullptr;
    g_chore_picker = nullptr;
    g_chore_picker_status = nullptr;
    g_chore_picker_back = nullptr;
    g_chore_source = nullptr;
    g_chore_progress_label = nullptr;
    g_chore_progress = nullptr;
    g_chore_empty = nullptr;
    g_meals_page = nullptr;

    memset(g_day_columns, 0, sizeof(g_day_columns));
    memset(g_filter_buttons, 0, sizeof(g_filter_buttons));
}

void create_page_roots() {
    for (size_t i = 0; i < DASHBOARD_COUNT; ++i) {
        lv_obj_t *root = lv_obj_create(g_screen);
        g_page_roots[i] = root;
        lv_obj_set_size(root, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(root, 0, 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(root, 0, LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        g_page_built[i] = false;
        g_page_dirty[i] = true;
    }
}

void update_shell_dashboard_state() {
    if (g_header_page_label) {
        char subtitle[64];
        snprintf(subtitle, sizeof(subtitle), "%s dashboard  |  v%s",
                 dashboard_name(g_dashboard), APP_VERSION);
        set_label_text(g_header_page_label, subtitle);
    }

    for (size_t i = 0; i < DASHBOARD_COUNT; ++i) {
        lv_obj_t *nav = g_nav_buttons[i];
        if (!nav) continue;

        const bool active = i == dashboard_index(g_dashboard);
        const uint32_t bg = active ? theme().accent : theme().button;
        const uint32_t pressed = active ? theme().accent : theme().button_pressed;
        const uint32_t text = active ? 0xFFFFFF : theme().text;

        lv_obj_set_style_bg_color(nav, lv_color_hex(bg), LV_PART_MAIN);
        lv_obj_set_style_border_width(nav, active ? 0 : 1, LV_PART_MAIN);
        if (!active) {
            lv_obj_set_style_border_color(nav, lv_color_hex(theme().border), LV_PART_MAIN);
        }
        lv_obj_set_style_bg_color(
            nav,
            lv_color_hex(pressed),
            static_cast<lv_style_selector_t>(
                static_cast<uint32_t>(LV_PART_MAIN) |
                static_cast<uint32_t>(LV_STATE_PRESSED)));

        if (g_nav_icons[i]) {
            lv_obj_set_style_text_color(g_nav_icons[i], lv_color_hex(text), LV_PART_MAIN);
        }
        if (g_nav_captions[i]) {
            lv_obj_set_style_text_color(g_nav_captions[i], lv_color_hex(text), LV_PART_MAIN);
        }
    }
}

void update_dashboard_page(Dashboard dashboard) {
    const size_t index = dashboard_index(dashboard);
    if (index >= DASHBOARD_COUNT || !g_page_built[index]) return;

    switch (dashboard) {
        case Dashboard::Calendar:
            render_week();
            break;
        case Dashboard::Chores:
            update_chores_dashboard();
            break;
        case Dashboard::Meals:
            update_meals_dashboard();
            break;
        case Dashboard::Weather:
            update_weather_dashboard();
            break;
        case Dashboard::Alarm:
            update_alarm_dashboard();
            break;
        case Dashboard::Settings:
            update_home_dashboard();
            break;
    }

    g_page_dirty[index] = false;
}

void build_dashboard_page(Dashboard dashboard) {
    const size_t index = dashboard_index(dashboard);
    if (index >= DASHBOARD_COUNT || !g_page_roots[index] || g_page_built[index]) return;

    lv_obj_t *root = g_page_roots[index];

    /* Dashboard object trees are constructed exactly once per theme lifetime.
     * Data refreshes update these objects in place; they never clean/recreate
     * a page merely because its backing Home Assistant data changed. */
    lv_obj_t *saved_screen = g_screen;
    g_screen = root;

    switch (dashboard) {
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

    g_screen = saved_screen;
    g_page_built[index] = true;
    g_page_dirty[index] = true;
    update_dashboard_page(dashboard);

    ESP_LOGI("FamilyCalendar",
             "[UI v2.2.3] Created persistent %s page; free heap=%u, free PSRAM=%u",
             dashboard_name(dashboard),
             static_cast<unsigned>(ESP.getFreeHeap()),
             static_cast<unsigned>(ESP.getFreePsram()));
}

void show_only_dashboard(Dashboard dashboard) {
    const size_t active_index = dashboard_index(dashboard);
    for (size_t i = 0; i < DASHBOARD_COUNT; ++i) {
        lv_obj_t *root = g_page_roots[i];
        if (!root) continue;
        if (i == active_index) {
            lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void activate_dashboard(Dashboard dashboard) {
    const size_t index = dashboard_index(dashboard);
    if (index >= DASHBOARD_COUNT) return;

    const bool changed = dashboard != g_dashboard;
    if (changed) g_dashboard = dashboard;

    /* UI-first navigation: update cached/local data, reveal the persistent
     * page, and invalidate it before starting the active-tab network settle
     * timer.  Normal tab changes therefore never wait on Home Assistant. */
    if (!g_page_built[index]) {
        build_dashboard_page(dashboard);
    } else if (g_page_dirty[index]) {
        update_dashboard_page(dashboard);
    }

    show_only_dashboard(dashboard);
    update_shell_dashboard_state();
    g_rebuild_pending = false;

    if (g_root_screen) lv_obj_invalidate(g_root_screen);

    if (changed) {
        g_dashboard_entered_ms = millis();
        ESP_LOGI("FamilyCalendar",
                 "[UI v2.2.3] %s visible from cached state; HA refresh eligible after %lums",
                 dashboard_name(dashboard),
                 static_cast<unsigned long>(HA_ACTIVE_TAB_SETTLE_MS));
    }
}

void rebuild_all_ui() {
    g_full_rebuild_pending = false;
    g_rebuild_pending = false;

    reset_ui_pointers();
    lv_obj_clean(g_screen);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(theme().bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, LV_PART_MAIN);

    /* Page roots are created first so the persistent header/footer always sit
     * above page content in LVGL's sibling order. */
    create_page_roots();
    create_header();
    create_footer();

    /* Build every persistent page before the network stack is started at boot.
     * This removes first-visit allocation/render spikes from normal navigation.
     * Theme changes intentionally rebuild the same complete persistent set. */
    for (size_t i = 0; i < DASHBOARD_COUNT; ++i) {
        build_dashboard_page(static_cast<Dashboard>(i));
    }

    show_only_dashboard(g_dashboard);
    update_shell_dashboard_state();
    update_clock();

    lv_obj_invalidate(g_screen);
    g_dashboard_entered_ms = millis();
    ESP_LOGI("FamilyCalendar",
             "[UI v2.2.3] Persistent shell and all pages ready; free heap=%u, free PSRAM=%u",
             static_cast<unsigned>(ESP.getFreeHeap()),
             static_cast<unsigned>(ESP.getFreePsram()));
}

bool auto_refresh_due(uint32_t now, uint32_t last_attempt_ms, uint32_t last_ui_request_ms, uint32_t interval_ms) {
    if (interval_ms == 0) return false;
    if (last_attempt_ms != 0 && now - last_attempt_ms < interval_ms) return false;
    if (last_ui_request_ms != 0 && now - last_ui_request_ms < interval_ms) return false;
    return true;
}

void service_active_dashboard_auto_refresh(uint32_t now) {
    /* Web maintenance mode lets the current HA worker drain but prevents the
     * UI from queuing any new automatic network work before an OTA upload. */
    if (web_manager_maintenance_active()) return;

    /* The display being asleep is a strong signal that nobody is looking at
     * the dashboard, so do not spend ESP-Hosted/TLS traffic refreshing it. */
    if (!board_display_awake()) return;

    /*
    * Do not auto-refresh while a modal/overlay is open.
    *
    * For Alarm, keep moving the refresh timestamp forward while the
    * user is interacting with the popup. This prevents an immediate
    * refresh the instant the popup closes.
    */
    if (g_overlay) {
        if (g_dashboard == Dashboard::Alarm) {
            g_last_alarm_auto_request_ms = now;
        }
        return;
    }

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
                /* Queue only one HA domain per UI pass.  Weather can be queued
                 * after the calendar worker returns idle instead of being
                 * stacked behind the same request burst. */
                return;
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
            if (!chore_service_configured() || g_chore_choose_list) {
                if (!home_assistant_todo_lists_ready()) {
                    ESP_LOGI("FamilyCalendar", "Post-render refresh: discover Home Assistant chore lists");
                    home_assistant_request_todo_discovery();
                }
                break;
            }
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
            if (!alarm_service_configured() || g_alarm_choose_panel) {
                if (!home_assistant_alarm_panels_ready()) {
                    ESP_LOGI("FamilyCalendar", "Post-render refresh: discover Alarmo panels");
                    home_assistant_request_alarm_discovery();
                }
                break;
            }
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
    const size_t index = dashboard_index(g_dashboard);
    if (index >= DASHBOARD_COUNT || !g_page_roots[index]) return;

    if (!g_page_built[index]) {
        build_dashboard_page(g_dashboard);
    } else {
        update_dashboard_page(g_dashboard);
    }
    show_only_dashboard(g_dashboard);
    update_shell_dashboard_state();
    update_clock();

    if (g_root_screen) lv_obj_invalidate(g_root_screen);
}

} // namespace

void calendar_ui_init() {
    g_root_screen = lv_screen_active();
    g_screen = g_root_screen;
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

    /* Calendar is deliberately not persisted as a preference: it remains the
     * boot/default dashboard.  The v2 LVGL page itself is persistent once built. */
    g_dashboard = Dashboard::Calendar;
    g_dashboard_entered_ms = millis();
    rebuild_all_ui();
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

    if (!g_overlay) {
        if (g_full_rebuild_pending) {
            rebuild_all_ui();
        } else {
            const size_t active_index = dashboard_index(g_dashboard);
            const bool active_dirty =
                active_index < DASHBOARD_COUNT && g_page_dirty[active_index];
            if (g_rebuild_pending || active_dirty) rebuild_ui();
        }
    }

    if (chore_service_loop()) {
        mark_dashboard_dirty(Dashboard::Chores);
        if (g_dashboard == Dashboard::Chores) request_rebuild();
    }

    if (now - g_last_clock_update >= 1000UL) {
        g_last_clock_update = now;
        update_clock();
    }

    service_active_dashboard_auto_refresh(now);

    if (now - g_last_status_update >= 1000UL) {
        g_last_status_update = now;
        update_wifi_header();
        update_battery_header();
        if (g_status_label) set_label_text(g_status_label, home_assistant_status());
        if (g_dashboard == Dashboard::Settings) update_home_dashboard();
    }
}

void calendar_ui_refresh(uint32_t change_flags) {
    if (change_flags & (HA_CHANGE_CALENDAR | HA_CHANGE_WEATHER)) {
        mark_dashboard_dirty(Dashboard::Calendar);
        if (g_dashboard == Dashboard::Calendar && !g_overlay) {
            /* Calendar already owns a reusable seven-column surface, so update
             * it in place instead of rebuilding the page. */
            render_week();
            g_page_dirty[dashboard_index(Dashboard::Calendar)] = false;
        }
    }

    if (change_flags & HA_CHANGE_CALENDAR) {
        mark_dashboard_dirty(Dashboard::Meals);
        if (g_dashboard == Dashboard::Meals) request_rebuild();
    }

    if (change_flags & HA_CHANGE_WEATHER) {
        mark_dashboard_dirty(Dashboard::Weather);
        if (g_dashboard == Dashboard::Weather) request_rebuild();
    }

    if (change_flags & (HA_CHANGE_CHORES | HA_CHANGE_TODO_LISTS)) {
        mark_dashboard_dirty(Dashboard::Chores);
        if (g_dashboard == Dashboard::Chores) request_rebuild();
    }

    if (change_flags & (HA_CHANGE_ALARM | HA_CHANGE_ALARM_PANELS)) {
        mark_dashboard_dirty(Dashboard::Alarm);
        if (g_dashboard == Dashboard::Alarm) request_rebuild();
    }

    if (change_flags != HA_CHANGE_NONE) {
        mark_dashboard_dirty(Dashboard::Settings);
        if (g_dashboard == Dashboard::Settings && !g_overlay) {
            update_home_dashboard();
            g_page_dirty[dashboard_index(Dashboard::Settings)] = false;
        }
    }
}

void calendar_ui_wake_refresh(uint32_t change_flags) {
    if (change_flags & (CAMERA_CHANGE_WAKE_SENSORS | CAMERA_CHANGE_WAKE_STATE)) {
        mark_dashboard_dirty(Dashboard::Settings);
    }

    if (g_dashboard == Dashboard::Settings &&
        (change_flags & (CAMERA_CHANGE_WAKE_SENSORS | CAMERA_CHANGE_WAKE_STATE))) {
        if (g_wake_sensor_picker_open && (change_flags & CAMERA_CHANGE_WAKE_SENSORS)) {
            show_wake_sensor_picker();
        } else {
            update_home_dashboard();
            g_page_dirty[dashboard_index(Dashboard::Settings)] = false;
        }
    }
}

int calendar_ui_week_offset() {
    return g_week_offset;
}
