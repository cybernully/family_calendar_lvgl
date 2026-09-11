#pragma once

#include <Arduino.h>

void network_service_begin();
void network_service_loop();
bool network_service_configured();
bool network_service_connected();
const char *network_service_status();
String network_service_ip();
int network_service_rssi();
