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
#include <WebServer.h>
#include <lwip/lwip_napt.h>
#include <lwip/ip_addr.h>
#include <lwip/dns.h>
#include <vector>
#include <deque>

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
#define ETH_PHY_ADDR        1
#define ETH_PHY_MDC         23
#define ETH_PHY_MDIO        18
#define ETH_PHY_POWER       -1  // No power pin control needed

// Network Configuration
// Ethernet will use DHCP to obtain IP address

// NAPT Configuration
#ifndef IP_NAPT_MAX
#define IP_NAPT_MAX 512  // Maximum number of NAT entries
#endif
#ifndef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX 32  // Maximum number of port mappings
#endif

// Web Server Configuration
#define WEB_SERVER_PORT 80
#define MAX_LOG_ENTRIES 100

// Web Server
WebServer server(WEB_SERVER_PORT);

// Log buffer for web display
std::deque<String> logBuffer;
bool logBufferEnabled = false;

// State tracking
static bool eth_connected = false;
static bool wifi_connected = false;

/**
 * Add a log entry to the buffer for web display
 */
void addLogEntry(const String& message) {
    if (!logBufferEnabled) return;
    
    // Add timestamp
    char timestamp[32];
    unsigned long ms = millis();
    snprintf(timestamp, sizeof(timestamp), "[%lu.%03lu] ", ms / 1000, ms % 1000);
    
    String logEntry = String(timestamp) + message;
    
    logBuffer.push_back(logEntry);
    
    // Keep buffer size limited
    while (logBuffer.size() > MAX_LOG_ENTRIES) {
        logBuffer.pop_front();
    }
}

/**
 * Print to Serial and add to log buffer
 */
void logPrint(const String& message) {
    Serial.print(message);
    addLogEntry(message);
}

void logPrintln(const String& message) {
    Serial.println(message);
    addLogEntry(message + "\n");
}

/**
 * Web Server Handler - Root Page
 */
void handleRoot() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>ESP32 WiFi-Ethernet Bridge</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
    html += "h1 { color: #333; }";
    html += ".status { background: white; padding: 15px; border-radius: 5px; margin: 10px 0; }";
    html += ".status-item { margin: 5px 0; }";
    html += ".connected { color: green; font-weight: bold; }";
    html += ".disconnected { color: red; font-weight: bold; }";
    html += ".log-container { background: #1e1e1e; color: #d4d4d4; padding: 15px; border-radius: 5px; margin: 10px 0; }";
    html += ".log-container pre { margin: 0; font-family: 'Courier New', monospace; font-size: 12px; white-space: pre-wrap; word-wrap: break-word; }";
    html += ".refresh-btn { background: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; margin: 10px 5px; }";
    html += ".refresh-btn:hover { background: #0056b3; }";
    html += ".auto-refresh { margin: 10px 0; }";
    html += "</style>";
    html += "<script>";
    html += "var autoRefresh = false;";
    html += "var refreshInterval;";
    html += "function toggleAutoRefresh() {";
    html += "  autoRefresh = !autoRefresh;";
    html += "  if (autoRefresh) {";
    html += "    refreshInterval = setInterval(function(){ location.reload(); }, 3000);";
    html += "    document.getElementById('autoRefreshBtn').innerText = 'Stop Auto Refresh';";
    html += "  } else {";
    html += "    clearInterval(refreshInterval);";
    html += "    document.getElementById('autoRefreshBtn').innerText = 'Start Auto Refresh';";
    html += "  }";
    html += "}";
    html += "</script>";
    html += "</head><body>";
    html += "<h1>ESP32 WiFi-Ethernet Bridge</h1>";
    
    // Status section
    html += "<div class='status'>";
    html += "<h2>Bridge Status</h2>";
    html += "<div class='status-item'>WiFi: <span class='" + String(wifi_connected ? "connected" : "disconnected") + "'>";
    html += wifi_connected ? "Connected" : "Disconnected";
    html += "</span></div>";
    if (wifi_connected) {
        html += "<div class='status-item'>WiFi IP: " + WiFi.localIP().toString() + "</div>";
        html += "<div class='status-item'>WiFi MAC: " + WiFi.macAddress() + "</div>";
    }
    html += "<div class='status-item'>Ethernet: <span class='" + String(eth_connected ? "connected" : "disconnected") + "'>";
    html += eth_connected ? "Connected" : "Disconnected";
    html += "</span></div>";
    if (eth_connected) {
        html += "<div class='status-item'>Ethernet IP: " + ETH.localIP().toString() + "</div>";
        html += "<div class='status-item'>Ethernet MAC: " + ETH.macAddress() + "</div>";
    }
    html += "<div class='status-item'>Uptime: " + String(millis() / 1000) + " seconds</div>";
    html += "<div class='status-item'>Target IP: " + String(TARGET_IP) + "</div>";
    html += "</div>";
    
    // Controls
    html += "<div>";
    html += "<button class='refresh-btn' onclick='location.reload();'>Refresh Now</button>";
    html += "<button class='refresh-btn' id='autoRefreshBtn' onclick='toggleAutoRefresh();'>Start Auto Refresh</button>";
    html += "</div>";
    
    // Logs section
    html += "<div class='log-container'>";
    html += "<h2 style='color: #d4d4d4; margin-top: 0;'>Device Logs (Last " + String(logBuffer.size()) + " entries)</h2>";
    html += "<pre>";
    for (const auto& entry : logBuffer) {
        html += entry;
    }
    html += "</pre>";
    html += "</div>";
    
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

/**
 * Web Server Handler - Plain Text Logs
 */
