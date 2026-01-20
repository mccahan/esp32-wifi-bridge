/*
 * Web Server with WebSerial and OTA Support
 * 
 * Provides:
 * - WebSerial interface for viewing logs via Server-Sent Events (SSE)
 * - OTA firmware update via HTTP POST
 * - Web UI for both features
 */

#include <string.h>
#include <sys/socket.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "config.h"
#include "webserver.h"

static const char *TAG = "webserver";

static httpd_handle_t server = NULL;
static SemaphoreHandle_t sse_clients_mutex = NULL;
static QueueHandle_t log_queue = NULL;

// SSE client tracking
typedef struct {
    int fd;
    bool active;
} sse_client_t;

static sse_client_t sse_clients[WEBSERIAL_MAX_CLIENTS] = {0};

// HTML page for WebSerial using Server-Sent Events
static const char *webserial_html = 
"<!DOCTYPE html><html><head><title>WebSerial - ESP32 Bridge</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:Arial,sans-serif;margin:20px;background:#1e1e1e;color:#fff}"
"h1{color:#4CAF50}h2{color:#2196F3;margin-top:30px}"
".container{max-width:1200px;margin:0 auto}"
".console{background:#000;color:#0f0;font-family:monospace;padding:10px;"
"height:500px;overflow-y:auto;border:1px solid #444;border-radius:5px;margin:10px 0}"
".status{padding:10px;margin:10px 0;border-radius:5px}"
".connected{background:#1b5e20;border:1px solid #4CAF50}"
".disconnected{background:#b71c1c;border:1px solid #f44336}"
"button{background:#4CAF50;color:#fff;border:none;padding:10px 20px;margin:5px;"
"cursor:pointer;border-radius:5px;font-size:14px}"
"button:hover{background:#45a049}"
"button:disabled{background:#666;cursor:not-allowed}"
".upload-form{background:#2e2e2e;padding:20px;border-radius:5px;margin:10px 0}"
"input[type=file]{margin:10px 0}"
".progress{width:100%;height:30px;background:#444;border-radius:5px;margin:10px 0;display:none}"
".progress-bar{height:100%;background:#4CAF50;border-radius:5px;transition:width 0.3s}"
"#progress-text{margin-top:5px;display:none}"
"</style></head><body><div class='container'>"
"<h1>ESP32 WiFi-Ethernet Bridge</h1>"
"<h2>WebSerial Monitor</h2>"
"<div id='status' class='status disconnected'>Disconnected</div>"
"<button onclick='connect()' id='connectBtn'>Connect</button>"
"<button onclick='clearConsole()'>Clear</button>"
"<button onclick='downloadLogs()'>Download Logs</button>"
"<div id='console' class='console'></div>"
"<h2>OTA Firmware Update</h2>"
"<div class='upload-form'>"
"<form id='uploadForm' onsubmit='uploadFirmware(event)'>"
"<input type='file' id='firmwareFile' accept='.bin' required>"
"<button type='submit' id='uploadBtn'>Upload Firmware</button>"
"</form>"
"<div class='progress' id='progress'><div class='progress-bar' id='progressBar'></div></div>"
"<div id='progress-text'></div>"
"</div>"
"<script>"
"let eventSource;let logs=[];"
"function connect(){"
"if(eventSource){eventSource.close();}"
"eventSource=new EventSource('/events');"
"eventSource.onopen=()=>{"
"document.getElementById('status').className='status connected';"
"document.getElementById('status').textContent='Connected';"
"document.getElementById('connectBtn').disabled=true;"
"addLog('WebSerial connected');"
"};"
"eventSource.onerror=()=>{"
"document.getElementById('status').className='status disconnected';"
"document.getElementById('status').textContent='Disconnected';"
"document.getElementById('connectBtn').disabled=false;"
"addLog('WebSerial disconnected');"
"};"
"eventSource.onmessage=(e)=>{addLog(e.data);};"
"}"
"function addLog(msg){"
"logs.push(msg);"
"const console=document.getElementById('console');"
"const line=document.createElement('div');"
"line.textContent=msg;console.appendChild(line);"
"console.scrollTop=console.scrollHeight;"
"}"
"function clearConsole(){"
"document.getElementById('console').innerHTML='';logs=[];"
"}"
"function downloadLogs(){"
"const blob=new Blob([logs.join('\\n')],{type:'text/plain'});"
"const url=URL.createObjectURL(blob);"
"const a=document.createElement('a');"
"a.href=url;a.download='esp32-logs.txt';a.click();"
"URL.revokeObjectURL(url);"
"}"
"function uploadFirmware(e){"
"e.preventDefault();"
"const file=document.getElementById('firmwareFile').files[0];"
"if(!file){alert('Please select a firmware file');return;}"
"const formData=new FormData();"
"formData.append('firmware',file);"
"const xhr=new XMLHttpRequest();"
"xhr.upload.onprogress=(e)=>{"
"if(e.lengthComputable){"
"const pct=(e.loaded/e.total)*100;"
"document.getElementById('progress').style.display='block';"
"document.getElementById('progressBar').style.width=pct+'%';"
"document.getElementById('progress-text').style.display='block';"
"document.getElementById('progress-text').textContent='Uploading: '+pct.toFixed(1)+'%';"
"}"
"};"
"xhr.onload=()=>{"
"if(xhr.status===200){"
"document.getElementById('progress-text').textContent='Upload complete! Device will reboot...';"
"setTimeout(()=>{location.reload();},5000);"
"}else{"
"document.getElementById('progress-text').textContent='Upload failed: '+xhr.responseText;"
"}"
"};"
"xhr.onerror=()=>{"
"document.getElementById('progress-text').textContent='Upload error';"
"};"
"document.getElementById('uploadBtn').disabled=true;"
"xhr.open('POST','/ota',true);"
"xhr.send(formData);"
"}"
"window.onload=connect;"
"</script></div></body></html>";

