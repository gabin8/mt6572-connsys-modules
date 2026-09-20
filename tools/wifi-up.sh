#!/bin/sh
# One-shot Wi-Fi bring-up on MT6572 mainline (run from /root/connsys).
# Prereq: connsys-up.sh ran first (btif+wmt loaded, launcher resident).
#
# PSM needs no special handling here: wlan_gen2 takes a keep-awake reference
# in wlanOpen() and drops it in wlanStop(), and the WMT core refuses to
# enable sleep while any reference is held. Do not force '0 0' here - that
# clears gPsEnable globally and stops BT sleeping after a Wi-Fi session.
cd /root/connsys || exit 1

lsmod | grep -q mtk_stp_wmt_soc || { echo "run connsys-up.sh first"; exit 1; }

ls /lib/firmware/WIFI_RAM_CODE* >/dev/null 2>&1 || {
	echo "no WIFI_RAM_CODE* in /lib/firmware — run deploy-modules-sd.sh"
	exit 1
}

# Wi-Fi chrdev: func-on entry point (/dev/wmtWifi, major 155)
lsmod | grep -q mtk_wmt_wifi_soc || insmod mtk_wmt_wifi_soc.ko || exit 1
[ -c /dev/wmtWifi ] || mknod /dev/wmtWifi c 155 0

# cfg80211 first, then the wlan driver (insmod resolves nothing itself).
# wlan_gen2 binds the DT wifi@180f0000 node at insmod and registers its
# probe with WMT; hardware is only touched at func-on below.
lsmod | grep -q '^cfg80211' || insmod wifi/cfg80211.ko || exit 1
lsmod | grep -q wlan_gen2 || insmod wifi/wlan_gen2.ko || exit 1
echo "wlan modules loaded"

# func-on: WIFI PALDO -> WMT patch -> wlan probe -> RAM code -> wlan0
echo 1 > /dev/wmtWifi

for i in $(seq 1 20); do
	[ -d /sys/class/net/wlan0 ] && break
	sleep 1
done
if [ -d /sys/class/net/wlan0 ]; then
	echo "WLAN0 UP: mac=$(cat /sys/class/net/wlan0/address 2>/dev/null)"
	echo "next: ip link set wlan0 up ; iw dev wlan0 scan | grep -E 'BSS|SSID'"
else
	echo "FAIL: wlan0 did not appear in 20s — check: dmesg | tail -40"
	exit 1
fi
