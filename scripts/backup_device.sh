#!/usr/bin/env bash
#
# Back up every flash partition of a TickrMeter running TickrDisplay firmware.
#
# Usage:  scripts/backup_device.sh <device-ip> [output-dir]
#
# Creates <output-dir>/ (default: backup_<YYYYmmdd_HHMMSS>/) with:
#   system_info.json          - /api/system/info
#   <label>_0x<addr>.bin      - raw contents of every partition (nvs, otadata, app0, app1, spiffs, ...)
#   SHA256SUMS                - checksums of all dumps
# and prints which app partition most likely contains the stock firmware.
#
# Requires: curl, and optionally jq (used for pretty output; falls back to a python one-liner).
# If the device has an API token configured (/system -> Security), export it as
# TICKR_API_TOKEN; it is sent as HTTP Basic auth (any user, password = token).
#
set -euo pipefail

IP="${1:-}"
if [[ -z "$IP" ]]; then
    echo "Usage: $0 <device-ip> [output-dir]" >&2
    exit 1
fi
OUT="${2:-backup_$(date +%Y%m%d_%H%M%S)}"
BASE="http://${IP}"
AUTH=()
if [[ -n "${TICKR_API_TOKEN:-}" ]]; then
    AUTH=(-u ":${TICKR_API_TOKEN}")
fi

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

# JSON helper: jq if available, otherwise python3.
json() { # json '<jq filter>' < file
    if command -v jq >/dev/null; then
        jq -r "$1"
    else
        python3 -c '
import json,sys
f=sys.argv[1]; d=json.load(sys.stdin)
# minimal support for the filters used in this script
if f==".partitions[] | .label":
    print("\n".join(p["label"] for p in d["partitions"]))
elif f==".running.label": print(d["running"]["label"])
elif f==".boot.label": print(d["boot"]["label"] if d.get("boot") else "")
elif f==".flash.chip_size": print(d["flash"]["chip_size"])
elif f==".firmware.version": print(d["firmware"]["version"])
elif f=="app_table":
    for p in d["partitions"]:
        if p["type"]!="app": continue
        desc=p.get("description") or {}
        print("%s\t%d\t%d\t%s\t%s\t%s\t%s %s %s" % (p["label"],p["address"],p["size"],
              "RUNNING" if p.get("running") else "-", "BOOT" if p.get("boot") else "-",
              p.get("ota_state") or "-", desc.get("project_name","(empty)"), desc.get("idf_ver",""), desc.get("date","")))
' "$1"
    fi
}

mkdir -p "$OUT"
echo "==> Device ${IP}: fetching /api/system/info"
if ! curl -fsS --max-time 15 ${AUTH[@]+"${AUTH[@]}"} "${BASE}/api/system/info" -o "${OUT}/system_info.json"; then
    echo "!! /api/system/info not reachable. Is this a TickrDisplay device? (stock firmware has no backup API)" >&2
    echo "   If an API token is set on /config, export TICKR_API_TOKEN=<token> first." >&2
    exit 2
fi

FW_VER="$(json '.firmware.version' < "${OUT}/system_info.json")"
RUNNING="$(json '.running.label' < "${OUT}/system_info.json")"
BOOT="$(json '.boot.label' < "${OUT}/system_info.json")"
FLASH="$(json '.flash.chip_size' < "${OUT}/system_info.json")"
echo "    firmware ${FW_VER}, flash ${FLASH} bytes, running=${RUNNING}, boot=${BOOT}"

echo "==> Downloading partitions into ${OUT}/"
while IFS= read -r label; do
    [[ -z "$label" ]] && continue
    printf '    %-10s ' "$label"
    # -J honours Content-Disposition (<label>_0x<addr>.bin), -O writes into cwd
    if (cd "$OUT" && curl -fsS --max-time 600 ${AUTH[@]+"${AUTH[@]}"} -O -J "${BASE}/api/system/partition/${label}"); then
        f="$(ls -t "${OUT}"/"${label}"_0x*.bin 2>/dev/null | head -n1)"
        echo "ok  $(stat -f%z "$f" 2>/dev/null || stat -c%s "$f") bytes -> $(basename "$f")"
    else
        echo "FAILED"
    fi
done < <(json '.partitions[] | .label' < "${OUT}/system_info.json")

if command -v shasum >/dev/null; then
    (cd "$OUT" && shasum -a 256 ./*.bin > SHA256SUMS)
elif command -v sha256sum >/dev/null; then
    (cd "$OUT" && sha256sum ./*.bin > SHA256SUMS)
fi

echo
echo "==> App partitions (RUNNING = TickrDisplay, the other slot is normally the stock firmware):"
printf '    %-8s %-10s %-9s %-8s %-5s %-15s %s\n' LABEL ADDRESS SIZE RUNNING BOOT OTA_STATE IMAGE
if command -v jq >/dev/null; then
    jq -r '.partitions[] | select(.type=="app") |
        [.label, (.address|tostring), (.size|tostring),
         (if .running then "RUNNING" else "-" end), (if .boot then "BOOT" else "-" end),
         (.ota_state // "-"),
         (if .description then (.description.project_name + " " + .description.idf_ver + " " + .description.date)
          elif .bootable then "header ok, no description" else "(empty)" end)] | @tsv' \
        < "${OUT}/system_info.json" | awk -F'\t' '{printf "    %-8s %-10s %-9s %-8s %-5s %-15s %s\n",$1,sprintf("0x%06x",$2),$3,$4,$5,$6,$7}'
else
    json app_table < "${OUT}/system_info.json" | awk -F'\t' '{printf "    %-8s %-10s %-9s %-8s %-5s %-15s %s\n",$1,sprintf("0x%06x",$2),$3,$4,$5,$6,$7}'
fi

echo
echo "Hints:"
echo "  * The stock TickrMeter image identifies itself as project 'arduino-lib-builder', idf v4.4.5, built Jun 12 2023."
echo "  * To return to stock later:  curl -X POST http://${IP}/api/system/boot_partition -H 'Content-Type: application/json' -d '{\"label\":\"<stock-slot>\"}'"
echo "  * nvs_*.bin holds the vendor device id / WiFi credentials; otadata_*.bin selects the boot slot."
echo "Done: ${OUT}/"
