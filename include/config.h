#ifndef CONFIG_H
#define CONFIG_H

// ===== WiFi Configuration =====
// Configure these to match your Tesla Powerwall WiFi network
#define WIFI_SSID "TeslaPowerwall"
#define WIFI_PASSWORD ""

// Powerwall IP address on the WiFi network
#define POWERWALL_IP_ADDR1 192
#define POWERWALL_IP_ADDR2 168
#define POWERWALL_IP_ADDR3 91
#define POWERWALL_IP_ADDR4 1
#define POWERWALL_IP_STR "192.168.91.1"

// ===== Ethernet Configuration =====
// MAC address for W5500 Ethernet (change if you have multiple devices)
#define ETH_MAC_ADDR { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }

// ===== W5500 SPI Pin Configuration =====
// These are the correct pins for ESP32-S3-POE-ETH (Waveshare)
#define W5500_INT_GPIO  10
#define W5500_MISO_GPIO 12
#define W5500_MOSI_GPIO 11
#define W5500_SCK_GPIO  13
#define W5500_CS_GPIO   14

// ===== Proxy Server Configuration =====
#define PROXY_PORT 443
#define PROXY_TIMEOUT_MS 30000  // 30 seconds
#define PROXY_BUFFER_SIZE 2048  // TLS record buffer size
#define HTTPS_CLIENT_TASK_STACK_SIZE 8192  // Stack size per client task

// ===== mDNS Configuration =====
#define MDNS_HOSTNAME "powerwall"
#define MDNS_SERVICE "_powerwall"
#define MDNS_PROTOCOL "_tcp"

#endif // CONFIG_H
