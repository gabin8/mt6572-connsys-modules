# mt6572-connsys-modules

Out-of-tree Linux kernel modules and userspace tools that bring up the
**MediaTek MT6572 on-die connectivity subsystem (CONSYS)** — the WiFi / BT /
GPS / FM block paired with an **MT6627N RF front-end** — on a mainline
kernel.

The stack is a forward-port of MediaTek's downstream `conn_soc` driver
family (3.4-era ALPS code, with the 4.14 BPI-Router-Linux forward-port used
as a porting roadmap). None of it is upstreamable; it is packaged as
out-of-tree modules in the spirit of
[mt8768-modules](https://codeberg.org/lowendlibre/mt8768-modules).

## Status

| Function | State |
|---|---|
| BTIF transport (PIO + APDMA) | working, loopback-verified |
| WMT/STP control plane, firmware download | working |
| Bluetooth (`/dev/stpbt` → BlueZ `hci0`) | working — pairing, HID keyboard, inbound reconnect |
| Power management (PSM / chip sleep) | working (see [PSM](#power-management-psm)) |
| WiFi (`wlan/` gen2 driver → cfg80211 `wlan0`) | working — scan, WPA2-PSK association, DHCP, traffic |
| GPS / FM | not started |

Verified on the Prestigio PAP5500 DUO; the Lenovo A369i carries the same
silicon.

## Architecture

- The WiFi/BT/GPS/FM **MAC/baseband is on the MT6572 die** (CONSYS: a
  dedicated MCU running MediaTek WMT firmware). The external MT6627N is
  only the RF transceiver, configured by the CONSYS firmware — there is no
  SDIO/UART combo bus.
- Host transport is **BTIF** (SoC block at `0x1100E000`, APDMA VFF channels
  at `0x11000380`/`0x11000400`).
- The **WMT/STP** layer multiplexes BT/GPS/FM/WMT channels over BTIF, and
  downloads the firmware patch at function-on.
- Char devices: `/dev/stpwmt` (control, major 190), `/dev/stpbt` (BT HCI,
  major 192, single-open), `/dev/wmtWifi` (WiFi func ctrl, major 155).

## Kernel prerequisites

The kernel tree the modules build against must provide:

- MT6572 platform support (clocks, pinctrl, pwrap/MT6323 regulators) plus:
  - `btif@1100e000` node (`mediatek,mt6572-btif`) with `btif_tx`/`btif_rx`
    APDMA nodes,
  - `connectivity@18070000` node (`mediatek,mt6572-consys`) with the MCU +
    TOPCKGEN reg ranges, the `conn2ap` wakeup + WDT interrupts, the CONN
    power domain, the TOPRGU `connsys` reset and the four `vcn*` supplies,
  - `wifi@180f0000` node (`mediatek,wifi`, the CONSYS Wi-Fi AHB window,
    GIC SPI 123 level-low) for the wlan driver;
- `CONFIG_BT=m` core with `CONFIG_BT_HCIVHCI=m`, `CONFIG_BT_HIDP=m`,
  `CONFIG_UHID=m` for the BlueZ/HID path;
- `CONFIG_CFG80211` (=m works; insmod `cfg80211.ko` before `wlan_gen2.ko`).

Build the kernel once (`make modules`) so `Module.symvers` exists.

## Building

```sh
make                    # expects the kernel tree at ../kernel/linux
make KDIR=/path/to/tree # or point at any prepared kernel tree
```

Produces `btif/mtk_btif_drv.ko`, `conn_soc/mtk_stp_wmt_soc.ko`,
`conn_soc/mtk_stp_bt_soc.ko`, `conn_soc/mtk_wmt_wifi_soc.ko`,
`wlan/wlan_gen2.ko`.

The wlan driver links against cfg80211; with `CONFIG_CFG80211=m` build it
first (`make -C <kernel> M=net/wireless modules`) — the top Makefile picks
up its `Module.symvers` automatically.

Wi-Fi needs three device-derived blobs that are not in git (see
[Firmware](#firmware)): the `WIFI_RAM_CODE*` image, the WMT patches
(installed under both accepted filename schemes), and the Wi-Fi NVRAM
(MAC + RF calibration) which lands at `/etc/firmware/nvram/WIFI`.
`tools/wifi-fw-extract.sh` pulls all of them from the stock image;
`tools/deploy-modules-sd.sh` reinstalls them from its `tools/wifi-fw/`
staging on every SD deploy. Bring-up on the device is `connsys-up.sh`
followed by `wifi-up.sh`.

Module vermagic must match the running kernel **exactly**; rebuild and
redeploy all modules together after any kernel rebuild.

Userspace tools cross-compile statically:

```sh
arm-linux-gnueabihf-gcc -static -O2 -o tools/stpbt-vhci-bridge tools/stpbt-vhci-bridge.c
arm-linux-gnueabihf-gcc -static -O2 -o tools/launcher/mtk_stp_launcher tools/launcher/stp_uart_launcher.c
```

## Firmware

All CONSYS firmware ships with the stock device image and is **not**
redistributed here. Run `tools/wifi-fw-extract.sh` on the device (booted
into the mainline rootfs, stock image still on eMMC) and it extracts and
installs every Wi-Fi piece; or place them by hand:

- `/system/etc/firmware/`: `mt6572_82_patch_e1_{0,1}_hdr.bin` — **also
  copied as `ROMv1_patch_{0,1}_hdr.bin`**: outside Android the launcher
  cannot read the system properties it derives the patch prefix from and
  falls back to `ROMv<hwver>_patch`; with no match it reports zero patches,
  the chip runs bare ROM firmware, BT still works but the Wi-Fi RAM code
  dies at entry — and `WMT_SOC.cfg`;
- `/lib/firmware/`: `WIFI_RAM_CODE_MT6582` (the Wi-Fi RAM image, loaded by
  the wlan driver via request_firmware);
- `/etc/firmware/nvram/WIFI`: the device's Wi-Fi NVRAM (MAC address + RF
  calibration, 514 B, stock keeps it at `/data/nvram/APCFG/APRDEB/WIFI`).
  Without it the firmware reports a fresh random MAC on every query and
  unicast RX is broken.

## Bring-up

Run at boot (`tools/S99bt`) or by hand:

1. `tools/connsys-up.sh` — insmods `btif` → `wmt` → `bt`, creates the char
   device nodes, starts the resident `mtk_stp_launcher` (firmware download +
   WMT handshake), keeps PSM off during bring-up, and runs an HCI-reset
   smoke test. **If the smoke test fails, reboot; never retry or rmmod.**
2. `tools/bt-up.sh` — full stack: prerequisites (quick-sleep off, module
   sanity check), BT/HID kernel modules, the `stpbt-vhci-bridge` (creates
   `hci0` and governs PSM), D-Bus + bluetoothd, adapter power-on.
3. `tools/wifi-up.sh` — Wi-Fi: insmods `cfg80211` + `wlan_gen2`, re-asserts
   PSM off, function-on via `/dev/wmtWifi`, waits for `wlan0`. Then the
   usual `iw` / `wpa_supplicant -D nl80211` / `udhcpc` flow.

Pairing a classic HID keyboard end-to-end is documented in
`tools/kbd-pair.md`.

## Power management (PSM)

PSM (the firmware's sleep mode) works, but only with all three of these in
place — each was a hard-won fix, see the commit history:

1. **`AP2CONN_OSC_EN` (TOPCKGEN `0x10001800` bit 16)** is forced on at
   CONSYS power-on: it grants the 26 MHz reference while the AP is awake.
   Without it the first sleep is unrecoverable.
2. **Quick-sleep is disabled** (`echo '1 0' > /proc/driver/wmt_dbg`): the
   driver defaults to the Android screen-off policy (immediate re-sleep
   after every wake) because the notifiers that would clear it don't exist
   on mainline. The firmware does not survive that policy under load.
3. **The vhci bridge governs PSM at runtime**: the firmware dies if a WMT
   SLEEP lands during a long RF operation (inquiry ≈ 10 s, paging ≈ 5 s),
   which produce no transport traffic for the 30 ms idle timer to notice.
   The bridge watches the HCI stream and holds PSM off while commands are
   outstanding or an RF operation is in flight, and enables it (30 ms idle)
   when quiescent. Idle ACL links sleep too: inbound data (e.g. a keystroke
   from a connected keyboard) wakes the host through the BGF EINT +
   HOST_AWAKE exchange.

**The governor only sees BT traffic — it is blind to Wi-Fi.** A WMT SLEEP
landing during an active Wi-Fi operation kills the firmware the same way,
so keep PSM off while Wi-Fi is in use (`wifi-up.sh` re-asserts this;
don't run the bridge's PSM governor in Wi-Fi sessions).

Additionally, chip-initiated wakes (BGF EINT) must be answered with the
`HOST_AWAKE` command exchange, not the `WAKEUP` pulse — the stock remap in
`wmt_lib_ps_do_host_awake()` wedges this firmware on the first inbound
event; this tree carries the fix.

`tools/psm-sleep-probe.sh` verifies wake-from-sleep across ascending idle
windows (10 s → 360 s) and stops at the first failure.

## Tools

| Tool | Purpose |
|---|---|
| `connsys-up.sh` | one-shot CONSYS bring-up + HCI smoke test |
| `bt-up.sh` / `S99bt` | full BT stack bring-up (boot service) |
| `wifi-up.sh` | Wi-Fi bring-up: wlan modules, func-on, waits for `wlan0` |
| `stpbt-vhci-bridge.c` | `/dev/stpbt` ↔ `/dev/vhci` pump (creates `hci0`), H4 reframing, firmware quirk shims, PSM governor |
| `launcher/stp_uart_launcher.c` | resident WMT launcher (`-m 3` = BTIF mode): firmware download + handshake |
| `btif-lpbk-test.c` | BTIF DMA loopback test (non-blocking; safe on the single-open device) |
| `stpbt-hci-test.c` | HCI reset smoke test over `/dev/stpbt` |
| `hci-localver.c` | HCI Read Local Version over `/dev/stpbt` |
| `btup-scan.c` | minimal inquiry scan over `hci0` |
| `psm-sleep-probe.sh` | PSM deep-sleep wake threshold probe |
| `connsys-regdump.sh` | CONSYS-related register dump (devmem) |
| `deploy-modules-sd.sh` | sync modules + tools + scripts onto an SD-card rootfs |
| `kbd-pair.md` | classic BT HID keyboard pairing runbook |

## Operational notes

- `/dev/stpbt` is **single-open** and a blocking read from a shell wedges
  the console; use the provided non-blocking tools.
- Never `rmmod mtk_stp_wmt_soc` — it hangs uninterruptibly; reboot instead.
- A crashed radio ("evt err trigger assert fail, do chip reset to recovery"
  in dmesg) comes back half-alive: HCI answers but outbound paging is dead.
  Reboot; don't trust any test result after that line.
- Do not access BTIF registers with `devmem` while the block is idle — the
  bus hangs and the watchdog reboots the board.

## Origins and license

GPL-2.0. Derived from:

- MediaTek downstream ALPS kernel (`conn_soc` stack, MT6572/MT6582
  platform glue) — 3.4 era, via `l33tnoob/MT65x2_kernel_lk`;
- `frank-w/BPI-Router-Linux` 4.14 forward-port of the same stack (used as
  the API-churn roadmap);
- the `combo_tool` STP launcher from the AOSP MediaTek vendor tree.
