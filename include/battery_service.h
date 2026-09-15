#pragma once

#include <stdint.h>

struct BatteryStatus {
    bool valid;
    uint16_t adc_mv;
    float adc_voltage_v;
    float voltage_v;
    uint8_t percent;
    uint32_t sampled_ms;
};

/* Initialize the JC8012P4A1 battery ADC and load persisted calibration/UI settings. */
void battery_service_begin();

/* Non-network periodic sampler. Safe to call from the main Arduino loop. */
void battery_service_loop();

/* Copy the most recent smoothed battery estimate. */
bool battery_service_get_status(BatteryStatus &out);

/* Web/UI configuration. Values are persisted in NVS and apply immediately. */
bool battery_service_show_percent();
float battery_service_voltage_multiplier();
uint8_t battery_service_critical_percent();
bool battery_service_set_config(bool show_percent,
                                float voltage_multiplier,
                                uint8_t critical_percent);
