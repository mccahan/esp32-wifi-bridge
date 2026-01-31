# ESP32 WiFi Bridge - Development Notes

## Project Overview

ESP32-S3 WiFi-Ethernet SSL bridge that forwards encrypted traffic from Ethernet to a Tesla Powerwall over WiFi. Uses SSL passthrough (no decryption) with TTL modification to appear as local traffic.

## Build Commands

```bash
pio run                    # Build firmware
pio run -t upload          # Upload via USB
./deploy.sh                # Build and deploy via OTA (mDNS discovery)
./deploy.sh -a             # Deploy to ALL eligible devices
./deploy.sh -d -i <IP>     # Deploy only to specific IP
./deploy.sh -b             # Build only
```

## Key Files

- `src/main.c` - Main application (proxy, OTA server, WiFi config)
- `include/config.h` - Configuration constants (WiFi defaults, pins, ports)
- `deploy.sh` - Build and OTA deployment script with mDNS discovery
- `partitions.csv` - OTA partition layout (ota_0, ota_1)
- `platformio.ini` - PlatformIO config (pinned to espressif32@6.9.0)

## Features

### OTA Updates
- HTTP server on port 8080 (Ethernet interface)
- Web UI for firmware upload at `http://<eth-ip>:8080/`
- Automatic rollback if firmware crashes before validation
- OTA server starts before WiFi connects (allows recovery from bad WiFi config)

### WiFi Configuration (Web UI)
- Credentials stored in NVS (persist across reboots)
- Scan for available networks from web UI
- Change WiFi settings without reflashing
- Falls back to compiled defaults if NVS empty

### mDNS Service
- Hostname: `powerwall.local`
- Service: `_powerwall._tcp`
- TXT records: `wifi_ssid`, `target`, `ota_port`

### Deploy Script Features
- mDNS device discovery (dns-sd on macOS, avahi on Linux)
- Multi-device selection menu
- `-a/--all` flag to deploy to all eligible devices
- Progress bar during firmware upload
- Device compatibility check (requires `ota_port` TXT record)

## Architecture

```
[Ethernet Client] <==SSL==> [ESP32 Bridge] <==SSL==> [Powerwall WiFi]
                            Port 443 proxy
                            Port 8080 OTA/Config UI
```

## Hardware

- Board: ESP32-S3-POE-ETH (Waveshare)
- Ethernet: W5500 via SPI
- Pins: MISO=12, MOSI=11, SCLK=13, CS=14, INT=10

## Configuration Defaults (config.h)

- WiFi: Stored in NVS, defaults in config.h
- Powerwall IP: 192.168.91.1
- Proxy port: 443
- OTA port: 8080
- TTL: 64 (hides external origin)
- Buffer size: 4096 bytes
- Max concurrent clients: 4

## Notes

- Ethernet MAC derived from WiFi MAC (locally administered bit set)
- WiFi scan requires temporary disconnect (ESP32 limitation)
- OTA validation happens after Ethernet IP obtained (prevents rollback during WiFi config)
- Platform pinned to espressif32@6.9.0 to avoid toolchain issues
