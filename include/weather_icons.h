#pragma once

#include <lvgl.h>
#include "weather_service.h"

const lv_image_dsc_t *weather_icon_asset(WeatherCondition condition, const char *symbol_code = nullptr);
