/*
 * ESP32-S3 W5500 Ethernet WiFi Bridge (ESP-IDF)
 *
 * This implementation uses ESP-IDF native esp_eth driver with W5500 over SPI.
 * Implements SSL/TLS passthrough proxy without decryption.
 * The proxy forwards encrypted traffic from Ethernet to WiFi and modifies TTL to hide external origin.
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "mdns.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_app_format.h"

#include "config.h"

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

// OTA HTTP server handle
static httpd_handle_t ota_server = NULL;

// ===== Buffer Pool =====
// Preallocated buffers to avoid malloc/free overhead per connection
typedef struct {
    uint8_t client_buffer[PROXY_BUFFER_SIZE];
    uint8_t powerwall_buffer[PROXY_BUFFER_SIZE];
    bool in_use;
} buffer_pair_t;

static buffer_pair_t buffer_pool[MAX_CONCURRENT_CLIENTS];
static SemaphoreHandle_t buffer_pool_mutex = NULL;

/** Initialize the buffer pool */
static void init_buffer_pool(void)
{
    buffer_pool_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
        buffer_pool[i].in_use = false;
    }
    ESP_LOGI(TAG, "Buffer pool initialized: %d slots, %d bytes each",
             MAX_CONCURRENT_CLIENTS, PROXY_BUFFER_SIZE * 2);
}

/** Acquire a buffer pair from the pool. Returns index or -1 if none available */
static int acquire_buffer_pair(void)
{
    int index = -1;
    if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < MAX_CONCURRENT_CLIENTS; i++) {
            if (!buffer_pool[i].in_use) {
                buffer_pool[i].in_use = true;
                index = i;
                break;
            }
        }
        xSemaphoreGive(buffer_pool_mutex);
    }
    return index;
}

/** Release a buffer pair back to the pool */
static void release_buffer_pair(int index)
{
    if (index >= 0 && index < MAX_CONCURRENT_CLIENTS) {
        if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            buffer_pool[index].in_use = false;
            xSemaphoreGive(buffer_pool_mutex);
        }
    }
}

// ===== OTA Update Handlers =====

