#!/bin/sh
# wifi-fw-extract.sh — run ON the device, booted into the mainline rootfs,
# with the stock Android image still present on eMMC.
#
# Pulls everything Wi-Fi needs out of the stock image and installs it in
# the places the stack expects (see README "Firmware"):
#
#   /lib/firmware/WIFI_RAM_CODE*                  (wlan driver, request_firmware)
#   /system/etc/firmware/mt6572_82_patch_*.bin    (WMT patches)
#   /system/etc/firmware/ROMv1_patch_*.bin        (same bytes - launcher fallback name)
#   /system/etc/firmware/WMT_SOC.cfg
#   /etc/firmware/nvram/WIFI                      (MAC + RF calibration)
#
# Copies of everything also land in $OUT to carry back to a build machine
# (stage them there so an SD re-deploy can reinstall without the stock image).
#
# Partition discovery: busybox blkid detects nothing on these eMMCs, so
# candidates are try-mounted read-only and inspected - mounting a wrong-fs
# partition fails cleanly. On mainline the eMMC enumerates as mmcblk1 when
# booted from SD (the SD card itself is mmcblk0).
#
# Env overrides (for dry runs): DEVPFX, MNT, OUT, FWDIR, SYSFW, NVDIR.

DEVPFX="${DEVPFX:-/dev/mmcblk1p}"
MNT="${MNT:-/mnt/stockpart}"
OUT="${OUT:-/root/fw-extract}"
FWDIR="${FWDIR:-/lib/firmware}"
SYSFW="${SYSFW:-/system/etc/firmware}"
NVDIR="${NVDIR:-/etc/firmware/nvram}"

mkdir -p "$MNT" "$OUT" "$FWDIR" "$SYSFW" "$NVDIR"

find_part() {
    # $1 = marker path that identifies the partition
    for p in 4 5 6 7 2 3 8 9; do
        dev="$DEVPFX$p"
        [ -e "$dev" ] || continue
        mount -t ext4 -o ro "$dev" "$MNT" 2>/dev/null || continue
        if [ -e "$MNT/$1" ]; then
            echo "$dev"
            return 0
        fi
        umount "$MNT"
    done
    return 1
}

# --- stock /system: RAM code, WMT patches, WMT_SOC.cfg -----------------------
SYSDEV=$(find_part etc/firmware) || {
    echo "FW-EXTRACT: FAIL - no candidate partition has /etc/firmware"
    exit 1
}
echo "FW-EXTRACT: system partition = $SYSDEV"
ls -la "$MNT/etc/firmware/" | tee "$OUT/stock-etc-firmware-listing.txt"

CNT=0
for f in "$MNT"/etc/firmware/WIFI_RAM_CODE*; do
    [ -f "$f" ] || continue
    cp "$f" "$FWDIR/" && cp "$f" "$OUT/" || { echo "FW-EXTRACT: FAIL - copy $f"; umount "$MNT"; exit 1; }
    echo "FW-EXTRACT: RAM code $(basename "$f") ($(wc -c < "$f") bytes)"
    CNT=$((CNT + 1))
done
[ "$CNT" -gt 0 ] || { echo "FW-EXTRACT: FAIL - no WIFI_RAM_CODE* in $SYSDEV"; umount "$MNT"; exit 1; }

PCNT=0
for f in "$MNT"/etc/firmware/mt6572*patch*_hdr.bin; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    # install under the stock name AND the ROMv<hwver> fallback the launcher
    # searches when Android system properties are absent (see README)
    idx=$(echo "$base" | sed -n 's/.*_\([0-9]\)_hdr\.bin/\1/p')
    cp "$f" "$SYSFW/$base" && cp "$f" "$OUT/" || { echo "FW-EXTRACT: FAIL - copy $f"; umount "$MNT"; exit 1; }
    [ -n "$idx" ] && cp "$f" "$SYSFW/ROMv1_patch_${idx}_hdr.bin"
    echo "FW-EXTRACT: patch $base (+ROMv1 alias)"
    PCNT=$((PCNT + 1))
done
[ "$PCNT" -gt 0 ] || echo "FW-EXTRACT: WARNING - no WMT patches found (Wi-Fi RAM code will die at entry without them)"

[ -f "$MNT/etc/firmware/WMT_SOC.cfg" ] && cp "$MNT/etc/firmware/WMT_SOC.cfg" "$SYSFW/" && cp "$MNT/etc/firmware/WMT_SOC.cfg" "$OUT/" && echo "FW-EXTRACT: WMT_SOC.cfg"
umount "$MNT"

# --- stock /data: Wi-Fi NVRAM (MAC + RF calibration) --------------------------
NVSRC="nvram/APCFG/APRDEB/WIFI"
if DATADEV=$(find_part "$NVSRC"); then
    echo "FW-EXTRACT: data partition = $DATADEV"
    cp "$MNT/$NVSRC" "$NVDIR/WIFI" && cp "$MNT/$NVSRC" "$OUT/nvram-WIFI" \
        && echo "FW-EXTRACT: NVRAM ($(wc -c < "$NVDIR/WIFI") bytes, MAC at offset 4)"
    umount "$MNT"
else
    echo "FW-EXTRACT: WARNING - Wi-Fi NVRAM not found on any partition;"
    echo "            without it the firmware reports a random MAC per query."
fi

sync
echo "FW-EXTRACT: OK - installed for this boot; carry $OUT back to your build machine"
