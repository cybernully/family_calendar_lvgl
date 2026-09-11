#include "network_service.h"
#include "app_config.h"
#include "app_secrets.h"

#include <WiFi.h>

#if defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED
#include "esp32-hal-hosted.h"
#endif

namespace {

char g_status[96] = "Wi-Fi starting";
bool g_was_connected = false;
uint32_t g_connect_started_ms = 0;
uint32_t g_last_retry_ms = 0;

#if defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED
extern "C" const uint8_t hosted_c6_firmware_start[];
extern "C" const uint8_t hosted_c6_firmware_end[];

constexpr uint32_t BUNDLED_HOSTED_MAJOR = 2;
constexpr uint32_t BUNDLED_HOSTED_MINOR = 12;
constexpr uint32_t BUNDLED_HOSTED_PATCH = 3;
constexpr size_t HOSTED_UPDATE_CHUNK = 2048;

uint32_t version_value(uint32_t major, uint32_t minor, uint32_t patch) {
    return (major << 16) | (minor << 8) | patch;
}

bool maybe_update_hosted_c6() {
#if HOSTED_C6_AUTO_UPDATE
    if (!hostedIsInitialized()) {
        Serial0.println("[Hosted] ESP-Hosted is not initialized; cannot check C6 firmware");
        return false;
    }

    uint32_t host_major = 0, host_minor = 0, host_patch = 0;
    uint32_t slave_major = 0, slave_minor = 0, slave_patch = 0;
    hostedGetHostVersion(&host_major, &host_minor, &host_patch);
    hostedGetSlaveVersion(&slave_major, &slave_minor, &slave_patch);

    Serial0.printf("[Hosted] Host firmware:  %lu.%lu.%lu\n",
                   static_cast<unsigned long>(host_major),
                   static_cast<unsigned long>(host_minor),
                   static_cast<unsigned long>(host_patch));
    Serial0.printf("[Hosted] C6 firmware:    %lu.%lu.%lu\n",
                   static_cast<unsigned long>(slave_major),
                   static_cast<unsigned long>(slave_minor),
                   static_cast<unsigned long>(slave_patch));

    const uint32_t host_version = version_value(host_major, host_minor, host_patch);
    const uint32_t slave_version = version_value(slave_major, slave_minor, slave_patch);
    const uint32_t bundled_version = version_value(
        BUNDLED_HOSTED_MAJOR, BUNDLED_HOSTED_MINOR, BUNDLED_HOSTED_PATCH);

    if (host_version == slave_version) {
        Serial0.println("[Hosted] Host and C6 firmware versions match");
        return false;
    }

    if (host_version != bundled_version) {
        Serial0.printf(
            "[Hosted] WARNING: host expects %lu.%lu.%lu but this build bundles %lu.%lu.%lu; "
            "automatic C6 update skipped\n",
            static_cast<unsigned long>(host_major),
            static_cast<unsigned long>(host_minor),
            static_cast<unsigned long>(host_patch),
            static_cast<unsigned long>(BUNDLED_HOSTED_MAJOR),
            static_cast<unsigned long>(BUNDLED_HOSTED_MINOR),
            static_cast<unsigned long>(BUNDLED_HOSTED_PATCH));
        return false;
    }

    if (slave_version > host_version) {
        Serial0.println("[Hosted] C6 firmware is newer than the host; automatic downgrade skipped");
        return false;
    }

    const size_t image_size = static_cast<size_t>(hosted_c6_firmware_end - hosted_c6_firmware_start);
    if (image_size < 64 * 1024 || image_size > 4 * 1024 * 1024 || hosted_c6_firmware_start[0] != 0xE9) {
        Serial0.printf("[Hosted] ERROR: embedded C6 image is invalid (%u bytes)\n",
                       static_cast<unsigned>(image_size));
        return false;
    }

    snprintf(g_status, sizeof(g_status), "Updating Wi-Fi coprocessor");
    Serial0.printf(
        "[Hosted] Updating ESP32-C6 from %lu.%lu.%lu to %lu.%lu.%lu (%u bytes)...\n",
        static_cast<unsigned long>(slave_major),
        static_cast<unsigned long>(slave_minor),
        static_cast<unsigned long>(slave_patch),
        static_cast<unsigned long>(BUNDLED_HOSTED_MAJOR),
        static_cast<unsigned long>(BUNDLED_HOSTED_MINOR),
        static_cast<unsigned long>(BUNDLED_HOSTED_PATCH),
        static_cast<unsigned>(image_size));

    if (!hostedBeginUpdate()) {
        Serial0.println("[Hosted] ERROR: C6 update could not start");
        snprintf(g_status, sizeof(g_status), "C6 firmware update failed");
        return false;
    }

    size_t offset = 0;
    int last_percent = -1;
    while (offset < image_size) {
        size_t chunk = image_size - offset;
        if (chunk > HOSTED_UPDATE_CHUNK) chunk = HOSTED_UPDATE_CHUNK;

        // Arduino's hosted API is non-const even though it does not modify the
        // firmware input buffer.
        uint8_t *chunk_ptr = const_cast<uint8_t *>(hosted_c6_firmware_start + offset);
        if (!hostedWriteUpdate(chunk_ptr, static_cast<uint32_t>(chunk))) {
            Serial0.printf("[Hosted] ERROR: C6 update failed at byte %u/%u\n",
                           static_cast<unsigned>(offset), static_cast<unsigned>(image_size));
            snprintf(g_status, sizeof(g_status), "C6 firmware update failed");
            return false;
        }

        offset += chunk;
        const int percent = static_cast<int>((offset * 100U) / image_size);
        if (percent / 10 != last_percent / 10 || percent == 100) {
            last_percent = percent;
            Serial0.printf("[Hosted] C6 update %d%%\n", percent);
        }
        delay(1);
    }

    if (!hostedEndUpdate()) {
        Serial0.println("[Hosted] ERROR: C6 update could not be finalized");
        snprintf(g_status, sizeof(g_status), "C6 firmware update failed");
        return false;
    }

    /*
     * Arduino-ESP32 intentionally treats activation failures from pre-2.6
     * co-processor firmware as non-fatal because those old versions may reset
     * before acknowledging the activation command.
     */
    if (!hostedActivateUpdate()) {
        Serial0.println("[Hosted] ERROR: C6 update activation failed");
        snprintf(g_status, sizeof(g_status), "C6 firmware activation failed");
        return false;
    }

    Serial0.println("[Hosted] ESP32-C6 update complete. Restarting ESP32-P4...");
    Serial0.flush();
    delay(1500);
    ESP.restart();
    delay(5000);
    return true;
#else
    return false;
#endif
}
#endif

bool usable_value(const char *value, const char *placeholder) {
    if (!value || !value[0]) return false;
    if (placeholder && strcmp(value, placeholder) == 0) return false;
    return true;
}

void begin_connection() {
#if defined(CONFIG_ESP_HOSTED_ENABLED) && CONFIG_ESP_HOSTED_ENABLED
    /*
     * JC8012P4A1C P4 -> onboard C6 ESP-Hosted SDIO wiring.  Arduino 3.3.x
     * supports selecting these pins at runtime.  They also match the generic
     * esp32p4 Arduino variant used by this project.
     */
    if (!WiFi.setPins(18, 19, 14, 15, 16, 17, 54)) {
        Serial0.println("[WiFi] WARNING: WiFi.setPins() reported failure");
    }
#else
    Serial0.println("[WiFi] ERROR: Arduino framework was built without ESP-Hosted support");
#endif

    /* WiFi.mode() initializes ESP-Hosted but does not yet associate to the AP. */
    WiFi.mode(WIFI_STA);

    /*
     * This panel is mains powered.  Keep the C6 radio awake and enable
     * automatic reconnection so association/key-handshake recovery is not
     * complicated by station power save.
     */
    if (!WiFi.setSleep(false)) {
        Serial0.println("[WiFi] WARNING: could not disable station power save");
    }
    if (!WiFi.setAutoReconnect(true)) {
        Serial0.println("[WiFi] WARNING: could not enable automatic reconnect");
    }

#if defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) && CONFIG_ESP_WIFI_REMOTE_ENABLED
    /*
     * Some JC8012P4A1C boards ship with very old ESP32-C6 ESP-Hosted firmware.
     * Update it before attempting association so an incompatible RPC transport
     * cannot immediately disconnect with ASSOC_LEAVE.
     */
    maybe_update_hosted_c6();
#endif

    WiFi.setHostname("family-calendar");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    g_connect_started_ms = millis();
    snprintf(g_status, sizeof(g_status), "Wi-Fi connecting to %s", WIFI_SSID);
    Serial0.printf("[WiFi] Connecting to %s using ESP-Hosted C6...\n", WIFI_SSID);
}

} // namespace

