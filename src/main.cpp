/**
 * ESP32-S3-POE-ETH WiFi-Ethernet Bridge
 * 
 * This application creates a transparent bridge between Ethernet and WiFi,
 * proxying requests from Ethernet clients to 192.168.91.1 via the ESP32's WiFi connection.
 * 
 * Hardware: ESP32-S3-ETH (Waveshare)
 * Features:
 * - Ethernet PHY: W5500 (SPI-based)
 * - WiFi Client mode connection
 * - IP forwarding/NAT from Ethernet to WiFi
 * - Proxies requests to target IP (192.168.91.1)
 * - Web interface for monitoring
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <SPI.h>
#include <WebServer.h>
#include <deque>

extern "C" {
#include "lwip/lwip_napt.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
}

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

// ESP32-S3-ETH (Waveshare) W5500 Pin Configuration
// Based on https://www.waveshare.com/wiki/ESP32-S3-ETH
#define ETH_SPI_HOST      SPI2_HOST
#define ETH_SPI_CLOCK_MHZ 20
#define ETH_MISO_GPIO     12
#define ETH_MOSI_GPIO     11
#define ETH_SCLK_GPIO     13
#define ETH_CS_GPIO       14
#define ETH_RST_GPIO      9
#define ETH_INT_GPIO      10

// Network Configuration
// Ethernet will use DHCP to obtain IP address

// NAPT Configuration
#define NAPT_ENABLE 1
#define IP_NAPT_MAX 512
#define IP_PORTMAP_MAX 32

// Web Server Configuration
#define WEB_SERVER_PORT 80
#define MAX_LOG_ENTRIES 50  // Reduced from 100 to save memory

// Web Server
WebServer server(WEB_SERVER_PORT);

// Log buffer for web display
std::deque<String> logBuffer;
bool logBufferEnabled = false;

// State tracking
static bool eth_connected = false;
static bool wifi_connected = false;
static bool napt_enabled = false;
static bool web_server_started = false;

// Flags for deferred initialization (set in ISR, processed in loop)
static volatile bool need_napt_check = false;
static volatile bool need_web_server_start = false;

// Forward declarations
void logPrint(const String& message);
void logPrintln(const String& message);

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
 * Callback function to enable NAPT (called from lwIP thread context)
 */
static void napt_enable_callback(void *arg) {
    uint32_t ip = *(uint32_t *)arg;
    ip_napt_enable(ip, 1);
}

/**
 * Enable NAPT (Network Address Port Translation) for packet forwarding
 * This allows Ethernet clients to access the WiFi network and target IP
 */
void enableNAPT() {
    if (napt_enabled) {
        return;  // Already enabled
    }
    
    if (!wifi_connected || !eth_connected) {
        return;  // Wait for both interfaces
    }
    
    Serial.println("Enabling NAPT for packet forwarding...");
    Serial.flush();
    logPrintln("Enabling NAPT for packet forwarding...");
    
    // Enable NAPT on the WiFi interface (upstream)
    // This allows packets from Ethernet to be forwarded to WiFi with address translation
    IPAddress wifi_ip = WiFi.localIP();
    uint32_t ip_val = (uint32_t)wifi_ip;
    
    // Use tcpip_callback to call ip_napt_enable from the correct thread context
    // This is the thread-safe way to call lwIP functions
    tcpip_callback(napt_enable_callback, &ip_val);
    
    Serial.println("NAPT enabled on WiFi interface");
    Serial.print("Forwarding Ethernet traffic through WiFi (");
    Serial.print(wifi_ip);
    Serial.println(") to target network");
    Serial.flush();
    
    logPrintln("NAPT enabled on WiFi interface");
    logPrint("Forwarding Ethernet traffic through WiFi (");
    logPrint(wifi_ip.toString());
    logPrintln(") to target network");
    
    napt_enabled = true;
}

/**
 * Web Server Handler - Root Page
 */
