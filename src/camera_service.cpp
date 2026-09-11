#include "camera_service.h"
#include "app_config.h"

#include <Arduino.h>
#include <esp_log.h>

namespace {
const char *kDisabledStatus = "Touch wake only";
}

void camera_service_begin() {
    ESP_LOGI("FamilyCalendar",
             "Camera feeds and HA wake-sensor polling disabled in network-safe mode; touch wake remains active");
}

uint32_t camera_service_loop() { return CAMERA_CHANGE_NONE; }

void camera_service_request_discovery() {}
void camera_service_set_active(bool) {}
bool camera_service_ready() { return false; }
size_t camera_service_count() { return 0; }
const char *camera_service_entity(size_t) { return ""; }
const char *camera_service_name(size_t) { return ""; }
const char *camera_service_state(size_t) { return ""; }

void camera_service_set_page(size_t) {}
size_t camera_service_page_start() { return 0; }
void camera_service_request_refresh() {}
void camera_service_open_full(size_t) {}
void camera_service_close_full() {}
int camera_service_full_index() { return -1; }

bool camera_service_get_tile_frame(size_t, CameraFrameView &out) {
    out = {};
    return false;
}

bool camera_service_get_full_frame(CameraFrameView &out) {
    out = {};
    return false;
}

void camera_service_request_wake_sensor_discovery() {}
bool camera_service_wake_sensors_ready() { return true; }
size_t camera_service_wake_sensor_count() { return 0; }
const char *camera_service_wake_sensor_entity(size_t) { return ""; }
const char *camera_service_wake_sensor_name(size_t) { return ""; }
const char *camera_service_selected_wake_sensor() { return ""; }
void camera_service_select_wake_sensor(const char *) {}
bool camera_service_presence_active() { return false; }
const char *camera_service_presence_status() { return kDisabledStatus; }
