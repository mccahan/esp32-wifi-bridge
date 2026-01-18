# ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy

This project uses the ESP32-S3-POE-ETH board (Waveshare) with **ESP-IDF framework** to create a transparent WiFi-Ethernet HTTPS proxy that:
- Accepts HTTPS connections on Ethernet (port 443)
- Forwards encrypted TLS traffic without decryption (transparent TCP proxy)
- Proxies to Tesla Powerwall at 192.168.91.1 over WiFi
- Preserves end-to-end TLS encryption between client and Powerwall

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
- **Transparent TCP Proxy**: Forwards HTTPS traffic on port 443 without TLS termination
- **End-to-End TLS**: TLS encryption preserved between Ethernet client and Powerwall
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional**: Handles traffic in both directions with 1KB buffers
- **Low Memory**: Efficient TCP-only forwarding without mbedTLS overhead

## Architecture

```
[Ethernet Client] <===HTTPS (TLS passthrough)===> [ESP32-S3] <===HTTPS (TLS)===> [Powerwall WiFi]
                                                   TCP Proxy
```

This implementation provides **transparent TCP proxying**:
- **No TLS Termination**: Traffic remains encrypted end-to-end
- **Low Resource Usage**: Simple socket forwarding without cryptographic overhead
- **Ethernet Side**: Accepts TCP connections on port 443
- **WiFi Side**: Forwards to Powerwall at 192.168.91.1:443

The ESP32-S3 acts as a pure TCP relay, forwarding encrypted TLS traffic bidirectionally without inspecting or modifying it. This preserves end-to-end encryption and minimizes memory usage.

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
