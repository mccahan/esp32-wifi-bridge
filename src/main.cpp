#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include "config.h"
#include "cert.h"
#include <lwip/sockets.h>

// WebServer_ESP32_SC_W5500 library for W5500 support on ESP32-S3
#define DEBUG_ETHERNET_WEBSERVER_PORT Serial
#define _ETHERNET_WEBSERVER_LOGLEVEL_ 3

// Define custom SPI pins for ESP32-S3-POE-ETH from config
#define INT_GPIO    W5500_INT_GPIO
#define MISO_GPIO   W5500_MISO_GPIO
#define MOSI_GPIO   W5500_MOSI_GPIO
#define SCK_GPIO    W5500_SCK_GPIO
#define CS_GPIO     W5500_CS_GPIO

#define USE_ETHERNET_WRAPPER  true
#include <WebServer_ESP32_SC_W5500.h>

/*
 * HTTPS/TLS Implementation Note:
 * 
 * The W5500 Ethernet chip does not support hardware TLS/SSL encryption.
 * This implementation provides a transparent TCP proxy on port 443 that forwards
 * traffic between Ethernet clients and the Powerwall WiFi endpoint.
 * 
 * The self-signed certificate (cert.h) is included for reference but not actively
 * used in this transparent proxy implementation. To implement proper TLS termination,
 * you would need to:
 * 1. Use mbedTLS or similar library for software TLS
 * 2. Decrypt incoming TLS traffic from Ethernet clients
 * 3. Re-encrypt when forwarding to Powerwall
 * 
 * For most use cases, this transparent TCP tunnel is sufficient as the actual
 * TLS encryption is handled end-to-end between the client and the Powerwall.
 */

// MAC address for W5500
byte mac[] = ETH_MAC_ADDR;

// Powerwall IP on WiFi network
IPAddress powerwallIP(POWERWALL_IP_ADDR1, POWERWALL_IP_ADDR2, POWERWALL_IP_ADDR3, POWERWALL_IP_ADDR4);

// TCP server socket
int serverSocket = -1;

bool ethConnected = false;

void setupEthernet() {
    Serial.println("Setting up Ethernet...");
    
    ET_LOGWARN(F("Custom SPI pinout:"));
    ET_LOGWARN1(F("MOSI:"), MOSI_GPIO);
    ET_LOGWARN1(F("MISO:"), MISO_GPIO);
    ET_LOGWARN1(F("SCK:"),  SCK_GPIO);
    ET_LOGWARN1(F("CS:"),   CS_GPIO);
    ET_LOGWARN1(F("INT:"),  INT_GPIO);
    ET_LOGWARN(F("========================="));
    
    // Initialize ESP32_W5500 event handler
    ESP32_W5500_onEvent();
    
    // Start ethernet with custom pins and DHCP
    ETH.begin(MISO_GPIO, MOSI_GPIO, SCK_GPIO, CS_GPIO, INT_GPIO, 25, SPI3_HOST, mac);
    
    // Wait for connection
    ESP32_W5500_waitForConnect();
    
    ethConnected = true;
    Serial.print("Ethernet IP: ");
    Serial.println(ETH.localIP());
}

void setupWiFi() {
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(1000);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        Serial.print("WiFi IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection failed!");
    }
}

void setupMDNS() {
    if (!MDNS.begin(MDNS_HOSTNAME)) {
        Serial.println("Error setting up mDNS");
        return;
    }
    
    // Advertise _powerwall service
    MDNS.addService(MDNS_SERVICE, MDNS_PROTOCOL, PROXY_PORT);
    Serial.print("mDNS responder started: ");
    Serial.print(MDNS_HOSTNAME);
    Serial.println(".local");
    Serial.print("Advertising ");
    Serial.print(MDNS_SERVICE);
    Serial.print(".");
    Serial.print(MDNS_PROTOCOL);
    Serial.print(" service on port ");
    Serial.println(PROXY_PORT);
}

void setupTCPServer() {
    // Create TCP socket
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        Serial.println("Failed to create socket");
        return;
    }
    
    // Set socket options to reuse address
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set to non-blocking
    int flags = fcntl(serverSocket, F_GETFL, 0);
    fcntl(serverSocket, F_SETFL, flags | O_NONBLOCK);
    
    // Bind to port
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PROXY_PORT);
    
    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        Serial.println("Bind failed");
        close(serverSocket);
        serverSocket = -1;
        return;
    }
    
    // Listen for connections
    if (listen(serverSocket, 5) < 0) {
        Serial.println("Listen failed");
        close(serverSocket);
        serverSocket = -1;
        return;
    }
    
    Serial.print("TCP Server started on port ");
    Serial.println(PROXY_PORT);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy ===");
    Serial.println("Target: Tesla Powerwall at 192.168.91.1");
    
    // Setup Ethernet first
    setupEthernet();
    
    // Setup WiFi connection to Powerwall
    setupWiFi();
    
    // Setup mDNS
    setupMDNS();
    
    // Start TCP server on Ethernet (port 443)
    if (ethConnected) {
        setupTCPServer();
        Serial.println("Ready to proxy traffic from Ethernet to WiFi (192.168.91.1)");
    } else {
        Serial.println("Cannot start server - Ethernet not connected");
    }
}

void handleClient(int clientSocket) {
    Serial.println("\n--- New client connected ---");
    
    // Connect to Powerwall over WiFi
    WiFiClient powerwallClient;
    
    if (!powerwallClient.connect(powerwallIP, 443)) {
        Serial.println("Failed to connect to Powerwall");
        close(clientSocket);
        return;
    }
    
    Serial.println("Connected to Powerwall");
    
    // Set client socket to non-blocking
    int flags = fcntl(clientSocket, F_GETFL, 0);
    fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK);
    
    // Proxy data bidirectionally
    uint8_t buf[1024];
    unsigned long timeout = millis();
    const unsigned long TIMEOUT_MS = PROXY_TIMEOUT_MS;
    
    while (true) {
        bool activity = false;
        
        // Client -> Powerwall
        int len = recv(clientSocket, buf, sizeof(buf), 0);
        if (len > 0) {
            powerwallClient.write(buf, len);
            timeout = millis();
            activity = true;
        } else if (len == 0) {
            // Connection closed
            break;
        }
        
        // Powerwall -> Client
        while (powerwallClient.available()) {
            len = powerwallClient.read(buf, sizeof(buf));
            if (len > 0) {
                send(clientSocket, buf, len, 0);
                timeout = millis();
                activity = true;
            }
        }
        
        // Check if Powerwall is still connected
        if (!powerwallClient.connected()) {
            Serial.println("Powerwall disconnected");
            break;
        }
        
        // Check for timeout
        if (millis() - timeout > TIMEOUT_MS) {
            Serial.println("Connection timeout");
            break;
        }
        
        if (!activity) {
            delay(1);
        }
    }
    
    powerwallClient.stop();
    close(clientSocket);
    Serial.println("Client disconnected");
}

void loop() {
    if (serverSocket < 0) {
        delay(1000);
        return;
    }
    
    // Accept new connections
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
    
    if (clientSocket >= 0) {
        handleClient(clientSocket);
    }
    
    delay(10);
}



