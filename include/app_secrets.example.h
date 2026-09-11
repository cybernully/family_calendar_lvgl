#pragma once
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

/* Use the exact HTTPS URL that you use to reach Home Assistant. */
#define HA_BASE_URL "https://homeassistant.local:8123"
#define HA_ACCESS_TOKEN "YOUR_HOME_ASSISTANT_LONG_LIVED_ACCESS_TOKEN"

/*
 * Optional: for full TLS certificate validation, place the PEM certificate in a
 * raw string and set HA_TLS_ALLOW_INSECURE to 0 in app_local.h.
 *
 * #define HA_TLS_CA_CERT_PEM R"PEM(
 * -----BEGIN CERTIFICATE-----
 * ...
 * -----END CERTIFICATE-----
 * )PEM"
 */
