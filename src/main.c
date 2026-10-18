/*
 * ESP32 WiFi-Ethernet Bridge (ESP-IDF)
 *
 * This implementation uses ESP-IDF native esp_eth driver: W5500 over SPI on the
 * ESP32-S3-POE-ETH, the internal EMAC with an IP101 PHY on the GL.iNet GL-S10.
 * Implements SSL/TLS passthrough proxy without decryption.
 * The proxy forwards encrypted traffic from Ethernet to WiFi and modifies TTL to hide external origin.
 */

#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
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
#include "esp_timer.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

#include "config.h"
#include "web_ui.h"
#include "favicon_png.h"
#include "proxy.h"
#include "remote_ota.h"
#include "wifi_metrics.h"

static const char *TAG = "wifi-eth-bridge";

// NVS namespace for WiFi credentials
#define NVS_WIFI_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

// NVS namespace for Ethernet LAN settings
#define NVS_ETH_NAMESPACE "eth_config"
#define NVS_KEY_ETH_STATIC "static"
#define NVS_KEY_ETH_IP "ip"
#define NVS_KEY_ETH_MASK "mask"
#define NVS_KEY_ETH_GW "gw"
#define NVS_KEY_ETH_DNS "dns"
#define NVS_KEY_ETH_FORCE_DHCP "force_dhcp"
#define NVS_KEY_ETH_FELL_BACK "fell_back"

#define NVS_ADMIN_NAMESPACE "admin"
#define NVS_KEY_ADMIN_SALT "salt"
#define NVS_KEY_ADMIN_HASH "hash"
#define NVS_WD_NAMESPACE "watchdog"
#define NVS_KEY_WD_MODE "mode"
#define ADMIN_SALT_LEN 16
#define ADMIN_HASH_LEN 32

// WiFi credentials (runtime, loaded from NVS)
static char wifi_ssid[33] = "";
static char wifi_password[65] = "";
static bool wifi_configured = false;  // True if credentials saved in NVS

// Ethernet LAN configuration (runtime, loaded from NVS)
typedef struct {
    bool use_static;            // Saved preference: static vs DHCP
    bool using_fallback;        // This boot is on DHCP due to fallback/BOOT
    bool fell_back_last_boot;   // Sticky UI warning until the user saves
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
} eth_lan_config_t;

static eth_lan_config_t eth_cfg = {0};
static volatile bool eth_lan_confirmed = false;  // HTTP or proxy traffic seen

static uint8_t admin_salt[ADMIN_SALT_LEN];
static uint8_t admin_hash[ADMIN_HASH_LEN];
static bool admin_configured = false;

// Powerwall connectivity status
static volatile bool powerwall_reachable = false;
static volatile int64_t last_powerwall_check = 0;

// Boot time for uptime calculation
static int64_t boot_time_us = 0;

// Connection watchdog timestamp (proxy-clients mode)
static volatile int64_t last_successful_connection_time = 0;
// false = proxy clients (default). true = Powerwall link.
static volatile bool wd_link_mode = false;
static volatile bool wifi_was_up = false;
static volatile int64_t link_down_since_us = 0;

// Event group for WiFi and Ethernet status
static EventGroupHandle_t s_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define ETH_CONNECTED_BIT BIT1
#define ETH_GOT_IP_BIT BIT2

// CPU usage tracking (percentage, 0-100)
static volatile uint8_t cpu_usage_percent = 0;

// On-die temperature (°C). chip_temp_ok is false until the first good sample.
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t temp_handle = NULL;
#endif
static volatile float chip_temp_c = 0;
static volatile float chip_temp_max_c = 0;
static volatile bool chip_temp_ok = false;
static volatile bool chip_temp_max_ok = false;

// ===== Log Capture Ring Buffer =====
#define LOG_BUFFER_SIZE 200
#define LOG_MSG_MAX_LEN 160

typedef struct {
    int64_t timestamp;      // Seconds since boot
    uint8_t level;          // ESP_LOG_ERROR=1, WARN=2, INFO=3, DEBUG=4, VERBOSE=5
    char message[LOG_MSG_MAX_LEN];
} log_entry_t;

static log_entry_t log_buffer[LOG_BUFFER_SIZE];
static int log_buffer_index = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t original_vprintf = NULL;

static void log_sanitize(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\x1b' && r[1] == '[') {
            r += 2;
            while (*r && *r != 'm') {
                r++;
            }
            if (*r == 'm') {
                r++;
            }
            continue;
        }
        if (*r == '\r') {
            r++;
            continue;
        }
        if (*r == '\n' || *r == '\t') {
            if (w > s && w[-1] != ' ') {
                *w++ = ' ';
            }
            r++;
            continue;
        }
        *w++ = *r++;
    }
    while (w > s && w[-1] == ' ') {
        w--;
    }
    *w = '\0';
}

static bool log_is_httpd_noise(const char *s)
{
    const char *p = strchr(s, ')');
    if (!p || p[1] != ' ') {
        return s[0] == '\0';
    }
    p += 2;
    return strncmp(p, "httpd", 5) == 0;
}

/** Custom log handler to capture logs to ring buffer */
static int custom_log_vprintf(const char *fmt, va_list args)
{
    // Always call original handler first
    int ret = 0;
    if (original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }

    // Try to capture log (non-blocking to avoid deadlocks)
    if (log_mutex && xSemaphoreTake(log_mutex, 0) == pdTRUE) {
        char temp[LOG_MSG_MAX_LEN + 32];
        va_list args_copy;
        va_copy(args_copy, args);
        vsnprintf(temp, sizeof(temp), fmt, args_copy);
        va_end(args_copy);
        log_sanitize(temp);

        if (temp[0] == '\0' || log_is_httpd_noise(temp)) {
            xSemaphoreGive(log_mutex);
            return ret;
        }

        log_entry_t *entry = &log_buffer[log_buffer_index];
        entry->timestamp = esp_timer_get_time() / 1000000;

        entry->level = 3;
        if (temp[0] == 'E') entry->level = 1;
        else if (temp[0] == 'W') entry->level = 2;
        else if (temp[0] == 'I') entry->level = 3;
        else if (temp[0] == 'D') entry->level = 4;
        else if (temp[0] == 'V') entry->level = 5;

        strncpy(entry->message, temp, LOG_MSG_MAX_LEN - 1);
        entry->message[LOG_MSG_MAX_LEN - 1] = '\0';

        log_buffer_index = (log_buffer_index + 1) % LOG_BUFFER_SIZE;
        xSemaphoreGive(log_mutex);
    }

    return ret;
}

/** Initialize the log capture system */
static void init_log_capture(void)
{
    log_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
        log_buffer[i].timestamp = 0;
        log_buffer[i].level = 0;
        log_buffer[i].message[0] = '\0';
    }
    // Install custom log handler
    original_vprintf = esp_log_set_vprintf(custom_log_vprintf);
    ESP_LOGI(TAG, "Log capture initialized: %d entries", LOG_BUFFER_SIZE);
}

// Ethernet and WiFi handles
static esp_eth_handle_t eth_handle = NULL;
static esp_netif_t *eth_netif = NULL;
static esp_netif_t *wifi_netif = NULL;

// OTA HTTP server handle
static httpd_handle_t web_server = NULL;
static uint32_t http_listen_addr = 0;  // IPv4 the HTTP socket is bound to (0 = not bound)

// ===== NVS WiFi Credential Storage =====

/** Load WiFi credentials from NVS - returns true if credentials exist */
static bool load_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi credentials in NVS");
        wifi_configured = false;
        return false;
    }

    size_t ssid_len = sizeof(wifi_ssid);
    size_t pass_len = sizeof(wifi_password);

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, wifi_ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, wifi_password, &pass_len);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK && strlen(wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Loaded WiFi credentials from NVS: SSID=%s", wifi_ssid);
        wifi_configured = true;
        return true;
    }

    ESP_LOGI(TAG, "No valid WiFi credentials found");
    wifi_configured = false;
    return false;
}

/** Save WiFi credentials to NVS */
static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi credentials saved to NVS: SSID=%s", ssid);
        // Update runtime credentials
        strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
        strncpy(wifi_password, password, sizeof(wifi_password) - 1);
        wifi_password[sizeof(wifi_password) - 1] = '\0';
    } else {
        ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
    }

    return err;
}

// ===== Ethernet LAN (static IP / DHCP fallback) =====

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]);
            int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *start = strstr(body, pattern);
    if (!start) {
        if (out_len) out[0] = '\0';
        return false;
    }
    start += strlen(pattern);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    url_decode_inplace(out);
    return true;
}

static bool hash_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void hash_admin_password(const char *password, const uint8_t salt[ADMIN_SALT_LEN],
                                uint8_t out[ADMIN_HASH_LEN])
{
    unsigned char buf[ADMIN_SALT_LEN + ADMIN_MAX_PASSWORD_LEN];
    size_t plen = strlen(password);
    if (plen > ADMIN_MAX_PASSWORD_LEN) plen = ADMIN_MAX_PASSWORD_LEN;
    memcpy(buf, salt, ADMIN_SALT_LEN);
    memcpy(buf + ADMIN_SALT_LEN, password, plen);
    mbedtls_sha256(buf, ADMIN_SALT_LEN + plen, out, 0);
    memset(buf, 0, sizeof(buf));
}

#if !SOC_TEMP_SENSOR_SUPPORTED
// The original ESP32 has no usable on-die sensor: the tile shows "—".
static void init_temp_sensor(void) {}
static void sample_chip_temp(void) { chip_temp_ok = false; }
#else
static void init_temp_sensor(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &temp_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Chip temperature sensor not available");
        temp_handle = NULL;
        return;
    }
    if (temperature_sensor_enable(temp_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable temperature sensor");
        temperature_sensor_uninstall(temp_handle);
        temp_handle = NULL;
        return;
    }
    ESP_LOGI(TAG, "Chip temperature sensor enabled");
}

static void sample_chip_temp(void)
{
    if (!temp_handle) {
        chip_temp_ok = false;
        return;
    }
    float t = 0;
    if (temperature_sensor_get_celsius(temp_handle, &t) == ESP_OK) {
        chip_temp_c = t;
        chip_temp_ok = true;
        if (!chip_temp_max_ok || t > chip_temp_max_c) {
            chip_temp_max_c = t;
            chip_temp_max_ok = true;
        }
    }
}
#endif // SOC_TEMP_SENSOR_SUPPORTED

static const char *chip_temp_color(void)
{
    if (!chip_temp_ok) {
        return "#94a3b8";
    }
    if (chip_temp_c >= 80.0f) {
        return "#ef4444";
    }
    if (chip_temp_c >= 65.0f) {
        return "#eab308";
    }
    return "#22c55e";
}

static void load_admin_config(void)
{
    memset(admin_salt, 0, sizeof(admin_salt));
    memset(admin_hash, 0, sizeof(admin_hash));
    admin_configured = false;

    nvs_handle_t h;
    if (nvs_open(NVS_ADMIN_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No admin password set - first-boot setup required");
        return;
    }

    size_t salt_len = ADMIN_SALT_LEN;
    size_t hash_len = ADMIN_HASH_LEN;
    esp_err_t salt_err = nvs_get_blob(h, NVS_KEY_ADMIN_SALT, admin_salt, &salt_len);
    esp_err_t hash_err = nvs_get_blob(h, NVS_KEY_ADMIN_HASH, admin_hash, &hash_len);
    nvs_close(h);

    if (salt_err == ESP_OK && hash_err == ESP_OK &&
        salt_len == ADMIN_SALT_LEN && hash_len == ADMIN_HASH_LEN) {
        admin_configured = true;
        ESP_LOGI(TAG, "Admin password is configured");
    } else {
        ESP_LOGI(TAG, "No admin password set - first-boot setup required");
    }
}

#define SESSION_COUNT 4
#define SESSION_ID_LEN 16
#define SESSION_IDLE_US (7LL * 24 * 3600 * 1000000)
#define SESSION_COOKIE_MAX_AGE 604800
#define SESSION_COOKIE_NAME "sid"

typedef struct {
    bool used;
    uint8_t id[SESSION_ID_LEN];
    int64_t last_us;
} admin_session_t;

static admin_session_t sessions[SESSION_COUNT];

static void session_clear_all(void)
{
    memset(sessions, 0, sizeof(sessions));
}

static void bytes_to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static bool hex_to_bytes(const char *s, uint8_t *out, size_t n)
{
    if (!s || strlen(s) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool session_create(char hex_out[SESSION_ID_LEN * 2 + 1])
{
    int slot = 0;
    int64_t now = esp_timer_get_time();
    int64_t oldest = sessions[0].last_us;
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (!sessions[i].used || now - sessions[i].last_us > SESSION_IDLE_US) {
            slot = i;
            break;
        }
        if (sessions[i].last_us < oldest) {
            oldest = sessions[i].last_us;
            slot = i;
        }
    }
    esp_fill_random(sessions[slot].id, SESSION_ID_LEN);
    sessions[slot].used = true;
    sessions[slot].last_us = now;
    bytes_to_hex(sessions[slot].id, SESSION_ID_LEN, hex_out);
    return true;
}

static bool session_touch(const char *hex)
{
    uint8_t id[SESSION_ID_LEN];
    if (!hex_to_bytes(hex, id, SESSION_ID_LEN)) return false;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (!sessions[i].used) continue;
        if (now - sessions[i].last_us > SESSION_IDLE_US) {
            sessions[i].used = false;
            continue;
        }
        if (hash_equal(sessions[i].id, id, SESSION_ID_LEN)) {
            sessions[i].last_us = now;
            return true;
        }
    }
    return false;
}

static void session_drop(const char *hex)
{
    uint8_t id[SESSION_ID_LEN];
    if (!hex_to_bytes(hex, id, SESSION_ID_LEN)) return;
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (sessions[i].used && hash_equal(sessions[i].id, id, SESSION_ID_LEN)) {
            sessions[i].used = false;
            memset(sessions[i].id, 0, SESSION_ID_LEN);
        }
    }
}

