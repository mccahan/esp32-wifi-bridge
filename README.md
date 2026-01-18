# ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy

This project uses the ESP32-S3-POE-ETH board (Waveshare) to create a WiFi-Ethernet bridge that proxies HTTPS traffic from the Ethernet interface to a Tesla Powerwall at 192.168.91.1 over WiFi.

## Hardware

- **Board**: ESP32-S3-POE-ETH (Waveshare)
- **Ethernet Controller**: W5500 (SPI)

### Pin Configuration

| Function | GPIO |
|----------|------|
| MISO     | 12   |
| MOSI     | 11   |
| SCLK     | 13   |
| CS       | 14   |
| RST      | 9    |
| INT      | 10   |

## Features

- **WiFi Client**: Connects to Tesla Powerwall AP (192.168.91.1)
- **Ethernet Server**: TCP server on port 443 (transparent HTTPS tunnel)
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional Proxy**: Forwards HTTPS traffic between Ethernet and WiFi
- **Self-signed Certificate**: Included for reference (cert.h)

## TLS/HTTPS Implementation

The W5500 Ethernet chip does not support hardware TLS/SSL. This implementation provides a **transparent TCP proxy** on port 443 that tunnels encrypted traffic between Ethernet clients and the Powerwall. The actual TLS encryption is handled end-to-end between the client and Powerwall.

For applications requiring TLS termination on the ESP32, you would need to implement software TLS using mbedTLS.

## Configuration

Edit `include/config.h` to customize:

```cpp
// WiFi Settings
#define WIFI_SSID "TeslaPowerwall"
#define WIFI_PASSWORD ""

// Powerwall IP
#define POWERWALL_IP_ADDR1 192
#define POWERWALL_IP_ADDR2 168
#define POWERWALL_IP_ADDR3 91
#define POWERWALL_IP_ADDR4 1

// Ethernet MAC Address
#define ETH_MAC_ADDR { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }

// Proxy Settings
#define PROXY_PORT 443
#define PROXY_TIMEOUT_MS 30000

// mDNS Settings
#define MDNS_HOSTNAME "powerwall"
#define MDNS_SERVICE "_powerwall"
#define MDNS_PROTOCOL "_tcp"
```

## Building

This project uses PlatformIO:

```bash
pio run
```

### Note for macOS Apple Silicon Users

If you encounter toolchain architecture errors on M1/M2/M3 Macs, you may need to:
1. Use Rosetta 2 emulation
2. Build on a different platform (Linux, Windows, or CI/CD)
3. Use remote development/build environment

The code is compatible and will build successfully on x86_64 systems.

## Uploading

```bash
pio run --target upload
```

## Monitoring

```bash
pio device monitor
```

## Usage

1. The device connects to the Tesla Powerwall WiFi network
2. Ethernet interface obtains IP via DHCP
3. TCP server starts on port 443 on Ethernet interface
4. mDNS service advertises as "powerwall.local" with "_powerwall._tcp" service
5. All HTTPS requests to Ethernet interface are proxied to 192.168.91.1 over WiFi

## mDNS Discovery

The service can be discovered on the local network as:
- Hostname: `powerwall.local`
- Service: `_powerwall._tcp`
- Port: 443

## Serial Output Example

```
=== ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy ===
Target: Tesla Powerwall at 192.168.91.1
Setting up Ethernet...
Custom SPI pinout:
MOSI: 11
MISO: 12
SCK: 13
CS: 14
INT: 10
=========================
Ethernet IP: 192.168.1.100
Connecting to WiFi: TeslaPowerwall
WiFi connected
WiFi IP: 192.168.91.2
mDNS responder started: powerwall.local
Advertising _powerwall._tcp service on port 443
TCP Server started on port 443
Ready to proxy traffic from Ethernet to WiFi (192.168.91.1)
```

## Files

- `src/main.cpp` - Main application code
- `include/config.h` - Configuration settings
- `include/cert.h` - Self-signed certificate (for reference)
- `platformio.ini` - PlatformIO configuration

## Dependencies

- WebServer_ESP32_SC_W5500 (v1.2.1) - W5500 Ethernet support for ESP32-S3
- ESPmDNS - mDNS responder
- WiFi - WiFi client functionality
- SPI - SPI communication for W5500

## License

This project is provided as-is for use with ESP32-S3-POE-ETH hardware.
