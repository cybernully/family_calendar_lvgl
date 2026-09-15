#!/usr/bin/env python3
"""Apply the v2.2.2 shared-worker/runtime-calendar changes to src/home_assistant.cpp.

This is intentionally strict: every expected v2.2.1 source block must match exactly.
It aborts without writing if the local file is not the expected baseline.
"""
from pathlib import Path
import sys

path = Path(sys.argv[1] if len(sys.argv) > 1 else "src/home_assistant.cpp")
if not path.exists():
    raise SystemExit(f"ERROR: {path} not found. Run from the repository root.")
text = path.read_text()

if '#include "runtime_config.h"' in text and 'home_assistant_request_connection_test' in text:
    print(f"{path}: v2.2.2 Home Assistant changes already present")
    raise SystemExit(0)

replacements = []

def replace_once(old: str, new: str, label: str):
    global text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly 1 match, found {count}")
    text = text.replace(old, new, 1)
    replacements.append(label)

replace_once(
'''#include "weather_service.h"\n''',
'''#include "weather_service.h"\n#include "runtime_config.h"\n''',
"runtime_config include")

replace_once(
'''uint32_t g_connected_since_ms = 0;\n''',
'''uint32_t g_connected_since_ms = 0;\nvolatile uint32_t g_last_ha_success_ms = 0;\nvolatile bool g_connection_test_requested = false;\nvolatile bool g_connection_test_in_progress = false;\nHomeAssistantConnectionTest g_connection_test = {};\nportMUX_TYPE g_connection_test_mux = portMUX_INITIALIZER_UNLOCKED;\n''',
"HA diagnostic state")

replace_once(
'''const char *CALENDAR_IDS[4] = {\n    HA_CALENDAR_1,\n    HA_CALENDAR_2,\n    HA_CALENDAR_3,\n    HA_CALENDAR_4,\n};\n\n''',
'''\n''',
"compile-time calendar ID array")

replace_once(
'''    if (code >= 200 && code < 300) g_authenticated = true;\n    return code;\n''',
'''    if (code >= 200 && code < 300) {\n        g_authenticated = true;\n        g_last_ha_success_ms = millis();\n    } else if (code == 401) {\n        g_authenticated = false;\n    }\n    return code;\n''',
"HA last-success tracking")

replace_once(
'''size_t configured_calendar_count() {\n    size_t count = 0;\n    for (const char *id : CALENDAR_IDS) if (id && id[0]) ++count;\n    return count;\n}\n''',
'''size_t configured_calendar_count() {\n    return runtime_config_calendar_count();\n}\n''',
"runtime calendar count")

replace_once(
'''    JsonDocument request;\n    JsonArray entities = request["entity_id"].to<JsonArray>();\n    for (const char *entity_id : CALENDAR_IDS) {\n        if (entity_id && entity_id[0]) entities.add(entity_id);\n    }\n''',
'''    JsonDocument request;\n    JsonArray entities = request["entity_id"].to<JsonArray>();\n    for (size_t person = 0; person < 4; ++person) {\n        const char *entity_id = runtime_config_calendar_entity(person);\n        if (entity_id && entity_id[0]) entities.add(entity_id);\n    }\n''',
"runtime calendar request targets")

replace_once(
'''    for (uint8_t person = 0; person < 4; ++person) {\n        const char *entity_id = CALENDAR_IDS[person];\n        if (!entity_id || !entity_id[0]) continue;\n''',
'''    for (uint8_t person = 0; person < 4; ++person) {\n        const char *entity_id = runtime_config_calendar_entity(person);\n        if (!entity_id || !entity_id[0]) continue;\n''',
"runtime calendar response mapping")

