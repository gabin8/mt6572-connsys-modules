# BT keyboard pairing runbook (classic BT HID)

**VERIFIED WORKING** — full chain: pair -> bond -> connect -> HID input
device -> keystrokes on /dev/input/eventN. Devices and dates below.

Three root causes explained every earlier failure; all must be avoided:

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
3. **BlueZ must carry the input plugin.** The 2026-07-21 run above used the
   hand-built stack in `/opt/bt`, which had it. A buildroot rootfs does not
   unless `BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_HID=y` (and `_HOG=y` for BLE
   peripherals) are set. Without it pairing still succeeds and then
   `connect` fails with `br-connection-profile-unavailable`, which reads
   like a radio fault but is purely a missing profile. Check with
   `bluetoothd -n -d` — the log must say `add_plugin() Loading input plugin`
   and register `profile input-hid`. `make bluez5_utils-reconfigure` is not
   enough to pick the option up; the builtin plugin table is generated into
   `src/builtin.h`, so a `dirclean` + full rebuild is required.

On-device sequence once modules + BlueZ rootfs are in place.

## 1. Bring up CONSYS + hci0 (from /root/connsys)
```
sh /root/connsys/connsys-up.sh          # insmod btif+wmt+bt, launcher, PSM off, HCI smoke test
# libaes is built into the kernel since the 2026-07-28 config (CRYPTO_LIB_AES=y) — no insmod
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

## Verified devices

| Device | Date | Pairing | Result |
| --- | --- | --- | --- |
| Genius Mini LuxePad | 2026-07-21 | legacy PIN | `/opt/bt` BlueZ, event5, clean press/release |
| Logitech K380 | 2026-09-21 | SSP passkey | buildroot BlueZ + input plugin, `hid-generic 0005:046D:B342`, event5, 118/118 press/release |

The K380 shows an SSP passkey (the host displays six digits, you type them
on the keyboard and press Enter) despite reporting `LegacyPairing: yes`.
The window is short, so the digits must be in front of whoever is typing:
driving it over a slow link and relaying them by hand times out with
`AuthenticationCanceled` and a `disconnected with reason 3` from the remote.
Writing the passkey to `/dev/tty0` as soon as it appears solves that — the
panel is next to the keyboard.

## Notes / likely snags
- Classic HID keyboards use SSP with a passkey the HOST shows and the USER
  types on the keyboard, then Enter. The `KeyboardDisplay` agent handles it.
- If `connect` fails after `pair`: the ACL/L2CAP data path is the unproven
  part — watch the bridge log and dmesg for STP/BTIF errors or new firmware
  quirks the vhci bridge needs to shim.
- **Connected links are held out of sniff mode** by the vhci bridge, which
  clears the sniff bit in the link policy on Connection Complete. In sniff
  the peripheral only transmits on anchor points and can skip 20-40 of them,
  so an inbound key-up arrives ~500 ms late; past the input layer's 250 ms
  autorepeat threshold the kernel repeats the key and "cat" types as
  "caaaaaaat". Symptom to watch for: inter-event gaps in
  `/dev/input/eventN` that are exact multiples of the sniff interval
  (12.5 ms here). Capping sniff subrating does NOT fix it -
  HCI_Sniff_Subrating's Max_Latency bounds the local device's transmissions,
  not the peripheral's. The cost is radio time on both ends while something
  is connected; chip PSM is separate and unaffected.
- PSM stays OFF during bring-up (connsys-up.sh does it); at runtime the
  bridge governs it and lets idle ACL links sleep. Measured on the K380 that
  costs ~20 ms on the first keystroke after a sleep (median hold 130 ms after
  an idle gap >1.5 s vs 107 ms while typing) and loses nothing: 118 presses,
  118 releases over a 60 s sample.
- Keep the phone on charge — pairing is a longer session.
