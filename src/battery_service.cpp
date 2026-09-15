#include "battery_service.h"

#include "app_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

namespace {

struct ChargePoint {
    uint16_t millivolts;
    uint8_t percent;
};

/*
 * Approximate single-cell Li-ion open-circuit/load-voltage curve.  Voltage is
 * not a true fuel gauge, so the result is intentionally presented as an
 * estimate.  Interpolation between these points keeps the header stable and
 * more realistic than a straight 3.50-4.20 V linear scale.
 */
constexpr ChargePoint CHARGE_CURVE[] = {
    {3500, 0},
    {3550, 10},
    {3650, 20},
    {3700, 30},
    {3740, 40},
    {3790, 50},
    {3850, 60},
    {3920, 70},
    {4000, 80},
    {4100, 90},
    {4200, 100},
};

BatteryStatus g_status = {};
bool g_ready = false;
bool g_show_percent = BATTERY_DEFAULT_SHOW_PERCENT != 0;
float g_voltage_multiplier = BATTERY_VOLTAGE_MULTIPLIER;
uint8_t g_critical_percent = BATTERY_DEFAULT_CRITICAL_PERCENT;
float g_filtered_adc_mv = NAN;
uint32_t g_last_sample_ms = 0;

float sanitize_multiplier(float value) {
    if (!isfinite(value) || value < BATTERY_MULTIPLIER_MIN || value > BATTERY_MULTIPLIER_MAX) {
        return BATTERY_VOLTAGE_MULTIPLIER;
    }
    return value;
}

uint8_t sanitize_critical(int value) {
    if (value < 1) return 1;
    if (value > 30) return 30;
    return static_cast<uint8_t>(value);
}

uint8_t voltage_percent(uint16_t battery_mv) {
    if (battery_mv <= CHARGE_CURVE[0].millivolts) return 0;
    constexpr size_t count = sizeof(CHARGE_CURVE) / sizeof(CHARGE_CURVE[0]);
    if (battery_mv >= CHARGE_CURVE[count - 1].millivolts) return 100;

    for (size_t i = 1; i < count; ++i) {
        const ChargePoint &high = CHARGE_CURVE[i];
        if (battery_mv > high.millivolts) continue;
        const ChargePoint &low = CHARGE_CURVE[i - 1];
        const uint32_t span_mv = high.millivolts - low.millivolts;
        const uint32_t into_mv = battery_mv - low.millivolts;
        const uint32_t span_pct = high.percent - low.percent;
        return static_cast<uint8_t>(low.percent + (into_mv * span_pct + span_mv / 2U) / span_mv);
    }
    return 0;
}

uint16_t read_trimmed_adc_mv() {
    constexpr size_t count = BATTERY_ADC_SAMPLE_COUNT;
    static_assert(count >= 5, "Battery ADC sampling needs at least five samples");
    uint32_t samples[count] = {};

    /* The divider has relatively high source impedance.  Discard a settling
     * conversion, then take a small burst and trim the two highest/lowest
     * samples before averaging. */
    (void)analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(250);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = analogReadMilliVolts(BATTERY_ADC_PIN);
        delayMicroseconds(250);
    }

    for (size_t i = 1; i < count; ++i) {
        const uint32_t value = samples[i];
        size_t j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }

    constexpr size_t trim = 2;
    uint64_t total = 0;
    for (size_t i = trim; i < count - trim; ++i) total += samples[i];
    const size_t kept = count - trim * 2;
    return static_cast<uint16_t>((total + kept / 2U) / kept);
}

