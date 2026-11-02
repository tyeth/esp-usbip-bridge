#!/usr/bin/env bash
# Deploy an S31 bridge app image over OTA and wait until each bridge runs and
# confirms it (or rolls it back).
#
#   scripts/ota-deploy.sh <esp32p4_usbip_bridge.bin> <bridge> [<bridge> ...]
#
# A bridge is a host name or address (http).  Bridges are updated one at a
# time; the first failure stops the run.
#   BRIDGE_TOKEN         Bearer token, when the bridges have an API token
#   OTA_CONFIRM_TIMEOUT  seconds to wait for the new image to confirm (150)
set -euo pipefail

if [[ $# -lt 2 ]]; then
    sed -n '2,12p' "$0" >&2
    exit 2
fi
IMAGE="$1"
shift
TIMEOUT="${OTA_CONFIRM_TIMEOUT:-150}"
PY="$(command -v python3 || command -v python)"
AUTH=()
if [[ -n "${BRIDGE_TOKEN:-}" ]]; then
    AUTH=(-H "Authorization: Bearer ${BRIDGE_TOKEN}")
fi

# esp_app_desc_t follows the image and first segment headers (0x20); its
# app_elf_sha256 starts 144 bytes in.  /api/ota reports the first 16 digits.
WANT="$(od -An -tx1 -j $((0x20 + 144)) -N8 "${IMAGE}" | tr -d ' \n')"
MAGIC="$(od -An -tx1 -j $((0x20)) -N4 "${IMAGE}" | tr -d ' \n')"
if [[ "${MAGIC}" != "3254cdab" ]]; then
    echo "${IMAGE}: not an ESP-IDF app image (no app description)" >&2
    exit 2
fi
echo "image ${IMAGE}: elf_sha256 ${WANT}..."

field() {   # field <json> <python expression on d>
    "${PY}" -c 'import json,sys; d=json.loads(sys.argv[1]); print(eval(sys.argv[2]))' "$1" "$2"
}

for BRIDGE in "$@"; do
    BASE="http://${BRIDGE}"
    STATUS="$(curl -fsS -m 10 "${BASE}/api/ota")"
    if [[ "$(field "${STATUS}" 'd.get("supported")')" != "True" ]]; then
        echo "${BRIDGE}: no OTA slots (old partition table): needs the one wired migration flash" >&2
        exit 1
    fi
    SLOT="$(field "${STATUS}" 'd["next_update"]')"
    echo "${BRIDGE}: running $(field "${STATUS}" 'd["running"]') ($(field "${STATUS}" 'd["version"]')), uploading to ${SLOT}"
    curl -fsS -m 300 "${AUTH[@]}" -X PUT -H "Content-Type: application/octet-stream" \
        --data-binary "@${IMAGE}" "${BASE}/api/ota?reboot=1"
    echo
    DEADLINE=$((SECONDS + TIMEOUT))
    sleep 5
    while true; do
        if STATUS="$(curl -fsS -m 5 "${BASE}/api/ota" 2>/dev/null)"; then
            RUNNING="$(field "${STATUS}" 'd["running"]')"
            STATE="$(field "${STATUS}" '[p.get("state") for p in d["partitions"] if p["label"] == d["running"]][0]')"
            SHA="$(field "${STATUS}" '[(p.get("app") or {}).get("elf_sha256", "") for p in d["partitions"] if p["label"] == d["running"]][0]')"
            if [[ "${RUNNING}" == "${SLOT}" && "${SHA}" == "${WANT}"* && "${STATE}" == "valid" ]]; then
                echo "${BRIDGE}: running ${SLOT} ($(field "${STATUS}" 'd["version"]')), confirmed"
                break
            fi
            if [[ "$(field "${STATUS}" 'd.get("last_invalid")')" == "${SLOT}" && "${RUNNING}" != "${SLOT}" ]]; then
                echo "${BRIDGE}: the new image did not confirm and rolled back to ${RUNNING}" >&2
                exit 1
            fi
        fi
        if (( SECONDS > DEADLINE )); then
            echo "${BRIDGE}: not confirmed within ${TIMEOUT} s (last: ${STATUS:-no answer})" >&2
            exit 1
        fi
        sleep 3
    done
done
