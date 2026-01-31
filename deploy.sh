#!/bin/bash
#
# ESP32 WiFi Bridge - Build and OTA Deploy Script
#
# Builds the firmware and uploads it to the device via HTTP OTA.
# Uses mDNS to automatically discover the device on the network.
#

# Note: We don't use set -e because dns-sd/grep commands may fail normally

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
    echo "  -a, --all            Deploy to ALL eligible devices (no prompt)"
    echo "  -h, --help           Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                   # Build and deploy using mDNS"
    echo "  $0 -i 192.168.1.100  # Build and deploy to specific IP"
    echo "  $0 -b                # Build only"
    echo "  $0 -d -i 10.0.0.50   # Deploy only to specific IP"
    echo "  $0 -a                # Build and deploy to all eligible devices"
}

# Parse arguments
BUILD=true
DEPLOY=true
DEPLOY_ALL=false
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
        -a|--all)
            DEPLOY_ALL=true
            shift
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

# Discover all powerwall devices via mDNS
# Returns lines in format: "IP|HOSTNAME|WIFI_SSID|TARGET|OTA_PORT" (empty fields if TXT records missing)
discover_devices() {
    local devices=""

    # Print status to stderr so it doesn't mix with returned device data
    echo -e "${BLUE}[*]${NC} Scanning for devices via mDNS (3 seconds)..." >&2

    if command -v dns-sd &> /dev/null; then
        # macOS: Use dns-sd to browse for services
        local tmpfile=$(mktemp)
        local tmpfile_lookup=$(mktemp)
        local tmpfile_instances=$(mktemp)

        echo -e "${BLUE}[*]${NC}   Browsing for _powerwall._tcp services..." >&2
        dns-sd -B _powerwall._tcp local > "$tmpfile" 2>&1 &
        local pid=$!
        sleep 3
        kill $pid 2>/dev/null || :
        wait $pid 2>/dev/null || :

        # Extract instance names (last field on lines containing "Add")
        grep "Add" "$tmpfile" 2>/dev/null | awk '{print $NF}' | sort -u > "$tmpfile_instances" || :
        local instance_count=$(wc -l < "$tmpfile_instances" | tr -d ' ')
        echo -e "${BLUE}[*]${NC}   Found ${instance_count} service(s)" >&2

        local lookup_output="" ip="" wifi_ssid="" target="" ota_port=""

        while read -r instance; do
            [[ -z "$instance" ]] && continue

            echo -e "${BLUE}[*]${NC}   Querying ${instance}..." >&2

            # Lookup service to get TXT records
            dns-sd -L "$instance" _powerwall._tcp local > "$tmpfile_lookup" 2>&1 &
            pid=$!
            sleep 2
            kill $pid 2>/dev/null || :
            wait $pid 2>/dev/null || :

            lookup_output=$(cat "$tmpfile_lookup" 2>/dev/null) || :

            # Resolve hostname to IP
            ip=$(ping -c 1 -t 2 "${instance}.local" 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1) || :

            if [[ -n "$ip" ]]; then
                # Extract TXT records (they appear on a line like: " key=value key=value")
                wifi_ssid=$(echo "$lookup_output" | grep -oE 'wifi_ssid=[^ ]+' | cut -d= -f2 | head -1) || :
                target=$(echo "$lookup_output" | grep -oE 'target=[^ ]+' | cut -d= -f2 | head -1) || :
                ota_port=$(echo "$lookup_output" | grep -oE 'ota_port=[^ ]+' | cut -d= -f2 | head -1) || :

                echo -e "${BLUE}[*]${NC}     -> ${ip} (wifi: ${wifi_ssid:-n/a})" >&2
                devices+="${ip}|${instance}|${wifi_ssid}|${target}|${ota_port}"$'\n'
            else
                echo -e "${YELLOW}[!]${NC}     -> Could not resolve IP" >&2
            fi

            # Reset for next iteration
            lookup_output="" ip="" wifi_ssid="" target="" ota_port=""
        done < "$tmpfile_instances"

        rm -f "$tmpfile" "$tmpfile_lookup" "$tmpfile_instances"

        # Fallback: try direct hostname resolution (no TXT records available)
        if [[ -z "$devices" ]]; then
            local ip
            ip=$(ping -c 1 -t 3 "${MDNS_HOSTNAME}.local" 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1) || true
            if [[ -n "$ip" ]]; then
                devices="${ip}|${MDNS_HOSTNAME}|||"$'\n'
            fi
        fi

    elif command -v avahi-browse &> /dev/null; then
        # Linux: Use avahi-browse with TXT records
        local browse_output
        browse_output=$(timeout 5 avahi-browse -rpt _powerwall._tcp 2>/dev/null || true)

        while IFS=';' read -r status iface proto name type domain hostname address port txt; do
            [[ "$status" != "=" ]] && continue
            [[ -z "$address" ]] && continue
            # Skip IPv6
            [[ "$address" == *:* ]] && continue

            local clean_hostname="${hostname%.local}"

            # Extract TXT records from avahi output
            local wifi_ssid="" target="" ota_port=""
            wifi_ssid=$(echo "$txt" | grep -oE '"wifi_ssid=[^"]*"' | sed 's/"wifi_ssid=\(.*\)"/\1/') || true
            target=$(echo "$txt" | grep -oE '"target=[^"]*"' | sed 's/"target=\(.*\)"/\1/') || true
            ota_port=$(echo "$txt" | grep -oE '"ota_port=[^"]*"' | sed 's/"ota_port=\(.*\)"/\1/') || true

            devices+="${address}|${clean_hostname}|${wifi_ssid}|${target}|${ota_port}"$'\n'
        done <<< "$browse_output"

        # Fallback
        if [[ -z "$devices" ]]; then
            local ip
            ip=$(avahi-resolve -4 -n "${MDNS_HOSTNAME}.local" 2>/dev/null | awk '{print $2}')
            if [[ -n "$ip" ]]; then
                devices="${ip}|${MDNS_HOSTNAME}|||"$'\n'
            fi
        fi

    else
        # Last resort: try ping (no TXT records available)
        local ip
        ip=$(ping -c 1 -W 3 "${MDNS_HOSTNAME}.local" 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | head -1) || true
        if [[ -n "$ip" ]]; then
            devices="${ip}|${MDNS_HOSTNAME}|||"$'\n'
        fi
    fi

    # Remove empty lines and duplicates
    echo "$devices" | grep -v '^$' | sort -u
}

