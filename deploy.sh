#!/bin/bash
#
# ESP32 WiFi Bridge - Build and OTA Deploy Script
#
# Builds the firmware and uploads it to the device via HTTP OTA.
# Uses mDNS to automatically discover the device on the network.
#

set -e

# Configuration
MDNS_HOSTNAME="powerwall"
OTA_PORT=8080
FIRMWARE_PATH=".pio/build/esp32-s3-devkitc-1/firmware.bin"
TIMEOUT=10

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_status() { echo -e "${BLUE}[*]${NC} $1"; }
print_success() { echo -e "${GREEN}[✓]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[!]${NC} $1"; }
print_error() { echo -e "${RED}[✗]${NC} $1"; }

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -b, --build-only     Only build, don't deploy"
    echo "  -d, --deploy-only    Only deploy (skip build)"
    echo "  -i, --ip ADDRESS     Use specific IP instead of mDNS discovery"
    echo "  -h, --help           Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                   # Build and deploy using mDNS"
    echo "  $0 -i 192.168.1.100  # Build and deploy to specific IP"
    echo "  $0 -b                # Build only"
    echo "  $0 -d -i 10.0.0.50   # Deploy only to specific IP"
}

# Parse arguments
BUILD=true
DEPLOY=true
DEVICE_IP=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--build-only)
            DEPLOY=false
            shift
            ;;
        -d|--deploy-only)
            BUILD=false
            shift
            ;;
        -i|--ip)
            DEVICE_IP="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Resolve mDNS hostname to IP address
resolve_mdns() {
    local hostname="$1"
    local ip=""

    print_status "Discovering device via mDNS (${hostname}.local)..."

    if command -v dns-sd &> /dev/null; then
        # macOS: Use dns-sd with timeout
        ip=$(dns-sd -G v4 "${hostname}.local" 2>/dev/null &
            PID=$!
            sleep 2
            kill $PID 2>/dev/null
            wait $PID 2>/dev/null
        ) || true

        # Parse the output - dns-sd outputs IP on a line like "Hostname.local. 10.0.0.1"
        ip=$(timeout 5 dns-sd -G v4 "${hostname}.local" 2>&1 | grep -E "^\s*${hostname}" | awk '{print $NF}' | head -1) || true

        # Alternative: use ping which resolves mDNS on macOS
        if [[ -z "$ip" ]]; then
            ip=$(ping -c 1 -t 3 "${hostname}.local" 2>/dev/null | head -1 | grep -oE '\d+\.\d+\.\d+\.\d+' | head -1) || true
        fi

    elif command -v avahi-resolve &> /dev/null; then
        # Linux: Use avahi-resolve
        ip=$(avahi-resolve -4 -n "${hostname}.local" 2>/dev/null | awk '{print $2}')

    elif command -v getent &> /dev/null; then
        # Linux fallback: getent with mdns
        ip=$(getent ahostsv4 "${hostname}.local" 2>/dev/null | head -1 | awk '{print $1}')

    else
        # Last resort: try ping
        ip=$(ping -c 1 -W 3 "${hostname}.local" 2>/dev/null | head -1 | grep -oE '\d+\.\d+\.\d+\.\d+' | head -1) || true
    fi

    echo "$ip"
}

# Build firmware
build_firmware() {
    print_status "Building firmware..."

    if ! command -v pio &> /dev/null; then
        print_error "PlatformIO not found. Install with: pip install platformio"
        exit 1
    fi

    if pio run; then
        print_success "Build completed successfully"

        # Show firmware size
        if [[ -f "$FIRMWARE_PATH" ]]; then
            local size=$(stat -f%z "$FIRMWARE_PATH" 2>/dev/null || stat -c%s "$FIRMWARE_PATH" 2>/dev/null)
            print_status "Firmware size: $(numfmt --to=iec-i --suffix=B $size 2>/dev/null || echo "$size bytes")"
        fi
    else
        print_error "Build failed"
        exit 1
    fi
}

# Deploy firmware via OTA
deploy_firmware() {
    local ip="$1"

    if [[ ! -f "$FIRMWARE_PATH" ]]; then
        print_error "Firmware not found at $FIRMWARE_PATH"
        print_error "Run with -b or without -d to build first"
        exit 1
    fi

    local url="http://${ip}:${OTA_PORT}/ota/upload"
    print_status "Uploading firmware to ${url}..."

    # Check if device is reachable
    if ! curl -s --connect-timeout 5 "http://${ip}:${OTA_PORT}/" > /dev/null; then
        print_error "Cannot connect to device at ${ip}:${OTA_PORT}"
        print_error "Make sure the device is powered on and connected to the network"
        exit 1
    fi

    # Get current firmware version
    print_status "Current device status:"
    curl -s "http://${ip}:${OTA_PORT}/" | grep -oE 'Version:</td><td[^>]*>[^<]+' | sed 's/.*>/  Version: /' || true

    # Upload firmware
    print_status "Uploading firmware (this may take a moment)..."

    local response
    response=$(curl -s -w "\n%{http_code}" \
        --connect-timeout 10 \
        --max-time 120 \
        -X POST \
        -F "firmware=@${FIRMWARE_PATH}" \
        "${url}" 2>&1)

    local http_code=$(echo "$response" | tail -1)
    local body=$(echo "$response" | sed '$d')

    if [[ "$http_code" == "200" ]] || echo "$body" | grep -qi "success"; then
        print_success "Firmware uploaded successfully!"
        print_status "Device is rebooting with new firmware..."
        print_warning "Wait ~10 seconds for device to restart"

        # Wait for device to come back online
        echo -n "Waiting for device"
        for i in {1..20}; do
            sleep 1
            echo -n "."
            if curl -s --connect-timeout 2 "http://${ip}:${OTA_PORT}/" > /dev/null 2>&1; then
                echo ""
                print_success "Device is back online!"

                # Show new version
                print_status "New device status:"
                curl -s "http://${ip}:${OTA_PORT}/" | grep -oE 'Version:</td><td[^>]*>[^<]+' | sed 's/.*>/  Version: /' || true
                return 0
            fi
        done
        echo ""
        print_warning "Device hasn't responded yet. It may still be booting."
        print_status "Try: curl http://${ip}:${OTA_PORT}/"
    else
        print_error "Upload failed (HTTP $http_code)"
        echo "$body" | head -5
        exit 1
    fi
}

# Main
main() {
    echo "========================================"
    echo "  ESP32 WiFi Bridge - Deploy Script"
    echo "========================================"
    echo ""

    # Build if requested
    if [[ "$BUILD" == true ]]; then
        build_firmware
        echo ""
    fi

    # Deploy if requested
    if [[ "$DEPLOY" == true ]]; then
        # Get device IP
        if [[ -z "$DEVICE_IP" ]]; then
            DEVICE_IP=$(resolve_mdns "$MDNS_HOSTNAME")

            if [[ -z "$DEVICE_IP" ]]; then
                print_error "Could not discover device via mDNS"
                print_error "Make sure the device is running and on the same network"
                print_status "You can specify IP manually with: $0 -i <IP_ADDRESS>"
                exit 1
            fi

            print_success "Found device at ${DEVICE_IP}"
        else
            print_status "Using specified IP: ${DEVICE_IP}"
        fi

        echo ""
        deploy_firmware "$DEVICE_IP"
    fi

    echo ""
    print_success "Done!"
}

main