static bool request_is_https(httpd_req_t *req)
{
    char proto[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Forwarded-Proto", proto, sizeof(proto)) != ESP_OK) {
        return false;
    }
    return strcasecmp(proto, "https") == 0;
}

static void cookie_expires_gmt(char *out, size_t len, int offset_sec)
{
    time_t now = time(NULL);
    if (now < 1700000000 || !out || len < 32) {
        if (out && len) {
            out[0] = '\0';
        }
        return;
    }
    time_t exp = now + offset_sec;
    struct tm tm;
    gmtime_r(&exp, &tm);
    strftime(out, len, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void session_cookie_set(char *buf, size_t len, const char *hex, httpd_req_t *req)
{
    char exp[40] = {0};
    cookie_expires_gmt(exp, sizeof(exp), SESSION_COOKIE_MAX_AGE);
    snprintf(buf, len,
             SESSION_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s%s%s",
             hex, SESSION_COOKIE_MAX_AGE,
             exp[0] ? "; Expires=" : "", exp,
             request_is_https(req) ? "; Secure" : "");
}

static void session_cookie_clear(char *buf, size_t len, httpd_req_t *req)
{
    snprintf(buf, len,
             SESSION_COOKIE_NAME "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0"
             "; Expires=Thu, 01 Jan 1970 00:00:00 GMT%s",
             request_is_https(req) ? "; Secure" : "");
}

static bool session_from_req(httpd_req_t *req, char *hex_out, size_t hex_len)
{
    char sid[SESSION_ID_LEN * 2 + 1] = {0};
    size_t sid_len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, SESSION_COOKIE_NAME, sid, &sid_len) != ESP_OK) {
        return false;
    }
    if (hex_out && hex_len) {
        strncpy(hex_out, sid, hex_len - 1);
        hex_out[hex_len - 1] = '\0';
    }
    return session_touch(sid);
}

static bool uri_path_is(const char *uri, const char *path)
{
    size_t n = strlen(path);
    if (strncmp(uri, path, n) != 0) return false;
    return uri[n] == '\0' || uri[n] == '?';
}

static bool uri_is_api(const char *uri)
{
    return strncmp(uri, "/api/", 5) == 0 || uri_path_is(uri, "/wifi/scan");
}

static esp_err_t save_admin_password(const char *password)
{
    uint8_t new_salt[ADMIN_SALT_LEN];
    uint8_t new_hash[ADMIN_HASH_LEN];
    esp_fill_random(new_salt, sizeof(new_salt));
    hash_admin_password(password, new_salt, new_hash);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_ADMIN_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_blob(h, NVS_KEY_ADMIN_SALT, new_salt, sizeof(new_salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_ADMIN_HASH, new_hash, sizeof(new_hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        memcpy(admin_salt, new_salt, sizeof(admin_salt));
        memcpy(admin_hash, new_hash, sizeof(admin_hash));
        admin_configured = true;
        ESP_LOGI(TAG, "Admin password saved");
    } else {
        ESP_LOGE(TAG, "Failed to save admin password: %s", esp_err_to_name(err));
    }
    return err;
}

static void erase_admin_password(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_ADMIN_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_ADMIN_SALT);
        nvs_erase_key(h, NVS_KEY_ADMIN_HASH);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(admin_salt, 0, sizeof(admin_salt));
    memset(admin_hash, 0, sizeof(admin_hash));
    admin_configured = false;
    session_clear_all();
    ESP_LOGW(TAG, "Admin password cleared");
}

static bool admin_password_valid(const char *password)
{
    if (!password) return false;
    size_t len = strlen(password);
    return len >= ADMIN_MIN_PASSWORD_LEN && len <= ADMIN_MAX_PASSWORD_LEN;
}

static bool ipv4_parse_ok(const char *s, esp_ip4_addr_t *out)
{
    if (!s || s[0] == '\0') return false;
    esp_ip4_addr_t tmp;
    if (esp_netif_str_to_ip4(s, &tmp) != ESP_OK) return false;
    if (out) *out = tmp;
    return true;
}

static bool eth_static_config_valid(const eth_lan_config_t *cfg)
{
    esp_ip4_addr_t ip, mask;
    if (!ipv4_parse_ok(cfg->ip, &ip) || !ipv4_parse_ok(cfg->netmask, &mask)) {
        return false;
    }
    if (ip.addr == 0 || ip.addr == 0xFFFFFFFFu) return false;
    if (mask.addr == 0) return false;
    if (cfg->gateway[0] && !ipv4_parse_ok(cfg->gateway, NULL)) return false;
    if (cfg->dns[0] && !ipv4_parse_ok(cfg->dns, NULL)) return false;
    return true;
}

static void load_eth_config(void)
{
    memset(&eth_cfg, 0, sizeof(eth_cfg));

    nvs_handle_t h;
    if (nvs_open(NVS_ETH_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved Ethernet IP config (using DHCP)");
        return;
    }

    uint8_t st = 0, force = 0, fell = 0;
    nvs_get_u8(h, NVS_KEY_ETH_STATIC, &st);
    nvs_get_u8(h, NVS_KEY_ETH_FORCE_DHCP, &force);
    nvs_get_u8(h, NVS_KEY_ETH_FELL_BACK, &fell);

    size_t len = sizeof(eth_cfg.ip);
    nvs_get_str(h, NVS_KEY_ETH_IP, eth_cfg.ip, &len);
    len = sizeof(eth_cfg.netmask);
    nvs_get_str(h, NVS_KEY_ETH_MASK, eth_cfg.netmask, &len);
    len = sizeof(eth_cfg.gateway);
    nvs_get_str(h, NVS_KEY_ETH_GW, eth_cfg.gateway, &len);
    len = sizeof(eth_cfg.dns);
    nvs_get_str(h, NVS_KEY_ETH_DNS, eth_cfg.dns, &len);
    nvs_close(h);

    eth_cfg.use_static = (st != 0) && eth_static_config_valid(&eth_cfg);
    eth_cfg.fell_back_last_boot = (fell != 0);

    if (force) {
        ESP_LOGW(TAG, "One-shot DHCP fallback requested; ignoring static IP this boot");
        eth_cfg.using_fallback = true;
        if (nvs_open(NVS_ETH_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, NVS_KEY_ETH_FORCE_DHCP, 0);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    ESP_LOGI(TAG, "Ethernet config: mode=%s ip=%s mask=%s gw=%s dns=%s fallback=%s",
             eth_cfg.use_static ? "static" : "dhcp",
             eth_cfg.ip[0] ? eth_cfg.ip : "-",
             eth_cfg.netmask[0] ? eth_cfg.netmask : "-",
             eth_cfg.gateway[0] ? eth_cfg.gateway : "-",
             eth_cfg.dns[0] ? eth_cfg.dns : "-",
             eth_cfg.using_fallback ? "yes" : "no");
}

static esp_err_t save_eth_config(bool use_static, const char *ip, const char *mask,
                                 const char *gw, const char *dns)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_ETH_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open eth NVS: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_u8(h, NVS_KEY_ETH_STATIC, use_static ? 1 : 0);
    nvs_set_str(h, NVS_KEY_ETH_IP, ip ? ip : "");
    nvs_set_str(h, NVS_KEY_ETH_MASK, mask ? mask : "");
    nvs_set_str(h, NVS_KEY_ETH_GW, gw ? gw : "");
    nvs_set_str(h, NVS_KEY_ETH_DNS, dns ? dns : "");
    nvs_set_u8(h, NVS_KEY_ETH_FORCE_DHCP, 0);
    nvs_set_u8(h, NVS_KEY_ETH_FELL_BACK, 0);
    err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        eth_cfg.use_static = use_static;
        strncpy(eth_cfg.ip, ip ? ip : "", sizeof(eth_cfg.ip) - 1);
        eth_cfg.ip[sizeof(eth_cfg.ip) - 1] = '\0';
        strncpy(eth_cfg.netmask, mask ? mask : "", sizeof(eth_cfg.netmask) - 1);
        eth_cfg.netmask[sizeof(eth_cfg.netmask) - 1] = '\0';
        strncpy(eth_cfg.gateway, gw ? gw : "", sizeof(eth_cfg.gateway) - 1);
        eth_cfg.gateway[sizeof(eth_cfg.gateway) - 1] = '\0';
        strncpy(eth_cfg.dns, dns ? dns : "", sizeof(eth_cfg.dns) - 1);
        eth_cfg.dns[sizeof(eth_cfg.dns) - 1] = '\0';
        eth_cfg.using_fallback = false;
        eth_cfg.fell_back_last_boot = false;
        ESP_LOGI(TAG, "Ethernet config saved: %s", use_static ? "static" : "dhcp");
    }
    return err;
}

static void request_dhcp_fallback_and_reboot(const char *reason)
{
    ESP_LOGW(TAG, "DHCP fallback: %s", reason);
    nvs_handle_t h;
    if (nvs_open(NVS_ETH_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_ETH_FORCE_DHCP, 1);
        nvs_set_u8(h, NVS_KEY_ETH_FELL_BACK, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static esp_err_t apply_eth_static_ip(void)
{
    if (!eth_netif) return ESP_ERR_INVALID_STATE;

    esp_err_t stop_err = esp_netif_dhcpc_stop(eth_netif);
    if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(stop_err));
    }

    esp_netif_ip_info_t info;
    memset(&info, 0, sizeof(info));
    if (esp_netif_str_to_ip4(eth_cfg.ip, &info.ip) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (esp_netif_str_to_ip4(eth_cfg.netmask, &info.netmask) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (eth_cfg.gateway[0]) {
        esp_netif_str_to_ip4(eth_cfg.gateway, &info.gw);
    }

    esp_err_t err = esp_netif_set_ip_info(eth_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(err));
        return err;
    }

    const char *dns_str = eth_cfg.dns[0] ? eth_cfg.dns : eth_cfg.gateway;
    if (dns_str[0]) {
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_str_to_ip4(dns_str, &dns.ip.u_addr.ip4) == ESP_OK) {
            esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns);
            remote_ota_remember_eth_dns(dns.ip.u_addr.ip4.addr, 0);
        }
    }

    ESP_LOGI(TAG, "Applied static Ethernet IP %s mask %s gw %s dns %s",
             eth_cfg.ip, eth_cfg.netmask,
             eth_cfg.gateway[0] ? eth_cfg.gateway : "0.0.0.0",
             dns_str[0] ? dns_str : "0.0.0.0");
    return ESP_OK;
}

typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seqno;
} __attribute__((packed)) icmp_echo_hdr_t;

static uint16_t icmp_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint16_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/** 1 = echo reply, 0 = no reply, -1 = ICMP not available */
static int icmp_ping_host(uint32_t dest_addr, int timeout_ms)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGW(TAG, "ICMP socket unavailable (%d)", errno);
        return -1;
    }

    struct timeval tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    icmp_echo_hdr_t echo = {0};
    echo.type = 8;  // ICMP_ECHO
    echo.id = htons(0xB10C);
    echo.seqno = htons(1);
    echo.checksum = 0;
    echo.checksum = htons(icmp_checksum(&echo, sizeof(echo)));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = dest_addr;

    if (sendto(sock, &echo, sizeof(echo), 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        return 0;
    }

    char buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    close(sock);
    if (n <= 0) return 0;

    int iphdr_len = 0;
    if (n >= 20 && (buf[0] >> 4) == 4) {
        iphdr_len = (buf[0] & 0x0F) * 4;
    }
    if (n < iphdr_len + (int)sizeof(icmp_echo_hdr_t)) return 0;
    icmp_echo_hdr_t *rx = (icmp_echo_hdr_t *)(buf + iphdr_len);
    return rx->type == 0 ? 1 : 0;  // ICMP_ECHOREPLY
}

static bool eth_lan_looks_alive(void)
{
    if (eth_lan_confirmed) return true;
    if (last_successful_connection_time > 0 &&
        last_successful_connection_time >= boot_time_us) {
        return true;
    }
    return false;
}

static void eth_dhcp_fallback_task(void *pvParameters)
{
    (void)pvParameters;

    if (!eth_cfg.use_static || eth_cfg.using_fallback) {
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group, ETH_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(ETH_DHCP_FALLBACK_SEC * 1000));
    if ((bits & ETH_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "Ethernet link never came up; not falling back to DHCP");
        vTaskDelete(NULL);
        return;
    }

    esp_ip4_addr_t gw = {0};
    bool have_gw = ipv4_parse_ok(eth_cfg.gateway, &gw) && gw.addr != 0;
    if (!have_gw) {
        ESP_LOGI(TAG, "No gateway configured; skipping DHCP fallback ping (BOOT still forces DHCP)");
        vTaskDelete(NULL);
        return;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)ETH_DHCP_FALLBACK_SEC * 1000000LL;
    bool pinged_ok = false;

    while (esp_timer_get_time() < deadline) {
        if (eth_lan_looks_alive()) {
            ESP_LOGI(TAG, "LAN traffic seen; keeping static IP");
            vTaskDelete(NULL);
            return;
        }
        bits = xEventGroupGetBits(s_event_group);
        if ((bits & ETH_CONNECTED_BIT) == 0) {
            ESP_LOGW(TAG, "Ethernet link dropped during fallback check");
            vTaskDelete(NULL);
            return;
        }
        if (have_gw) {
            int ping = icmp_ping_host(gw.addr, 1000);
            if (ping == 1) {
                pinged_ok = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ETH_DHCP_FALLBACK_PING_INTERVAL_MS));
    }

    if (eth_lan_looks_alive() || pinged_ok) {
        ESP_LOGI(TAG, "Gateway reachable; keeping static IP");
        vTaskDelete(NULL);
        return;
    }

    request_dhcp_fallback_and_reboot("static IP gateway unreachable and no LAN traffic");
    vTaskDelete(NULL);
}

// ===== Board LEDs =====

#if BOARD_GL_S10
static void init_leds(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_POWER_GPIO) | (1ULL << LED_NETWORK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LED_POWER_GPIO, 0);    // active low: on
    gpio_set_level(LED_NETWORK_GPIO, 1);  // off until Wi-Fi is up
}