# Check if device has required TXT records for OTA
is_device_compatible() {
    local wifi_ssid="$1"
    local target="$2"
    local ota_port="$3"

    [[ -n "$ota_port" ]]
}

# Let user select a device if multiple are found
# Note: All interactive output goes to stderr so stdout can be captured for the IP
select_device() {
    local devices="$1"
    local device_count
    device_count=$(echo "$devices" | wc -l | tr -d ' ')

    if [[ "$device_count" -eq 0 ]] || [[ -z "$devices" ]]; then
        return 1
    elif [[ "$device_count" -eq 1 ]]; then
        # Single device - check compatibility and use it
        local ip hostname wifi_ssid target ota_port
        IFS='|' read -r ip hostname wifi_ssid target ota_port <<< "$devices"

        if is_device_compatible "$wifi_ssid" "$target" "$ota_port"; then
            print_success "Found device: ${hostname}.local (${ip})"
            print_status "  WiFi: ${wifi_ssid}"
            print_status "  Target: ${target}"
            echo "$ip"
            return 0
        else
            print_error "Found device ${hostname}.local (${ip}) but it is incompatible"
            print_error "Missing required TXT record (ota_port)"
            print_error "Update the device firmware first using: pio run -t upload"
            return 1
        fi
    else
        # Multiple devices - let user choose
        # All output to stderr so stdout only contains the selected IP
        echo -e "${BLUE}[*]${NC} Found ${device_count} devices:" >&2
        echo "" >&2

        # Display table header
        printf "      %-15s  %-16s  %-22s  %-15s  %s\n" "IP" "HOSTNAME" "WIFI" "TARGET" "STATUS" >&2
        printf "      %-15s  %-16s  %-22s  %-15s  %s\n" "---------------" "----------------" "----------------------" "---------------" "------------" >&2

        local i=1
        local device_array=()
        local compatible_array=()

        # Display devices
        while IFS='|' read -r ip hostname wifi_ssid target ota_port; do
            [[ -z "$ip" ]] && continue
            device_array+=("$ip")

            local status
            if is_device_compatible "$wifi_ssid" "$target" "$ota_port"; then
                compatible_array+=(1)
                printf "  ${BLUE}%d)${NC}  %-15s  %-16s  %-22s  %-15s  ${GREEN}OK${NC}\n" \
                    "$i" "$ip" "${hostname}.local" "${wifi_ssid}" "${target}" >&2
            else
                compatible_array+=(0)
                printf "  ${YELLOW}%d)${NC}  %-15s  %-16s  %-22s  %-15s  ${RED}incompatible${NC}\n" \
                    "$i" "$ip" "${hostname}.local" "${wifi_ssid:--}" "${target:--}" >&2
            fi

            ((i++))
        done <<< "$devices"

        echo "" >&2
        local max_selection=$((i-1))
        local selection
        while true; do
            # Read from /dev/tty to get user input even when stdout is captured
            echo -n "Select device [1-${max_selection}]: " >&2
            read -r selection < /dev/tty
            if [[ "$selection" =~ ^[0-9]+$ ]] && [[ "$selection" -ge 1 ]] && [[ "$selection" -le "$max_selection" ]]; then
                local idx=$((selection-1))
                if [[ "${compatible_array[$idx]}" -eq 1 ]]; then
                    echo "${device_array[$idx]}"
                    return 0
                else
                    print_error "Device #${selection} is incompatible. Please select a compatible device or update it first."
                fi
            else
                print_error "Invalid selection. Please enter a number between 1 and ${max_selection}"
            fi
        done
    fi
}

