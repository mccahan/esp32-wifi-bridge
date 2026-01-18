idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES esp_eth esp_wifi esp_netif lwip nvs_flash esp-tls
    EMBED_TXTFILES certs/server_cert.pem certs/server_key.pem
)

