idf_component_register(
    SRCS "main.c" "webserver.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls esp_http_server app_update
)