/** OTA status page - shows current firmware info and upload form */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t ota_state;
    esp_ota_get_state_partition(running, &ota_state);

    char response[1536];
    snprintf(response, sizeof(response),
        "<!DOCTYPE html><html><head><title>ESP32 OTA Update</title>"
        "<style>body{font-family:Arial,sans-serif;margin:40px;background:#f5f5f5;}"
        ".card{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:500px;}"
        "h1{color:#333;margin-top:0;}table{width:100%%;margin:15px 0;}td{padding:5px 0;}"
        ".label{color:#666;}.value{font-family:monospace;}"
        "input[type=file]{margin:10px 0;}"
        "button{background:#4CAF50;color:white;padding:10px 20px;border:none;border-radius:4px;cursor:pointer;}"
        "button:hover{background:#45a049;}.warn{color:#f44336;}</style></head>"
        "<body><div class='card'><h1>OTA Firmware Update</h1>"
        "<table>"
        "<tr><td class='label'>Version:</td><td class='value'>%s</td></tr>"
        "<tr><td class='label'>Build Date:</td><td class='value'>%s %s</td></tr>"
        "<tr><td class='label'>Running Partition:</td><td class='value'>%s (0x%lx)</td></tr>"
        "<tr><td class='label'>Next Update Partition:</td><td class='value'>%s</td></tr>"
        "<tr><td class='label'>App State:</td><td class='value'>%s</td></tr>"
        "</table>"
        "<hr><h3>Upload New Firmware</h3>"
        "<form method='POST' action='/ota/upload' enctype='multipart/form-data'>"
        "<input type='file' name='firmware' accept='.bin'><br>"
        "<button type='submit'>Upload &amp; Update</button>"
        "</form>"
        "<p class='warn'>Device will reboot after successful update.</p>"
        "</div></body></html>",
        app_desc->version,
        app_desc->date, app_desc->time,
        running->label, (unsigned long)running->address,
        update ? update->label : "none",
        ota_state == ESP_OTA_IMG_VALID ? "Valid" :
        ota_state == ESP_OTA_IMG_NEW ? "New (pending validation)" :
        ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "Pending verification" : "Unknown"
    );

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** OTA firmware upload handler */
static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA upload started, content length: %d", req->content_len);

    if (req->content_len > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Firmware too large: %d > %d", req->content_len, OTA_MAX_FIRMWARE_SIZE);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    // Receive and write firmware in chunks
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    int total_received = 0;
    bool header_skipped = false;

    while (total_received < req->content_len) {
        int remaining = req->content_len - total_received;
        received = httpd_req_recv(req, buf, (remaining < 4096) ? remaining : 4096);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Error receiving data: %d", received);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        // Skip multipart form header on first chunk
        char *data = buf;
        int data_len = received;
        if (!header_skipped) {
            char *bin_start = memmem(buf, received, "\r\n\r\n", 4);
            if (bin_start) {
                bin_start += 4;
                data = bin_start;
                data_len = received - (bin_start - buf);
                header_skipped = true;
            }
        }

        if (header_skipped && data_len > 0) {
            // Check for multipart boundary at end (simplified: trim last boundary)
            err = esp_ota_write(ota_handle, data, data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                return ESP_FAIL;
            }
        }

        total_received += received;
        if (total_received % 65536 < 4096) {
            ESP_LOGI(TAG, "OTA progress: %d / %d bytes", total_received, req->content_len);
        }
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed - invalid image?");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting...");

    const char *response = "<!DOCTYPE html><html><head><title>OTA Success</title>"
        "<meta http-equiv='refresh' content='10;url=/'>"
        "<style>body{font-family:Arial,sans-serif;margin:40px;text-align:center;}"
        ".success{color:#4CAF50;font-size:24px;}</style></head>"
        "<body><p class='success'>&#10004; Firmware updated successfully!</p>"
        "<p>Device is rebooting... Redirecting in 10 seconds.</p></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    // Delay to allow response to be sent, then reboot
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/** Manual rollback handler */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Manual rollback requested");

    const esp_partition_t *last_invalid = esp_ota_get_last_invalid_partition();

    if (last_invalid == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No previous partition to rollback to");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_set_boot_partition(last_invalid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rollback failed");
        return ESP_FAIL;
    }

    const char *response = "<!DOCTYPE html><html><head><title>Rollback</title>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "</head><body><p>Rolling back to previous firmware... Rebooting.</p></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK;
}

/** Start the OTA HTTP server */
static esp_err_t start_ota_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = OTA_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&ota_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OTA server: %s", esp_err_to_name(err));
        return err;
    }

    // Register URI handlers
    httpd_uri_t ota_status = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ota_status_handler,
    };
    httpd_register_uri_handler(ota_server, &ota_status);

    httpd_uri_t ota_upload = {
        .uri = "/ota/upload",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
    };
    httpd_register_uri_handler(ota_server, &ota_upload);

    httpd_uri_t ota_rollback = {
        .uri = "/ota/rollback",
        .method = HTTP_POST,
        .handler = ota_rollback_handler,
    };
    httpd_register_uri_handler(ota_server, &ota_rollback);

    ESP_LOGI(TAG, "OTA server started on port %d", OTA_HTTP_PORT);
    return ESP_OK;
}

/** Validate the running firmware (call after successful boot) */
static void validate_ota_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA - validating new firmware...");
            // Mark as valid - firmware booted successfully
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware validated successfully!");
        }
    }
}

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

    // Create TXT records with device info
    mdns_txt_item_t txt_records[] = {
        {"wifi_ssid", WIFI_SSID},
        {"target", POWERWALL_IP_STR},
        {"ota_port", "8080"},
    };

    mdns_service_add(NULL, MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT, txt_records,
                     sizeof(txt_records) / sizeof(txt_records[0]));
    ESP_LOGI(TAG, "mDNS service added: %s.%s on port %d (wifi: %s)",
             MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT, WIFI_SSID);
}

