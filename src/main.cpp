#include <Arduino.h>
#include <SPI.h>

// Define board type before including Ethernet library
#define USING_SPI2  false
#define USE_ETHERNET_GENERIC  true
#define USE_ETHERNET_ESP8266  false 
#define USE_ETHERNET_ENC      false
#define USE_UIP_ETHERNET      false
#define USE_CUSTOM_ETHERNET   false

#include <Ethernet_Generic.h>

#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include "config.h"
#include "cert.h"

/*
 * ESP32-S3 W5500 Ethernet WiFi Bridge
 * 
 * This implementation uses Ethernet_Generic library with W5500.
 * The W5500 Ethernet chip does not support hardware TLS/SSL encryption.
 * This provides a transparent TCP proxy on port 443 that forwards traffic 
 * between Ethernet clients and the Powerwall WiFi endpoint.
 * 
 * The self-signed certificate (cert.h) is included for reference but not actively
 * used in this transparent proxy implementation.
 */

// MAC address for W5500
byte mac[] = ETH_MAC_ADDR;

// Powerwall IP on WiFi network
IPAddress powerwallIP(POWERWALL_IP_ADDR1, POWERWALL_IP_ADDR2, POWERWALL_IP_ADDR3, POWERWALL_IP_ADDR4);

// Ethernet server
EthernetServer *server = nullptr;

bool ethConnected = false;

void setupEthernet() {
    Serial.println("Setting up Ethernet with W5500...");
    
    // Initialize SPI with custom pins
    SPI.begin(W5500_SCK_GPIO, W5500_MISO_GPIO, W5500_MOSI_GPIO, W5500_CS_GPIO);
    
    // Initialize Ethernet library
    Ethernet.init(W5500_CS_GPIO);
    
    Serial.printf("SPI Pins - MOSI:%d MISO:%d SCLK:%d CS:%d\n", 
                  W5500_MOSI_GPIO, W5500_MISO_GPIO, W5500_SCK_GPIO, W5500_CS_GPIO);
    
    // Start Ethernet with DHCP
    Serial.println("Starting Ethernet with DHCP...");
    if (Ethernet.begin(mac, 10000, 4000) == 0) {
        Serial.println("Failed to configure Ethernet using DHCP");
        if (Ethernet.linkStatus() == LinkOFF) {
            Serial.println("Ethernet cable is not connected");
        }
        return;
    }
    
    ethConnected = true;
    Serial.println("Ethernet connected successfully");
    Serial.print("IP: ");
    Serial.println(Ethernet.localIP());
    Serial.print("Subnet: ");
    Serial.println(Ethernet.subnetMask());
    Serial.print("Gateway: ");
    Serial.println(Ethernet.gatewayIP());
    Serial.print("DNS: ");
    Serial.println(Ethernet.dnsServerIP());
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
    
    // Start Ethernet server on port 443
    if (ethConnected) {
        server = new EthernetServer(PROXY_PORT);
        server->begin();
        Serial.print("Ethernet server started on port ");
        Serial.println(PROXY_PORT);
        Serial.println("Ready to proxy traffic from Ethernet to WiFi (192.168.91.1)");
    } else {
        Serial.println("Cannot start server - Ethernet not connected");
    }
}

void handleClient(EthernetClient& client) {
    Serial.println("\n--- New client connected ---");
    
    // Connect to Powerwall over WiFi
    WiFiClient powerwallClient;
    
    if (!powerwallClient.connect(powerwallIP, 443)) {
        Serial.println("Failed to connect to Powerwall");
        client.stop();
        return;
    }
    
    Serial.println("Connected to Powerwall");
    
    // Proxy data bidirectionally
    uint8_t buf[1024];
    unsigned long timeout = millis();
    const unsigned long TIMEOUT_MS = PROXY_TIMEOUT_MS;
    
    while (client.connected() && powerwallClient.connected()) {
        bool activity = false;
        
        // Client -> Powerwall
        while (client.available()) {
            int len = client.read(buf, sizeof(buf));
            if (len > 0) {
                powerwallClient.write(buf, len);
                timeout = millis();
                activity = true;
            }
        }
        
        // Powerwall -> Client
        while (powerwallClient.available()) {
            int len = powerwallClient.read(buf, sizeof(buf));
            if (len > 0) {
                client.write(buf, len);
                timeout = millis();
                activity = true;
            }
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
    client.stop();
    Serial.println("Client disconnected");
}

void loop() {
    // Maintain DHCP lease
    Ethernet.maintain();
    
    // Check for server validity
    if (!server) {
        delay(1000);
        return;
    }
    
    // Check for incoming connections
    EthernetClient client = server->available();
    
    if (client) {
        handleClient(client);
    }
    
    delay(10);
}