void handleRoot() {
    // Send HTML header
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Send HTML in chunks to avoid memory issues
    server.sendContent("<!DOCTYPE html><html><head>");
    server.sendContent("<meta charset='UTF-8'>");
    server.sendContent("<meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    server.sendContent("<title>ESP32 WiFi-Ethernet Bridge</title>");
    server.sendContent("<style>");
    server.sendContent("body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }");
    server.sendContent("h1 { color: #333; }");
    server.sendContent(".status { background: white; padding: 15px; border-radius: 5px; margin: 10px 0; }");
    server.sendContent(".status-item { margin: 5px 0; }");
    server.sendContent(".connected { color: green; font-weight: bold; }");
    server.sendContent(".disconnected { color: red; font-weight: bold; }");
    server.sendContent(".log-container { background: #1e1e1e; color: #d4d4d4; padding: 15px; border-radius: 5px; margin: 10px 0; }");
    server.sendContent(".log-container pre { margin: 0; font-family: 'Courier New', monospace; font-size: 12px; white-space: pre-wrap; word-wrap: break-word; }");
    server.sendContent(".refresh-btn { background: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 5px; cursor: pointer; margin: 10px 5px; }");
    server.sendContent(".refresh-btn:hover { background: #0056b3; }");
    server.sendContent("</style>");
    server.sendContent("<script>");
    server.sendContent("var autoRefresh = false;");
    server.sendContent("var refreshInterval;");
    server.sendContent("function toggleAutoRefresh() {");
    server.sendContent("  autoRefresh = !autoRefresh;");
    server.sendContent("  if (autoRefresh) {");
    server.sendContent("    refreshInterval = setInterval(function(){ location.reload(); }, 3000);");
    server.sendContent("    document.getElementById('autoRefreshBtn').innerText = 'Stop Auto Refresh';");
    server.sendContent("  } else {");
    server.sendContent("    clearInterval(refreshInterval);");
    server.sendContent("    document.getElementById('autoRefreshBtn').innerText = 'Start Auto Refresh';");
    server.sendContent("  }");
    server.sendContent("}");
    server.sendContent("</script>");
    server.sendContent("</head><body>");
    server.sendContent("<h1>ESP32 WiFi-Ethernet Bridge</h1>");
    
    // Status section
    server.sendContent("<div class='status'>");
    server.sendContent("<h2>Bridge Status</h2>");
    
    String statusLine = "<div class='status-item'>WiFi: <span class='" + String(wifi_connected ? "connected" : "disconnected") + "'>";
    statusLine += wifi_connected ? "Connected" : "Disconnected";
    statusLine += "</span></div>";
    server.sendContent(statusLine);
    
    if (wifi_connected) {
        server.sendContent("<div class='status-item'>WiFi IP: " + WiFi.localIP().toString() + "</div>");
        server.sendContent("<div class='status-item'>WiFi MAC: " + WiFi.macAddress() + "</div>");
    }
    
    statusLine = "<div class='status-item'>Ethernet: <span class='" + String(eth_connected ? "connected" : "disconnected") + "'>";
    statusLine += eth_connected ? "Connected" : "Disconnected";
    statusLine += "</span></div>";
    server.sendContent(statusLine);
    
    if (eth_connected) {
        server.sendContent("<div class='status-item'>Ethernet IP: " + ETH.localIP().toString() + "</div>");
        server.sendContent("<div class='status-item'>Ethernet MAC: " + ETH.macAddress() + "</div>");
    }
    
    server.sendContent("<div class='status-item'>Uptime: " + String(millis() / 1000) + " seconds</div>");
    
    statusLine = "<div class='status-item'>NAPT Forwarding: <span class='" + String(napt_enabled ? "connected" : "disconnected") + "'>";
    statusLine += napt_enabled ? "Enabled" : "Disabled";
    statusLine += "</span></div>";
    server.sendContent(statusLine);
    
    server.sendContent("<div class='status-item'>Target IP: " + String(TARGET_IP) + "</div>");
    server.sendContent("</div>");
    
    // Controls
    server.sendContent("<div>");
    server.sendContent("<button class='refresh-btn' onclick='location.reload();'>Refresh Now</button>");
    server.sendContent("<button class='refresh-btn' id='autoRefreshBtn' onclick='toggleAutoRefresh();'>Start Auto Refresh</button>");
    server.sendContent("</div>");
    
    // Logs section
    server.sendContent("<div class='log-container'>");
    server.sendContent("<h2 style='color: #d4d4d4; margin-top: 0;'>Device Logs (Last " + String(logBuffer.size()) + " entries)</h2>");
    server.sendContent("<pre>");
    
    // Send logs in smaller chunks to avoid memory issues
    String logChunk = "";
    int chunkCount = 0;
    for (const auto& entry : logBuffer) {
        logChunk += entry;
        chunkCount++;
        // Send every 10 log entries to keep memory usage low
        if (chunkCount >= 10) {
            server.sendContent(logChunk);
            logChunk = "";
            chunkCount = 0;
        }
    }
    // Send remaining logs
    if (logChunk.length() > 0) {
        server.sendContent(logChunk);
    }
    
    server.sendContent("</pre>");
    server.sendContent("</div>");
    server.sendContent("</body></html>");
    server.sendContent("");  // End chunked response
}

/**
 * Web Server Handler - Plain Text Logs
 */
void handleLogs() {
    String logs = "";
    logs.reserve(logBuffer.size() * 80);  // Pre-allocate approximate space
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
    if (wifi_connected) {
        json += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
    } else {
        json += "\"wifi_ip\":null,";
    }
    json += "\"eth_connected\":" + String(eth_connected ? "true" : "false") + ",";
    if (eth_connected) {
        json += "\"eth_ip\":\"" + ETH.localIP().toString() + "\",";
    } else {
        json += "\"eth_ip\":null,";
    }
    json += "\"napt_enabled\":" + String(napt_enabled ? "true" : "false") + ",";
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
            
            // Schedule NAPT check (don't do it in interrupt context)
            need_napt_check = true;
            
            logPrintln("WiFi interface ready for bridging");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            logPrintln("WiFi Disconnected - Reconnecting...");
            wifi_connected = false;
            napt_enabled = false;  // Reset NAPT state when WiFi disconnects
            need_napt_check = false;  // Cancel pending NAPT check
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
            
            // Schedule NAPT and web server (don't do it in interrupt context)
            need_napt_check = true;
            need_web_server_start = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            logPrintln("ETH Disconnected");
            eth_connected = false;
            napt_enabled = false;  // Reset NAPT state when Ethernet disconnects
            need_napt_check = false;  // Cancel pending NAPT check
            need_web_server_start = false;  // Cancel pending web server start
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
    Serial.println("Setting up WiFi...");
    Serial.flush();
    logPrintln("Setting up WiFi...");
    
    // Register event handlers
    WiFi.onEvent(WiFiEvent);
    
    // Configure WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    Serial.print("Connecting to WiFi SSID: ");
    Serial.println(WIFI_SSID);
    Serial.flush();
    logPrint("Connecting to WiFi SSID: ");
    logPrintln(WIFI_SSID);
    
    // Wait for connection
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(1000);
        Serial.print(".");
        logPrint(".");
        attempts++;
    }
    Serial.println();
    Serial.flush();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected successfully");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        Serial.print("MAC address: ");
        Serial.println(WiFi.macAddress());
        Serial.flush();
        
        logPrintln("\nWiFi connected successfully");
        logPrint("IP address: ");
        logPrintln(WiFi.localIP().toString());
        logPrint("MAC address: ");
        logPrintln(WiFi.macAddress());
    } else {
        Serial.println("\nFailed to connect to WiFi");
        Serial.flush();
        logPrintln("\nFailed to connect to WiFi");
    }
}

