idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls
    EMBED_TXTFILES "${CMAKE_CURRENT_LIST_DIR}/certs/server_cert.pem"
                   "${CMAKE_CURRENT_LIST_DIR}/certs/server_key.pem"
)