/** WiFi quality monitoring task - periodically logs connection quality */
static void wifi_quality_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WiFi quality monitoring started (interval: %d seconds)", WIFI_QUALITY_LOG_INTERVAL_SEC);
    
    while (1) {
        // Wait for the configured interval
        vTaskDelay(pdMS_TO_TICKS(WIFI_QUALITY_LOG_INTERVAL_SEC * 1000));
        
        // Check if WiFi is connected
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            wifi_ap_record_t ap_info;
            esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
            
            if (err == ESP_OK) {
                // Log WiFi connection quality metrics
                ESP_LOGI(TAG, "WiFi Quality - RSSI: %d dBm, Channel: %d, Auth: %d", 
                         ap_info.rssi, ap_info.primary, ap_info.authmode);
                
                // Provide quality interpretation
                if (ap_info.rssi > -50) {
                    ESP_LOGI(TAG, "WiFi Signal: Excellent");
                } else if (ap_info.rssi > -60) {
                    ESP_LOGI(TAG, "WiFi Signal: Good");
                } else if (ap_info.rssi > -70) {
                    ESP_LOGI(TAG, "WiFi Signal: Fair");
                } else {
                    ESP_LOGW(TAG, "WiFi Signal: Weak");
                }
            } else {
                ESP_LOGW(TAG, "Failed to get WiFi AP info: %d", err);
            }
        } else {
            ESP_LOGW(TAG, "WiFi not connected - skipping quality check");
        }
    }
}

/** System monitoring task - periodically logs system metrics */
static void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitoring started (interval: %d seconds)", SYSTEM_MONITOR_INTERVAL_SEC);
    
    while (1) {
        // Wait for the configured interval
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_INTERVAL_SEC * 1000));
        
        // Get free heap size
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();
        
        // Log system status
        ESP_LOGI(TAG, "System Status - Free Heap: %lu bytes, Min Free: %lu bytes", 
                 (unsigned long)free_heap, 
                 (unsigned long)min_free_heap);
        
        // Provide heap health interpretation
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "Heap Status: Critical - Low memory!");
        } else if (free_heap < 50000) {
            ESP_LOGW(TAG, "Heap Status: Warning - Limited memory");
        } else if (free_heap < 100000) {
            ESP_LOGI(TAG, "Heap Status: Fair");
        } else {
            ESP_LOGI(TAG, "Heap Status: Good");
        }
    }
}