/**
 * Initialize Ethernet
 */
void setupEthernet() {
    Serial.println("Setting up Ethernet...");
    Serial.flush();
    logPrintln("Setting up Ethernet...");
    
    // Register event handler
    WiFi.onEvent(EthernetEvent);
    
    // Initialize SPI for W5500
    Serial.println("Initializing SPI bus...");
    Serial.flush();
    logPrintln("Initializing SPI bus...");
    SPI.begin(ETH_SCLK_GPIO, ETH_MISO_GPIO, ETH_MOSI_GPIO, ETH_CS_GPIO);
    
    // Give SPI time to initialize
    delay(100);
    
    // Initialize Ethernet with W5500 via SPI
    // ESP32-S3-ETH uses W5500 chip, not RMII PHY
    Serial.println("Initializing W5500 Ethernet chip...");
    Serial.flush();
    logPrintln("Initializing W5500 Ethernet chip...");
    // ETH.begin signature: (type, phy_addr, cs, irq, rst, spi, spi_freq_mhz)
    if (!ETH.begin(ETH_PHY_W5500, 1, ETH_CS_GPIO, ETH_INT_GPIO, ETH_RST_GPIO, SPI, ETH_SPI_CLOCK_MHZ)) {
        Serial.println("ETH initialization failed!");
        Serial.println("Check W5500 SPI connections:");
        Serial.println("  MISO: GPIO12, MOSI: GPIO11, SCLK: GPIO13");
        Serial.println("  CS: GPIO14, RST: GPIO9, INT: GPIO10");
        Serial.flush();
        
        logPrintln("ETH initialization failed!");
        logPrintln("Check W5500 SPI connections:");
        logPrintln("  MISO: GPIO12, MOSI: GPIO11, SCLK: GPIO13");
        logPrintln("  CS: GPIO14, RST: GPIO9, INT: GPIO10");
        return;
    }
    
    // Set hostname after initialization
    ETH.setHostname("esp32-bridge");
    
    Serial.println("W5500 Ethernet initialized - waiting for DHCP...");
    Serial.flush();
    logPrintln("W5500 Ethernet initialized - waiting for DHCP...");
}

