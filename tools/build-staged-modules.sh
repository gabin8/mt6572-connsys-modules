#!/bin/sh
# build-staged-modules.sh — regenerate the gitignored staging directories
# (tools/wifi-stage/, tools/kbd-stage/) from a prepared kernel tree.
#
# These are plain mainline kernel modules that deploy-modules-sd.sh copies
# onto the SD rootfs next to the connsys stack:
#
#   wifi-stage/  cfg80211.ko                      (wlan_gen2 links against it)
#   kbd-stage/   bluetooth, hidp, hci_vhci, uhid, (BlueZ + HID keyboard path)
#                ecc, ecdh_generic                (SMP pairing crypto)
#
# The kernel config must have them =m (CONFIG_CFG80211, CONFIG_BT,
# CONFIG_BT_HIDP, CONFIG_BT_HCIVHCI, CONFIG_UHID and the BT-selected
# ECDH bits). Module vermagic must match the kernel the device runs -
# rebuild after every kernel rebuild, together with the connsys modules.
#
# Usage: sh tools/build-staged-modules.sh [KDIR]
#        (defaults to ../kernel/linux like the top-level Makefile)

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
KDIR="${1:-${KDIR:-$HERE/../../kernel/linux}}"
ARCH="${ARCH:-arm}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

[ -f "$KDIR/Makefile" ] || { echo "no kernel tree at $KDIR" >&2; exit 1; }

# explicit targets: a bare "M=<dir> modules" also builds every other =m
# module in the directory, some of which fail modpost (e.g. btqcomsmd)
build() { # build <dir> <ko>...
    d="$1"; shift
    make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" M="$d" "$@"
}
build net/wireless      cfg80211.ko
build net/bluetooth     bluetooth.ko hidp/hidp.ko
build drivers/bluetooth hci_vhci.ko
build drivers/hid       uhid.ko
build crypto            ecc.ko ecdh_generic.ko

mkdir -p "$HERE/wifi-stage" "$HERE/kbd-stage"
cp "$KDIR/net/wireless/cfg80211.ko"        "$HERE/wifi-stage/"
cp "$KDIR/net/bluetooth/bluetooth.ko"      "$HERE/kbd-stage/"
cp "$KDIR/net/bluetooth/hidp/hidp.ko"      "$HERE/kbd-stage/"
cp "$KDIR/drivers/bluetooth/hci_vhci.ko"   "$HERE/kbd-stage/"
cp "$KDIR/drivers/hid/uhid.ko"             "$HERE/kbd-stage/"
cp "$KDIR/crypto/ecc.ko"                   "$HERE/kbd-stage/"
cp "$KDIR/crypto/ecdh_generic.ko"          "$HERE/kbd-stage/"

echo "staged:"
for k in "$HERE"/wifi-stage/*.ko "$HERE"/kbd-stage/*.ko; do
    printf '  %-14s %s\n' "$(basename "$k")" \
        "$(modinfo -F vermagic "$k" 2>/dev/null || echo '?')"
done
echo "vermagic above must match the running kernel exactly."