static void set_network_led(bool on)
{
    gpio_set_level(LED_NETWORK_GPIO, on ? 0 : 1);
}
#else
static void init_leds(void) {}
static void set_network_led(bool on) { (void)on; }
#endif

static void eth_boot_button_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << ETH_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    int held_ticks = 0;
    const int hold_ticks = (ETH_BOOT_HOLD_SEC * 1000) / ETH_BOOT_POLL_MS;
    while (1) {
        if (gpio_get_level(ETH_BOOT_GPIO) == 0) {
            held_ticks++;
            if (held_ticks == 1) {
                ESP_LOGI(TAG, RECOVERY_BUTTON_NAME " held - keep holding %d seconds to force DHCP and clear admin password",
                         ETH_BOOT_HOLD_SEC);
            } else if (held_ticks < hold_ticks && (held_ticks * ETH_BOOT_POLL_MS) % 5000 == 0) {
                ESP_LOGI(TAG, RECOVERY_BUTTON_NAME " held %ds / %ds",
                         (held_ticks * ETH_BOOT_POLL_MS) / 1000, ETH_BOOT_HOLD_SEC);
            }
            if (held_ticks >= hold_ticks) {
                erase_admin_password();
                request_dhcp_fallback_and_reboot(RECOVERY_BUTTON_NAME " button held (DHCP + admin reset)");
            }
        } else {
            held_ticks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(ETH_BOOT_POLL_MS));
    }
}

// ===== Powerwall Connectivity Check =====

/** Link mode probes slowly. Proxy mode keeps the live 5 s tile while the page is open. */
static bool powerwall_check_due(void)
{
    if (last_powerwall_check == 0) {
        return true;
    }
    int64_t age_ms = (esp_timer_get_time() / 1000) - last_powerwall_check;
    int64_t limit_ms = wd_link_mode
        ? (int64_t)WATCHDOG_LINK_PROBE_SEC * 1000
        : 5000;
    return age_ms >= limit_ms;
}

/** Check if Powerwall is reachable (non-blocking TCP connect test) */
static void check_powerwall_connectivity(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        powerwall_reachable = false;
        return;
    }

    // Set socket to non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    inet_pton(AF_INET, POWERWALL_IP_STR, &addr.sin_addr);

    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (result == 0) {
        powerwall_reachable = true;
    } else if (errno == EINPROGRESS) {
        // Wait for connection with timeout
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        struct timeval tv = {.tv_sec = 2, .tv_usec = 0};

        if (select(sock + 1, NULL, &write_fds, NULL, &tv) > 0) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
            powerwall_reachable = (error == 0);
        } else {
            powerwall_reachable = false;
        }
    } else {
        powerwall_reachable = false;
    }

    close(sock);
    last_powerwall_check = esp_timer_get_time() / 1000;  // Convert to ms
    if (wd_link_mode && wifi_was_up) {
        if (powerwall_reachable) {
            link_down_since_us = 0;
        } else if (link_down_since_us == 0) {
            link_down_since_us = esp_timer_get_time();
        }
    }
}

// ===== OTA Update Handlers =====
// Icons, CSS, and JavaScript are defined in web_ui.h

/** Favicon handler - PNG so Android Chrome / iOS shortcuts render it */
static esp_err_t send_png(httpd_req_t *req, const unsigned char *data, size_t len)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    httpd_resp_send(req, (const char *)data, (ssize_t)len);
    return ESP_OK;
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    return send_png(req, favicon_png, favicon_png_len);
}

static esp_err_t apple_touch_handler(httpd_req_t *req)
{
    return send_png(req, apple_touch_png, apple_touch_png_len);
}

/** Liveness for HAProxy. No auth, no Wi-Fi/proxy checks — 200 means httpd is up. */
static esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}

