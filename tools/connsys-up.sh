#!/bin/sh
# One-shot CONSYS bring-up on PAP5500 mainline (run from /root/connsys).
# insmod chain -> device nodes -> resident launcher -> BT func-on + HCI test.
cd /root/connsys || exit 1

lsmod | grep -q mtk_btif_drv || insmod mtk_btif_drv.ko || exit 1
lsmod | grep -q mtk_stp_wmt_soc || insmod mtk_stp_wmt_soc.ko || exit 1
lsmod | grep -q mtk_stp_bt_soc || insmod mtk_stp_bt_soc.ko || exit 1
echo "modules loaded"

[ -c /dev/stpwmt ] || mknod /dev/stpwmt c 190 0
[ -c /dev/stpbt ] || mknod /dev/stpbt c 192 0

if ! ps | grep -q '[m]tk_stp_launcher'; then
	./mtk_stp_launcher -m 3 -p /system/etc/firmware/ > /tmp/launcher.log 2>&1 &
	sleep 2
fi
echo "launcher: $(head -1 /tmp/launcher.log 2>/dev/null)"

# PSM off for the whole bring-up: a sleep landing inside any BT func-on
# transition wedges the STP stack. The vhci bridge governs PSM afterwards.
echo '0 0' > /proc/driver/wmt_dbg
echo "PSM off for bring-up"

./stpbt-hci-test
echo "HCI_TEST_RC=$?"
