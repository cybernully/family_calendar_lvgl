# Family Hub 2.2.1 Hybrid Weather Setup

Version 2.2.1 lets the display combine two Home Assistant weather entities in a single snapshot request. This is useful when a local PWS integration supplies excellent current conditions and daily forecasts but does not expose hourly forecasts.

## Home Assistant script

Replace the existing `family_calendar_weather_snapshot` entry in `/config/scripts.yaml` with the contents of `docs/family_calendar_weather_snapshot.yaml`, then reload Scripts in Home Assistant. The script now accepts `current_daily_entity` and `hourly_entity` fields from the ESP and still returns the same `current`, `units`, `daily`, and `hourly` structure expected by the firmware.

Because your `scripts.yaml` can contain `!secret` references, editing the file directly is safer than using the Home Assistant script YAML editor when that editor refuses secrets.

## Web configuration

Open the Family Hub web manager and set:

- **Weather current + daily entity** to the PWS weather entity, for example `weather.kwineena131`.
- **Weather hourly entity** to a weather provider that supports hourly forecasts, for example your existing `weather.forecast_home`.

Save. The ESP stores both values in NVS and queues an immediate weather refresh. You can also use **Refresh weather now** to force another snapshot.

## Compatibility

Existing installations that only define `HA_WEATHER_ENTITY` continue to work: both hybrid source defaults inherit that single entity until the web settings or new `HA_WEATHER_CURRENT_DAILY_ENTITY` / `HA_WEATHER_HOURLY_ENTITY` compile-time defaults are set.
