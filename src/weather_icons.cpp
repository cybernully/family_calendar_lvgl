#include "weather_icons.h"
#include "weather_icon_assets.h"

#include <cstring>

const lv_image_dsc_t *weather_icon_asset(WeatherCondition condition, const char *symbol_code) {
    if (symbol_code && std::strcmp(symbol_code, "clear-night") == 0) {
        return &weather_icon_clear_night;
    }

    switch (condition) {
        case WeatherCondition::Clear:
            return &weather_icon_clear;
        case WeatherCondition::PartlyCloudy:
            return &weather_icon_partly_cloudy;
        case WeatherCondition::Cloudy:
            return &weather_icon_cloudy;
        case WeatherCondition::Rain:
        case WeatherCondition::Showers:
            return &weather_icon_rain;
        case WeatherCondition::Thunderstorm:
            return &weather_icon_thunderstorm;
        case WeatherCondition::Snow:
            return &weather_icon_snow;
        case WeatherCondition::Fog:
            return &weather_icon_fog;
        case WeatherCondition::Unknown:
        default:
            return &weather_icon_unknown;
    }
}
