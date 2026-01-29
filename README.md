# ESP32-S3-POE-ETH WiFi-Ethernet SSL Bridge

This project uses the ESP32-S3-POE-ETH board (Waveshare) with **ESP-IDF framework** to create a WiFi-Ethernet SSL bridge that:
- Accepts SSL/TLS connections on Ethernet (port 443)
- **Forwards encrypted traffic without decryption** (SSL passthrough)
- Modifies TTL (Time-To-Live) to hide that traffic originates from outside the network
- Forwards to Tesla Powerwall at 192.168.91.1 over WiFi

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
- **SSL Passthrough**: Forwards encrypted SSL/TLS traffic without decryption
- **TTL Modification**: Modifies Time-To-Live on outgoing packets to hide external origin
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional**: Handles encrypted traffic in both directions with 2KB buffers
- **Memory Optimized**: Simple TCP socket forwarding without TLS overhead

## Architecture

```
[Ethernet Client] <=SSL/TLS (Encrypted)=> [ESP32-S3 Bridge] <=SSL/TLS (Encrypted)=> [Powerwall WiFi]
                                           TCP passthrough
                                           TTL modification
```

This implementation provides **SSL passthrough with TTL modification**:
- **No TLS Termination**: Traffic remains encrypted end-to-end
- **TCP Forwarding**: Simple socket-to-socket forwarding of encrypted data
- **TTL Modification**: Sets TTL=64 on outgoing packets to appear as local traffic
- **Transparent Bridge**: Client connects directly to Powerwall through the bridge
- **Lower Overhead**: No encryption/decryption overhead on the ESP32-S3

The ESP32-S3 acts as a transparent SSL bridge, forwarding encrypted traffic while modifying the TTL field to make the traffic appear to originate from within the local network.

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
#define PROXY_TIMEOUT_MS 60000
#define TTL_VALUE 64  // TTL to hide external origin

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
5. All SSL/TLS connections to Ethernet interface are forwarded to 192.168.91.1 over WiFi with TTL modification

## mDNS Discovery

The service can be discovered on the local network as:
- Hostname: `powerwall.local`
- Service: `_powerwall._tcp`
- Port: 443

## Serial Output Example

```
I (328) wifi-eth-bridge: === ESP32-S3-POE-ETH WiFi-Ethernet SSL Bridge ===
I (333) wifi-eth-bridge: Mode: SSL Passthrough (no decryption, TTL modification)
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
I (3288) wifi-eth-bridge: TCP Server (SSL passthrough) listening on port 443
I (3298) wifi-eth-bridge: Ready to forward encrypted SSL/TLS traffic to Powerwall (192.168.91.1:443) with TTL modification
```
atformIO (uses `sdkconfig.defaults`)

## Files

- `src/main.c` - Main application code (ESP-IDF)
- `include/config.h` - Configuration settings including TTL value
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
