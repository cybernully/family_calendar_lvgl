#pragma once

#include <stddef.h>
#include <stdint.h>

struct CameraFrameView {
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint32_t sequence;
    bool valid;
};

struct WakeSensorInfo {
    char entity_id[96];
    char name[80];
};

constexpr uint32_t CAMERA_CHANGE_NONE = 0;
constexpr uint32_t CAMERA_CHANGE_DISCOVERY = 1U << 0;
constexpr uint32_t CAMERA_CHANGE_TILE_0 = 1U << 1;
constexpr uint32_t CAMERA_CHANGE_TILE_1 = 1U << 2;
constexpr uint32_t CAMERA_CHANGE_TILE_2 = 1U << 3;
constexpr uint32_t CAMERA_CHANGE_TILE_3 = 1U << 4;
constexpr uint32_t CAMERA_CHANGE_FULL = 1U << 5;
constexpr uint32_t CAMERA_CHANGE_WAKE_SENSORS = 1U << 6;
constexpr uint32_t CAMERA_CHANGE_WAKE_STATE = 1U << 7;

void camera_service_begin();
uint32_t camera_service_loop();

void camera_service_request_discovery();
void camera_service_set_active(bool active);
bool camera_service_ready();
size_t camera_service_count();
const char *camera_service_entity(size_t index);
const char *camera_service_name(size_t index);
const char *camera_service_state(size_t index);

void camera_service_set_page(size_t first_index);
size_t camera_service_page_start();
void camera_service_request_refresh();
void camera_service_open_full(size_t camera_index);
void camera_service_close_full();
int camera_service_full_index();

bool camera_service_get_tile_frame(size_t slot, CameraFrameView &out);
bool camera_service_get_full_frame(CameraFrameView &out);

/* Optional Home Assistant binary_sensor used to wake the display when a
 * camera/person/motion/presence sensor becomes active. */
void camera_service_request_wake_sensor_discovery();
bool camera_service_wake_sensors_ready();
size_t camera_service_wake_sensor_count();
const char *camera_service_wake_sensor_entity(size_t index);
const char *camera_service_wake_sensor_name(size_t index);
const char *camera_service_selected_wake_sensor();
void camera_service_select_wake_sensor(const char *entity_id);
bool camera_service_presence_active();
const char *camera_service_presence_status();
