#!/bin/sh
# Refresh all kernel modules on the SD rootfs after a kernel rebuild.
# Every .ko must match the running kernel's vermagic exactly — a kernel
# rebuild invalidates ALL of them at once.
# Usage: sudo sh deploy-modules-sd.sh /media/<user>/<sd-rootfs-mountpoint>
set -e
SD="${1:?pass the SD rootfs mountpoint}"
M="$(cd "$(dirname "$0")/.." && pwd)"

# Right-mount check against the rootfs itself, not /root/connsys: after a fresh
# sd-deploy.sh the card is a bare buildroot tree and connsys/ does not exist yet.
[ -x "$SD/sbin/init" ] || { echo "no /sbin/init on $SD — wrong mount?" >&2; exit 1; }
mkdir -p "$SD/root/connsys"

# 1. CONSYS out-of-tree modules -> /root/connsys (connsys-up.sh insmods from there)
cp "$M/btif/mtk_btif_drv.ko" \
   "$M/conn_soc/mtk_stp_wmt_soc.ko" \
   "$M/conn_soc/mtk_stp_bt_soc.ko" \
   "$M/conn_soc/mtk_wmt_wifi_soc.ko" \
   "$SD/root/connsys/"

# 2. In-tree BT/HID stack -> /root/connsys/bt (kbd-pair.md insmod order)
mkdir -p "$SD/root/connsys/bt"
cp "$M"/tools/kbd-stage/*.ko "$SD/root/connsys/bt/"

# 3. Wi-Fi: gen2 wlan driver + staged in-tree cfg80211 -> /root/connsys/wifi,
# RAM code -> /lib/firmware (request_firmware path). Stage cfg80211.ko into
# tools/wifi-stage/ and WIFI_RAM_CODE* into tools/wifi-fw/ after a kernel
# rebuild (same vermagic rule as everything else).
mkdir -p "$SD/root/connsys/wifi" "$SD/lib/firmware"
cp "$M/wlan/wlan_gen2.ko" "$SD/root/connsys/wifi/"
cp "$M"/tools/wifi-stage/*.ko "$SD/root/connsys/wifi/"
cp "$M"/tools/wifi-fw/WIFI_RAM_CODE* "$SD/lib/firmware/"

# WMT patches under the name the launcher actually searches for. On Android it
# derives the prefix from system properties; on our rootfs those lookups fail
# and it falls back to "ROMv<hwver>_patch" — with no match it reports 0 patches,
# the chip runs bare ROM firmware, and the Wi-Fi RAM code dies at entry (BT is
# unaffected, it needs no patch). Same bytes, both names installed.
mkdir -p "$SD/system/etc/firmware"
cp "$M"/tools/wifi-fw/mt6572_82_patch_e1_0_hdr.bin "$SD/system/etc/firmware/"
cp "$M"/tools/wifi-fw/mt6572_82_patch_e1_1_hdr.bin "$SD/system/etc/firmware/"
cp "$M"/tools/wifi-fw/mt6572_82_patch_e1_0_hdr.bin "$SD/system/etc/firmware/ROMv1_patch_0_hdr.bin"
cp "$M"/tools/wifi-fw/mt6572_82_patch_e1_1_hdr.bin "$SD/system/etc/firmware/ROMv1_patch_1_hdr.bin"
# WMT config the launcher reads from the same -p folder (coex/antenna/sdio knobs)
cp "$M"/tools/wifi-fw/WMT_SOC.cfg "$SD/system/etc/firmware/"

# Wi-Fi NVRAM (MAC address + RF calibration), read by the driver from this
# exact path. Without it the firmware invents a RANDOM MAC on every query, the
# netdev MAC and the per-BSS receive filter end up different, and the fw drops
# every unicast data frame (EAPOL included) while management traffic works.
if [ -f "$M"/tools/wifi-fw/nvram-WIFI ]; then
	mkdir -p "$SD/etc/firmware/nvram"
	cp "$M"/tools/wifi-fw/nvram-WIFI "$SD/etc/firmware/nvram/WIFI"
else
	echo "WARNING: tools/wifi-fw/nvram-WIFI missing (gitignored, device-specific)" >&2
	echo "         extract it from the stock NVRAM or Wi-Fi runs with a random MAC" >&2
fi

# Resident launcher + HCI smoke test. Both are gitignored build products, and
# nothing else installs them — without the launcher connsys-up.sh cannot serve
# patch info and BOTH BT and Wi-Fi are dead. Build them first (see README):
#   arm-linux-gnueabihf-gcc -static -O2 -I tools/launcher \
#       -o tools/launcher/mtk_stp_launcher tools/launcher/stp_uart_launcher.c
#   arm-linux-gnueabihf-gcc -static -O2 -o tools/stpbt-hci-test tools/stpbt-hci-test.c
# NOTE: use these glibc builds, NOT backups/connsys-firmware/6620_launcher —
# that stock binary needs /system/bin/linker (bionic) and fails with a bare
# "not found" on our rootfs.
for b in launcher/mtk_stp_launcher stpbt-hci-test; do
	[ -f "$M/tools/$b" ] || { echo "MISSING $M/tools/$b — build it (see README)" >&2; exit 1; }
	install -m 0755 "$M/tools/$b" "$SD/root/connsys/$(basename "$b")"
done
cp "$M/tools/wifi-up.sh" "$SD/root/connsys/wifi-up.sh"
chmod +x "$SD/root/connsys/wifi-up.sh"

# 4. Scripts + bridge — keep in lockstep with the .ko: the PSM stack needs
# all three fixes together (AP2CONN_OSC_EN in mtk_stp_wmt_soc.ko, quick-sleep
# off in bt-up.sh, PSM governor in stpbt-vhci-bridge).
cp "$M/tools/stpbt-vhci-bridge" "$SD/root/connsys/stpbt-vhci-bridge"
chmod +x "$SD/root/connsys/stpbt-vhci-bridge"
cp "$M/tools/psm-sleep-probe.sh" "$SD/root/connsys/psm-sleep-probe.sh"
cp "$M/tools/connsys-up.sh" "$SD/root/connsys/connsys-up.sh"
cp "$M/tools/bt-up.sh" "$SD/root/connsys/bt-up.sh"
chmod +x "$SD/root/connsys/bt-up.sh"
mkdir -p "$SD/etc/init.d"
cp "$M/tools/S99bt" "$SD/etc/init.d/S99bt"
chmod +x "$SD/etc/init.d/S99bt"

sync
echo "deployed vermagic: $(modinfo -F vermagic "$M/btif/mtk_btif_drv.ko" 2>/dev/null | cut -d' ' -f1)"
ls -la "$SD/root/connsys/"*.ko "$SD/root/connsys/bt/"*.ko