/** OTA status page - modern dark theme with WiFi config */
static esp_err_t send_admin_setup_page(httpd_req_t *req, const char *error_msg);

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    eth_lan_confirmed = true;

    if (!admin_configured) {
        return send_admin_setup_page(req, NULL);
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    esp_ota_get_state_partition(running, &ota_state);

    // Get WiFi status
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    wifi_ap_record_t ap_info = {0};
    int rssi = 0;
    if (wifi_connected) {
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    // Check Powerwall connectivity (rate-limited)
    if (powerwall_check_due()) {
        check_powerwall_connectivity();
    }

    // Get IP address
    esp_netif_ip_info_t ip_info;
    char ip_str[16] = "N/A";
    if (wifi_netif && esp_netif_get_ip_info(wifi_netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    esp_netif_ip_info_t eth_ip_info;
    char eth_ip_str[16] = "N/A";
    char eth_mask_str[16] = "";
    char eth_gw_str[16] = "";
    char eth_dns_str[16] = "";
    if (eth_netif && esp_netif_get_ip_info(eth_netif, &eth_ip_info) == ESP_OK) {
        snprintf(eth_ip_str, sizeof(eth_ip_str), IPSTR, IP2STR(&eth_ip_info.ip));
        snprintf(eth_mask_str, sizeof(eth_mask_str), IPSTR, IP2STR(&eth_ip_info.netmask));
        snprintf(eth_gw_str, sizeof(eth_gw_str), IPSTR, IP2STR(&eth_ip_info.gw));
    }
    if (eth_netif) {
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            snprintf(eth_dns_str, sizeof(eth_dns_str), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }
    }

    const char *form_ip = eth_cfg.ip[0] ? eth_cfg.ip : eth_ip_str;
    const char *form_mask = eth_cfg.netmask[0] ? eth_cfg.netmask : (eth_mask_str[0] ? eth_mask_str : "255.255.255.0");
    const char *form_gw = eth_cfg.gateway[0] ? eth_cfg.gateway : eth_gw_str;
    const char *form_dns = eth_cfg.dns[0] ? eth_cfg.dns : eth_dns_str;
    if (strcmp(form_ip, "N/A") == 0) form_ip = "";

    const char *eth_mode_label = eth_cfg.using_fallback ? "DHCP (fallback)" :
                                 (eth_cfg.use_static ? "Static" : "DHCP");

    // Build response - split into chunks for memory efficiency
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    // Send HTML head and CSS separately (CSS is too large for single buffer)
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<link rel=\"apple-touch-icon\" href=\"/apple-touch-icon.png\">"
        "<title>ESP32 WiFi Bridge</title><style>");
    httpd_resp_sendstr_chunk(req, DARK_CSS);
    httpd_resp_sendstr_chunk(req,
        "svg.i{width:1.125rem;height:1.125rem;vertical-align:middle;margin-right:0.25rem;fill:currentColor}"
        "</style></head><body><div class=\"container\">");

    char buf[512];

    // Status card - header
    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h1>" ICON_ROUTER " ESP32 WiFi Bridge</h1><div class=\"grid\">");

    // WiFi status (clickable to show/hide WiFi config)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"status-item\" style=\"cursor:pointer\" onclick=\"document.getElementById('wificfg').style.display=document.getElementById('wificfg').style.display==='none'?'block':'none'\">"
        "<div class=\"label\">" ICON_WIFI " WiFi " ICON_SETTINGS "</div>");
    snprintf(buf, sizeof(buf),
        "<div class=\"value\"><span class=\"status-dot %s\"></span>%s</div></div>",
        wifi_connected ? "status-ok" : "status-err",
        wifi_connected ? "Connected" : "Disconnected");
    httpd_resp_sendstr_chunk(req, buf);

    // Signal strength (4-bar icon + colored RSSI; JS sets bar count)
    if (wifi_connected) {
        const char *sig_color = rssi > -50 ? "#22c55e" : rssi > -60 ? "#84cc16" : rssi > -70 ? "#eab308" : "#ef4444";
        const char *sig_quality = rssi > -50 ? "Excellent" : rssi > -60 ? "Good" : rssi > -70 ? "Fair" : "Weak";
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">" ICON_SIGNAL " Signal</div>"
            "<div class=\"value\" id=\"sig\"><span style=\"color:%s\">%d dBm (%s)</span></div></div>",
            sig_color, rssi, sig_quality);
    } else {
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">" ICON_SIGNAL " Signal</div><div class=\"value\" id=\"sig\">-</div></div>");
    }
    httpd_resp_sendstr_chunk(req, buf);

    // Powerwall status
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">" ICON_BATTERY " Powerwall</div>"
        "<div class=\"value\"><span class=\"status-dot %s\"></span>%s</div></div>",
        powerwall_reachable ? "status-ok" : "status-err",
        powerwall_reachable ? "Reachable" : "Unreachable");
    httpd_resp_sendstr_chunk(req, buf);

    // Target IP
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">" ICON_DNS " Target</div><div class=\"value\">%s</div></div>",
        POWERWALL_IP_STR);
    httpd_resp_sendstr_chunk(req, buf);

    // Ethernet IP (clickable to show/hide Ethernet config)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"status-item\" style=\"cursor:pointer\" onclick=\"document.getElementById('ethcfg').style.display=document.getElementById('ethcfg').style.display==='none'?'block':'none'\">"
        "<div class=\"label\">" ICON_LAN " Ethernet " ICON_SETTINGS "</div>");
    snprintf(buf, sizeof(buf),
        "<div class=\"value\" id=\"ethip\">%s</div>"
        "<div class=\"text-xs text-muted\">%s</div></div>"
        "</div></div>",
        eth_ip_str, eth_mode_label);
    httpd_resp_sendstr_chunk(req, buf);

    // WiFi Configuration card (hidden by default, toggle via WiFi Status click)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\" id=\"wificfg\" style=\"display:none\"><h2>" ICON_SETTINGS " WiFi Configuration</h2>"
        "<form method=\"POST\" action=\"/wifi/save\">"
        "<div class=\"form-group\"><label class=\"label\">Network SSID</label>");

    snprintf(buf, sizeof(buf),
        "<input type=\"text\" name=\"ssid\" id=\"ssid\" value=\"%s\" placeholder=\"Enter SSID\" class=\"mt-1\">", wifi_ssid);
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "<div class=\"flex mt-1\">"
        "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"scanWifi()\">" ICON_SEARCH " Scan</button>"
        "<select id=\"wl\" style=\"display:none;flex:1\" onchange=\"document.getElementById('ssid').value=this.value\"></select>"
        "</div></div>");

    httpd_resp_sendstr_chunk(req,
        "<div class=\"form-group\"><label class=\"label\">Password</label>"
        "<input type=\"password\" name=\"password\" placeholder=\"Enter password\" class=\"mt-1\"></div>");

    snprintf(buf, sizeof(buf),
        "<div class=\"text-xs text-muted\" style=\"margin-bottom:0.75rem\">Current: %s</div>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Save &amp; Reconnect</button>"
        "</form></div>", wifi_ssid);
    httpd_resp_sendstr_chunk(req, buf);

    // Ethernet Configuration card (hidden by default, toggle via Ethernet status click)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\" id=\"ethcfg\" style=\"display:none\"><h2>" ICON_LAN " Ethernet Configuration</h2>");
    if (eth_cfg.using_fallback || eth_cfg.fell_back_last_boot) {
        httpd_resp_sendstr_chunk(req,
            "<div class=\"alert alert-warn\" style=\"margin-bottom:0.75rem\">" ICON_WARN
            " Fell back to DHCP because the static gateway was unreachable. "
            "Saved static settings are unchanged. Save again to retry, or switch to DHCP.</div>");
    }
    httpd_resp_sendstr_chunk(req,
        "<form method=\"POST\" action=\"/eth/save\">"
        "<div class=\"form-group\"><label class=\"label\">Address mode</label>"
        "<select name=\"mode\" id=\"ethmode\" onchange=\"toggleEthMode()\" class=\"mt-1\">");
    snprintf(buf, sizeof(buf),
        "<option value=\"dhcp\"%s>DHCP</option>"
        "<option value=\"static\"%s>Static IP</option></select></div>",
        eth_cfg.use_static ? "" : " selected",
        eth_cfg.use_static ? " selected" : "");
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, sizeof(buf),
        "<div id=\"ethstatic\" style=\"display:%s\">"
        "<div class=\"form-group\"><label class=\"label\">IP address</label>"
        "<input type=\"text\" name=\"ip\" value=\"%s\" placeholder=\"192.168.1.50\" class=\"mt-1\"></div>",
        eth_cfg.use_static ? "block" : "none", form_ip);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"form-group\"><label class=\"label\">Subnet mask</label>"
        "<input type=\"text\" name=\"netmask\" value=\"%s\" placeholder=\"255.255.255.0\" class=\"mt-1\"></div>",
        form_mask);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"form-group\"><label class=\"label\">Gateway</label>"
        "<input type=\"text\" name=\"gateway\" value=\"%s\" placeholder=\"192.168.1.1\" class=\"mt-1\"></div>",
        form_gw);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"form-group\"><label class=\"label\">DNS</label>"
        "<input type=\"text\" name=\"dns\" value=\"%s\" placeholder=\"192.168.1.1\" class=\"mt-1\"></div>"
        "</div>", form_dns);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<div class=\"text-xs text-muted\" style=\"margin-bottom:0.75rem\">"
        "Device reboots to apply. If the gateway is unreachable for 45s with no LAN traffic, "
        "it falls back to DHCP. Hold " RECOVERY_BUTTON_NAME " 15 seconds to force DHCP and clear the admin password.</div>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Save & Reboot</button>"
        "</form></div>");

    // System info card
    sample_chip_temp();
    char temp_disp[16] = "—";
    if (chip_temp_ok) {
        snprintf(temp_disp, sizeof(temp_disp), "%.1f °C", (double)chip_temp_c);
    }
    httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h2>" ICON_MEMORY " System</h2><div class=\"grid\">");
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">CPU</div><div class=\"value\" id=\"cpu\">%u%%</div></div>"
        "<div class=\"status-item\"><div class=\"label\">Chip temp</div>"
        "<div class=\"value\" id=\"temp\" style=\"color:%s\">%s</div></div>",
        cpu_usage_percent, chip_temp_color(), temp_disp);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Heap</div><div class=\"value\">%lu KB</div></div>"
        "<div class=\"status-item\"><div class=\"label\">WiFi IP</div><div class=\"value\">%s</div></div>",
        (unsigned long)(esp_get_free_heap_size() / 1024), ip_str);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Ethernet IP</div><div class=\"value\" id=\"ethip2\">%s</div></div>",
        eth_ip_str);
    httpd_resp_sendstr_chunk(req, buf);
    {
        char wd_disp[40];
        const char *wd_color;
        if (wd_link_mode) {
            if (!wifi_was_up) {
                snprintf(wd_disp, sizeof(wd_disp), "Idle");
                wd_color = "#94a3b8";
            } else if (powerwall_reachable) {
                snprintf(wd_disp, sizeof(wd_disp), "Link up");
                wd_color = "#22c55e";
            } else {
                int64_t wd_s = link_down_since_us
                    ? (esp_timer_get_time() - link_down_since_us) / 1000000 : 0;
                if (wd_s >= 60) {
                    snprintf(wd_disp, sizeof(wd_disp), "Link down · %lldm", (long long)(wd_s / 60));
                } else {
                    snprintf(wd_disp, sizeof(wd_disp), "Link down · %llds", (long long)wd_s);
                }
                if (wd_s >= (WATCHDOG_TIMEOUT_SEC * 9) / 10) {
                    wd_color = "#ef4444";
                } else if (wd_s >= (WATCHDOG_TIMEOUT_SEC * 3) / 4) {
                    wd_color = "#eab308";
                } else {
                    wd_color = "#22c55e";
                }
            }
        } else if (last_successful_connection_time == 0) {
            snprintf(wd_disp, sizeof(wd_disp), "Idle");
            wd_color = "#94a3b8";
        } else {
            int64_t wd_s = (esp_timer_get_time() - last_successful_connection_time) / 1000000;
            if (wd_s >= 60) {
                snprintf(wd_disp, sizeof(wd_disp), "Armed · %lldm", (long long)(wd_s / 60));
            } else {
                snprintf(wd_disp, sizeof(wd_disp), "Armed · %llds", (long long)wd_s);
            }
            if (wd_s >= (WATCHDOG_TIMEOUT_SEC * 9) / 10) {
                wd_color = "#ef4444";
            } else if (wd_s >= (WATCHDOG_TIMEOUT_SEC * 3) / 4) {
                wd_color = "#eab308";
            } else {
                wd_color = "#22c55e";
            }
        }
        httpd_resp_sendstr_chunk(req,
            "<div class=\"status-item\" style=\"cursor:pointer\" onclick=\"var p=document.getElementById('wdcfg');p.style.display=p.style.display==='none'?'block':'none'\">"
            "<div class=\"label\">Watchdog " ICON_SETTINGS "</div>");
        snprintf(buf, sizeof(buf),
            "<div class=\"value\" id=\"wdog\" style=\"color:%s\">%s</div></div></div>",
            wd_color, wd_disp);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req,
        "<div id=\"wdcfg\" style=\"display:none\"><hr>"
        "<form method=\"POST\" action=\"/watchdog/save\">"
        "<div class=\"form-group\"><label class=\"label\">Watchdog trigger</label>"
        "<select name=\"mode\" class=\"mt-1\">");
    snprintf(buf, sizeof(buf),
        "<option value=\"proxy\"%s>Proxy clients</option>"
        "<option value=\"link\"%s>Powerwall link</option></select></div>",
        wd_link_mode ? "" : " selected",
        wd_link_mode ? " selected" : "");
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<p class=\"text-xs text-muted\" style=\"margin-bottom:0.75rem\">"
        "Proxy clients reboots after 10 minutes with no proxied client. "
        "Powerwall link reboots only if the Powerwall stays unreachable for 10 minutes. "
        "Client traffic is ignored. Use Powerwall link when another path reaches the gateway "
        "and this bridge is only a fallback.</p>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Save</button>"
        "</form></div>");
    httpd_resp_sendstr_chunk(req,
        "<hr><div class=\"flex\" style=\"gap:0.5rem;flex-wrap:wrap\">"
        "<form id=\"rebootform\" method=\"POST\" action=\"/reboot\">"
        "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"if(confirm('Reboot device?'))document.getElementById('rebootform').submit()\">"
        ICON_UPDATE " Reboot</button></form>"
        "<form method=\"POST\" action=\"/logout\">"
        "<button type=\"submit\" class=\"btn btn-secondary\">Sign out</button></form></div>"
        "<hr><details class=\"adminpw\"><summary><h2>" ICON_LOCK " Admin password " ICON_EXPAND "</h2></summary>"
        "<p class=\"text-xs text-muted\" style=\"margin-bottom:0.75rem\">Username is <code>admin</code>. Hold " RECOVERY_BUTTON_NAME " 15 seconds to clear the password if you get locked out.</p>"
        "<form method=\"POST\" action=\"/admin/password\">"
        "<div class=\"form-group\"><label class=\"label\">Current password</label>"
        "<input type=\"password\" name=\"current\" autocomplete=\"current-password\" class=\"mt-1\"></div>"
        "<div class=\"form-group\"><label class=\"label\">New password</label>"
        "<input type=\"password\" name=\"password\" minlength=\"8\" maxlength=\"64\" autocomplete=\"new-password\" class=\"mt-1\"></div>"
        "<div class=\"form-group\"><label class=\"label\">Confirm new password</label>"
        "<input type=\"password\" name=\"password2\" minlength=\"8\" maxlength=\"64\" autocomplete=\"new-password\" class=\"mt-1\"></div>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Change password</button>"
        "</form></details></div>");

    // Statistics card
    {
        // Get stats from proxy module
        proxy_stats_t stats;
        proxy_get_stats(&stats);

        int64_t uptime_sec = (esp_timer_get_time() - boot_time_us) / 1000000;
        int days = uptime_sec / 86400;
        int hours = (uptime_sec % 86400) / 3600;
        int mins = (uptime_sec % 3600) / 60;
        int secs = uptime_sec % 60;

        uint32_t success_rate = 0;
        if (stats.total_requests > 0) {
            success_rate = (stats.successful_requests * 100) / stats.total_requests;
        }

        // Format bytes with appropriate unit
        const char *in_unit = "B", *out_unit = "B";
        double bytes_in_fmt = stats.total_bytes_in, bytes_out_fmt = stats.total_bytes_out;
        if (bytes_in_fmt >= 1073741824) { bytes_in_fmt /= 1073741824; in_unit = "GB"; }
        else if (bytes_in_fmt >= 1048576) { bytes_in_fmt /= 1048576; in_unit = "MB"; }
        else if (bytes_in_fmt >= 1024) { bytes_in_fmt /= 1024; in_unit = "KB"; }
        if (bytes_out_fmt >= 1073741824) { bytes_out_fmt /= 1073741824; out_unit = "GB"; }
        else if (bytes_out_fmt >= 1048576) { bytes_out_fmt /= 1048576; out_unit = "MB"; }
        else if (bytes_out_fmt >= 1024) { bytes_out_fmt /= 1024; out_unit = "KB"; }

        httpd_resp_sendstr_chunk(req, "<div class=\"card\"><h2>" ICON_SWAP " Statistics</h2><div class=\"grid\">");
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Uptime</div><div class=\"value\" id=\"uptime\">%dd %dh %dm %ds</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Requests</div><div class=\"value\" id=\"reqcnt\">%lu</div></div>",
            days, hours, mins, secs, (unsigned long)stats.total_requests);
        httpd_resp_sendstr_chunk(req, buf);
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Success Rate</div><div class=\"value\" id=\"succrate\" style=\"color:%s\">%lu%%</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Failed</div><div class=\"value\" id=\"failcnt\" style=\"color:#ef4444\">%lu</div></div>",
            success_rate >= 90 ? "#22c55e" : success_rate >= 70 ? "#eab308" : "#ef4444",
            (unsigned long)success_rate, (unsigned long)stats.failed_requests);
        httpd_resp_sendstr_chunk(req, buf);
        snprintf(buf, sizeof(buf),
            "<div class=\"status-item\"><div class=\"label\">Bytes In</div><div class=\"value\" id=\"bytesin\">%.1f %s</div></div>"
            "<div class=\"status-item\"><div class=\"label\">Bytes Out</div><div class=\"value\" id=\"bytesout\">%.1f %s</div></div>",
            bytes_in_fmt, in_unit, bytes_out_fmt, out_unit);
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, "</div></div>");
    }

    // WiFi History Chart card
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_CHART " WiFi Signal History <span id=\"chspan\" class=\"text-muted text-xs\">(since boot)</span></h2>"
        "<div class=\"chart-container\"><canvas id=\"wifichart\"></canvas></div>"
        "<div class=\"chart-legend\">"
        "<span><span class=\"dot\" style=\"background:#3b82f6\"></span>Signal (dBm)</span>"
        "<span><span class=\"dot\" style=\"background:#22c55e\"></span>Connected %</span>"
        "</div>"
        "<div id=\"wifiinfo\" class=\"flex text-xs text-muted\" style=\"justify-content:space-between;margin-top:0.5rem\"></div>"
        "</div>");

    // Recent requests card with TTFB (IDs for auto-refresh)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_SWAP " Recent Requests</h2>"
        "<div class=\"flex\" style=\"justify-content:space-between;margin-bottom:0.5rem\">");
    snprintf(buf, sizeof(buf),
        "<span class=\"text-sm text-muted\">Avg TTFB: <span id=\"avgttfb\">%lu</span> ms</span>",
        (unsigned long)proxy_get_avg_ttfb());
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req,
        "<span class=\"text-xs text-muted\">Updated: <span id=\"lastref\">now</span></span></div>"
        "<table style=\"width:100%;font-size:0.875rem\">"
        "<tr style=\"color:#94a3b8\"><td>Age</td><td>Source</td><td>Req/Resp</td><td>Response</td><td>Status</td></tr>"
        "<tbody id=\"reqtbl\">");

    // Get request log from proxy module
    {
        request_log_entry_t entries[REQUEST_LOG_SIZE];
        int count = 0;
        proxy_get_request_log(entries, &count, REQUEST_LOG_SIZE);

        int64_t now = esp_timer_get_time() / 1000000;
        for (int i = 0; i < count; i++) {
            request_log_entry_t *e = &entries[i];

            int64_t age = now - e->timestamp;
            const char *age_unit = "s";
            if (age >= 3600) { age /= 3600; age_unit = "h"; }
            else if (age >= 60) { age /= 60; age_unit = "m"; }

            const char *status = e->result == 0 ? "OK" : (e->result == 1 ? "TMO" : "ERR");
            const char *color = e->result == 0 ? "#22c55e" : (e->result == 1 ? "#eab308" : "#ef4444");

            // Format source IP
            uint8_t *ip = (uint8_t *)&e->source_ip;

            snprintf(buf, sizeof(buf),
                "<tr><td>%lld%s</td><td>%d.%d.%d.%d</td><td>%lu/%lu</td><td>%u-%ums</td><td style=\"color:%s\">%s</td></tr>",
                (long long)age, age_unit,
                ip[0], ip[1], ip[2], ip[3],
                (unsigned long)e->bytes_in, (unsigned long)e->bytes_out,
                e->ttfb_ms, e->ttlb_ms, color, status);
            httpd_resp_sendstr_chunk(req, buf);
        }
    }

    httpd_resp_sendstr_chunk(req, "</tbody></table></div>");

    // Log Viewer card
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><div class=\"flex\" style=\"justify-content:space-between;align-items:center;margin-bottom:0.5rem\">"
        "<h2 style=\"margin:0\">" ICON_MEMORY " System Logs</h2>"
        "<button type=\"button\" class=\"btn btn-secondary\" onclick=\"dlLogs()\">Download</button></div>"
        "<div style=\"max-height:200px;overflow-y:auto;font-family:monospace;font-size:0.75rem;background:#0f172a;padding:0.5rem;border-radius:0.375rem\" id=\"logview\">");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
            int idx = (log_buffer_index - 1 - i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
            log_entry_t *e = &log_buffer[idx];
            if (e->timestamp == 0 && e->message[0] == '\0') continue;

            const char *color = "#94a3b8";  // Default gray
            if (e->level == 1) color = "#ef4444";       // ERROR - red
            else if (e->level == 2) color = "#eab308";  // WARN - yellow
            else if (e->level == 3) color = "#22c55e";  // INFO - green
            else if (e->level == 4) color = "#94a3b8";  // DEBUG - gray

            // HTML-escape the message
            char escaped[LOG_MSG_MAX_LEN * 2];
            char *out = escaped;
            for (const char *in = e->message; *in && (out - escaped) < (int)sizeof(escaped) - 6; in++) {
                switch (*in) {
                    case '<': memcpy(out, "&lt;", 4); out += 4; break;
                    case '>': memcpy(out, "&gt;", 4); out += 4; break;
                    case '&': memcpy(out, "&amp;", 5); out += 5; break;
                    default: *out++ = *in; break;
                }
            }
            *out = '\0';

            snprintf(buf, sizeof(buf),
                "<div style=\"color:%s;white-space:pre-wrap;word-break:break-word\">%s</div>", color, escaped);
            httpd_resp_sendstr_chunk(req, buf);
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "</div></div>");

    // Firmware card (at bottom)
    httpd_resp_sendstr_chunk(req,
        "<div class=\"card\"><h2>" ICON_UPDATE " Firmware</h2><div class=\"grid\">");

    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Version</div><div class=\"value\">%s</div></div>"
        "<div class=\"status-item\"><div class=\"label\">Built</div><div class=\"value text-sm\">%s</div></div>",
        app_desc->version, app_desc->date);
    httpd_resp_sendstr_chunk(req, buf);

    snprintf(buf, sizeof(buf),
        "<div class=\"status-item\"><div class=\"label\">Partition</div><div class=\"value\">%s</div></div>"
        "<div class=\"status-item\"><div class=\"label\">State</div><div class=\"value\">%s</div></div></div><hr>",
        running->label,
        ota_state == ESP_OTA_IMG_VALID ? "Valid" :
        ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "Pending" : "New");
    httpd_resp_sendstr_chunk(req, buf);

    // Remote update section
    httpd_resp_sendstr_chunk(req,
        "<div id=\"remote-update\" class=\"status-item\" style=\"grid-column:span 2;background:#0f172a\">"
        "<div class=\"flex\" style=\"justify-content:space-between\">"
        "<div><span class=\"label\">Remote Updates</span>"
        "<div class=\"value text-sm\" id=\"update-status\">Not checked</div></div>"
        "<button class=\"btn btn-secondary\" onclick=\"checkUpdate()\">Check for Updates</button>"
        "</div>"
        "<div id=\"update-actions\" style=\"display:none;margin-top:0.75rem\" class=\"flex\">"
        "<button class=\"btn btn-primary\" onclick=\"installUpdate()\">Install Update</button>"
        "<button class=\"btn btn-secondary\" onclick=\"revertFirmware()\" id=\"revert-btn\" style=\"display:none\">Revert to Previous</button>"
        "</div></div><hr>");

    // OTA upload form
    httpd_resp_sendstr_chunk(req,
        "<form method=\"POST\" action=\"/ota/upload\" enctype=\"multipart/form-data\">"
        "<div class=\"form-group\"><label class=\"label\">" ICON_UPLOAD " Upload Firmware (.bin)</label>"
        "<input type=\"file\" name=\"firmware\" accept=\".bin\" class=\"mt-1\"></div>"
        "<div class=\"flex\"><button type=\"submit\" class=\"btn btn-primary\">" ICON_UPLOAD " Upload</button>");
    httpd_resp_sendstr_chunk(req,
        "<button type=\"button\" class=\"btn btn-danger\" onclick=\"if(confirm('Rollback to previous partition?'))document.getElementById('rb').submit()\">" ICON_HISTORY " Rollback</button></div>"
        "</form><form id=\"rb\" method=\"POST\" action=\"/ota/rollback\"></form>"
        "<div class=\"alert alert-warn mt-2\">" ICON_WARN " Device will reboot after update</div></div>");

    // JavaScript for WiFi scanning and auto-refresh (defined in web_ui.h)
    httpd_resp_sendstr_chunk(req, WEB_UI_SCRIPT "</div></body></html>");

    httpd_resp_sendstr_chunk(req, NULL);  // End chunked response
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

