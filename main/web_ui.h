#pragma once

#include "esp_err.h"

/* Start local AP + HTTP server with WebSocket endpoint. */
esp_err_t web_ui_start(void);
