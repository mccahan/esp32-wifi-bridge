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
- **Ethernet Server**: HTTPS server on port 443 with self-signed certificate
- **DHCP**: Both WiFi and Ethernet interfaces use DHCP
- **mDNS**: Advertises "_powerwall" service on Ethernet interface
- **Bidirectional Proxy**: Forwards HTTPS traffic between Ethernet and WiFi

## Building

This project uses PlatformIO:

```bash
pio run
```

## Uploading

```bash
pio run --target upload
```

## Monitoring

```bash
pio device monitor
```

## Configuration

Edit `src/main.cpp` to configure WiFi credentials:

```cpp
const char* wifi_ssid = "TeslaPowerwall";
const char* wifi_password = "";
```

## Usage

1. The device will connect to the Tesla Powerwall WiFi network
2. Ethernet interface will obtain IP via DHCP
3. HTTPS server starts on port 443 on Ethernet interface
4. mDNS service advertises as "powerwall.local" with "_powerwall._tcp" service
5. All HTTPS requests to Ethernet interface are proxied to 192.168.91.1 over WiFi

## mDNS Discovery

The service can be discovered on the local network as:
- Hostname: `powerwall.local`
- Service: `_powerwall._tcp`
- Port: 443
