# ESP32-S3-POE-ETH WiFi-Ethernet HTTPS Proxy

This project uses the ESP32-S3-POE-ETH board (Waveshare) with **ESP-IDF framework** to create a WiFi-Ethernet HTTPS proxy with TLS termination that:
- Accepts HTTPS connections on Ethernet (port 443) with self-signed certificate
- Decrypts incoming TLS traffic using esp_tls (high-level mbedTLS wrapper)
- Re-encrypts and forwards HTTP traffic to Tesla Powerwall at 192.168.91.1 over WiFi
- Handles SNI/hostname requirements of the Powerwall server

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
- **TLS Termination Proxy**: Decrypts HTTPS on Ethernet side using self-signed certificate
- **HTTPS Client**: Re-encrypts and forwards to Powerwall with proper hostname handling
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional**: Handles decrypted HTTP traffic in both directions with 2KB buffers
- **Memory Optimized**: Uses esp_tls with dynamic buffers and asymmetric content lengths
- **WebSerial**: Web-based serial monitor for viewing logs in real-time via browser
- **OTA Updates**: Over-the-air firmware updates via web interface

## Architecture

```
[Ethernet Client] <=HTTPS (TLS)=> [ESP32-S3 Proxy] <=HTTPS (TLS)=> [Powerwall WiFi]
                                  decrypt | re-encrypt
                                     HTTP proxy
```

This implementation provides **TLS termination with HTTP proxying**:
- **TLS Termination**: Decrypts client HTTPS using self-signed certificate
- **HTTP Proxy**: Forwards decrypted HTTP requests
- **Re-encryption**: Establishes separate TLS connection to Powerwall
- **Hostname Handling**: Properly handles SNI requirements for Powerwall server
- **Ethernet Side**: esp_tls server with self-signed cert (cert.h)
- **WiFi Side**: esp_tls client to Powerwall (skips cert verification)

The ESP32-S3 acts as a man-in-the-middle HTTPS proxy, allowing inspection and modification of HTTP traffic while maintaining separate TLS sessions on both sides. This is required because the Powerwall server validates the hostname in the request.

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

// Web Server Settings
#define WEB_SERVER_PORT 80

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

## WebSerial - Web-Based Serial Monitor

Access the WebSerial interface to view real-time serial logs from your browser:

1. Connect to the same network as the ESP32 (Ethernet network)
2. Open a web browser and navigate to:
   - `http://<device-ip>/` (find IP in serial output or use `powerwall.local` if mDNS is working)
   - Default port: 80
3. The WebSerial page will automatically connect and display logs in real-time
4. Features:
   - Real-time log streaming via Server-Sent Events (SSE)
   - Auto-reconnect on disconnect
   - Clear console button
   - Download logs to file

### WebSerial Interface

The web interface provides:
- **Live Console**: Real-time serial output with auto-scroll
- **Connection Status**: Shows SSE connection state
- **Controls**: Connect, Clear, and Download logs buttons
- **Dark Theme**: Easy-to-read console with green text on black background

## OTA Firmware Updates

Update firmware wirelessly without connecting USB cable:

1. Build your firmware binary:
   ```bash
   pio run
   # or
   idf.py build
   ```

2. Access the WebSerial interface at `http://<device-ip>/`

3. Scroll to the "OTA Firmware Update" section

4. Click "Choose File" and select your firmware binary:
   - PlatformIO: `.pio/build/esp32-s3-devkitc-1/firmware.bin`
   - ESP-IDF: `build/main.bin`

5. Click "Upload Firmware"

6. Wait for upload to complete (progress bar shows status)

7. Device will automatically reboot with new firmware

### OTA Partition Scheme

The device uses a dual-partition OTA scheme:
- `ota_0`: 1MB - First OTA partition
- `ota_1`: 1MB - Second OTA partition
- `otadata`: Stores which partition to boot from

Updates alternate between partitions, allowing rollback to previous firmware if needed.

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
- `src/webserver.c` - HTTP server with WebSerial and OTA support
- `include/config.h` - Configuration settings
- `include/webserver.h` - Web server header
- `include/cert.h` - Self-signed certificate (for reference)
- `platformio.ini` - PlatformIO configuration (ESP-IDF framework)
- `CMakeLists.txt` - ESP-IDF build configuration
- `sdkconfig.defaults` - ESP-IDF default configuration
- `partitions.csv` - OTA partition table (dual partition scheme)

## Dependencies

Uses ESP-IDF components:
- `esp_eth` - Ethernet driver with W5500 support
- `esp_wifi` - WiFi client functionality  
- `esp_netif` - Network interface abstraction
- `esp_http_server` - HTTP/SSE server for WebSerial and OTA
- `app_update` - OTA update functionality
- `mdns` - mDNS responder
- `lwip` - TCP/IP stack
- `nvs_flash` - Non-volatile storage

## License

This project is provided as-is for use with ESP32-S3-POE-ETH hardware.
