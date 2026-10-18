# CLAUDE.md

Guidance for working in this repository.

## Overview

ESP32-S3 WiFi–Ethernet SSL bridge. Forwards encrypted traffic from Ethernet to a Tesla Powerwall Wi-Fi AP. SSL passthrough (no decryption), TTL 64. Dashboard and proxy bind the Ethernet IP only.

## Build

```bash
pio run                    # build
pio run -t upload          # USB
pio device monitor         # serial
./deploy.sh                # OTA via mDNS
./deploy.sh -a             # all discovered devices
./deploy.sh -d -i <IP>     # one IP
```

Tagged `v*` releases publish firmware to GitHub Pages. OTA URL is derived from the git remote. `include/config.local.h` is optional.

## Architecture

```
[LAN / HAProxy] --TLS :443--> [ESP32-S3-POE-ETH] --Wi-Fi TLS--> [Powerwall 192.168.91.1]
                --HTTP :80-->  dashboard (session cookie)
```

- Ethernet (W5500 SPI): LAN, internet (NTP/OTA)
- Wi-Fi STA: Powerwall AP only (no internet)
- Proxy: TCP passthrough :443, no TLS termination
- HTTP :80: HTML login (`admin`), `HttpOnly; SameSite=Lax` cookie; `Secure` if `X-Forwarded-Proto: https`. `/health` is unauthenticated.

## Source

| File | Role |
|------|------|
| `src/main.c` | netif, HTTP, auth, watchdog, BOOT recovery, log ring |
| `src/proxy.c` | passthrough, buffer pool, request log |
| `src/remote_ota.c` | GitHub OTA, Ethernet DNS snapshot |
| `src/wifi_metrics.c` | NTP + RSSI history |
| `include/config.h` | compile-time constants |
| `include/web_ui.h` | CSS/JS/icons as C string macros |

## Patterns

- Dual netifs: HTTP clients must use Ethernet (`if_name`) for internet. Pin DNS to the Ethernet snapshot before Wi-Fi associates.
- HTTP `:80` bind is wrapped (`__wrap_lwip_bind`) to the Ethernet IP; proxy `:443` likewise.
- Watchdog: NVS `watchdog` / `mode` (`0` proxy clients, default; `1` Powerwall link). Proxy mode idle until first successful proxy, then `WATCHDOG_TIMEOUT_SEC`. Link mode probes `192.168.91.1:443` every `WATCHDOG_LINK_PROBE_SEC` (5 min) and reboots only if it stays down for `WATCHDOG_TIMEOUT_SEC`, after Wi-Fi has associated once. Dashboard refresh does not probe faster than that in link mode. One confirm probe runs before reboot. `POST /watchdog/save` does not reboot. BOOT does not clear it.
- Ethernet static IP in NVS `eth_config`. Apply `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info` before `esp_eth_start`. `force_dhcp` is one-shot. ICMP gateway for `ETH_DHCP_FALLBACK_SEC`; `/api/status` or a proxy success cancels fallback. GPIO0 BOOT 15 s → DHCP + clear admin password.
- Login is 200 HTML + `Set-Cookie` then JS/`meta` bounce to `/` (not 302). Safari drops cookies on 302-from-POST.
- Log ring 200 × 160 chars; skip ESP-IDF `httpd*` tags; strip ANSI.
- `web_ui.h`: single `%` in CSS/JS; `%%` only in printf formats.
- Mark OTA valid after Ethernet IP so Wi-Fi setup cannot roll back a good image.

## Hardware

Waveshare ESP32-S3-POE-ETH. W5500 SPI: MISO=12 MOSI=11 SCLK=13 CS=14 INT=10. Platform `espressif32@6.9.0`.

GL.iNet GL-S10 (`-e gl-s10`, `BOARD_GL_S10` from `CONFIG_IDF_TARGET_ESP32`): internal EMAC + IP101, MDC=23 MDIO=18 clock in GPIO0, PHY addr 1, PHY power GPIO5; RESET button GPIO33 replaces BOOT (`RECOVERY_BUTTON_NAME`); LEDs 14 (power) and 32 (Wi-Fi up), active low; no temperature sensor (`SOC_TEMP_SENSOR_SUPPORTED`). Target settings in `sdkconfig.defaults.esp32`. OTA channel `gl-s10/version.json`. `pio run` builds only the S3 (`default_envs`); `./deploy.sh -e gl-s10`.

## HTTP (auth unless noted)

| Path | Notes |
|------|--------|
| `GET /health` | no auth, `200 ok` |
| `GET/POST /login`, `GET /logout` | no auth |
| `GET /logs.txt` | downloadable dump |
| `GET /api/status` | wifi, eth, temp, temp_max_c, heap, watchdog |
| `GET /api/requests`, `/api/logs`, `/api/wifi-history` | |
| `GET /api/update`, `POST /api/check-update`, `POST /api/install-update` | |
| `GET /wifi/scan`, `POST /wifi/save`, `POST /eth/save` | eth save reboots |
| `POST /watchdog/save` | `mode=proxy` or `mode=link`, no reboot |
| `POST /admin/setup` | first boot only |
| `POST /admin/password`, `POST /ota/upload`, `POST /reboot` | |

Pages without a session 302 to `/login`; APIs JSON 401.

## CI

Push to `main` and tags `v*` build firmware. Tags also release + Pages (`https://<owner>.github.io/esp32-wifi-bridge/`).
