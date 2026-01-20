#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "esp_http_server.h"

// Initialize HTTP server with WebSerial and OTA support
esp_err_t start_webserver(void);

// Stop HTTP server
void stop_webserver(void);

// Send log message to WebSerial clients
void webserial_send(const char *message);

#endif // WEBSERVER_H
