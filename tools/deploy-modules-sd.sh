#!/bin/sh
# Refresh all kernel modules on the SD rootfs after a kernel rebuild.
# Every .ko must match the running kernel's vermagic exactly — a kernel
# rebuild invalidates ALL of them at once.
# Usage: sudo sh deploy-modules-sd.sh /media/<user>/<sd-rootfs-mountpoint>
set -e
SD="${1:?pass the SD rootfs mountpoint}"
M="$(cd "$(dirname "$0")/.." && pwd)"

[ -d "$SD/root/connsys" ] || { echo "no /root/connsys on $SD — wrong mount?" >&2; exit 1; }

# 1. CONSYS out-of-tree modules -> /root/connsys (connsys-up.sh insmods from there)
cp "$M/btif/mtk_btif_drv.ko" \
   "$M/conn_soc/mtk_stp_wmt_soc.ko" \
   "$M/conn_soc/mtk_stp_bt_soc.ko" \
   "$M/conn_soc/mtk_wmt_wifi_soc.ko" \
   "$SD/root/connsys/"

# 2. In-tree BT/HID stack -> /root/connsys/bt (kbd-pair.md insmod order)
mkdir -p "$SD/root/connsys/bt"
cp "$M"/tools/kbd-stage/*.ko "$SD/root/connsys/bt/"

# 3. Scripts + bridge — keep in lockstep with the .ko: the PSM stack needs
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
