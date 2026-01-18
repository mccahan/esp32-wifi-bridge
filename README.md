# ESP32-S3-POE-ETH WiFi-Ethernet Bridge

A WiFi to Ethernet bridge implementation for the ESP32-S3-ETH (Waveshare) board that proxies requests from Ethernet clients to a target IP (192.168.91.1) via WiFi, with HTTPS proxy support.

## Features

- **Transparent Bridging**: Routes traffic from Ethernet clients through ESP32's WiFi connection
- **NAPT/NAT Forwarding**: Automatic Network Address Port Translation for seamless packet forwarding
- **HTTPS Proxy**: Dedicated HTTPS proxy server on port 443 forwarding to https://192.168.91.1
- **Target IP Routing**: Specifically configured to route to 192.168.91.1 (e.g., Tesla Powerwall AP)
- **Hardware Support**: Optimized for ESP32-S3-ETH (Waveshare) with W5500 SPI Ethernet
- **Web Interface**: Built-in HTTP server for viewing logs and monitoring status from Ethernet (port 80)
- **Self-Signed SSL**: Embedded self-signed certificate for HTTPS proxy
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
- **Real-time Status**: WiFi and Ethernet connection status, HTTPS proxy status
- **Network Information**: IP addresses, MAC addresses, gateway info
- **Device Logs**: Last 50 log entries with timestamps
- **Auto-refresh**: Optional auto-refresh every 3 seconds

Endpoints:
- `/` - Main web interface with logs and status
- `/logs` - Plain text logs (useful for debugging)
- `/status` - JSON status API (includes `https_proxy_running` field)

Example: If the ESP32 gets Ethernet IP `192.168.1.100`, access the web interface at `http://192.168.1.100`

**HTTPS Proxy (Port 443):**

The ESP32 also runs an HTTPS proxy server that forwards requests to `https://192.168.91.1`. This is useful for:
- Accessing HTTPS services on the WiFi network through Ethernet
- Proxying Tesla Powerwall HTTPS interface
- Bypassing SSL certificate issues on client devices

Example: Connect to `https://192.168.1.100` and traffic will be proxied to `https://192.168.91.1`

**Note**: The HTTPS proxy uses a self-signed SSL certificate. Your browser will show a security warning - this is expected. Accept the certificate to proceed.

## How It Works

1. **WiFi Connection**: ESP32 connects to the specified WiFi network as a station
2. **Ethernet Interface**: Connects to Ethernet and obtains IP via DHCP
3. **NAPT Configuration**: When both interfaces are ready, NAPT (Network Address Port Translation) is automatically enabled
4. **Packet Forwarding**: Traffic from Ethernet is translated and forwarded through WiFi interface
5. **HTTP Web Server**: Provides HTTP interface on Ethernet port 80 for monitoring and logs
6. **HTTPS Proxy Server**: Provides HTTPS proxy on Ethernet port 443, forwarding to https://192.168.91.1
7. **Traffic Routing**: All Ethernet traffic flows through WiFi NAT, enabling access to the WiFi network and target IP

### HTTPS Proxy Details

The HTTPS proxy:
- Listens on port 443 (HTTPS)
- Uses a self-signed SSL certificate for incoming connections
- Forwards decrypted traffic to `https://192.168.91.1` over WiFi
- Accepts self-signed certificates from the target server
- Handles bidirectional data transfer with 2KB buffer
- Timeout: 30 seconds for idle connections

## Use Case: Tesla Powerwall

This bridge is designed to connect to a Tesla Powerwall's WiFi AP and expose its interface via Ethernet:

1. ESP32 connects to Powerwall's WiFi (TEG-XXXX)
2. Powerwall gateway is typically at 192.168.91.1
3. Ethernet clients can access the Powerwall through the ESP32 bridge
4. **HTTPS Access**: Use `https://<ethernet-ip>` to access the Powerwall's HTTPS interface securely

The HTTPS proxy is particularly useful for the Tesla Powerwall as it:
- Forwards HTTPS requests to the Powerwall gateway
- Handles SSL/TLS encryption for Ethernet clients
- Accepts the Powerwall's self-signed certificate
- Provides seamless access to the Powerwall web interface

## Architecture

```
[Ethernet Client] <-HTTP(80)-> [ESP32-S3-ETH] <-WiFi-> [Target Network]
                                  Monitoring
                  
[Ethernet Client] <-HTTPS(443)-> [ESP32-S3-ETH] <-HTTPS-> [192.168.91.1:443]
                                  HTTPS Proxy              (Tesla Powerwall)
                  
[Ethernet Network] <-NAPT/NAT-> [ESP32-S3-ETH] <-WiFi-> [192.168.91.x]
      (DHCP)                      All Traffic              (Other Devices)
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
- Ensure NAPT is enabled (check serial logs)

### HTTPS Proxy Issues
- Verify WiFi is connected (proxy requires WiFi to forward requests)
- Check that HTTPS proxy shows "Running" in status (web interface or serial)
- Accept the self-signed certificate warning in your browser
- Verify target IP (192.168.91.1) is accessible from WiFi network
- Check serial logs for "HTTPS proxy: New client connection" messages

## License

This project is provided as-is for use with ESP32-S3-ETH hardware.
