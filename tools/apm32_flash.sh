#!/bin/sh
# Update an APM32 radio without opening it: ask the running firmware to reboot into the ROM DFU loader through
# its USB serial port ("DFU;"), write the image, read it back and compare, then leave the ROM loader into the
# firmware. The ROM jumps from inside its USB interrupt; the firmware sees that at its very first instruction
# (SystemInit) and resets the chip, so the radio restarts on its own. Images older than that check hang instead:
# switch the radio off and on.
#
# Usage: tools/apm32_flash.sh [image.bin]          default image: build_apm32/firmware_apm32.bin
#        PORT=/dev/cu.usbmodemXXXX tools/apm32_flash.sh   when the serial port cannot be found on its own
#
# If the radio is already in DFU (menu, rescue gesture or BOOT0), the serial step is skipped.
set -eu

IMG=${1:-build_apm32/firmware_apm32.bin}
ADDR=0x08020000
DFU_ID=314b:0106

[ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 1; }
command -v dfu-util >/dev/null || { echo "dfu-util not installed" >&2; exit 1; }

in_dfu() { dfu-util -l 2>/dev/null | grep -q "$DFU_ID"; }

find_port() {
    if [ -n "${PORT:-}" ]; then echo "$PORT"; return; fi
    # macOS: the serial device of the USB device 28e9:018b (10473:395), the firmware's own IDs.
    if command -v ioreg >/dev/null; then
        p=$(ioreg -r -c IOUSBHostDevice -l -w0 | awk '
            /"idVendor" = /  { v = $NF }
            /"idProduct" = / { d = $NF }
            /"IOCalloutDevice" = / { if (v == 10473 && d == 395) { gsub(/"/, "", $NF); print $NF; exit } }')
        if [ -n "$p" ]; then echo "$p"; return; fi
    fi
    # Otherwise only if there is exactly one candidate.
    set -- /dev/cu.usbmodem* /dev/ttyACM*
    c=""
    for f in "$@"; do [ -e "$f" ] && c="$c $f"; done
    set -- $c
    [ $# -eq 1 ] && echo "$1"
}

if ! in_dfu; then
    port=$(find_port || true)
    [ -n "$port" ] || { echo "radio not found: no DFU device and no single USB serial port (set PORT=...)" >&2; exit 1; }
    echo "asking $port for DFU"
    printf 'DFU;' > "$port"
    i=0
    until in_dfu; do
        i=$((i + 1))
        [ $i -le 20 ] || { echo "the radio did not enter DFU" >&2; exit 1; }
        sleep 0.5
    done
fi

size=$(wc -c < "$IMG" | tr -d ' ')
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

dfu-util -d "$DFU_ID" -a 0 -s "$ADDR" -D "$IMG"
dfu-util -d "$DFU_ID" -a 0 -s "$ADDR:$size" -U "$tmp/readback.bin"
if ! cmp -s "$IMG" "$tmp/readback.bin"; then
    echo "VERIFY FAILED: the flash does not match the image; the radio stays in DFU" >&2
    exit 1
fi

echo "image verified, restarting the radio"
dfu-util -d "$DFU_ID" -a 0 -s "$ADDR:leave" >/dev/null 2>&1 || true
i=0
until [ -n "$(find_port || true)" ]; do
    i=$((i + 1))
    [ $i -le 30 ] || { echo "the radio did not come back on USB within 15 s: switch it off and on" >&2; exit 1; }
    sleep 0.5
done
echo "the radio has restarted"