void setup() {
    // Initialize Serial and wait for it to be ready
    Serial.begin(115200);
    
    // For ESP32-S3 with USB CDC, wait for Serial to be ready
    unsigned long start = millis();
    while (!Serial && (millis() - start < 5000)) {
        delay(100);
    }
    delay(1000);
    
    // Send initial newlines to ensure clean output
    Serial.println();
    Serial.println();
    Serial.flush();
    
    // Enable log buffering
    logBufferEnabled = true;
    
    Serial.println("\n\n=================================");
    Serial.println("ESP32-S3-POE-ETH WiFi Bridge");
    Serial.println("=================================");
    Serial.print("Target IP: ");
    Serial.println(TARGET_IP);
    Serial.println("");
    Serial.flush();
    
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
    
    logPrintln("\n=================================");
    logPrintln("Bridge Setup Complete!");
    logPrintln("=================================");
    logPrintln("Initializing WiFi and Ethernet interfaces...");
    logPrintln("NAPT will be enabled automatically when both interfaces are ready");
    logPrint("Traffic will be forwarded through WiFi to: ");
    logPrintln(TARGET_IP);
    logPrintln("Web interface will be available on Ethernet IP (HTTP port 80)");
    logPrintln("");
}

/**
 * Main loop
 */
void loop() {
    // Process deferred initialization (must be done outside interrupt context)
    if (need_web_server_start && !web_server_started && eth_connected) {
        need_web_server_start = false;
        setupWebServer();
        web_server_started = true;
    }
    
    if (need_napt_check) {
        need_napt_check = false;
        enableNAPT();
    }
    
    // Handle web server requests
    if (web_server_started) {
        server.handleClient();
    }
    
    // Monitor connection status
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 10000) {  // Print every 10 seconds
        Serial.println("=== Status ===");
        Serial.print("WiFi: ");
        Serial.println(wifi_connected ? "Connected" : "Disconnected");
        Serial.print("Ethernet: ");
        Serial.println(eth_connected ? "Connected" : "Disconnected");
        Serial.print("NAPT: ");
        Serial.println(napt_enabled ? "Enabled (forwarding active)" : "Disabled");
        
        if (wifi_connected) {
            Serial.print("WiFi IP: ");
            Serial.println(WiFi.localIP());
        }
        
        if (eth_connected) {
            Serial.print("ETH IP: ");
            Serial.println(ETH.localIP());
        }
        
        Serial.println("==============\n");
        Serial.flush();
        
        logPrintln("=== Status ===");
        logPrint("WiFi: ");
        logPrintln(wifi_connected ? "Connected" : "Disconnected");
        logPrint("Ethernet: ");
        logPrintln(eth_connected ? "Connected" : "Disconnected");
        logPrint("NAPT: ");
        logPrintln(napt_enabled ? "Enabled (forwarding active)" : "Disabled");
        
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
