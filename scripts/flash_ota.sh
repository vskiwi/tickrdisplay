#!/usr/bin/env bash
#
# Flash a firmware image over HTTP to a TickrMeter.
#
# Works against BOTH:
#   * TickrDisplay firmware        - POST /update  (multipart field "update")
#   * stock TickrMeter firmware    - WiFiManager config portal, POST /u (multipart field "update"),
#                                    reachable at http://192.168.4.1 while the "TickrMeter" AP is up.
#
# Usage:  scripts/flash_ota.sh <device-ip> <firmware.bin> [--stock|--fork]
#
# Without --stock/--fork the script probes GET /api/status (a cheap endpoint that only TickrDisplay
# has) to detect TickrDisplay; if that fails it assumes the stock WiFiManager portal.
# (It deliberately does NOT call /api/system/info: that endpoint walks the partition table and
# hashes the running image; early TickrDisplay builds panicked on it.)
#
# If the TickrDisplay device has an API token configured (/system -> Security), export it as
# TICKR_API_TOKEN; it is sent as HTTP Basic auth (any user, password = token).
#
set -euo pipefail

IP="${1:-}"
FW="${2:-}"
MODE="${3:-auto}"

if [[ -z "$IP" || -z "$FW" ]]; then
    echo "Usage: $0 <device-ip> <firmware.bin> [--stock|--fork]" >&2
    exit 1
fi
[[ -f "$FW" ]] || { echo "File not found: $FW" >&2; exit 1; }
command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

BASE="http://${IP}"
# Optional API token for TickrDisplay endpoints (ignored by the stock portal).
AUTH=()
if [[ -n "${TICKR_API_TOKEN:-}" ]]; then
    AUTH=(-u ":${TICKR_API_TOKEN}")
fi
SIZE=$(stat -f%z "$FW" 2>/dev/null || stat -c%s "$FW")
MAGIC=$(head -c1 "$FW" | od -An -tx1 | tr -d ' \n')
if [[ "$MAGIC" != "e9" ]]; then
    echo "!! $FW does not start with 0xE9 - not an ESP32 application image (did you pick firmware.bin, not a merged/bootloader image?)" >&2
    exit 1
fi
# Standard Arduino OTA slot on the stock 4 MB table is 0x140000 bytes.
OTA_SLOT_STOCK=1310720
if (( SIZE > OTA_SLOT_STOCK )); then
    echo "!! Image is ${SIZE} bytes, larger than the stock OTA slot (${OTA_SLOT_STOCK}). It will only fit if the device uses a bigger partition table." >&2
fi

case "$MODE" in
    --stock) TARGET="stock" ;;
    --fork)  TARGET="fork" ;;
    auto)
        if curl -fsS --max-time 5 ${AUTH[@]+"${AUTH[@]}"} "${BASE}/api/status" -o /tmp/tickr_status_$$.json 2>/dev/null; then
            TARGET="fork"
            if command -v jq >/dev/null; then
                echo "Detected TickrDisplay (uptime $(jq -r '.uptime_s // "?"' /tmp/tickr_status_$$.json) s, free heap $(jq -r '.heap_free // "?"' /tmp/tickr_status_$$.json) bytes)"
            else
                echo "Detected TickrDisplay firmware"
            fi
            rm -f /tmp/tickr_status_$$.json
        else
            TARGET="stock"
            echo "No /api/status - assuming stock WiFiManager portal"
            echo "   (a TickrDisplay with an API token answers 401 here: export TICKR_API_TOKEN=<token>)"
        fi
        ;;
    *) echo "Unknown mode: $MODE" >&2; exit 1 ;;
esac

if [[ "$TARGET" == "fork" ]]; then
    URL="${BASE}/update"
else
    URL="${BASE}/u"
    # Sanity check that the stock portal answers (GET /update shows the upload form).
    if ! curl -fsS --max-time 5 "${BASE}/update" -o /dev/null; then
        echo "!! ${BASE}/update did not answer. For the stock firmware: power the device from USB, make your" >&2
        echo "   home WiFi unavailable, toggle the rear switch off/on, join the open 'TickrMeter' AP and use 192.168.4.1." >&2
        exit 2
    fi
fi

echo "Uploading ${FW} (${SIZE} bytes) to ${URL} ..."
echo "Do NOT remove power until the device reboots."
CURL_AUTH=()
[[ "$TARGET" == "fork" ]] && CURL_AUTH=(${AUTH[@]+"${AUTH[@]}"})
HTTP_CODE=$(curl -sS --max-time 600 -o /tmp/tickr_upload_$$.out -w '%{http_code}' ${CURL_AUTH[@]+"${CURL_AUTH[@]}"} \
    -F "update=@${FW};type=application/octet-stream" "${URL}") || {
        echo "!! curl failed" >&2; cat /tmp/tickr_upload_$$.out 2>/dev/null; rm -f /tmp/tickr_upload_$$.out; exit 3; }

BODY=$(cat /tmp/tickr_upload_$$.out); rm -f /tmp/tickr_upload_$$.out
echo "HTTP ${HTTP_CODE}"
if [[ "$TARGET" == "fork" ]]; then
    echo "$BODY"
    [[ "$HTTP_CODE" == "200" ]] || exit 4
    echo "Device is rebooting into the new image. Reload http://${IP}/update in ~15 s."
else
    # WiFiManager answers with an HTML page containing either "Update successful" or "Update failed".
    if grep -qi "Update successful" <<<"$BODY"; then
        echo "Stock portal reports: Update successful. Device is rebooting into the new image."
    elif grep -qi "Update failed" <<<"$BODY"; then
        echo "!! Stock portal reports: Update failed. Reboot and try again (make sure the AP did not time out)." >&2
        exit 4
    else
        echo "Unexpected response from stock portal (HTTP ${HTTP_CODE}):"
        echo "$BODY" | sed 's/<[^>]*>/ /g' | tr -s ' \n' | head -c 600; echo
        [[ "$HTTP_CODE" == "200" ]] || exit 4
    fi
fi
