# ESP32-S3-POE-ETH WiFi-Ethernet Bridge

A WiFi to Ethernet bridge implementation for the ESP32-S3-ETH (Waveshare) board that routes traffic from Ethernet clients to a target IP (192.168.91.1) via WiFi using NAPT.

## Features

- **Transparent Bridging**: Routes traffic from Ethernet clients through ESP32's WiFi connection
- **NAPT/NAT Forwarding**: Automatic Network Address Port Translation for seamless packet forwarding at layer 3
- **Target IP Routing**: Specifically configured to route to 192.168.91.1 (e.g., Tesla Powerwall AP)
- **Hardware Support**: Optimized for ESP32-S3-ETH (Waveshare) with W5500 SPI Ethernet
- **Web Interface**: Built-in HTTP server for viewing logs and monitoring status from Ethernet (port 80)
- **PlatformIO Build**: Easy building and flashing with PlatformIO
- **GitHub Actions CI**: Automated builds on push

## Hardware Requirements

- **Board**: [ESP32-S3-ETH](https://www.waveshare.com/wiki/ESP32-S3-ETH) (Waveshare)
- **Ethernet PHY**: W5500 (SPI-based, integrated on board)
- **Features**:
  - Power over Ethernet (PoE) support (with optional PoE module)
  - WiFi 802.11 b/g/n
  - 10/100 Mbps Ethernet via W5500 chip

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

**HTTP Monitoring Interface (Port 80):**
- **Real-time Status**: WiFi and Ethernet connection status, NAPT forwarding status
- **Network Information**: IP addresses, MAC addresses, gateway info
- **Device Logs**: Last 50 log entries with timestamps
- **Auto-refresh**: Optional auto-refresh every 3 seconds

Endpoints:
- `/` - Main web interface with logs and status
- `/logs` - Plain text logs (useful for debugging)
- `/status` - JSON status API

Example: If the ESP32 gets Ethernet IP `192.168.1.100`, access the web interface at `http://192.168.1.100`

## How It Works

1. **WiFi Connection**: ESP32 connects to the specified WiFi network as a station
2. **Ethernet Interface**: Connects to Ethernet and obtains IP via DHCP
3. **NAPT Configuration**: When both interfaces are ready, NAPT (Network Address Port Translation) is automatically enabled
4. **Packet Forwarding**: Traffic from Ethernet is translated and forwarded through WiFi interface using NAPT
5. **HTTP Web Server**: Provides HTTP interface on Ethernet port 80 for monitoring and logs
6. **Traffic Routing**: All Ethernet traffic (including HTTPS) flows through WiFi NAT, enabling transparent access to the WiFi network and target IP (192.168.91.1)

## Use Case: Tesla Powerwall

This bridge is designed to connect to a Tesla Powerwall's WiFi AP and expose its interface via Ethernet:

1. ESP32 connects to Powerwall's WiFi (TEG-XXXX)
2. Powerwall gateway is typically at 192.168.91.1
3. Ethernet clients can access the Powerwall through the ESP32 bridge via NAPT
4. **Direct Access**: Ethernet clients can access `https://192.168.91.1` directly (NAPT forwards all traffic transparently)

Benefits for Tesla Powerwall:
- Transparent layer 3 routing - all protocols supported (HTTP, HTTPS, SSH, etc.)
- No proxy configuration needed on client devices
- Handles Powerwall's self-signed certificate natively (client browser validates it directly)
- Seamless access to the Powerwall web interface at its actual IP address

## Architecture

```
[Ethernet Client] <-HTTP(80)-> [ESP32-S3-ETH]
                                   Web Monitoring UI
                  
[Ethernet Network] <-NAPT/NAT-> [ESP32-S3-ETH] <-WiFi-> [192.168.91.x]
      (DHCP)                      All Traffic              (Target Network including Powerwall)
                                                          
Client accesses https://192.168.91.1 → NAPT forwards → Powerwall at 192.168.91.1
```

## Pin Configuration (ESP32-S3-ETH)

The code configures the W5500 Ethernet chip connected via SPI on the ESP32-S3-ETH board:

- **Ethernet Chip**: W5500 (SPI-based, not RMII)
- **SPI Bus**: SPI2_HOST
- **MISO**: GPIO 12
- **MOSI**: GPIO 11
- **SCLK**: GPIO 13
- **CS**: GPIO 14
- **RST**: GPIO 9
- **INT**: GPIO 10
- **SPI Clock**: 20 MHz

These pins are defined in the source code for the Waveshare ESP32-S3-ETH board.

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
- Ensure NAPT is enabled (check serial logs for "NAPT enabled on WiFi interface")
- Set the ESP32's Ethernet IP as the default gateway on your client device
- Verify target IP (192.168.91.1) is accessible from WiFi network

### Accessing HTTPS Services (e.g., Tesla Powerwall)
- With NAPT enabled, you can access https://192.168.91.1 directly from Ethernet clients
- NAPT forwards all traffic transparently (HTTP, HTTPS, SSH, etc.)
- No special proxy configuration needed - it works at the network layer
- Ensure your client device uses the ESP32's Ethernet IP as its gateway

## License

This project is provided as-is for use with ESP32-S3-ETH hardware.