/** JSON-escape a string into out (NUL-terminated). Handles ", \\, and control bytes. */
static void json_escape_str(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!out || out_len == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 1 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            if (j + 2 >= out_len) {
                break;
            }
            out[j++] = '\\';
            out[j++] = (char)*p;
        } else if (*p < 0x20) {
            if (j + 6 >= out_len) {
                break;
            }
            int n = snprintf(out + j, out_len - j, "\\u%04x", *p);
            if (n < 0 || (size_t)n >= out_len - j) {
                break;
            }
            j += (size_t)n;
        } else {
            out[j++] = (char)*p;
        }
    }
    out[j] = '\0';
}

/** WiFi scan handler - returns JSON list of networks */
static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting WiFi scan...");

    // Disconnect first - scanning not allowed while connecting
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));  // Brief delay for disconnect to complete

    // Configure scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // Blocking scan
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        // Try to reconnect even if scan failed
        esp_wifi_connect();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        if (ap_count > 20) ap_count = 20;  // Limit results
        ap_list = malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
        }
    }

    // Build JSON response
    httpd_resp_set_type(req, "application/json");

    char buf[256];
    char ssid_json[192];
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");

    for (int i = 0; i < ap_count && ap_list; i++) {
        json_escape_str((const char *)ap_list[i].ssid, ssid_json, sizeof(ssid_json));
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 i > 0 ? "," : "",
                 ssid_json,
                 ap_list[i].rssi);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);

    if (ap_list) free(ap_list);

    // Reconnect to WiFi after scan
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi scan complete: %d networks found", ap_count);
    return ESP_OK;
}