// Server-Sent Events handler for log streaming
static esp_err_t events_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "New SSE client connected");
    
    // Set headers for Server-Sent Events
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    
    int fd = httpd_req_to_sockfd(req);
    
    // Register this client
    if (sse_clients_mutex && xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(1000))) {
        for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
            if (!sse_clients[i].active) {
                sse_clients[i].fd = fd;
                sse_clients[i].active = true;
                ESP_LOGI(TAG, "SSE client registered: fd=%d, slot=%d", fd, i);
                break;
            }
        }
        xSemaphoreGive(sse_clients_mutex);
    }
    
    // Send welcome message
    const char *welcome = "data: === ESP32 WiFi-Ethernet Bridge WebSerial ===\n\n";
    httpd_resp_send_chunk(req, welcome, strlen(welcome));
    
    // Keep connection alive - client will handle reconnect if needed
    // The connection will be maintained and messages sent via webserial_send_task
    
    return ESP_OK;
}

// Root page handler - serve WebSerial interface
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, webserial_html, strlen(webserial_html));
}

// OTA update handler
static esp_err_t ota_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle;
    const esp_partition_t *ota_partition = NULL;
    esp_err_t err;
    char buf[1024];
    int received;
    int remaining = req->content_len;
    bool image_header_checked = false;
    
    ESP_LOGI(TAG, "Starting OTA update, size: %d bytes", remaining);
    
    // Get next OTA partition
    ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx", 
             ota_partition->label, ota_partition->address);
    
    // Begin OTA
    err = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return err;
    }
    
    // Receive and write firmware data
    while (remaining > 0) {
        int to_read = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        received = httpd_req_recv(req, buf, to_read);
        
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "File reception failed");
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Reception failed");
            return ESP_FAIL;
        }
        
        // Check image header on first chunk
        if (!image_header_checked && received > 32) {
            // Simple validation - check ESP32 image magic byte (0xE9)
            if (buf[0] == 0xE9) {
                ESP_LOGI(TAG, "OTA image header validated (ESP32 magic byte present)");
                
                // Try to get running partition info for comparison
                const esp_partition_t *running = esp_ota_get_running_partition();
                esp_app_desc_t running_app_info;
                if (running && esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                    ESP_LOGI(TAG, "Current firmware version: %s", running_app_info.version);
                    ESP_LOGI(TAG, "Updating to new firmware...");
                }
            } else {
                ESP_LOGE(TAG, "Invalid firmware image - missing ESP32 magic byte");
                esp_ota_abort(ota_handle);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware image");
                return ESP_FAIL;
            }
            image_header_checked = true;
        }
        
        // Write chunk to OTA partition
        err = esp_ota_write(ota_handle, (const void *)buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return err;
        }
        
        remaining -= received;
        ESP_LOGD(TAG, "OTA progress: %d bytes remaining", remaining);
    }
    
    // End OTA and set boot partition
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return err;
    }
    
    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return err;
    }
    
    ESP_LOGI(TAG, "OTA update successful. Rebooting...");
    httpd_resp_sendstr(req, "OTA update successful. Rebooting...");
    
    // Reboot after short delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