# Get list of all compatible device IPs
get_compatible_devices() {
    local devices="$1"
    local compatible_ips=""

    while IFS='|' read -r ip hostname wifi_ssid target ota_port; do
        [[ -z "$ip" ]] && continue
        if is_device_compatible "$wifi_ssid" "$target" "$ota_port"; then
            compatible_ips+="$ip "
        fi
    done <<< "$devices"

    echo "$compatible_ips"
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

    # Upload firmware with progress bar
    local fw_size=$(stat -f%z "$FIRMWARE_PATH" 2>/dev/null || stat -c%s "$FIRMWARE_PATH" 2>/dev/null)
    print_status "Uploading firmware ($(numfmt --to=iec-i --suffix=B $fw_size 2>/dev/null || echo "$fw_size bytes"))..."

    local http_code
    local body
    local tmpfile=$(mktemp)

    # Show progress bar on stderr, capture response body to file, return http code
    # --progress-bar writes to stderr which shows on terminal
    # -o writes response body to file
    # -w writes http code to stdout which we capture
    http_code=$(curl --progress-bar \
        --connect-timeout 10 \
        --max-time 180 \
        -X POST \
        -F "firmware=@${FIRMWARE_PATH}" \
        -o "$tmpfile" \
        -w "%{http_code}" \
        "${url}")

    body=$(cat "$tmpfile" 2>/dev/null)
    rm -f "$tmpfile"

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
            local devices
            devices=$(discover_devices)

            if [[ -z "$devices" ]]; then
                print_error "No devices discovered via mDNS"
                print_error "Make sure the device is running and on the same network"
                print_status "You can specify IP manually with: $0 -i <IP_ADDRESS>"
                exit 1
            fi

            if [[ "$DEPLOY_ALL" == true ]]; then
                # Deploy to all compatible devices
                local compatible_ips
                compatible_ips=$(get_compatible_devices "$devices")

                if [[ -z "$compatible_ips" ]]; then
                    print_error "No compatible devices found"
                    print_error "Devices must have ota_port TXT record"
                    exit 1
                fi

                local ip_array=($compatible_ips)
                local device_count=${#ip_array[@]}
                print_success "Found ${device_count} compatible device(s)"

                local success_count=0
                local fail_count=0

                for ip in "${ip_array[@]}"; do
                    echo ""
                    print_status "Deploying to ${ip}..."
                    if deploy_firmware "$ip"; then
                        ((success_count++))
                    else
                        ((fail_count++))
                        print_error "Failed to deploy to ${ip}"
                    fi
                done

                echo ""
                print_success "Deployment complete: ${success_count} succeeded, ${fail_count} failed"
            else
                DEVICE_IP=$(select_device "$devices")

                if [[ -z "$DEVICE_IP" ]]; then
                    print_error "No device selected"
                    exit 1
                fi
                echo ""
                deploy_firmware "$DEVICE_IP"
            fi
        else
            print_status "Using specified IP: ${DEVICE_IP}"
            echo ""
            deploy_firmware "$DEVICE_IP"
        fi
    fi

    echo ""
    print_success "Done!"
}

main