/** WiFi save handler - saves new credentials and reconnects */
static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    char content[256];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "WiFi save request for SSID (password not logged)");

    // Parse form data (ssid=xxx&password=xxx)
    char new_ssid[33] = {0};
    char new_password[65] = {0};

    // Find ssid=
    char *ssid_start = strstr(content, "ssid=");
    if (ssid_start) {
        ssid_start += 5;
        char *ssid_end = strchr(ssid_start, '&');
        size_t ssid_len = ssid_end ? (size_t)(ssid_end - ssid_start) : strlen(ssid_start);
        if (ssid_len > sizeof(new_ssid) - 1) ssid_len = sizeof(new_ssid) - 1;
        strncpy(new_ssid, ssid_start, ssid_len);
    }

    // Find password=
    char *pass_start = strstr(content, "password=");
    if (pass_start) {
        pass_start += 9;
        char *pass_end = strchr(pass_start, '&');
        size_t pass_len = pass_end ? (size_t)(pass_end - pass_start) : strlen(pass_start);
        if (pass_len > sizeof(new_password) - 1) pass_len = sizeof(new_password) - 1;
        strncpy(new_password, pass_start, pass_len);
    }

    // URL decode (basic: + to space, %XX)
    for (char *p = new_ssid; *p; p++) if (*p == '+') *p = ' ';
    for (char *p = new_password; *p; p++) if (*p == '+') *p = ' ';

    if (strlen(new_ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saving new WiFi credentials: SSID=%s", new_ssid);

    // Save to NVS
    esp_err_t err = save_wifi_credentials(new_ssid, new_password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    // Update runtime credentials
    strncpy(wifi_ssid, new_ssid, sizeof(wifi_ssid) - 1);
    strncpy(wifi_password, new_password, sizeof(wifi_password) - 1);
    wifi_configured = true;

    // Send success response before reconnecting
    const char *response =
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"10;url=/\">"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#1e293b;padding:2rem;border-radius:0.75rem;text-align:center;border:1px solid #334155}"
        ".spinner{width:3rem;height:3rem;border:3px solid #334155;border-top:3px solid #3b82f6;border-radius:50%;animation:spin 1s linear infinite;margin:1rem auto}"
        "@keyframes spin{to{transform:rotate(360deg)}}</style></head>"
        "<body><div class=\"box\"><div class=\"spinner\"></div>"
        "<h2>Connecting to WiFi...</h2>"
        "<p>Connecting to network. Page will refresh automatically.</p>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    // Configure and connect to WiFi
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_wifi_disconnect();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, new_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, new_password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi connecting to: %s", new_ssid);
    return ESP_OK;
}

/** Ethernet save handler - saves LAN settings and reboots */
static esp_err_t eth_save_handler(httpd_req_t *req)
{
    char content[384];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    content[received] = '\0';
    ESP_LOGI(TAG, "Ethernet save request: %s", content);

    char mode[16] = {0};
    char ip[16] = {0};
    char mask[16] = {0};
    char gw[16] = {0};
    char dns[16] = {0};
    form_get(content, "mode", mode, sizeof(mode));
    form_get(content, "ip", ip, sizeof(ip));
    form_get(content, "netmask", mask, sizeof(mask));
    form_get(content, "gateway", gw, sizeof(gw));
    form_get(content, "dns", dns, sizeof(dns));

    bool use_static = (strcmp(mode, "static") == 0);
    if (use_static) {
        eth_lan_config_t tmp = {0};
        tmp.use_static = true;
        strncpy(tmp.ip, ip, sizeof(tmp.ip) - 1);
        strncpy(tmp.netmask, mask, sizeof(tmp.netmask) - 1);
        strncpy(tmp.gateway, gw, sizeof(tmp.gateway) - 1);
        strncpy(tmp.dns, dns, sizeof(tmp.dns) - 1);
        if (!eth_static_config_valid(&tmp)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                "Invalid static IP settings. Check IP and subnet mask.");
            return ESP_FAIL;
        }
    }

    esp_err_t err = save_eth_config(use_static, ip, mask, gw, dns);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    const char *new_addr = use_static ? ip : "DHCP address";
    char response[1024];
    snprintf(response, sizeof(response),
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"12;url=http://%s/\">"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#1e293b;padding:2rem;border-radius:0.75rem;text-align:center;border:1px solid #334155;max-width:28rem}"
        ".spinner{width:3rem;height:3rem;border:3px solid #334155;border-top:3px solid #3b82f6;border-radius:50%%;animation:spin 1s linear infinite;margin:1rem auto}"
        "@keyframes spin{to{transform:rotate(360deg)}} a{color:#3b82f6}</style></head>"
        "<body><div class=\"box\"><div class=\"spinner\"></div>"
        "<h2>Applying Ethernet settings</h2>"
        "<p>Rebooting. New address: <strong>%s</strong></p>"
        "<p class=\"text-sm\">If the gateway is unreachable for 45s, the device falls back to DHCP. "
        "Hold " RECOVERY_BUTTON_NAME " 15 seconds to force DHCP. mDNS: <a href=\"http://powerwall.local/\">powerwall.local</a></p>"
        "</div></body></html>",
        use_static ? ip : "powerwall.local", new_addr);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static void load_wd_config(void)
{
    nvs_handle_t h;
    uint8_t mode = 0;
    wd_link_mode = false;
    if (nvs_open(NVS_WD_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (nvs_get_u8(h, NVS_KEY_WD_MODE, &mode) == ESP_OK) {
        wd_link_mode = (mode == 1);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Watchdog trigger: %s", wd_link_mode ? "Powerwall link" : "Proxy clients");
}

/** Watchdog trigger — no reboot. mode=proxy (default) or mode=link. */
static esp_err_t watchdog_save_handler(httpd_req_t *req)
{
    char content[64];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    content[received] = '\0';

    char mode[16] = {0};
    form_get(content, "mode", mode, sizeof(mode));
    bool link = (strcmp(mode, "link") == 0);
    if (!link && strcmp(mode, "proxy") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown watchdog mode");
        return ESP_FAIL;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_WD_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_WD_MODE, link ? 1 : 0);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    wd_link_mode = link;
    link_down_since_us = 0;
    if (!link) {
        /* Fresh window. Do not reboot because the last proxy was long ago. */
        last_successful_connection_time = 0;
    }
    ESP_LOGI(TAG, "Watchdog trigger set to %s", link ? "Powerwall link" : "Proxy clients");

    const char *response =
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/\">"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}</style>"
        "</head><body><p>Saved. Returning to the dashboard.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint for status JSON */
static esp_err_t api_status_handler(httpd_req_t *req)
{
    eth_lan_confirmed = true;

    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    wifi_ap_record_t ap_info = {0};
    int rssi = 0;
    if (wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    // Check Powerwall (link mode reuses a result for WATCHDOG_LINK_PROBE_SEC)
    if (powerwall_check_due()) {
        check_powerwall_connectivity();
    }

    // Calculate uptime in seconds
    int64_t uptime_sec = (esp_timer_get_time() - boot_time_us) / 1000000;

    // Get proxy stats
    proxy_stats_t stats;
    proxy_get_stats(&stats);

    char eth_ip_str[16] = "0.0.0.0";
    char eth_mask_str[16] = "0.0.0.0";
    char eth_gw_str[16] = "0.0.0.0";
    char eth_dns_str[16] = "0.0.0.0";
    if (eth_netif) {
        esp_netif_ip_info_t eth_ip_info;
        if (esp_netif_get_ip_info(eth_netif, &eth_ip_info) == ESP_OK) {
            snprintf(eth_ip_str, sizeof(eth_ip_str), IPSTR, IP2STR(&eth_ip_info.ip));
            snprintf(eth_mask_str, sizeof(eth_mask_str), IPSTR, IP2STR(&eth_ip_info.netmask));
            snprintf(eth_gw_str, sizeof(eth_gw_str), IPSTR, IP2STR(&eth_ip_info.gw));
        }
        esp_netif_dns_info_t dns_info;
        if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            snprintf(eth_dns_str, sizeof(eth_dns_str), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        }
    }

    const char *eth_mode = eth_cfg.using_fallback ? "fallback" :
                           (eth_cfg.use_static ? "static" : "dhcp");

    sample_chip_temp();
    char temp_json[16];
    char temp_max_json[16];
    if (chip_temp_ok) {
        snprintf(temp_json, sizeof(temp_json), "%.1f", (double)chip_temp_c);
    } else {
        snprintf(temp_json, sizeof(temp_json), "null");
    }
    if (chip_temp_max_ok) {
        snprintf(temp_max_json, sizeof(temp_max_json), "%.1f", (double)chip_temp_max_c);
    } else {
        snprintf(temp_max_json, sizeof(temp_max_json), "null");
    }

    bool wd_armed;
    char wd_last[16];
    bool wd_link_up = false;
    if (wd_link_mode) {
        wd_armed = wifi_was_up;
        wd_link_up = wifi_was_up && powerwall_reachable;
        if (wd_armed && !wd_link_up && link_down_since_us != 0) {
            snprintf(wd_last, sizeof(wd_last), "%lld",
                     (long long)((esp_timer_get_time() - link_down_since_us) / 1000000));
        } else {
            snprintf(wd_last, sizeof(wd_last), "0");
        }
    } else {
        wd_armed = last_successful_connection_time != 0;
        if (wd_armed) {
            snprintf(wd_last, sizeof(wd_last), "%lld",
                     (long long)((esp_timer_get_time() - last_successful_connection_time) / 1000000));
        } else {
            snprintf(wd_last, sizeof(wd_last), "null");
        }
    }

    char response[1024];
    snprintf(response, sizeof(response),
        "{\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d},"
        "\"powerwall\":{\"reachable\":%s,\"ip\":\"%s\"},"
        "\"eth\":{\"ip\":\"%s\",\"netmask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
        "\"mode\":\"%s\",\"fallback\":%s},"
        "\"cpu\":%u,\"temp_c\":%s,\"temp_max_c\":%s,\"heap\":%lu,"
        "\"watchdog\":{\"mode\":\"%s\",\"armed\":%s,\"link_up\":%s,\"last_s\":%s,\"timeout_s\":%d},"
        "\"uptime\":%lld,"
        "\"total_bytes_in\":%llu,\"total_bytes_out\":%llu,"
        "\"total_requests\":%lu,\"successful_requests\":%lu,\"failed_requests\":%lu}",
        wifi_connected ? "true" : "false",
        wifi_ssid, rssi,
        powerwall_reachable ? "true" : "false",
        POWERWALL_IP_STR,
        eth_ip_str, eth_mask_str, eth_gw_str, eth_dns_str,
        eth_mode,
        (eth_cfg.using_fallback || eth_cfg.fell_back_last_boot) ? "true" : "false",
        cpu_usage_percent,
        temp_json,
        temp_max_json,
        (unsigned long)esp_get_free_heap_size(),
        wd_link_mode ? "link" : "proxy",
        wd_armed ? "true" : "false",
        wd_link_up ? "true" : "false",
        wd_last,
        WATCHDOG_TIMEOUT_SEC,
        (long long)uptime_sec,
        (unsigned long long)stats.total_bytes_in, (unsigned long long)stats.total_bytes_out,
        (unsigned long)stats.total_requests, (unsigned long)stats.successful_requests, (unsigned long)stats.failed_requests);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint for RSSI value only */
static esp_err_t api_rssi_handler(httpd_req_t *req)
{
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    bool wifi_connected = (bits & WIFI_CONNECTED_BIT) != 0;

    int rssi = 0;
    if (wifi_connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }
    }

    char response[16];
    snprintf(response, sizeof(response), "%d", rssi);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/** API endpoint for recent requests */
static esp_err_t api_requests_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"avg_ttfb\":%lu,\"requests\":[", (unsigned long)proxy_get_avg_ttfb());
    httpd_resp_sendstr_chunk(req, buf);

    // Get request log from proxy module
    request_log_entry_t entries[REQUEST_LOG_SIZE];
    int count = 0;
    proxy_get_request_log(entries, &count, REQUEST_LOG_SIZE);

    int64_t now = esp_timer_get_time() / 1000000;
    for (int i = 0; i < count; i++) {
        request_log_entry_t *e = &entries[i];
        int64_t age = now - e->timestamp;
        uint8_t *ip = (uint8_t *)&e->source_ip;

        snprintf(buf, sizeof(buf),
            "%s{\"age\":%lld,\"ip\":\"%d.%d.%d.%d\",\"in\":%lu,\"out\":%lu,\"ttfb\":%u,\"ttlb\":%u,\"ok\":%d}",
            i == 0 ? "" : ",",
            (long long)age, ip[0], ip[1], ip[2], ip[3],
            (unsigned long)e->bytes_in, (unsigned long)e->bytes_out,
            e->ttfb_ms, e->ttlb_ms, e->result == 0 ? 1 : 0);
        httpd_resp_sendstr_chunk(req, buf);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/** API endpoint for system logs */
static esp_err_t api_logs_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"logs\":[");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool first = true;
        char buf[1024];

        for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
            // Read in reverse order (most recent first)
            int idx = (log_buffer_index - 1 - i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
            log_entry_t *e = &log_buffer[idx];
            if (e->timestamp == 0 && e->message[0] == '\0') continue;

            // Escape special JSON characters in message (limit to 100 chars to fit buffer)
            char escaped[LOG_MSG_MAX_LEN * 5];
            char *out = escaped;
            for (const char *in = e->message; *in && (out - escaped) < (int)sizeof(escaped) - 6; in++) {
                switch (*in) {
                    case '"':  *out++ = '\\'; *out++ = '"'; break;
                    case '\\': *out++ = '\\'; *out++ = '\\'; break;
                    case '\n': *out++ = '\\'; *out++ = 'n'; break;
                    case '\r': *out++ = '\\'; *out++ = 'r'; break;
                    case '\t': *out++ = '\\'; *out++ = 't'; break;
                    default:
                        if ((unsigned char)*in >= 32) *out++ = *in;
                        break;
                }
            }
            *out = '\0';

            snprintf(buf, sizeof(buf),
                "%s{\"ts\":%lld,\"lvl\":%u,\"msg\":\"%s\"}",
                first ? "" : ",",
                (long long)e->timestamp, e->level, escaped);
            httpd_resp_sendstr_chunk(req, buf);
            first = false;
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/** Plain-text log download for off-device debugging */
static esp_err_t logs_txt_handler(httpd_req_t *req)
{
    char disp[72];
    snprintf(disp, sizeof(disp),
             "attachment; filename=\"bridge-logs-%llds.txt\"",
             (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const esp_app_desc_t *app = esp_app_get_description();
    char head[192];
    snprintf(head, sizeof(head),
             "# ESP32 WiFi Bridge %s  uptime %llds  heap %lu KB\n",
             app ? app->version : "?",
             (long long)(esp_timer_get_time() / 1000000),
             (unsigned long)(esp_get_free_heap_size() / 1024));
    httpd_resp_sendstr_chunk(req, head);

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        char line[LOG_MSG_MAX_LEN + 32];
        for (int i = 0; i < LOG_BUFFER_SIZE; i++) {
            int idx = (log_buffer_index + i) % LOG_BUFFER_SIZE;
            log_entry_t *e = &log_buffer[idx];
            if (e->timestamp == 0 && e->message[0] == '\0') continue;
            snprintf(line, sizeof(line), "[%llds] %s\n",
                     (long long)e->timestamp, e->message);
            httpd_resp_sendstr_chunk(req, line);
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
static esp_err_t api_wifi_history_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // Get metrics summary
    wifi_metrics_summary_t summary;
    wifi_metrics_get_summary(&summary);

    // Start JSON response
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"time_synced\":%s,\"boot_time_utc\":%lld,\"bucket_minutes\":%d,"
        "\"total_buckets\":%d,\"current_bucket\":%d,"
        "\"connected_sec\":%lu,\"disconnected_sec\":%lu,\"buckets\":[",
        summary.time_synced ? "true" : "false",
        (long long)summary.boot_time_utc,
        WIFI_METRICS_BUCKET_MINUTES,
        WIFI_METRICS_BUCKET_COUNT,
        summary.current_bucket_index,
        (unsigned long)summary.total_connected_sec,
        (unsigned long)summary.total_disconnected_sec);
    httpd_resp_sendstr_chunk(req, buf);

    // Get history buckets
    wifi_metrics_bucket_t *buckets = malloc(sizeof(wifi_metrics_bucket_t) * WIFI_METRICS_BUCKET_COUNT);
    if (buckets) {
        int current_index;
        wifi_metrics_get_history(buckets, &current_index);

        bool first = true;
        // Output buckets in chronological order (oldest first)
        // Start from current_index + 1 (oldest) and wrap around
        for (int i = 0; i < WIFI_METRICS_BUCKET_COUNT; i++) {
            int idx = (current_index + 1 + i) % WIFI_METRICS_BUCKET_COUNT;
            wifi_metrics_bucket_t *b = &buckets[idx];

            if (b->valid) {
                snprintf(buf, sizeof(buf), "%s[%d,%u,%u]",
                    first ? "" : ",",
                    b->avg_rssi,
                    b->connection_pct,
                    b->sample_count);
                httpd_resp_sendstr_chunk(req, buf);
                first = false;
            }
        }
        free(buckets);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/** Reboot handler */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Manual reboot requested");

    const char *response = "<!DOCTYPE html><html><head><title>Reboot</title>"
        "<meta http-equiv='refresh' content='10;url=/'>"
        "<style>body{font-family:system-ui;background:#0f172a;color:#e2e8f0;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".box{background:#1e293b;padding:2rem;border-radius:0.75rem;text-align:center;border:1px solid #334155}"
        ".spinner{width:3rem;height:3rem;border:3px solid #334155;border-top:3px solid #3b82f6;border-radius:50%;animation:spin 1s linear infinite;margin:1rem auto}"
        "@keyframes spin{to{transform:rotate(360deg)}}</style></head>"
        "<body><div class=\"box\"><div class=\"spinner\"></div>"
        "<h2>Rebooting...</h2>"
        "<p>Device is restarting. Page will refresh automatically.</p>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));

    vTaskDelay(pdMS_TO_TICKS(500));
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

// Remote OTA handlers are in remote_ota.c

static void send_simple_page_begin(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<link rel=\"icon\" type=\"image/png\" href=\"/favicon.ico\">"
        "<link rel=\"apple-touch-icon\" href=\"/apple-touch-icon.png\">"
        "<title>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</title><style>");
    httpd_resp_sendstr_chunk(req, DARK_CSS);
    httpd_resp_sendstr_chunk(req,
        "svg.i{width:1.125rem;height:1.125rem;vertical-align:middle;margin-right:0.25rem;fill:currentColor}"
        "</style></head><body><div class=\"container\"><div class=\"card\">");
}

static void send_simple_page_end(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "</div></div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t send_admin_setup_page(httpd_req_t *req, const char *error_msg)
{
    send_simple_page_begin(req, "Set admin password");
    httpd_resp_sendstr_chunk(req, "<h1>" ICON_LOCK " Set admin password</h1>");
    httpd_resp_sendstr_chunk(req,
        "<p class=\"text-sm text-muted\" style=\"margin-bottom:1rem\">"
        "Protects the dashboard, WiFi/Ethernet settings, and firmware updates. "
        "The Powerwall data stream on port 443 is not affected. "
        "Username is <code>" ADMIN_USERNAME "</code>.</p>");
    if (error_msg && error_msg[0]) {
        httpd_resp_sendstr_chunk(req, "<div class=\"alert alert-warn\" style=\"margin-bottom:0.75rem\">" ICON_WARN " ");
        httpd_resp_sendstr_chunk(req, error_msg);
        httpd_resp_sendstr_chunk(req, "</div>");
    }
    httpd_resp_sendstr_chunk(req,
        "<form method=\"POST\" action=\"/admin/setup\">"
        "<div class=\"form-group\"><label class=\"label\">Password (min 8 characters)</label>"
        "<input type=\"password\" name=\"password\" minlength=\"8\" maxlength=\"64\" required autocomplete=\"new-password\" class=\"mt-1\"></div>"
        "<div class=\"form-group\"><label class=\"label\">Confirm password</label>"
        "<input type=\"password\" name=\"password2\" minlength=\"8\" maxlength=\"64\" required autocomplete=\"new-password\" class=\"mt-1\"></div>"
        "<button type=\"submit\" class=\"btn btn-primary\">" ICON_SAVE " Save password</button>"
        "</form>");
    send_simple_page_end(req);
    return ESP_OK;
}

static esp_err_t send_login_page(httpd_req_t *req, const char *error_msg)
{
    send_simple_page_begin(req, "Sign in");
    httpd_resp_sendstr_chunk(req, "<h1>" ICON_LOCK " Sign in</h1>");
    if (error_msg && error_msg[0]) {
        httpd_resp_sendstr_chunk(req, "<div class=\"alert alert-warn\" style=\"margin-bottom:0.75rem\">" ICON_WARN " ");
        httpd_resp_sendstr_chunk(req, error_msg);
        httpd_resp_sendstr_chunk(req, "</div>");
    }
    httpd_resp_sendstr_chunk(req,
        "<form method=\"POST\" action=\"/login\">"
        "<div class=\"form-group\"><label class=\"label\">Username — " ADMIN_USERNAME "</label>"
        "<input type=\"text\" name=\"username\" value=\"" ADMIN_USERNAME "\" autocomplete=\"username\" required class=\"mt-1\"></div>"
        "<div class=\"form-group\"><label class=\"label\">Password</label>"
        "<input type=\"password\" name=\"password\" autocomplete=\"current-password\" required class=\"mt-1\"></div>"
        "<button type=\"submit\" class=\"btn btn-primary\">Sign in</button>"
        "</form>");
    send_simple_page_end(req);
    return ESP_OK;
}

static esp_err_t login_get_handler(httpd_req_t *req)
{
    if (!admin_configured) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    if (session_from_req(req, NULL, 0)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    return send_login_page(req, NULL);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    if (!admin_configured) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    char content[256];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        return send_login_page(req, "No data received");
    }
    content[received] = '\0';

    char username[32] = {0};
    char password[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    form_get(content, "username", username, sizeof(username));
    form_get(content, "password", password, sizeof(password));
    memset(content, 0, sizeof(content));

    uint8_t got[ADMIN_HASH_LEN];
    hash_admin_password(password, admin_salt, got);
    bool user_ok = strcmp(username, ADMIN_USERNAME) == 0;
    bool pass_ok = hash_equal(got, admin_hash, ADMIN_HASH_LEN);
    memset(got, 0, sizeof(got));
    memset(password, 0, sizeof(password));

    if (!user_ok || !pass_ok) {
        vTaskDelay(pdMS_TO_TICKS(ADMIN_AUTH_FAIL_DELAY_MS));
        ESP_LOGW(TAG, "Admin login failed");
        return send_login_page(req, "Invalid username or password");
    }

    char hex[SESSION_ID_LEN * 2 + 1];
    char cookie[192];
    session_create(hex);
    session_cookie_set(cookie, sizeof(cookie), hex, req);
    ESP_LOGI(TAG, "Admin login ok%s", request_is_https(req) ? " (secure cookie)" : "");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/\">"
        "<title>Signed in</title></head><body>"
        "<script>location.replace('/');</script>"
        "<p><a href=\"/\">Continue</a></p></body></html>");
    return ESP_OK;
}

static esp_err_t logout_handler(httpd_req_t *req)
{
    char sid[SESSION_ID_LEN * 2 + 1] = {0};
    size_t sid_len = sizeof(sid);
    if (httpd_req_get_cookie_val(req, SESSION_COOKIE_NAME, sid, &sid_len) == ESP_OK) {
        session_drop(sid);
    }
    char cookie[192];
    session_cookie_clear(cookie, sizeof(cookie), req);
    ESP_LOGI(TAG, "Admin logout");

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static bool require_admin(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (uri_path_is(uri, "/favicon.ico") ||
        uri_path_is(uri, "/apple-touch-icon.png") ||
        uri_path_is(uri, "/health") ||
        uri_path_is(uri, "/login") ||
        uri_path_is(uri, "/logout")) {
        return true;
    }

    if (!admin_configured) {
        bool allowed = (req->method == HTTP_GET && uri_path_is(uri, "/")) ||
                       (req->method == HTTP_POST && uri_path_is(uri, "/admin/setup"));
        if (allowed) {
            return true;
        }
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return false;
    }

    if (session_from_req(req, NULL, 0)) {
        return true;
    }

    if (uri_is_api(uri)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
        return false;
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return false;
}

static esp_err_t with_admin(httpd_req_t *req)
{
    if (!require_admin(req)) {
        return ESP_FAIL;
    }
    esp_err_t (*inner)(httpd_req_t *) = (esp_err_t (*)(httpd_req_t *))req->user_ctx;
    return inner(req);
}

static esp_err_t admin_setup_handler(httpd_req_t *req)
{
    if (admin_configured) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Password already set");
        return ESP_FAIL;
    }

    char content[192];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        return send_admin_setup_page(req, "No data received");
    }
    content[received] = '\0';

    char password[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    char password2[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    form_get(content, "password", password, sizeof(password));
    form_get(content, "password2", password2, sizeof(password2));
    memset(content, 0, sizeof(content));

    const char *err = NULL;
    if (!admin_password_valid(password)) {
        err = "Password must be 8–64 characters";
    } else if (strcmp(password, password2) != 0) {
        err = "Passwords do not match";
    }

    if (err) {
        memset(password, 0, sizeof(password));
        memset(password2, 0, sizeof(password2));
        return send_admin_setup_page(req, err);
    }

    esp_err_t save_err = save_admin_password(password);
    memset(password, 0, sizeof(password));
    memset(password2, 0, sizeof(password2));
    if (save_err != ESP_OK) {
        return send_admin_setup_page(req, "Failed to save password");
    }

    char hex[SESSION_ID_LEN * 2 + 1];
    char cookie[192];
    session_create(hex);
    session_cookie_set(cookie, sizeof(cookie), hex, req);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    send_simple_page_begin(req, "Password saved");
    httpd_resp_sendstr_chunk(req, "<h1>" ICON_LOCK " Password saved</h1>");
    httpd_resp_sendstr_chunk(req,
        "<p>You are signed in as <code>" ADMIN_USERNAME "</code>.</p>"
        "<p class=\"text-xs text-muted\" style=\"margin:0.75rem 0\">"
        "If you forget the password, hold " RECOVERY_BUTTON_NAME " for 15 seconds to clear it (also forces DHCP).</p>"
        "<p><a class=\"btn btn-primary\" href=\"/\" style=\"display:inline-block;text-decoration:none;margin-top:0.75rem\">Continue to dashboard</a></p>");
    send_simple_page_end(req);
    return ESP_OK;
}

static esp_err_t admin_password_handler(httpd_req_t *req)
{
    char content[256];
    int received = httpd_req_recv(req, content, sizeof(content) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    content[received] = '\0';

    char current[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    char password[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    char password2[ADMIN_MAX_PASSWORD_LEN + 1] = {0};
    form_get(content, "current", current, sizeof(current));
    form_get(content, "password", password, sizeof(password));
    form_get(content, "password2", password2, sizeof(password2));
    memset(content, 0, sizeof(content));

    uint8_t got[ADMIN_HASH_LEN];
    hash_admin_password(current, admin_salt, got);
    bool current_ok = hash_equal(got, admin_hash, ADMIN_HASH_LEN);
    memset(got, 0, sizeof(got));
    memset(current, 0, sizeof(current));

    const char *err = NULL;
    if (!current_ok) {
        err = "Current password is incorrect";
    } else if (!admin_password_valid(password)) {
        err = "New password must be 8–64 characters";
    } else if (strcmp(password, password2) != 0) {
        err = "New passwords do not match";
    }

    if (err) {
        memset(password, 0, sizeof(password));
        memset(password2, 0, sizeof(password2));
        send_simple_page_begin(req, "Password not changed");
        httpd_resp_sendstr_chunk(req, "<h1>" ICON_WARN " Password not changed</h1><p>");
        httpd_resp_sendstr_chunk(req, err);
        httpd_resp_sendstr_chunk(req, "</p><p><a href=\"/\">Back</a></p>");
        send_simple_page_end(req);
        return ESP_OK;
    }

    esp_err_t save_err = save_admin_password(password);
    memset(password, 0, sizeof(password));
    memset(password2, 0, sizeof(password2));
    if (save_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    session_clear_all();
    char hex[SESSION_ID_LEN * 2 + 1];
    char cookie[192];
    session_create(hex);
    session_cookie_set(cookie, sizeof(cookie), hex, req);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    send_simple_page_begin(req, "Password changed");
    httpd_resp_sendstr_chunk(req,
        "<h1>" ICON_LOCK " Password changed</h1>"
        "<p>Other sessions were signed out. This browser stays signed in.</p>"
        "<p><a class=\"btn btn-primary\" href=\"/\" style=\"display:inline-block;text-decoration:none;margin-top:0.75rem\">Back to dashboard</a></p>");
    send_simple_page_end(req);
    return ESP_OK;
}

/** Start the HTTP server (port 80) - dashboard / OTA bound to Ethernet IP */

/*
 * esp_http_server always bind()s INADDR_ANY. Rewrite :80 binds to the Ethernet
 * address so the dashboard is not reachable on the Tesla Wi‑Fi IP.
 */
extern int __real_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);

int __wrap_lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    if (name && namelen >= sizeof(struct sockaddr_in) && name->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)name;
        if (in->sin_port == htons(WEB_HTTP_PORT) && in->sin_addr.s_addr == htonl(INADDR_ANY)) {
            esp_netif_ip_info_t ip;
            if (eth_netif &&
                esp_netif_get_ip_info(eth_netif, &ip) == ESP_OK &&
                ip.ip.addr != 0) {
                struct sockaddr_in rewritten = *in;
                rewritten.sin_addr.s_addr = ip.ip.addr;
                ESP_LOGI(TAG, "HTTP listen bound to " IPSTR ":%d (Ethernet)",
                         IP2STR(&ip.ip), WEB_HTTP_PORT);
                http_listen_addr = ip.ip.addr;
                return __real_lwip_bind(s, (struct sockaddr *)&rewritten, sizeof(rewritten));
            }
        }
    }
    return __real_lwip_bind(s, name, namelen);
}

static bool http_local_ip_is_wifi(int sockfd)
{
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    memset(&name, 0, sizeof(name));
    if (getsockname(sockfd, (struct sockaddr *)&name, &namelen) != 0) {
        return false;
    }
    if (name.sin_family != AF_INET) {
        return false;
    }
    esp_netif_ip_info_t wifi_ip;
    if (!wifi_netif || esp_netif_get_ip_info(wifi_netif, &wifi_ip) != ESP_OK || wifi_ip.ip.addr == 0) {
        return false;
    }
    return name.sin_addr.s_addr == wifi_ip.ip.addr;
}

static esp_err_t http_open_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (http_local_ip_is_wifi(sockfd)) {
        ESP_LOGW(TAG, "Rejected HTTP session on WiFi STA interface");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_HTTP_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 28;
    config.lru_purge_enable = true;
    config.open_fn = http_open_fn;

    esp_err_t err = httpd_start(&web_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
    };
    httpd_register_uri_handler(web_server, &favicon);
    httpd_uri_t apple_touch = {
        .uri = "/apple-touch-icon.png",
        .method = HTTP_GET,
        .handler = apple_touch_handler,
    };
    httpd_register_uri_handler(web_server, &apple_touch);
    httpd_uri_t health = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    httpd_register_uri_handler(web_server, &health);
    httpd_uri_t login_get = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_get_handler,
    };
    httpd_register_uri_handler(web_server, &login_get);
    httpd_uri_t login_post = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
    };
    httpd_register_uri_handler(web_server, &login_post);
    httpd_uri_t logout = {
        .uri = "/logout",
        .method = HTTP_POST,
        .handler = logout_handler,
    };
    httpd_register_uri_handler(web_server, &logout);

#define AUTH_URI(path, meth, fn) do { \
        httpd_uri_t _u = { .uri = (path), .method = (meth), .handler = with_admin, .user_ctx = (void *)(fn) }; \
        httpd_register_uri_handler(web_server, &_u); \
    } while (0)

    AUTH_URI("/", HTTP_GET, ota_status_handler);
    AUTH_URI("/ota/upload", HTTP_POST, ota_upload_handler);
    AUTH_URI("/ota/rollback", HTTP_POST, ota_rollback_handler);
    AUTH_URI("/reboot", HTTP_POST, reboot_handler);
    AUTH_URI("/wifi/scan", HTTP_GET, wifi_scan_handler);
    AUTH_URI("/wifi/save", HTTP_POST, wifi_save_handler);
    AUTH_URI("/eth/save", HTTP_POST, eth_save_handler);
    AUTH_URI("/watchdog/save", HTTP_POST, watchdog_save_handler);
    AUTH_URI("/api/status", HTTP_GET, api_status_handler);
    AUTH_URI("/api/rssi", HTTP_GET, api_rssi_handler);
    AUTH_URI("/api/requests", HTTP_GET, api_requests_handler);
    AUTH_URI("/api/logs", HTTP_GET, api_logs_handler);
    AUTH_URI("/logs.txt", HTTP_GET, logs_txt_handler);
    AUTH_URI("/api/wifi-history", HTTP_GET, api_wifi_history_handler);
    AUTH_URI("/api/update", HTTP_GET, api_update_status_handler);
    AUTH_URI("/api/check-update", HTTP_POST, api_check_update_handler);
    AUTH_URI("/api/install-update", HTTP_POST, api_install_update_handler);
    AUTH_URI("/api/revert", HTTP_POST, api_revert_handler);
    AUTH_URI("/admin/setup", HTTP_POST, admin_setup_handler);
    AUTH_URI("/admin/password", HTTP_POST, admin_password_handler);

#undef AUTH_URI

    ESP_LOGI(TAG, "HTTP server started on port %d (Ethernet bind, admin session %s)",
             WEB_HTTP_PORT, admin_configured ? "enabled" : "setup required");
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

    {
        uint32_t main_dns = 0;
        uint32_t backup_dns = 0;
        esp_netif_dns_info_t dns_info;
        memset(&dns_info, 0, sizeof(dns_info));
        if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            main_dns = dns_info.ip.u_addr.ip4.addr;
        }
        memset(&dns_info, 0, sizeof(dns_info));
        if (esp_netif_get_dns_info(eth_netif, ESP_NETIF_DNS_BACKUP, &dns_info) == ESP_OK &&
            dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
            backup_dns = dns_info.ip.u_addr.ip4.addr;
        }
        if (eth_cfg.use_static && eth_cfg.dns[0]) {
            esp_ip4_addr_t parsed;
            if (esp_netif_str_to_ip4(eth_cfg.dns, &parsed) == ESP_OK) {
                main_dns = parsed.addr;
            }
        }
        if (!remote_ota_remember_eth_dns(main_dns, backup_dns)) {
            remote_ota_remember_eth_dns(ip_info->gw.addr, 0);
        }
        remote_ota_apply_eth_dns();
    }

    if (web_server && http_listen_addr != 0 &&
        ip_info->ip.addr != 0 && ip_info->ip.addr != http_listen_addr) {
        ESP_LOGW(TAG, "Ethernet IP changed; rebooting to rebind HTTP");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
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
        set_network_led(false);
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
    set_network_led(true);
    wifi_was_up = true;
    /* Tesla DHCP just overwrote lwIP DNS with 192.168.91.1. Put LAN DNS back. */
    remote_ota_apply_eth_dns();
}

/** Initialize Ethernet (W5500 or the internal EMAC, by board) */
static esp_err_t init_ethernet(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet (%s)...", BOARD_GL_S10 ? "IP101" : "W5500");

    // Create event group
    s_event_group = xEventGroupCreate();

    // Initialize TCP/IP network interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create default event loop for Ethernet
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&cfg);

    load_eth_config();
    load_wd_config();

    // Stop DHCP before the interface comes up if we will use a static address
    if (eth_cfg.use_static && !eth_cfg.using_fallback) {
        esp_err_t stop_err = esp_netif_dhcpc_stop(eth_netif);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "Early dhcpc_stop: %s", esp_err_to_name(stop_err));
        }
    }

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    uint8_t mac_addr[6];
#if BOARD_GL_S10
    // Internal EMAC with the IP101 PHY. ETH_ESP32_EMAC_DEFAULT_CONFIG already
    // takes the RMII clock in on GPIO0; set the SMI pins explicitly anyway.
    gpio_config_t phy_power = {
        .pin_bit_mask = 1ULL << ETH_PHY_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&phy_power);
    gpio_set_level(ETH_PHY_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    // The EMAC uses the chip's own Ethernet MAC (base + 3), the address the
    // LAN already knows this device by.
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_ETH));
    ESP_LOGI(TAG, "Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
#else
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

    // W5500 RX is interrupt-driven. The ISR service must exist before
    // esp_eth_driver_install() adds the INT GPIO handler.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
    }

    // Configure MAC and PHY
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));

    // Clone MAC address from the built-in WiFi interface
    // W5500 doesn't have a burned-in MAC, so we derive one from the chip's base MAC
    ESP_ERROR_CHECK(esp_read_mac(mac_addr, ESP_MAC_WIFI_STA));
    // Modify locally-administered bit to indicate this is a derived address
    // This ensures the Ethernet MAC is unique but related to the WiFi MAC
    mac_addr[0] = (mac_addr[0] | 0x02) & 0xFE;  // Set local bit, clear multicast bit
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
    ESP_LOGI(TAG, "Ethernet MAC (derived from WiFi): %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
#endif

    // Set hostname for DHCP (powerwall-XXXX using last 2 bytes of MAC)
    char hostname[32];
    snprintf(hostname, sizeof(hostname), "%s-%02X%02X", MDNS_HOSTNAME, mac_addr[4], mac_addr[5]);
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif, hostname));
    ESP_LOGI(TAG, "DHCP hostname set to: %s", hostname);

    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // Apply static IP after attach so GOT_IP is posted when the driver starts
    if (eth_cfg.use_static && !eth_cfg.using_fallback) {
        if (apply_eth_static_ip() != ESP_OK) {
            ESP_LOGE(TAG, "Static IP apply failed; starting DHCP");
            esp_netif_dhcpc_start(eth_netif);
        }
    }

    // Start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "Ethernet initialized - waiting for connection...");
    return ESP_OK;
}

/** Initialize WiFi Station mode - only if credentials are configured */
static esp_err_t init_wifi(void)
{
    // Load saved WiFi credentials from NVS
    bool has_credentials = load_wifi_credentials();

    // Create default WiFi station (needed for scanning even without credentials)
    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (has_credentials) {
        // Configure and start WiFi with saved credentials
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, wifi_password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi initialized - connecting to %s", wifi_ssid);
    } else {
        // Start WiFi without connecting (allows scanning)
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGW(TAG, "WiFi initialized but no credentials configured - use web UI to configure");
    }

    return ESP_OK;
}

/** Initialize mDNS with simple hostname (powerwall.local) */
static void init_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());

    // Use simple hostname for mDNS (powerwall.local) for easy discovery
    // DHCP hostname keeps the MAC suffix for unique identification
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_LOGI(TAG, "mDNS hostname set to: %s.local", MDNS_HOSTNAME);

    // Get firmware version for TXT records
    const esp_app_desc_t *app_desc = esp_app_get_description();

    // Create TXT records with device info (using runtime wifi_ssid)
    mdns_txt_item_t txt_records[] = {
        {"wifi_ssid", wifi_ssid},
        {"target", POWERWALL_IP_STR},
        {"ota_port", "80"},
        {"version", app_desc->version},
    };

    // Add _powerwall._tcp service for proxy discovery (port 443)
    mdns_service_add("powerwall", MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT, txt_records,
                     sizeof(txt_records) / sizeof(txt_records[0]));
    ESP_LOGI(TAG, "mDNS service added: powerwall.%s.%s on port %d", MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT);

    // Add _http._tcp service for status/OTA web UI
    mdns_txt_item_t http_txt[] = {
        {"path", "/"},
        {"wifi_ssid", wifi_ssid},
        {"version", app_desc->version},
    };
    mdns_service_add("Powerwall Bridge", "_http", "_tcp", WEB_HTTP_PORT, http_txt,
                     sizeof(http_txt) / sizeof(http_txt[0]));
    ESP_LOGI(TAG, "mDNS service added: _http._tcp on port %d", WEB_HTTP_PORT);
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

/** System monitoring task - periodically logs system metrics and calculates CPU usage */
static void system_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "System monitoring started (interval: %d seconds)", SYSTEM_MONITOR_INTERVAL_SEC);

    // For CPU usage calculation - track idle task runtime
    #if configGENERATE_RUN_TIME_STATS
    TaskHandle_t idle_task_0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idle_task_1 = xTaskGetIdleTaskHandleForCore(1);
    uint32_t prev_idle_0 = 0, prev_idle_1 = 0;
    int64_t prev_time_us = 0;
    TaskStatus_t task_status;
    #endif

    while (1) {
        // Wait for the configured interval
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_MONITOR_INTERVAL_SEC * 1000));

        // Calculate CPU usage from idle task runtime
        #if configGENERATE_RUN_TIME_STATS
        uint32_t idle_runtime_0 = 0, idle_runtime_1 = 0;

        // Get idle task runtimes (in microseconds when using ESP_TIMER)
        if (idle_task_0) {
            vTaskGetInfo(idle_task_0, &task_status, pdFALSE, eRunning);
            idle_runtime_0 = task_status.ulRunTimeCounter;
        }
        if (idle_task_1) {
            vTaskGetInfo(idle_task_1, &task_status, pdFALSE, eRunning);
            idle_runtime_1 = task_status.ulRunTimeCounter;
        }

        // Get current time in microseconds (same unit as runtime counters)
        int64_t current_time_us = esp_timer_get_time();

        if (prev_time_us > 0) {
            // Delta time in microseconds for this interval
            int64_t delta_time_us = current_time_us - prev_time_us;
            // Total available CPU time = delta * 2 cores (in microseconds)
            int64_t total_cpu_time_us = delta_time_us * 2;
            // Delta idle time (both cores combined)
            uint32_t delta_idle = (idle_runtime_0 - prev_idle_0) + (idle_runtime_1 - prev_idle_1);

            if (total_cpu_time_us > 0) {
                // CPU usage = 100 - idle_percentage
                int64_t idle_pct = (delta_idle * 100LL) / total_cpu_time_us;
                cpu_usage_percent = (idle_pct > 100) ? 0 : (uint8_t)(100 - idle_pct);
            }
        }

        prev_idle_0 = idle_runtime_0;
        prev_idle_1 = idle_runtime_1;
        prev_time_us = current_time_us;
        #endif

        sample_chip_temp();
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t min_free_heap = esp_get_minimum_free_heap_size();

        // Log system status
        #if configGENERATE_RUN_TIME_STATS
        if (chip_temp_ok) {
            ESP_LOGI(TAG, "System Status - CPU: %u%%, Temp: %.1f C, Max: %.1f C, Heap: %lu KB free, Min: %lu KB",
                     cpu_usage_percent, (double)chip_temp_c,
                     chip_temp_max_ok ? (double)chip_temp_max_c : (double)chip_temp_c,
                     (unsigned long)(free_heap / 1024),
                     (unsigned long)(min_free_heap / 1024));
        } else {
            ESP_LOGI(TAG, "System Status - CPU: %u%%, Heap: %lu KB free, Min: %lu KB",
                     cpu_usage_percent, (unsigned long)(free_heap / 1024),
                     (unsigned long)(min_free_heap / 1024));
        }
        #else
        ESP_LOGI(TAG, "System Status - Heap: %lu KB free, Min: %lu KB",
                 (unsigned long)(free_heap / 1024),
                 (unsigned long)(min_free_heap / 1024));
        #endif

        // Warn on low heap
        if (free_heap < 20000) {
            ESP_LOGW(TAG, "Heap Status: Critical - Low memory!");
        }
    }
}

