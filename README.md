# ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy

This project uses the ESP32-S3-POE-ETH board (Waveshare) with **ESP-IDF framework** to create a WiFi-Ethernet bridge with **full TLS termination** that:
- Accepts HTTPS connections on Ethernet (port 443) using a self-signed certificate
- Decrypts incoming TLS traffic
- Re-encrypts and proxies to Tesla Powerwall at 192.168.91.1 over WiFi

## Hardware

- **Board**: ESP32-S3-POE-ETH (Waveshare)
- **Ethernet Controller**: W5500 (SPI)
- **Framework**: ESP-IDF (native, not Arduino)

### Pin Configuration

| Function | GPIO |
|----------|------|
| MISO     | 12   |
| MOSI     | 11   |
| SCLK     | 13   |
| CS       | 14   |
| INT      | 10   |

## Features

- **WiFi Client**: Connects to Tesla Powerwall AP (192.168.91.1)
- **HTTPS Server**: Full TLS termination on Ethernet (port 443)
- **Self-signed Certificate**: Decrypts client traffic using provided cert/key
- **TLS Proxy**: Re-encrypts and forwards to Powerwall HTTPS endpoint
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional**: Handles HTTPS traffic in both directions
- **ESP-IDF Native**: Uses mbedTLS for TLS termination

## TLS/HTTPS Implementation

This implementation provides **full TLS termination**:
- **Ethernet Side**: Acts as HTTPS server with self-signed certificate
- **Decryption**: Uses mbedTLS to decrypt incoming client requests
- **Re-encryption**: Establishes separate TLS connection to Powerwall
- **WiFi Side**: Acts as HTTPS client to Powerwall (skips cert verification)

The ESP32-S3 terminates the TLS connection from Ethernet clients, inspects/proxies the decrypted HTTP traffic, then re-establishes an encrypted connection to the Powerwall.

## Configuration

Edit `include/config.h` to customize:

```c
// WiFi Settings
#define WIFI_SSID "TeslaPowerwall"
#define WIFI_PASSWORD ""

// Powerwall IP
#define POWERWALL_IP_STR "192.168.91.1"

// Ethernet MAC Address
#define ETH_MAC_ADDR { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED }

// W5500 SPI Pins (ESP32-S3-POE-ETH defaults)
#define W5500_MOSI_GPIO 11
#define W5500_MISO_GPIO 12  
#define W5500_SCK_GPIO  13
#define W5500_CS_GPIO   14
#define W5500_INT_GPIO  10

// Proxy Settings
#define PROXY_PORT 443
#define PROXY_TIMEOUT_MS 30000

// mDNS Settings
#define MDNS_HOSTNAME "powerwall"
#define MDNS_SERVICE "_powerwall"
#define MDNS_PROTOCOL "_tcp"
```

## Building with PlatformIO

This project uses PlatformIO with ESP-IDF framework:

```bash
pio run
```

## Building with ESP-IDF

Alternatively, you can use ESP-IDF directly:

```bash
idf.py build
```

## Uploading

```bash
pio run --target upload
```

Or with ESP-IDF:

```bash
idf.py flash
```

## Monitoring

```bash
pio device monitor
```

Or with ESP-IDF:

```bash
idf.py monitor
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
I (328) wifi-eth-bridge: === ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy ===
I (338) wifi-eth-bridge: Target: Tesla Powerwall at 192.168.91.1:443
I (348) wifi-eth-bridge: Initializing Ethernet W5500...
I (358) wifi-eth-bridge: Ethernet initialized - waiting for connection...
I (368) wifi-eth-bridge: Initializing WiFi...
I (378) wifi-eth-bridge: WiFi initialized - connecting to TeslaPowerwall
I (888) wifi-eth-bridge: WiFi got IP:192.168.91.2
I (1258) wifi-eth-bridge: Ethernet Link Up
I (1258) wifi-eth-bridge: HW Addr de:ad:be:ef:fe:ed
I (3268) wifi-eth-bridge: Ethernet Got IP Address
I (3268) wifi-eth-bridge: ETHIP:192.168.1.100
I (3268) wifi-eth-bridge: mDNS hostname set to: powerwall
I (3278) wifi-eth-bridge: mDNS service added: _powerwall._tcp on port 443
I (3288) wifi-eth-bridge: TCP Server listening on port 443
I (3298) wifi-eth-bridge: Ready to proxy traffic from Ethernet to WiFi (192.168.91.1:443)
```

## Migration from Arduino

This project was migrated from Arduino framework to ESP-IDF to resolve W5500 library compatibility issues. ESP-IDF provides native, fully-supported W5500 drivers through the `esp_eth` component.

### Key Changes:
- **Framework**: Arduino → ESP-IDF
- **Language**: C++ → C
- **Build System**: Arduino libraries → ESP-IDF components
- **W5500 Driver**: Third-party libraries → Native `esp_eth` driver
- **Configuration**: No `sdkconfig` needed for PlatformIO (uses `sdkconfig.defaults`)

## Files

- `src/main.c` - Main application code (ESP-IDF)
- `include/config.h` - Configuration settings
- `include/cert.h` - Self-signed certificate (for reference)
- `platformio.ini` - PlatformIO configuration (ESP-IDF framework)
- `CMakeLists.txt` - ESP-IDF build configuration
- `sdkconfig.defaults` - ESP-IDF default configuration
- `partitions.csv` - Partition table

## Dependencies

Uses ESP-IDF components:
- `esp_eth` - Ethernet driver with W5500 support
- `esp_wifi` - WiFi client functionality  
- `esp_netif` - Network interface abstraction
- `mdns` - mDNS responder
- `lwip` - TCP/IP stack
- `nvs_flash` - Non-volatile storage

## License

This project is provided as-is for use with ESP32-S3-POE-ETH hardware.