void sample_battery(bool initial) {
    const bool previous_valid = g_status.valid;
    const uint8_t previous_percent = g_status.percent;
    const uint16_t adc_mv = read_trimmed_adc_mv();
    if (initial || !isfinite(g_filtered_adc_mv)) {
        g_filtered_adc_mv = static_cast<float>(adc_mv);
    } else {
        /* Battery voltage changes slowly.  A 25% EMA prevents the header from
         * jumping because of display/network load transients. */
        g_filtered_adc_mv = g_filtered_adc_mv * 0.75f + static_cast<float>(adc_mv) * 0.25f;
    }

    const uint16_t smoothed_adc_mv = static_cast<uint16_t>(lroundf(g_filtered_adc_mv));
    const float battery_v = (static_cast<float>(smoothed_adc_mv) / 1000.0f) * g_voltage_multiplier;
    const uint16_t battery_mv = static_cast<uint16_t>(lroundf(battery_v * 1000.0f));
    const bool valid = battery_mv >= BATTERY_VALID_MIN_MV && battery_mv <= BATTERY_VALID_MAX_MV;

    g_status.valid = valid;
    g_status.adc_mv = smoothed_adc_mv;
    g_status.adc_voltage_v = static_cast<float>(smoothed_adc_mv) / 1000.0f;
    g_status.voltage_v = battery_v;
    g_status.percent = valid ? voltage_percent(battery_mv) : 0;
    g_status.sampled_ms = millis();
    g_last_sample_ms = g_status.sampled_ms;

    const int percent_delta = static_cast<int>(g_status.percent) - static_cast<int>(previous_percent);
    const bool should_log = initial || valid != previous_valid ||
                            (valid && (percent_delta >= 5 || percent_delta <= -5));
    if (should_log && valid) {
        Serial0.printf("[Battery] GPIO%u ADC=%u mV, multiplier=%.3f, battery=%.3f V, charge=%u%%\n",
                       static_cast<unsigned>(BATTERY_ADC_PIN),
                       static_cast<unsigned>(g_status.adc_mv),
                       static_cast<double>(g_voltage_multiplier),
                       static_cast<double>(g_status.voltage_v),
                       static_cast<unsigned>(g_status.percent));
    } else if (should_log) {
        Serial0.printf("[Battery] GPIO%u reading unavailable: ADC=%u mV, estimated=%.3f V\n",
                       static_cast<unsigned>(BATTERY_ADC_PIN),
                       static_cast<unsigned>(g_status.adc_mv),
                       static_cast<double>(g_status.voltage_v));
    }
}

void load_preferences() {
    Preferences prefs;
    if (!prefs.begin("famcal_batt", true)) return;
    g_show_percent = prefs.getBool("show_pct", BATTERY_DEFAULT_SHOW_PERCENT != 0);
    const uint32_t multiplier_x1000 = prefs.getUInt(
        "mult_x1000", static_cast<uint32_t>(lroundf(BATTERY_VOLTAGE_MULTIPLIER * 1000.0f)));
    g_voltage_multiplier = sanitize_multiplier(static_cast<float>(multiplier_x1000) / 1000.0f);
    g_critical_percent = sanitize_critical(
        static_cast<int>(prefs.getUChar("critical", BATTERY_DEFAULT_CRITICAL_PERCENT)));
    prefs.end();
}

} // namespace

void battery_service_begin() {
    load_preferences();
    pinMode(BATTERY_ADC_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    sample_battery(true);
    g_ready = true;
}

void battery_service_loop() {
    if (!g_ready) return;
    const uint32_t now = millis();
    if (g_last_sample_ms != 0 && now - g_last_sample_ms < BATTERY_SAMPLE_INTERVAL_MS) return;
    sample_battery(false);
}

bool battery_service_get_status(BatteryStatus &out) {
    out = g_status;
    return g_status.valid;
}

bool battery_service_show_percent() {
    return g_show_percent;
}

float battery_service_voltage_multiplier() {
    return g_voltage_multiplier;
}

uint8_t battery_service_critical_percent() {
    return g_critical_percent;
}

bool battery_service_set_config(bool show_percent,
                                float voltage_multiplier,
                                uint8_t critical_percent) {
    const float clean_multiplier = sanitize_multiplier(voltage_multiplier);
    const uint8_t clean_critical = sanitize_critical(critical_percent);
    if (!isfinite(voltage_multiplier) || fabsf(clean_multiplier - voltage_multiplier) > 0.0005f) return false;

    Preferences prefs;
    if (!prefs.begin("famcal_batt", false)) return false;
    prefs.putBool("show_pct", show_percent);
    prefs.putUInt("mult_x1000", static_cast<uint32_t>(lroundf(clean_multiplier * 1000.0f)));
    prefs.putUChar("critical", clean_critical);
    prefs.end();

    const bool multiplier_changed = fabsf(g_voltage_multiplier - clean_multiplier) > 0.0005f;
    g_show_percent = show_percent;
    g_voltage_multiplier = clean_multiplier;
    g_critical_percent = clean_critical;
    if (multiplier_changed) {
        g_filtered_adc_mv = NAN;
        sample_battery(true);
    }
    return true;
}