/** Connection watchdog. Proxy clients: idle until the first successful proxy.
 *  Powerwall link: reboot only if 192.168.91.1:443 stays unreachable. */
static void connection_watchdog_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Connection watchdog: %s (%d s)",
             wd_link_mode ? "Powerwall link" : "Proxy clients",
             WATCHDOG_TIMEOUT_SEC);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_INTERVAL_SEC * 1000));

        if (wd_link_mode) {
            if (!wifi_was_up) {
                static int idle_logs = 0;
                if ((++idle_logs % 5) == 0) {
                    ESP_LOGI(TAG, "Watchdog still idle — Wi-Fi has not associated yet");
                }
                continue;
            }
            bool probed = powerwall_check_due();
            if (probed) {
                check_powerwall_connectivity();
            }
            if (powerwall_reachable) {
                static int up_logs = 0;
                if ((++up_logs % 5) == 0) {
                    ESP_LOGI(TAG, "Watchdog: Powerwall link up");
                }
                continue;
            }
            int64_t elapsed_sec = link_down_since_us
                ? (esp_timer_get_time() - link_down_since_us) / 1000000 : 0;
            if (elapsed_sec > WATCHDOG_TIMEOUT_SEC) {
                check_powerwall_connectivity();
                if (powerwall_reachable) {
                    ESP_LOGI(TAG, "Watchdog: Powerwall link recovered before reboot");
                    continue;
                }
                elapsed_sec = link_down_since_us
                    ? (esp_timer_get_time() - link_down_since_us) / 1000000 : 0;
                ESP_LOGE(TAG, "Watchdog triggered: Powerwall unreachable for %lld seconds",
                         (long long)elapsed_sec);
                ESP_LOGW(TAG, "Rebooting device...");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
            if (probed) {
                ESP_LOGW(TAG, "Watchdog: Powerwall unreachable for %lld seconds", (long long)elapsed_sec);
            }
            continue;
        }

        if (last_successful_connection_time == 0) {
            static int idle_logs = 0;
            if ((++idle_logs % 5) == 0) {
                ESP_LOGI(TAG, "Watchdog still idle — no Powerwall proxy traffic yet");
            }
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t elapsed_sec = (now - last_successful_connection_time) / 1000000;

        if (elapsed_sec > WATCHDOG_TIMEOUT_SEC) {
            proxy_stats_t stats;
            proxy_get_stats(&stats);
            ESP_LOGE(TAG, "Connection watchdog triggered! No successful connections for %lld seconds", (long long)elapsed_sec);
            ESP_LOGE(TAG, "Stats: total=%lu, success=%lu, failed=%lu",
                     (unsigned long)stats.total_requests,
                     (unsigned long)stats.successful_requests,
                     (unsigned long)stats.failed_requests);
            ESP_LOGW(TAG, "Rebooting device...");

            vTaskDelay(pdMS_TO_TICKS(1000));  // Give time for logs to flush
            esp_restart();
        }

        // Log watchdog status every 5 minutes (every 5th check)
        static int check_count = 0;
        check_count++;
        if (check_count % 5 == 0) {
            ESP_LOGI(TAG, "Watchdog: Last successful connection %lld seconds ago", (long long)elapsed_sec);
        }
    }
}

