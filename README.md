# ESP32-S3-POE-ETH WiFi-Ethernet Bridge

A WiFi to Ethernet bridge implementation for the ESP32-S3-ETH (Waveshare) board that proxies requests from Ethernet clients to a target IP (192.168.91.1) via WiFi.

## Features

- **Transparent Bridging**: Routes traffic from Ethernet clients through ESP32's WiFi connection
- **Automatic Packet Forwarding**: ESP32-S3 handles forwarding between interfaces automatically
- **Target IP Routing**: Specifically configured to route to 192.168.91.1 (e.g., Tesla Powerwall AP)
- **Hardware Support**: Optimized for ESP32-S3-ETH (Waveshare) with LAN8720A PHY
- **Web Interface**: Built-in web server for viewing logs and monitoring status from Ethernet
- **PlatformIO Build**: Easy building and flashing with PlatformIO
- **GitHub Actions CI**: Automated builds on push

## Hardware Requirements

- **Board**: [ESP32-S3-ETH](https://www.waveshare.com/wiki/ESP32-S3-ETH) (Waveshare)
- **Ethernet PHY**: LAN8720A (integrated on board)
- **Features**:
  - Power over Ethernet (PoE) support
  - WiFi 802.11 b/g/n
  - 10/100 Mbps Ethernet

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- ESP32-S3-ETH board
- WiFi credentials for the target network

### Configuration

**Option 1: Environment Variables (Recommended)**

Set environment variables before building:

```bash
export WIFI_SSID="YourNetworkSSID"
export WIFI_PASSWORD="YourNetworkPassword"
export TARGET_IP="192.168.91.1"

# Build the project
pio run
```

**Option 2: Edit platformio.ini**

Edit the build flags in `platformio.ini`, but be careful not to commit credentials:

```ini
build_flags = 
    -DWIFI_SSID=\"YourNetworkSSID\"
    -DWIFI_PASSWORD=\"YourNetworkPassword\"
    -DTARGET_IP=\"192.168.91.1\"
```

**Note:** The platformio.ini file uses environment variables by default. If an environment variable is not set, it will use the default value shown.

### Building

```bash
# Build the project
pio run

# Build and upload to ESP32
pio run --target upload

# Monitor serial output
pio device monitor
```

### Network Configuration

The ESP32 will:
1. Connect to your WiFi network as a client
2. Connect to the Ethernet network and obtain an IP via DHCP
3. Bridge traffic between Ethernet and WiFi interfaces
4. Forward all traffic through WiFi to the target network
5. Provide a web interface on the Ethernet IP for monitoring

The Ethernet interface will automatically obtain its IP address from the network's DHCP server.

### Web Interface

Once the ESP32 obtains an Ethernet IP address via DHCP, you can access the web interface by opening a browser and navigating to the Ethernet IP address (displayed in serial output).

The web interface provides:
- **Real-time Status**: WiFi and Ethernet connection status
- **Network Information**: IP addresses, MAC addresses, gateway info
- **Device Logs**: Last 100 log entries with timestamps
- **Auto-refresh**: Optional auto-refresh every 3 seconds

Endpoints:
- `/` - Main web interface with logs and status
- `/logs` - Plain text logs (useful for debugging)
- `/status` - JSON status API

Example: If the ESP32 gets Ethernet IP `192.168.1.100`, access the web interface at `http://192.168.1.100`

## How It Works

1. **WiFi Connection**: ESP32 connects to the specified WiFi network as a station
2. **Ethernet Interface**: Connects to Ethernet and obtains IP via DHCP
3. **Automatic Bridging**: ESP32-S3 automatically forwards packets between Ethernet and WiFi interfaces
4. **Web Server**: Provides HTTP interface on Ethernet for monitoring and logs
5. **Traffic Routing**: All traffic flows between interfaces, enabling Ethernet devices to access the WiFi network and target IP

## Use Case: Tesla Powerwall

This bridge is designed to connect to a Tesla Powerwall's WiFi AP and expose its interface via Ethernet:

1. ESP32 connects to Powerwall's WiFi (TEG-XXXX)
2. Powerwall gateway is typically at 192.168.91.1
3. Ethernet clients can access the Powerwall through the ESP32 bridge

## Architecture

```
[Ethernet Network] <--> [ESP32-S3-ETH] <--> [WiFi Network] <--> [Target: 192.168.91.1]
     (DHCP)           (Auto Forwarding)
```

## Pin Configuration (ESP32-S3-ETH)

The code is pre-configured for the Waveshare ESP32-S3-ETH board:

- **ETH PHY Type**: LAN8720
- **PHY Address**: 1
- **MDC**: GPIO 23
- **MDIO**: GPIO 18
- **Clock Mode**: GPIO0 (external clock input)

## Building with GitHub Actions

The repository includes a GitHub Actions workflow that:
- Builds the project automatically on push
- Caches dependencies for faster builds
- Generates firmware artifacts

## Reference

This C++ implementation is inspired by the Rust-based [esp32-wifi-bridge](https://github.com/owenthewizard/esp32-wifi-bridge) project.

## Troubleshooting

### WiFi Not Connecting
- Verify SSID and password in `platformio.ini`
- Check WiFi signal strength
- Ensure the WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)

### Ethernet Not Working
- Check Ethernet cable connection
- Verify DHCP server is available on the Ethernet network
- Verify PHY power and clock configuration
- Check serial output for initialization errors

### No Traffic Forwarding
- Verify both WiFi and Ethernet are connected (see serial output)
- Check that both interfaces have obtained IP addresses
- Ensure NAPT is enabled (check serial logs)

## License

This project is provided as-is for use with ESP32-S3-ETH hardware.
