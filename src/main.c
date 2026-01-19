/*
 * ESP32-S3 W5500 Ethernet WiFi Bridge (ESP-IDF)
 * 
 * This implementation uses ESP-IDF native esp_eth driver with W5500 over SPI.
 * Implements HTTPS proxy with TLS termination using esp_tls (high-level mbedTLS wrapper).
 * The proxy decrypts HTTPS traffic from Ethernet clients and re-encrypts it to the Powerwall.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "mdns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "config.h"
#include "cert.h"
#include "certs.h"

static const char *TAG = "wifi-eth-bridge";

// Event group for WiFi and Ethernet status
static EventGroupHandle_t s_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define ETH_CONNECTED_BIT BIT1
#define ETH_GOT_IP_BIT BIT2

// Ethernet and WiFi handles
static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;
static esp_netif_t *wifi_netif = NULL;

// Server socket
static int server_socket = -1;

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        xEventGroupSetBits(s_event_group, ETH_CONNECTED_BIT);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        xEventGroupClearBits(s_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        xEventGroupClearBits(s_event_group, ETH_CONNECTED_BIT | ETH_GOT_IP_BIT);
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    xEventGroupSetBits(s_event_group, ETH_GOT_IP_BIT);
}

/** Event handler for WiFi events */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    }
}

/** Event handler for IP_EVENT_STA_GOT_IP */
static void wifi_got_ip_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG, "WiFi got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
}

/** Initialize W5500 Ethernet */
static esp_err_t init_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet W5500...");

    // Create event group
    s_event_group = xEventGroupCreate();

    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default event loop for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // Configure SPI bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_MOSI_GPIO,
        .miso_io_num = W5500_MISO_GPIO,
        .sclk_io_num = W5500_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Configure SPI device for W5500
    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,  // 20 MHz
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 20,
        .cs_ena_posttrans = 1,
    };

    // Configure W5500
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI3_HOST, &spi_devcfg);
    w5500_config.int_gpio_num = W5500_INT_GPIO;

    // Configure MAC and PHY
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    // Set custom MAC address (W5500 doesn't have burned-in MAC)
    uint8_t mac_addr[6] = ETH_MAC_ADDR;
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
    ESP_LOGI(TAG, "MAC Address set to: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet initialized - waiting for connection...");
    return ESP_OK;
}

/** Initialize WiFi Station mode */
static esp_err_t init_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");

    // Create default WiFi station
    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_got_ip_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialized - connecting to %s", WIFI_SSID);
    return ESP_OK;
}

/** Initialize mDNS */
static void init_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_LOGI(TAG, "mDNS hostname set to: %s", MDNS_HOSTNAME);

    mdns_service_add(NULL, MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS service added: %s.%s on port %d", MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT);
}

