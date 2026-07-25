# Genius Mini LuxePad pairing runbook (classic BT HID)

**VERIFIED WORKING 2026-07-21** — full chain: pair (legacy PIN) -> bond ->
connect -> uhid input device -> keystrokes on /dev/input/eventN.

Two root causes explained every earlier failure; both must be avoided:

1. **A crashed radio must be rebooted, never retried.** PSM is now managed
   automatically (off during bring-up, governed by the vhci bridge at
   runtime), but if the chip ever crash-recovers ("evt err trigger assert
   fail, do chip reset to recovery" in dmesg) the radio comes back half-alive:
   HCI + inquiry work but **outbound paging is silently dead** (Create
   Connection -> Command Status Success -> no event at all, MGMT pair times
   out after 120 s). Only a reboot restores paging. Never proceed past a
   failed `connsys-up.sh` HCI test — reboot instead.
2. **Verify the link key hit disk after pairing** (checkpoint in step 3
   below). Watch for `Bonded: yes` in bluetoothctl — `Paired: yes` alone is
   NOT enough; a pairing on a degraded radio can "succeed" without storing a
   LinkKey, and then every HID connect is rejected/reset as un-bonded.

On-device sequence once modules + BlueZ rootfs are in place.

## 1. Bring up CONSYS + hci0 (from /root/connsys)
```
sh /root/connsys/connsys-up.sh          # insmod btif+wmt+bt, launcher, PSM off, HCI smoke test
insmod /root/connsys/bt/libaes.ko
insmod /root/connsys/bt/ecc.ko
insmod /root/connsys/bt/ecdh_generic.ko
insmod /root/connsys/bt/bluetooth.ko
insmod /root/connsys/bt/hci_vhci.ko
insmod /root/connsys/bt/hidp.ko         # NEW: classic HID profile
insmod /root/connsys/bt/uhid.ko         # NEW: HID -> input injection
/root/connsys/stpbt-vhci-bridge > /tmp/bridge.log 2>&1 &   # creates hci0
```

## 2. Start D-Bus + bluetoothd
```
mkdir -p /var/run/dbus /var/lib/bluetooth
dbus-daemon --system --fork
/usr/libexec/bluetooth/bluetoothd -n -d &   # foreground-ish, debug; or bluetoothd &
hciconfig hci0 up          # or: bluetoothctl power on
```

## 3. Pair the LuxePad (put keyboard in pairing mode first)
```
bluetoothctl
  power on
  agent KeyboardDisplay      # keyboard needs an agent that can show/enter a PIN
  default-agent
  scan on
  # ... wait for the LuxePad's bdaddr to appear ...
  pair    <BDADDR>           # shows "[agent] PIN code: NNNNNN"
                             #  -> TYPE the digits ON THE KEYBOARD + Enter, fast
                             # MUST see "Bonded: yes" (not just "Paired: yes")
  trust   <BDADDR>
  quit
```
CHECKPOINT — link key on disk (skip = repeat of the 2026-07-15 dead end):
```
grep -A2 LinkKey /var/lib/bluetooth/<adapter-bdaddr>/<BDADDR>/info; sync
# must print Key=<32 hex>. Missing -> pairing silently failed, re-pair.
```

## 4. Connect + verify keypresses
```
bluetoothctl connect <BDADDR>       # if it fails: press a key on the keyboard
                                    # instead (it reconnects inbound), wait 5 s
grep -A5 LuxePad /proc/bus/input/devices   # uhid device, Handlers=... eventN
hexdump -C /dev/input/eventN        # type on keyboard -> EV_MSC+EV_KEY+EV_SYN
```
Verified result 2026-07-21: `Genius Mini LuxePad Keyboard` on
`/devices/virtual/misc/uhid/0005:05AC:023C.0001`, Handlers `sysrq kbd leds
event5`, clean press/release events for every key.

## Notes / likely snags
- Classic HID keyboards use SSP with a passkey the HOST shows and the USER
  types on the keyboard, then Enter. The `KeyboardDisplay` agent handles it.
- If `connect` fails after `pair`: the ACL/L2CAP data path is the unproven
  part — watch the bridge log and dmesg for STP/BTIF errors or new firmware
  quirks the vhci bridge needs to shim.
- PSM stays OFF (connsys-up.sh does it) so the link never sleeps mid-session.
- Keep the phone on charge — pairing is a longer session.
