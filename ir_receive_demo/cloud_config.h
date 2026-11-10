#pragma once

#if __has_include("cloud_config.local.h")
#include "cloud_config.local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef BEMFA_PRIVATE_KEY
#define BEMFA_PRIVATE_KEY ""
#endif

#ifndef BEMFA_AC_TOPIC
#define BEMFA_AC_TOPIC ""
#endif

#ifndef WIFI_PROV_POP
#define WIFI_PROV_POP "irac8261"
#endif

// Optional separate Bemfa topic for threshold commands and status.
#ifndef BEMFA_THRESHOLD_TOPIC
#define BEMFA_THRESHOLD_TOPIC ""
#endif