// Proxy tasks are in proxy.c

/** Task to initialize WiFi-dependent services after connection */
static void wifi_services_task(void *pvParameters)
{
    // Wait for WiFi credentials to be configured (if not already)
    if (!wifi_configured) {
        ESP_LOGW(TAG, "No WiFi credentials configured - waiting for configuration via web UI");
        while (!wifi_configured) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "Waiting for WiFi configuration via web UI at http://<eth-ip>/");
        }
        ESP_LOGI(TAG, "WiFi credentials configured - connecting to %s", wifi_ssid);
    }

    // Wait for WiFi connection (with timeout for logging)
    ESP_LOGI(TAG, "Waiting for WiFi connection to %s...", wifi_ssid);

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT,
                                                false, true, pdMS_TO_TICKS(30000));
        if (bits & WIFI_CONNECTED_BIT) {
            break;
        }
        ESP_LOGW(TAG, "WiFi not connected yet - check credentials via web UI at http://<eth-ip>/");
    }

    ESP_LOGI(TAG, "WiFi connected - starting proxy services");

    // Start WiFi quality monitoring task
    xTaskCreate(wifi_quality_monitor_task, "wifi_monitor", 3072, NULL, 3, NULL);

    // Start WiFi metrics collection (for 24h charts)
    wifi_metrics_start();

    // Start proxy server (handles buffer pool init and TCP server)
    proxy_start();

    vTaskDelete(NULL);
}

void app_main(void)
{
    // Record boot time for uptime calculation
    boot_time_us = esp_timer_get_time();

    // Initialize log capture early to capture startup logs
    init_log_capture();

    ESP_LOGI(TAG, "=== " BOARD_NAME " WiFi-Ethernet SSL Bridge ===");
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

    init_leds();
    load_admin_config();
    init_temp_sensor();
    sample_chip_temp();

    // Initialize Ethernet first (OTA server runs on Ethernet)
    ESP_ERROR_CHECK(init_ethernet());

    // Initialize WiFi (starts connection attempt in background)
    ESP_ERROR_CHECK(init_wifi());

    // Wait for Ethernet to get IP before starting OTA server
    ESP_LOGI(TAG, "Waiting for Ethernet IP...");
    EventBits_t ip_bits = xEventGroupWaitBits(s_event_group, ETH_GOT_IP_BIT, pdFALSE, pdTRUE,
                                              pdMS_TO_TICKS(15000));
    if ((ip_bits & ETH_GOT_IP_BIT) == 0) {
        esp_netif_ip_info_t assigned;
        if (eth_netif && esp_netif_get_ip_info(eth_netif, &assigned) == ESP_OK && assigned.ip.addr != 0) {
            ESP_LOGW(TAG, "GOT_IP event missed; using " IPSTR, IP2STR(&assigned.ip));
            xEventGroupSetBits(s_event_group, ETH_GOT_IP_BIT);
        } else {
            ESP_LOGI(TAG, "Still waiting for DHCP lease...");
            xEventGroupWaitBits(s_event_group, ETH_GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        }
    }

    // Initialize modules that depend on Ethernet
    remote_ota_init(eth_netif);
    proxy_init(s_event_group, ETH_GOT_IP_BIT, &last_successful_connection_time, eth_netif);
    wifi_metrics_init(s_event_group, WIFI_CONNECTED_BIT, eth_netif);

    // Start HTTP server on Ethernet (open_fn rejects sessions whose local IP is WiFi)
    start_http_server();
    ESP_LOGI(TAG, "HTTP server started - http://<eth-ip>/");

    // Initialize mDNS on Ethernet (for device discovery, doesn't need WiFi)
    init_mdns();

    // Validate OTA image early so device doesn't rollback while user configures WiFi
    validate_ota_image();

    // Static-IP DHCP fallback + BOOT-button recovery
    xTaskCreate(eth_dhcp_fallback_task, "eth_fallback", 3072, NULL, 3, NULL);
    xTaskCreate(eth_boot_button_task, "eth_boot_btn", 4096, NULL, 3, NULL);

    // Start system monitoring task
    xTaskCreate(system_monitor_task, "sys_monitor", 3072, NULL, 3, NULL);

    // Start connection watchdog task
    xTaskCreate(connection_watchdog_task, "conn_watchdog", 4096, NULL, 3, NULL);

    // Start WiFi-dependent services in background task
    // This allows OTA to remain responsive while waiting for WiFi
    xTaskCreate(wifi_services_task, "wifi_services", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "System initialized - configure WiFi via OTA UI if needed");
}