/** SSL/TLS Passthrough Proxy task - forwards encrypted packets without decryption */
static void handle_client_task(void *pvParameters)
{
    int client_sock = (int)pvParameters;
    int buffer_index = -1;

    ESP_LOGI(TAG, "Handling client connection (SSL passthrough mode)");

    // Acquire buffer pair from pool (avoids malloc/free overhead)
    buffer_index = acquire_buffer_pair();
    if (buffer_index < 0) {
        ESP_LOGE(TAG, "No buffers available - max concurrent clients (%d) reached", MAX_CONCURRENT_CLIENTS);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Connect to Powerwall via TCP (no TLS, just raw socket)
    struct sockaddr_in powerwall_addr;
    powerwall_addr.sin_family = AF_INET;
    powerwall_addr.sin_port = htons(443);
    inet_pton(AF_INET, POWERWALL_IP_STR, &powerwall_addr.sin_addr);

    int powerwall_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (powerwall_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket to Powerwall");
        release_buffer_pair(buffer_index);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Set TTL to hide that traffic is coming from outside the network
    // Common TTL values: 64 (Linux/Unix), 128 (Windows), 255 (Cisco)
    // Using 64 as it's the most common default
    int ttl = TTL_VALUE;
    if (setsockopt(powerwall_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
        ESP_LOGW(TAG, "Failed to set TTL on socket: %d", errno);
    } else {
        ESP_LOGI(TAG, "Set TTL to %d on outgoing connection", ttl);
    }

    // Set timeouts on both sockets
    struct timeval timeout = {.tv_sec = PROXY_TIMEOUT_MS / 1000, .tv_usec = (PROXY_TIMEOUT_MS % 1000) * 1000};
    if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set timeout on client socket: %d", errno);
    }
    if (setsockopt(powerwall_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGW(TAG, "Failed to set timeout on powerwall socket: %d", errno);
    }

    // Connect to Powerwall
    if (connect(powerwall_sock, (struct sockaddr *)&powerwall_addr, sizeof(powerwall_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to connect to Powerwall at %s:443 - error: %d", POWERWALL_IP_STR, errno);
        release_buffer_pair(buffer_index);
        close(powerwall_sock);
        close(client_sock);
        vTaskDelete(NULL);
        return;
    }

    // Disable Nagle's algorithm for lower latency on both sockets
    int nodelay = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    setsockopt(powerwall_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ESP_LOGI(TAG, "Connected to Powerwall at %s:443 (encrypted passthrough)", POWERWALL_IP_STR);

    // Get buffer pointers from the preallocated pool
    uint8_t *client_buffer = buffer_pool[buffer_index].client_buffer;
    uint8_t *powerwall_buffer = buffer_pool[buffer_index].powerwall_buffer;

    // Set both sockets to non-blocking mode for bidirectional forwarding
    int flags = fcntl(client_sock, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGW(TAG, "Failed to set client socket to non-blocking mode: %d", errno);
        }
    } else {
        ESP_LOGW(TAG, "Failed to get client socket flags: %d", errno);
    }
    
    flags = fcntl(powerwall_sock, F_GETFL, 0);
    if (flags >= 0) {
        if (fcntl(powerwall_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            ESP_LOGW(TAG, "Failed to set powerwall socket to non-blocking mode: %d", errno);
        }
    } else {
        ESP_LOGW(TAG, "Failed to get powerwall socket flags: %d", errno);
    }

    TickType_t last_activity = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(PROXY_TIMEOUT_MS);

    #if DEBUG_MODE
    // Track request/response timing for performance logging
    TickType_t request_sent_time = 0;
    bool awaiting_response = false;
    #endif

    // Bidirectional forwarding loop using select() for efficient I/O multiplexing
    while (1) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(client_sock, &read_fds);
        FD_SET(powerwall_sock, &read_fds);
        
        int max_fd = (client_sock > powerwall_sock) ? client_sock : powerwall_sock;
        
        // Use select with a small timeout for activity checking
        struct timeval select_timeout = {.tv_sec = 0, .tv_usec = 100000}; // 100ms
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &select_timeout);
        
        if (ready < 0) {
            ESP_LOGE(TAG, "select() error: %d", errno);
            break;
        } else if (ready == 0) {
            // Timeout - check for inactivity timeout
            if ((xTaskGetTickCount() - last_activity) > timeout_ticks) {
                ESP_LOGI(TAG, "Connection timeout - no activity for %d ms", PROXY_TIMEOUT_MS);
                break;
            }
            continue;
        }

        // Client -> Powerwall: Forward encrypted data
        if (FD_ISSET(client_sock, &read_fds)) {
            int len = recv(client_sock, client_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                #if DEBUG_MODE
                // Record request sent time for response time tracking
                request_sent_time = xTaskGetTickCount();
                awaiting_response = true;
                #endif

                // Forward encrypted data to Powerwall
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(powerwall_sock, client_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Wait for socket to become writable using select()
                            fd_set write_fds;
                            FD_ZERO(&write_fds);
                            FD_SET(powerwall_sock, &write_fds);
                            struct timeval write_timeout = {.tv_sec = 5, .tv_usec = 0};
                            if (select(powerwall_sock + 1, NULL, &write_fds, NULL, &write_timeout) <= 0) {
                                ESP_LOGE(TAG, "Timeout waiting for Powerwall socket writability");
                                goto cleanup;
                            }
                            continue;
                        }
                        ESP_LOGE(TAG, "Error sending to Powerwall: %d", errno);
                        goto cleanup;
                    }
                    total_sent += sent;
                }
                
                last_activity = xTaskGetTickCount();
                
                #if DEBUG_MODE
                ESP_LOGI(TAG, "Forwarded %d bytes from client to Powerwall (encrypted)", len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, client_buffer, len < 64 ? len : 64, ESP_LOG_INFO);
                #endif
            } else if (len == 0) {
                ESP_LOGI(TAG, "Client closed connection");
                break;
            } else {
                ESP_LOGE(TAG, "Error reading from client: %d", errno);
                break;
            }
        }

        // Powerwall -> Client: Forward encrypted data
        if (FD_ISSET(powerwall_sock, &read_fds)) {
            int len = recv(powerwall_sock, powerwall_buffer, PROXY_BUFFER_SIZE, 0);
            if (len > 0) {
                #if DEBUG_MODE
                // Calculate and log response time if we were awaiting a response
                if (awaiting_response) {
                    TickType_t response_time = xTaskGetTickCount() - request_sent_time;
                    uint32_t response_time_ms = response_time * portTICK_PERIOD_MS;
                    ESP_LOGI(TAG, "Response time: %lu ms", (unsigned long)response_time_ms);
                    awaiting_response = false;
                }
                #endif
                
                // Forward encrypted data to client
                int total_sent = 0;
                while (total_sent < len) {
                    int sent = send(client_sock, powerwall_buffer + total_sent, len - total_sent, 0);
                    if (sent < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Wait for socket to become writable using select()
                            fd_set write_fds;
                            FD_ZERO(&write_fds);
                            FD_SET(client_sock, &write_fds);
                            struct timeval write_timeout = {.tv_sec = 5, .tv_usec = 0};
                            if (select(client_sock + 1, NULL, &write_fds, NULL, &write_timeout) <= 0) {
                                ESP_LOGE(TAG, "Timeout waiting for client socket writability");
                                goto cleanup;
                            }
                            continue;
                        }
                        ESP_LOGE(TAG, "Error sending to client: %d", errno);
                        goto cleanup;
                    }
                    total_sent += sent;
                }
                
                last_activity = xTaskGetTickCount();
                
                #if DEBUG_MODE
                ESP_LOGI(TAG, "Forwarded %d bytes from Powerwall to client (encrypted)", len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, powerwall_buffer, len < 64 ? len : 64, ESP_LOG_INFO);
                #endif
            } else if (len == 0) {
                ESP_LOGI(TAG, "Powerwall closed connection");
                break;
            } else {
                ESP_LOGE(TAG, "Error reading from Powerwall: %d", errno);
                break;
            }
        }
    }

cleanup:
    release_buffer_pair(buffer_index);
    close(powerwall_sock);
    close(client_sock);

    ESP_LOGI(TAG, "Client connection closed (passthrough mode)");
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

    ESP_LOGI(TAG, "TCP Server (SSL passthrough) listening on port %d", PROXY_PORT);
    ESP_LOGI(TAG, "Ready to forward encrypted SSL/TLS traffic to Powerwall (%s:443) with TTL modification", POWERWALL_IP_STR);

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
        BaseType_t task_created = xTaskCreate(handle_client_task, "ssl_passthrough", 
                                               SSL_PASSTHROUGH_TASK_STACK_SIZE, (void *)client_sock, 5, NULL);
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
    ESP_LOGI(TAG, "=== ESP32-S3-POE-ETH WiFi-Ethernet SSL Bridge ===");
    ESP_LOGI(TAG, "Mode: SSL Passthrough (no decryption, TTL modification)");
    ESP_LOGI(TAG, "Target: Tesla Powerwall at %s:443", POWERWALL_IP_STR);

    // Print firmware version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware version: %s (built %s %s)", app_desc->version, app_desc->date, app_desc->time);

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

    // Initialize buffer pool for proxy connections
    init_buffer_pool();

    // Start WiFi quality monitoring task (increased stack size for logging)
    xTaskCreate(wifi_quality_monitor_task, "wifi_monitor", 3072, NULL, 3, NULL);

    // Start system monitoring task (CPU load, memory)
    xTaskCreate(system_monitor_task, "sys_monitor", 3072, NULL, 3, NULL);

    // Start TCP server task
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    // Start OTA HTTP server (on Ethernet interface)
    start_ota_server();

    // Validate OTA image after successful network initialization
    // This marks the firmware as valid, preventing automatic rollback
    // If the device crashes before this point, bootloader will rollback
    validate_ota_image();

    ESP_LOGI(TAG, "System fully initialized - OTA available at http://<eth-ip>:%d/", OTA_HTTP_PORT);
}
