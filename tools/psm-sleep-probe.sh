#!/bin/sh
# PSM deep-sleep threshold probe (bug #3: wake fails after long sleep).
# Ascending idle windows; after each, one HCI poke must wake the chip.
# Stops at the first failure - the radio is wedged at that point and the
# boot is spent. Flight recorder (bt-up.sh) captures the wire throughout.
# Run on the device: sh /root/connsys/psm-sleep-probe.sh
export LD_LIBRARY_PATH=/opt/bt/libs
BTCTL=/opt/bt/bin/bluetoothctl

poke() {
	$BTCTL -- discoverable on > /dev/null 2>&1 || return 1
	$BTCTL -- discoverable off > /dev/null 2>&1 || return 1
	return 0
}

echo "=== psm-sleep-probe start (uptime $(cut -d' ' -f1 /proc/uptime)s) ==="
poke || { echo "baseline poke FAILED - radio already bad"; exit 1; }
echo "baseline poke OK"

for T in 10 30 60 90 120 180 240 360; do
	echo "sleeping ${T}s..."
	sleep $T
	if poke; then
		echo "T=${T}s wake OK"
	else
		echo "T=${T}s wake FAILED <=== threshold"
		echo "spam=$(dmesg | grep -c 'detect whole')"
		exit 1
	fi
done
echo "=== all windows passed - no threshold up to 360s ==="
