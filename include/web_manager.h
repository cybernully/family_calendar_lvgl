#pragma once

/* Lightweight local web management and browser-based OTA firmware updates. */
void web_manager_begin();
void web_manager_loop();

/* Maintenance suppresses new active-tab HA refreshes while existing work drains. */
bool web_manager_maintenance_active();

/* True only while firmware bytes are being written to the inactive OTA slot. */
bool web_manager_ota_in_progress();
