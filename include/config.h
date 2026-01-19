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
#define PROXY_TIMEOUT_MS 60000  // 60 seconds (increased from 30)
#define PROXY_BUFFER_SIZE 2048  // Buffer size for forwarding encrypted data
#define SSL_PASSTHROUGH_TASK_STACK_SIZE 6144  // Stack size per client task (reduced from 8192)

// ===== TTL Configuration =====
// TTL (Time-To-Live) value to set on outgoing packets to hide external origin
// Common TTL values: 64 (Linux/Unix default), 128 (Windows default), 255 (Cisco default)
// Setting to 64 as it's the most common default for web servers
#define TTL_VALUE 64

// ===== mDNS Configuration =====
#define MDNS_HOSTNAME "powerwall"
#define MDNS_SERVICE "_powerwall"
#define MDNS_PROTOCOL "_tcp"

// ===== WiFi Quality Monitoring =====
// Interval for logging WiFi connection quality (in seconds)
#define WIFI_QUALITY_LOG_INTERVAL_SEC 30  // Log every 30 seconds

// ===== Debug Configuration =====
// Enable DEBUG_MODE to show encrypted packet forwarding details
#define DEBUG_MODE 0  // Set to 1 to enable debug logging

#endif // CONFIG_H
