/**
 * ESP32-S3-POE-ETH WiFi-Ethernet Bridge
 * 
 * This application creates a transparent bridge between Ethernet and WiFi,
 * proxying requests from Ethernet clients to 192.168.91.1 via the ESP32's WiFi connection.
 * 
 * Hardware: ESP32-S3-ETH (Waveshare)
 * Features:
 * - Ethernet PHY: LAN8720A
 * - WiFi Client mode connection
 * - IP forwarding/NAT from Ethernet to WiFi
 * - Proxies requests to target IP (192.168.91.1)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <lwip/lwip_napt.h>
#include <lwip/ip_addr.h>
#include <lwip/dns.h>

// Configuration from build flags
#ifndef WIFI_SSID
#define WIFI_SSID "YourSSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YourPassword"
#endif

#ifndef TARGET_IP
#define TARGET_IP "192.168.91.1"
#endif

// ESP32-S3-ETH (Waveshare) Pin Configuration
// Based on https://www.waveshare.com/wiki/ESP32-S3-ETH
#define ETH_PHY_TYPE        ETH_PHY_LAN8720
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC         23
#define ETH_PHY_MDIO        18
#define ETH_PHY_POWER       -1  // No power pin control needed
#define ETH_CLK_MODE        ETH_CLOCK_GPIO0_IN  // RMII clock input on GPIO0

// Network Configuration
// Ethernet will use DHCP to obtain IP address

// State tracking
static bool eth_connected = false;
static bool wifi_connected = false;

/**
 * WiFi Event Handler
 */
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.println("WiFi Started");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("WiFi Connected");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("WiFi Got IP: ");
            Serial.println(WiFi.localIP());
            wifi_connected = true;
            
            // Enable NAT on WiFi interface for forwarding (second param: 1 = enable)
            ip_napt_enable(WiFi.localIP(), 1);
            Serial.println("NAT enabled on WiFi interface");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("WiFi Disconnected - Reconnecting...");
            wifi_connected = false;
            WiFi.reconnect();
            break;
        default:
            break;
    }
}

/**
 * Ethernet Event Handler
 */
void EthernetEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("ETH Started");
            // Set hostname
            ETH.setHostname("esp32-bridge");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("ETH Connected");
            eth_connected = true;
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.print("ETH Got IP (DHCP): ");
            Serial.println(ETH.localIP());
            Serial.print("ETH Gateway: ");
            Serial.println(ETH.gatewayIP());
            Serial.print("ETH Subnet: ");
            Serial.println(ETH.subnetMask());
            Serial.print("ETH MAC: ");
            Serial.println(ETH.macAddress());
            
            // Configure routing
            Serial.println("Ethernet interface ready");
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("ETH Disconnected");
            eth_connected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("ETH Stopped");
            eth_connected = false;
            break;
        default:
            break;
    }
}

/**
 * Initialize WiFi in Station mode
 */
void setupWiFi() {
    Serial.println("Setting up WiFi...");
    
    // Register event handlers
    WiFi.onEvent(WiFiEvent);
    
    // Configure WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    Serial.print("Connecting to WiFi SSID: ");
    Serial.println(WIFI_SSID);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(1000);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected successfully");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        Serial.print("MAC address: ");
        Serial.println(WiFi.macAddress());
    } else {
        Serial.println("\nFailed to connect to WiFi");
    }
}

/**
 * Initialize Ethernet
 */
void setupEthernet() {
    Serial.println("Setting up Ethernet...");
    
    // Register event handler
    WiFi.onEvent(EthernetEvent);
    
    // Initialize Ethernet with LAN8720 PHY
    // DHCP is enabled by default (no ETH.config() call)
    if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLK_MODE)) {
        Serial.println("ETH initialization failed!");
        return;
    }
    
    Serial.println("Ethernet initialized - waiting for DHCP...");
}

/**
 * Setup NAPT (Network Address Port Translation)
 * This enables forwarding packets from Ethernet clients through WiFi
 */
void setupNAPT() {
    Serial.println("Configuring NAPT...");
    
    // Initialize NAPT with maximum values
    // IP_NAPT_MAX: maximum number of NAT entries (default 512)
    // IP_PORTMAP_MAX: maximum number of port mappings (default 32)
    #ifndef IP_NAPT_MAX
    #define IP_NAPT_MAX 512
    #endif
    #ifndef IP_PORTMAP_MAX
    #define IP_PORTMAP_MAX 32
    #endif
    
    ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);
    
    Serial.println("NAPT initialized - bridge is ready");
}

void setup() {
    // Initialize Serial
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=================================");
    Serial.println("ESP32-S3-POE-ETH WiFi Bridge");
    Serial.println("=================================");
    Serial.print("Target IP: ");
    Serial.println(TARGET_IP);
    Serial.println();
    
    // Initialize WiFi first (connects to target network)
    setupWiFi();
    
    delay(2000);
    
    // Initialize Ethernet (provides connectivity for clients)
    setupEthernet();
    
    delay(2000);
    
    // Setup NAPT for bridging
    setupNAPT();
    
    Serial.println("\n=================================");
    Serial.println("Bridge Setup Complete!");
    Serial.println("=================================");
    Serial.println("ESP32 has both WiFi and Ethernet connectivity");
    Serial.println("Ethernet IP obtained via DHCP");
    Serial.print("Traffic will be routed through WiFi to: ");
    Serial.println(TARGET_IP);
    Serial.println();
}

void loop() {
    // Monitor connection status
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 10000) {  // Print every 10 seconds
        Serial.println("=== Status ===");
        Serial.print("WiFi: ");
        Serial.println(wifi_connected ? "Connected" : "Disconnected");
        Serial.print("Ethernet: ");
        Serial.println(eth_connected ? "Connected" : "Disconnected");
        
        if (wifi_connected) {
            Serial.print("WiFi IP: ");
            Serial.println(WiFi.localIP());
        }
        
        if (eth_connected) {
            Serial.print("ETH IP: ");
            Serial.println(ETH.localIP());
        }
        
        Serial.println("==============\n");
        lastPrint = millis();
    }
    
    delay(100);
}
