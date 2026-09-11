#include <Arduino.h>
#include "app_config.h"
#include "board_lvgl.h"
#include "time_service.h"
#include "calendar_model.h"
#include "chore_service.h"
#include "alarm_service.h"
#include "camera_service.h"
#include "calendar_ui.h"
#include "network_service.h"
#include "home_assistant.h"
#include "weather_service.h"

void setup() {
    Serial0.begin(115200);
    delay(250);
    Serial0.printf("\n%s v%s starting...\n", APP_NAME, APP_VERSION);

    if (!board_lvgl_begin(APP_DISPLAY_ROTATION)) {
        Serial0.println("Fatal display initialization error.");
        while (true) delay(1000);
    }

    board_set_backlight(APP_DEFAULT_BACKLIGHT);
    time_service_begin();
    calendar_cache_begin();
    calendar_model_load_demo();
    chore_service_begin();
    alarm_service_begin();
    weather_service_begin();
    calendar_ui_init();

    /* Initialize ESP-Hosted first.  Camera feeds and HA presence polling are
     * disabled in network-safe mode; touch-to-wake remains local. */
    network_service_begin();
    camera_service_begin();
    home_assistant_begin();
    weather_service_request_refresh(false, "startup");

    Serial0.println("Family Calendar UI ready.");
}

void loop() {
    network_service_loop();
    home_assistant_loop(calendar_ui_week_offset());

    const uint32_t ha_changes = home_assistant_take_changes();
    if (ha_changes != HA_CHANGE_NONE) {
        calendar_ui_refresh(ha_changes);
    }

    const uint32_t wake_changes = camera_service_loop();
    if (wake_changes != CAMERA_CHANGE_NONE) {
        calendar_ui_wake_refresh(wake_changes);
    }

    calendar_ui_loop();
    board_lvgl_loop();
    delay(5);
}