replace_once(
'''        if (!network_service_connected()) continue;\n\n        if (g_alarm_discovery_requested && !g_alarm_discovery_in_progress) {\n''',
'''        if (!network_service_connected()) continue;\n\n        /* Web diagnostics use the same shared HA worker; never open a second\n         * concurrent HTTP/TLS path. */\n        if (g_connection_test_requested && !g_connection_test_in_progress) {\n            portENTER_CRITICAL(&g_connection_test_mux);\n            g_connection_test_requested = false;\n            g_connection_test_in_progress = true;\n            g_connection_test.pending = false;\n            g_connection_test.in_progress = true;\n            portEXIT_CRITICAL(&g_connection_test_mux);\n\n            HomeAssistantConnectionTest result = {};\n            const uint32_t started = millis();\n            String payload;\n            result.http_code = http_get("/api/", payload);\n            result.latency_ms = millis() - started;\n            result.tested_ms = millis();\n            result.valid = true;\n            result.authenticated = result.http_code >= 200 && result.http_code < 300;\n            if (result.authenticated) {\n                snprintf(result.message, sizeof(result.message),\n                         "Home Assistant API reachable; token accepted");\n            } else if (result.http_code == 401) {\n                snprintf(result.message, sizeof(result.message),\n                         "Home Assistant rejected the access token (401)");\n            } else if (result.http_code < 0) {\n                snprintf(result.message, sizeof(result.message),\n                         "Transport/TLS error %d", result.http_code);\n            } else {\n                snprintf(result.message, sizeof(result.message),\n                         "Home Assistant returned HTTP %d", result.http_code);\n            }\n\n            portENTER_CRITICAL(&g_connection_test_mux);\n            g_connection_test = result;\n            g_connection_test_in_progress = false;\n            portEXIT_CRITICAL(&g_connection_test_mux);\n            ESP_LOGI("FamilyCalendar", "HA connection test: HTTP %d in %lums",\n                     result.http_code, static_cast<unsigned long>(result.latency_ms));\n            vTaskDelay(1);\n        }\n\n        if (g_alarm_discovery_requested && !g_alarm_discovery_in_progress) {\n''',
"shared-worker HA test")

replace_once(
'''    if (g_todo_discovery_requested || g_todo_discovery_in_progress ||\n        g_alarm_discovery_requested || g_alarm_discovery_in_progress) return false;\n''',
'''    if (g_todo_discovery_requested || g_todo_discovery_in_progress ||\n        g_alarm_discovery_requested || g_alarm_discovery_in_progress ||\n        g_connection_test_requested || g_connection_test_in_progress) return false;\n''',
"HA idle coordination")

replace_once(
'''uint32_t home_assistant_last_alarm_request_ms() {\n    return g_last_alarm_attempt_ms;\n}\n\nvoid home_assistant_request_todo_discovery() {\n''',
'''uint32_t home_assistant_last_alarm_request_ms() {\n    return g_last_alarm_attempt_ms;\n}\n\nuint32_t home_assistant_last_success_ms() {\n    return g_last_ha_success_ms;\n}\n\nbool home_assistant_request_connection_test() {\n    if (!home_assistant_configured() || !g_worker_task || !network_service_connected()) return false;\n    bool queued = false;\n    portENTER_CRITICAL(&g_connection_test_mux);\n    if (!g_connection_test_requested && !g_connection_test_in_progress) {\n        g_connection_test_requested = true;\n        g_connection_test.pending = true;\n        g_connection_test.in_progress = false;\n        queued = true;\n    }\n    portEXIT_CRITICAL(&g_connection_test_mux);\n    if (queued) xTaskNotifyGive(g_worker_task);\n    return queued;\n}\n\nvoid home_assistant_get_connection_test(HomeAssistantConnectionTest &out) {\n    memset(&out, 0, sizeof(out));\n    portENTER_CRITICAL(&g_connection_test_mux);\n    out = g_connection_test;\n    out.pending = g_connection_test_requested;\n    out.in_progress = g_connection_test_in_progress;\n    portEXIT_CRITICAL(&g_connection_test_mux);\n}\n\nvoid home_assistant_request_todo_discovery() {\n''',
"HA diagnostic API")

backup = path.with_suffix(path.suffix + ".v221.bak")
backup.write_text(path.read_text())
path.write_text(text)
print(f"Updated {path}; backup: {backup}")
for item in replacements:
    print(f"  + {item}")