// Send message to all WebSerial clients
void webserial_send(const char *message)
{
    if (!server || !log_queue) {
        return;
    }
    
    // Queue the message for sending by the WebSerial task
    // Don't block if queue is full - just drop the message
    char *msg_copy = malloc(strlen(message) + 1);
    if (msg_copy) {
        strcpy(msg_copy, message);
        if (xQueueSend(log_queue, &msg_copy, 0) != pdTRUE) {
            // Queue full, drop message
            free(msg_copy);
        }
    }
}

// Task to send queued log messages to SSE clients
static void webserial_send_task(void *pvParameters)
{
    char *message;
    // Buffer for SSE formatted message: "data: <message>\n\n"
    // Reserve 10 bytes for SSE protocol overhead
    char sse_buffer[WEBSERIAL_LOG_LINE_MAX + 10];
    
    while (1) {
        // Wait for a message in the queue
        if (xQueueReceive(log_queue, &message, portMAX_DELAY) == pdTRUE) {
            if (!message || !server || !sse_clients_mutex) {
                if (message) free(message);
                continue;
            }
            
            // Format as SSE message: "data: <message>\n\n"
            int len = snprintf(sse_buffer, sizeof(sse_buffer), "data: %s\n\n", message);
            if (len < 0 || len >= sizeof(sse_buffer)) {
                free(message);
                continue;
            }
            
            // Take mutex to access client list
            if (xSemaphoreTake(sse_clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
                    if (sse_clients[i].active && sse_clients[i].fd >= 0) {
                        // Set a short timeout on the socket
                        struct timeval tv = {.tv_sec = 0, .tv_usec = 50000}; // 50ms
                        setsockopt(sse_clients[i].fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                        
                        // Send SSE formatted message
                        ssize_t sent = send(sse_clients[i].fd, sse_buffer, len, MSG_DONTWAIT);
                        if (sent <= 0) {
                            // Failed to send, mark client inactive
                            ESP_LOGD(TAG, "SSE client fd=%d inactive, removing", sse_clients[i].fd);
                            sse_clients[i].active = false;
                            sse_clients[i].fd = -1;
                        }
                    }
                }
                xSemaphoreGive(sse_clients_mutex);
            }
            
            free(message);
        }
    }
}

// Start HTTP server
esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.ctrl_port = WEB_SERVER_PORT + 1;
    config.max_open_sockets = WEB_SERVER_MAX_SOCKETS;
    config.lru_purge_enable = true;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    // Create mutex for SSE clients
    sse_clients_mutex = xSemaphoreCreateMutex();
    if (!sse_clients_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // Create queue for log messages
    log_queue = xQueueCreate(WEBSERIAL_LOG_QUEUE_SIZE, sizeof(char *));
    if (!log_queue) {
        ESP_LOGE(TAG, "Failed to create log queue");
        vSemaphoreDelete(sse_clients_mutex);
        return ESP_FAIL;
    }
    
    // Initialize client tracking
    for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
        sse_clients[i].fd = -1;
        sse_clients[i].active = false;
    }
    
    // Start task to send messages to SSE clients
    if (xTaskCreate(webserial_send_task, "webserial_send", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create webserial send task");
        vQueueDelete(log_queue);
        vSemaphoreDelete(sse_clients_mutex);
        return ESP_FAIL;
    }
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Root page
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
        
        // SSE endpoint for log streaming
        httpd_uri_t events_uri = {
            .uri = "/events",
            .method = HTTP_GET,
            .handler = events_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &events_uri);
        
        // OTA endpoint
        httpd_uri_t ota_uri = {
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = ota_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_uri);
        
        ESP_LOGI(TAG, "HTTP server started successfully");
        ESP_LOGI(TAG, "WebSerial available at http://<device-ip>/");
        ESP_LOGI(TAG, "OTA endpoint available at http://<device-ip>/ota");
        
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return ESP_FAIL;
}

// Stop HTTP server
void stop_webserver(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
    }
    
    if (sse_clients_mutex) {
        vSemaphoreDelete(sse_clients_mutex);
        sse_clients_mutex = NULL;
    }
    
    if (log_queue) {
        vQueueDelete(log_queue);
        log_queue = NULL;
    }
}
