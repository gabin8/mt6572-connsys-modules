#!/bin/sh
# CONSYS + BT + BlueZ one-shot bring-up (PAP5500 mainline).
# Run at boot from /etc/init.d/S99bt or by hand. Safe to re-run after a
# partial failure — every step checks before acting. NEVER rmmod the wmt
# module to "retry"; if the HCI smoke test fails the radio is degraded
# (paging silently dead) and the only fix is a reboot.
LOG=/root/bt-up.log
exec >> $LOG 2>&1
echo "=== bt-up start (uptime: $(cut -d' ' -f1 /proc/uptime)s) ==="

# 1. CONSYS core: insmod chain, launcher, PSM off, HCI smoke test.
# Skipped when the bridge already holds /dev/stpbt (single-open device —
# the smoke test would fail spuriously on an already-running stack).
if ps | grep -q '[s]tpbt-vhci-bridge'; then
    echo "bt-up: bridge already running, skipping CONSYS bring-up"
else
    OUT=$(sh /root/connsys/connsys-up.sh 2>&1)
    echo "$OUT"
    if ! echo "$OUT" | grep -q "HCI_TEST_RC=0"; then
        echo "bt-up: CONSYS HCI test FAILED - radio degraded, REBOOT (no retry)"
        exit 1
    fi
fi

# 2. PSM prerequisites. The bridge owns the PSM on/off toggle at runtime
# (see stpbt-vhci-bridge.c); it may enable PSM ~1 s after bus quiescence,
# so quick-sleep must already be off here. Stale mtk_stp_wmt_soc.ko
# (no AP2CONN_OSC_EN force) cannot survive any sleep: force PSM off.
echo '1 0' > /proc/driver/wmt_dbg
if [ $(( $(devmem 0x10001800) & 0x10000 )) -ne 0 ]; then
    echo "PSM governed by bridge (quick-sleep off, OSC_EN ok)"
else
    echo '0 0' > /proc/driver/wmt_dbg
    echo "WARNING: AP2CONN_OSC_EN missing (stale mtk_stp_wmt_soc.ko) - PSM forced off"
fi

# 3. BT/HID kernel modules
cd /root/connsys/bt || exit 1
# Some of these are built into the kernel depending on the config (libaes is
# CONFIG_CRYPTO_LIB_AES=y here), in which case there is no .ko to load and
# lsmod never lists them. Skip those instead of failing the whole bring-up.
for m in libaes ecc kpp ecdh_generic bluetooth hci_vhci hidp uhid; do
    lsmod | grep -q "^$m " && continue
    [ -f "$m.ko" ] || { echo "$m: built into the kernel, skipping"; continue; }
    insmod $m.ko || exit 1
done

# 4. stpbt <-> vhci bridge (creates hci0, governs PSM)
if ! ps | grep -q '[s]tpbt-vhci-bridge'; then
    /root/connsys/stpbt-vhci-bridge > /root/bridge.log 2>&1 &
    sleep 2
fi

# 5. Locate BlueZ. /opt/bt is the hand-built stack from the early bring-up; a
# buildroot rootfs ships BlueZ in the usual places and already runs dbus from
# its own init script. Take whichever is present.
if [ -x /opt/bt/bin/bluetoothctl ]; then
    export LD_LIBRARY_PATH=/opt/bt/libs
    BTCTL=/opt/bt/bin/bluetoothctl
    BTD=/opt/bt/libexec/bluetoothd
    DBUS_UUIDGEN=/opt/bt/bin/dbus-uuidgen
    DBUS_START="/opt/bt/bin/dbus-daemon --config-file=/opt/bt/dbus/system.conf --fork"
else
    BTCTL=$(command -v bluetoothctl)
    BTD=/usr/libexec/bluetooth/bluetoothd
    DBUS_UUIDGEN=$(command -v dbus-uuidgen)
    DBUS_START="$(command -v dbus-daemon) --system --fork"
fi
[ -x "$BTCTL" ] || { echo "no bluetoothctl found"; exit 1; }
[ -x "$BTD" ]   || { echo "no bluetoothd found ($BTD)"; exit 1; }

# 5b. D-Bus system bus
[ -n "$DBUS_UUIDGEN" ] && "$DBUS_UUIDGEN" --ensure
mkdir -p /run/dbus /var/lib/bluetooth
if [ ! -S /run/dbus/system_bus_socket ]; then
    $DBUS_START
fi

# 6. bluetoothd. Start it here even though an init script may have tried at
# boot: there is no adapter that early (connsys-up.sh has not run yet), so it
# gives up and exits, and bluetoothctl then hangs on an empty bus.
if ! ps | grep -q '[b]luetoothd'; then
    "$BTD" -n > /root/btd.log 2>&1 &
    sleep 3
fi

# 7. adapter up (bonded+trusted keyboard then reconnects on any keypress).
# Retry: bluetoothd's adapter registration races the first power-on attempt.
PWR=""
for i in 1 2 3 4 5 6 7 8 9 10; do
    PWR=$("$BTCTL" power on </dev/null 2>&1)
    echo "$PWR" | grep -q succeeded && break
    sleep 1
done
echo "power on: $PWR"

echo "=== bt-up done ==="