bool network_service_configured() {
    return usable_value(WIFI_SSID, "YOUR_WIFI_SSID") &&
           usable_value(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD");
}

void network_service_begin() {
    if (!network_service_configured()) {
        snprintf(g_status, sizeof(g_status), "Wi-Fi not configured");
        Serial0.println("[WiFi] Configure WIFI_SSID and WIFI_PASSWORD in include/app_secrets.h");
        return;
    }
    begin_connection();
}

void network_service_loop() {
    if (!network_service_configured()) return;

    const bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !g_was_connected) {
        g_was_connected = true;
        const String ip = WiFi.localIP().toString();
        snprintf(g_status, sizeof(g_status), "Wi-Fi connected | %s", ip.c_str());
        Serial0.printf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n", ip.c_str(), WiFi.RSSI());

        /* Start SNTP. time_service_now() automatically switches to system time once valid. */
        configTzTime(APP_TIMEZONE_POSIX, "pool.ntp.org", "time.nist.gov");
        Serial0.println("[Time] NTP synchronization requested");
    } else if (!connected && g_was_connected) {
        g_was_connected = false;
        snprintf(g_status, sizeof(g_status), "Wi-Fi disconnected");
        Serial0.println("[WiFi] Connection lost");
        g_last_retry_ms = millis();
    }

    if (!connected) {
        const uint32_t now = millis();
        if (now - g_connect_started_ms > 20000UL && now - g_last_retry_ms > 15000UL) {
            g_last_retry_ms = now;
            Serial0.println("[WiFi] Retrying connection...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            g_connect_started_ms = now;
            snprintf(g_status, sizeof(g_status), "Wi-Fi reconnecting");
        }
    }
}

bool network_service_connected() {
    return WiFi.status() == WL_CONNECTED;
}

const char *network_service_status() {
    return g_status;
}

String network_service_ip() {
    return network_service_connected() ? WiFi.localIP().toString() : String();
}

int network_service_rssi() {
    return network_service_connected() ? WiFi.RSSI() : 0;
}