/** HTTPS Proxy task with TLS termination - handles decrypt/re-encrypt */
static void handle_client_task(void *pvParameters)
{
    int client_sock = (int)pvParameters;
    char addr_str[32] = "unknown";
    
    ESP_LOGI(TAG, "Handling client connection");

    // Server-side TLS configuration (accept connections with self-signed cert)
    esp_tls_cfg_server_t server_cfg = {
        .servercert_buf = (const unsigned char *)server_cert_pem,
        .servercert_bytes = sizeof(server_cert_pem),
        .serverkey_buf = (const unsigned char *)server_key_pem,
        .serverkey_bytes = sizeof(server_key_pem),
    };

    // Create TLS connection for client (server role)
    esp_tls_t *client_tls = esp_tls_init();
    if (!client_tls) {
        ESP_LOGE(TAG, "Failed to allocate esp_tls for client");
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    int ret = esp_tls_server_session_create(&server_cfg, client_sock, client_tls);
    if (ret != 0) {
        ESP_LOGE(TAG, "TLS handshake with client failed: %d", ret);
        esp_tls_conn_destroy(client_tls);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TLS handshake with client successful");

    // Client-side TLS configuration (connect to Powerwall, skip verification)
    // Provide empty CA cert to satisfy esp_tls verification requirement while skipping actual verification
    esp_tls_cfg_t powerwall_cfg = {
        .skip_cert_verify = true,
        .non_block = false,
        .timeout_ms = 10000,
    };

    // Connect to Powerwall with TLS
    esp_tls_t *powerwall_tls = esp_tls_init();
    if (!powerwall_tls) {
        ESP_LOGE(TAG, "Failed to allocate esp_tls for Powerwall");
        esp_tls_conn_destroy(client_tls);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    ret = esp_tls_conn_new_sync(POWERWALL_IP_STR, strlen(POWERWALL_IP_STR), 443, &powerwall_cfg, powerwall_tls);
    if (ret != 1) {
        ESP_LOGE(TAG, "Failed to connect to Powerwall via TLS: %d", ret);
        esp_tls_conn_destroy(powerwall_tls);
        esp_tls_conn_destroy(client_tls);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TLS connection to Powerwall established");

    // Proxy decrypted HTTP data bidirectionally
    uint8_t *buffer = malloc(PROXY_BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate proxy buffer");
        esp_tls_conn_destroy(powerwall_tls);
        esp_tls_conn_destroy(client_tls);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_activity = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(PROXY_TIMEOUT_MS);

    while (1) {
        // Set read timeout for non-blocking behavior
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
        
        // Client -> Powerwall
        int len = esp_tls_conn_read(client_tls, buffer, PROXY_BUFFER_SIZE);
        if (len > 0) {
            int written = esp_tls_conn_write(powerwall_tls, buffer, len);
            if (written < 0) {
                ESP_LOGE(TAG, "Error writing to Powerwall");
                break;
            }
            last_activity = xTaskGetTickCount();
        } else if (len < 0 && len != ESP_TLS_ERR_SSL_WANT_READ && len != ESP_TLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGI(TAG, "Client connection closed or error: %d", len);
            break;
        }

        // Powerwall -> Client  
        len = esp_tls_conn_read(powerwall_tls, buffer, PROXY_BUFFER_SIZE);
        if (len > 0) {
            int written = esp_tls_conn_write(client_tls, buffer, len);
            if (written < 0) {
                ESP_LOGE(TAG, "Error writing to client");
                break;
            }
            last_activity = xTaskGetTickCount();
        } else if (len < 0 && len != ESP_TLS_ERR_SSL_WANT_READ && len != ESP_TLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGI(TAG, "Powerwall connection closed or error: %d", len);
            break;
        }

        // Check timeout
        if ((xTaskGetTickCount() - last_activity) > timeout_ticks) {
            ESP_LOGI(TAG, "Connection timeout");
            break;
        }

        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(buffer);
    esp_tls_conn_destroy(powerwall_tls);
    esp_tls_conn_destroy(client_tls);
    close(client_sock);
    ESP_LOGI(TAG, "Client connection closed");
    vTaskDelete(NULL);
}

/** TCP Server task */
static void tcp_server_task(void *pvParameters)
{
    // Wait for Ethernet to get IP
    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    xEventGroupWaitBits(s_event_group, ETH_GOT_IP_BIT, false, true, portMAX_DELAY);

    // Create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_socket < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PROXY_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_socket, 3) != 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(server_socket);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTPS Server (TLS termination) listening on port %d", PROXY_PORT);
    ESP_LOGI(TAG, "Ready to decrypt and proxy HTTPS traffic to Powerwall (%s:443)", POWERWALL_IP_STR);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_sock = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection");
            continue;
        }

        char addr_str[32];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Client connected from %s:%d", addr_str, ntohs(client_addr.sin_port));

        // Spawn a new task to handle each client connection
        // This allows multiple simultaneous connections
        BaseType_t task_created = xTaskCreate(handle_client_task, "https_client", 
                                               HTTPS_CLIENT_TASK_STACK_SIZE, (void *)client_sock, 5, NULL);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client handler task");
            close(client_sock);
        }
    }

    close(server_socket);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy ===");
    ESP_LOGI(TAG, "Target: Tesla Powerwall at %s:443", POWERWALL_IP_STR);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Ethernet
    ESP_ERROR_CHECK(init_ethernet());

    // Initialize WiFi
    ESP_ERROR_CHECK(init_wifi());

    // Wait for WiFi connection
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);

    // Initialize mDNS
    init_mdns();

    // Start TCP server task
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}
