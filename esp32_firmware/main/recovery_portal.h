#ifndef RECOVERY_PORTAL_H
#define RECOVERY_PORTAL_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t recovery_portal_start(void);
esp_err_t recovery_portal_start_server(void);
esp_err_t recovery_portal_stop_ap(void);
bool recovery_portal_is_active(void);
const char *recovery_portal_ssid(void);
const char *recovery_portal_ip(void);

#endif // RECOVERY_PORTAL_H