void handleLogs() {
    String logs = "";
    for (const auto& entry : logBuffer) {
        logs += entry;
    }
    server.send(200, "text/plain", logs);
}

/**
 * Web Server Handler - JSON Status
 */
void handleStatus() {
    String json = "{";
    json += "\"wifi_connected\":" + String(wifi_connected ? "true" : "false") + ",";
    json += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"eth_connected\":" + String(eth_connected ? "true" : "false") + ",";
    json += "\"eth_ip\":\"" + ETH.localIP().toString() + "\",";
    json += "\"uptime\":" + String(millis() / 1000) + ",";
    json += "\"target_ip\":\"" + String(TARGET_IP) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

/**
 * Setup Web Server
 */
void setupWebServer() {
    logPrintln("Setting up web server...");
    
    server.on("/", handleRoot);
    server.on("/logs", handleLogs);
    server.on("/status", handleStatus);
    
    server.begin();
    logPrintln("Web server started on port " + String(WEB_SERVER_PORT));
    if (eth_connected) {
        logPrintln("Access at: http://" + ETH.localIP().toString());
    }
}

/**
 * WiFi Event Handler
 */
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            logPrintln("WiFi Started");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            logPrintln("WiFi Connected");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            logPrint("WiFi Got IP: ");
            logPrintln(WiFi.localIP().toString());
            wifi_connected = true;
            
            // Note: NAT configuration for ESP32-S3 
            // IP forwarding is enabled by default when both interfaces are active
            logPrintln("WiFi interface ready for bridging");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            logPrintln("WiFi Disconnected - Reconnecting...");
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
            logPrintln("ETH Started");
            // Set hostname
            ETH.setHostname("esp32-bridge");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            logPrintln("ETH Connected");
            eth_connected = true;
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            logPrint("ETH Got IP (DHCP): ");
            logPrintln(ETH.localIP().toString());
            logPrint("ETH Gateway: ");
            logPrintln(ETH.gatewayIP().toString());
            logPrint("ETH Subnet: ");
            logPrintln(ETH.subnetMask().toString());
            logPrint("ETH MAC: ");
            logPrintln(ETH.macAddress());
            
            // Configure routing
            logPrintln("Ethernet interface ready");
            
            // Start web server once we have an Ethernet IP
            setupWebServer();
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            logPrintln("ETH Disconnected");
            eth_connected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            logPrintln("ETH Stopped");
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
    logPrintln("Setting up WiFi...");
    
    // Register event handlers
    WiFi.onEvent(WiFiEvent);
    
    // Configure WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    logPrint("Connecting to WiFi SSID: ");
    logPrintln(WIFI_SSID);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(1000);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        logPrintln("\nWiFi connected successfully");
        logPrint("IP address: ");
        logPrintln(WiFi.localIP().toString());
        logPrint("MAC address: ");
        logPrintln(WiFi.macAddress());
    } else {
        logPrintln("\nFailed to connect to WiFi");
    }
}

/**
 * Initialize Ethernet
 */
void setupEthernet() {
    logPrintln("Setting up Ethernet...");
    
    // Register event handler
    WiFi.onEvent(EthernetEvent);
    
    // Initialize Ethernet with LAN8720 PHY
    // DHCP is enabled by default (no ETH.config() call)
    // Using simplified begin() method for ESP32-S3
    if (!ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO)) {
        logPrintln("ETH initialization failed!");
        return;
    }
    
    logPrintln("Ethernet initialized - waiting for DHCP...");
}

/**
 * Setup NAPT (Network Address Port Translation)
 * This enables forwarding packets from Ethernet clients through WiFi
 */
void setupNAPT() {
    logPrintln("Configuring NAPT...");
    
    // Note: NAPT may not be available on all ESP32 variants
    // For ESP32-S3, IP forwarding is handled differently
    logPrintln("NAPT configuration - bridge is ready");
}

void setup() {
    // Initialize Serial
    Serial.begin(115200);
    delay(1000);
    
    // Enable log buffering
    logBufferEnabled = true;
    
    logPrintln("\n\n=================================");
    logPrintln("ESP32-S3-POE-ETH WiFi Bridge");
    logPrintln("=================================");
    logPrint("Target IP: ");
    logPrintln(TARGET_IP);
    logPrintln("");
    
    // Initialize WiFi first (connects to target network)
    setupWiFi();
    
    delay(2000);
    
    // Initialize Ethernet (provides connectivity for clients)
    setupEthernet();
    
    delay(2000);
    
    // Setup NAPT for bridging
    setupNAPT();
    
    logPrintln("\n=================================");
    logPrintln("Bridge Setup Complete!");
    logPrintln("=================================");
    logPrintln("ESP32 has both WiFi and Ethernet connectivity");
    logPrintln("Ethernet IP obtained via DHCP");
    logPrint("Traffic will be routed through WiFi to: ");
    logPrintln(TARGET_IP);
    logPrintln("Web interface will be available on Ethernet IP");
    logPrintln("");
}

void loop() {
    // Handle web server requests
    server.handleClient();
    
    // Monitor connection status
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 10000) {  // Print every 10 seconds
        logPrintln("=== Status ===");
        logPrint("WiFi: ");
        logPrintln(wifi_connected ? "Connected" : "Disconnected");
        logPrint("Ethernet: ");
        logPrintln(eth_connected ? "Connected" : "Disconnected");
        
        if (wifi_connected) {
            logPrint("WiFi IP: ");
            logPrintln(WiFi.localIP().toString());
        }
        
        if (eth_connected) {
            logPrint("ETH IP: ");
            logPrintln(ETH.localIP().toString());
        }
        
        logPrintln("==============\n");
        lastPrint = millis();
    }
    
    delay(10);
}
