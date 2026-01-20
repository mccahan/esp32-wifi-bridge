/*
 * Web Server with WebSerial and OTA Support
 * 
 * Provides:
 * - WebSerial interface for viewing logs via WebSocket
 * - OTA firmware update via HTTP POST
 * - Web UI for both features
 */

#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "webserver.h"

static const char *TAG = "webserver";

static httpd_handle_t server = NULL;
static SemaphoreHandle_t ws_clients_mutex = NULL;

// WebSocket client tracking
typedef struct {
    int fd;
    bool active;
} ws_client_t;

static ws_client_t ws_clients[WEBSERIAL_MAX_CLIENTS] = {0};

// HTML page for WebSerial
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
"let ws;let logs=[];"
"function connect(){"
"const proto=(location.protocol==='https:')?'wss:':'ws:';"
"ws=new WebSocket(proto+'//'+location.host+'/ws');"
"ws.onopen=()=>{"
"document.getElementById('status').className='status connected';"
"document.getElementById('status').textContent='Connected';"
"document.getElementById('connectBtn').disabled=true;"
"addLog('WebSerial connected');"
"};"
"ws.onclose=()=>{"
"document.getElementById('status').className='status disconnected';"
"document.getElementById('status').textContent='Disconnected';"
"document.getElementById('connectBtn').disabled=false;"
"addLog('WebSerial disconnected');"
"};"
"ws.onerror=(e)=>{addLog('WebSocket error: '+e);};"
"ws.onmessage=(e)=>{addLog(e.data);};"
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

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake request");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // First, check frame type
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket frame: type=%d, len=%d", ws_pkt.type, ws_pkt.len);
    
    // If it's a ping, send pong
    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &ws_pkt);
    }
    
    // Track new client connection
    if (ws_clients_mutex && xSemaphoreTake(ws_clients_mutex, pdMS_TO_TICKS(1000))) {
        int fd = httpd_req_to_sockfd(req);
        bool found = false;
        
        for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
            if (ws_clients[i].fd == fd && ws_clients[i].active) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
                if (!ws_clients[i].active) {
                    ws_clients[i].fd = fd;
                    ws_clients[i].active = true;
                    ESP_LOGI(TAG, "New WebSerial client connected: fd=%d, slot=%d", fd, i);
                    
                    // Send welcome message
                    const char *welcome = "=== ESP32 WiFi-Ethernet Bridge WebSerial ===\n";
                    httpd_ws_frame_t welcome_pkt = {
                        .type = HTTPD_WS_TYPE_TEXT,
                        .payload = (uint8_t *)welcome,
                        .len = strlen(welcome)
                    };
                    httpd_ws_send_frame(req, &welcome_pkt);
                    break;
                }
            }
        }
        
        xSemaphoreGive(ws_clients_mutex);
    }
    
    return ESP_OK;
}

// Root page handler - redirect to WebSerial
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
        if (!image_header_checked) {
            esp_app_desc_t new_app_info;
            if (received >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                memcpy(&new_app_info, &buf[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
                
                const esp_partition_t *running = esp_ota_get_running_partition();
                esp_app_desc_t running_app_info;
                if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                    ESP_LOGI(TAG, "Current firmware version: %s", running_app_info.version);
                }
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
    if (!server || !ws_clients_mutex) {
        return;
    }
    
    if (xSemaphoreTake(ws_clients_mutex, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
            if (ws_clients[i].active) {
                httpd_ws_frame_t ws_pkt = {
                    .type = HTTPD_WS_TYPE_TEXT,
                    .payload = (uint8_t *)message,
                    .len = strlen(message)
                };
                
                // Try to send, if it fails, mark client as inactive
                if (httpd_ws_send_frame_async(server, ws_clients[i].fd, &ws_pkt) != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to send to WebSerial client fd=%d, marking inactive", ws_clients[i].fd);
                    ws_clients[i].active = false;
                    ws_clients[i].fd = -1;
                }
            }
        }
        xSemaphoreGive(ws_clients_mutex);
    }
}

// Start HTTP server
esp_err_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.ctrl_port = WEB_SERVER_PORT + 1;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    // Create mutex for WebSocket clients
    ws_clients_mutex = xSemaphoreCreateMutex();
    if (!ws_clients_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // Initialize client tracking
    for (int i = 0; i < WEBSERIAL_MAX_CLIENTS; i++) {
        ws_clients[i].fd = -1;
        ws_clients[i].active = false;
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
        
        // WebSocket endpoint
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &ws_uri);
        
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
    
    if (ws_clients_mutex) {
        vSemaphoreDelete(ws_clients_mutex);
        ws_clients_mutex = NULL;
    }
}
