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
| Bluetooth (`/dev/stpbt` → BlueZ `hci0`) | working — pairing, BR/EDR HID keyboard + BLE HID mouse together, inbound reconnect (see [kbd-pair.md](tools/kbd-pair.md)) |
| Power management (PSM / chip sleep) | working — sleeps on an idle link with BT and Wi-Fi up (see [PSM](#power-management-psm)) |
| WiFi (`wlan/` gen2 driver → cfg80211 `wlan0`) | working — scan, WPA2-PSK association, DHCP, station stats, ~34/22 Mbit/s TCP down/up |
| BT + WiFi together | working with Wi-Fi in CAM, the default — HID keyboard + mouse alongside Wi-Fi traffic; with 802.11 power save on, BT HID traffic makes the BT firmware assert, and a BLE mouse can still drop now and then (see [Operational notes](#operational-notes)) |
| WiFi AP / P2P (Wi-Fi Direct) | not started — hardware and firmware support it, driver sources are in git history (see [AP / P2P](#ap--p2p)) |
| FM receiver (`fmradio/` MT6627 driver → `/dev/fm`) | working — tune, scan/seek, RDS (PI, station name, radio text), audio to the headphones through the AFE's CONSYS I2S input, recording and streaming through the AFE's capture device; the headphone cable is the antenna |
| GPS (`conn_soc/` → `/dev/stpgps`) | working — first fix in about 50 s from an empty aiding store, about 10 s warm, with the device's own stock positioning engine; holds the chip awake while open, alongside BT; `tools/gps-up.sh` serves it through gpsd (see [GPS](#gps)) |

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
- Char devices: `/dev/stpwmt` (control, major 190), `/dev/stpgps` (GPS,
  major 191), `/dev/stpbt` (BT HCI, major 192), `/dev/wmtWifi` (WiFi func
  ctrl, major 155), `/dev/fm` (FM receiver, dynamic major).
- FM audio leaves CONSYS digitally: the WMT audio-interface command puts it
  in FM-I2S mode, the receiver drives the AFE's I2S input as master at
  32 kHz, and the kernel's AFE driver resamples it onto the downlink.

## Kernel prerequisites

Built and verified against **linux-next 7.2.0-rc4-next-20260721**. The
forward-port tracks current APIs — `cfg80211` ops that take a
`wireless_dev *`, `eth_hw_addr_set()` for the netdev MAC, and
`kernel_read_file_from_path_initns()` for firmware/NVRAM — so building
against a materially older tree needs those three reverted, not just
adjusted.

The kernel tree the modules build against must provide:

- MT6572 platform support (clocks, pinctrl, pwrap/MT6323 regulators) plus:
  - `btif@1100e000` node (`mediatek,mt6572-btif`) with `btif_tx`/`btif_rx`
    APDMA nodes,
  - `connectivity@18070000` node (`mediatek,mt6572-consys`) with the MCU +
    TOPCKGEN reg ranges, the `conn2ap` wakeup + WDT interrupts, the CONN
    power domain, the TOPRGU `connsys` reset and the four `vcn*` supplies,
    and on boards with an external GPS LNA the `gps_lna_state_init`,
    `gps_lna_state_oh` and `gps_lna_state_ol` pinctrl states that drive its
    enable pin (low, high, low),
  - `wifi@180f0000` node (`mediatek,wifi`, the CONSYS Wi-Fi AHB window,
    GIC SPI 123 level-low) for the wlan driver;
- `CONFIG_BT=m` core with `CONFIG_BT_HCIVHCI=m`, `CONFIG_BT_HIDP=m`,
  `CONFIG_UHID=m` for the BlueZ/HID path, plus the SMP pairing crypto the
  BT core selects — `ecc`, `ecdh_generic` and `kpp`. Whether each of those
  is a module or built in varies with the config: `bt-up.sh` loads the ones
  that exist as `.ko` and skips the rest, so either is fine (`libaes` is
  typically `CONFIG_CRYPTO_LIB_AES=y` and has no module at all);
- `CONFIG_CFG80211` (=m works; insmod `cfg80211.ko` before `wlan_gen2.ko`);
- for FM audio, the MT6572 AFE driver's FM path: the `FM Playback Switch`
  and `FM Playback Volume` controls, which take the CONSYS I2S input
  through its ASRC and HW gain 2 onto the downlink, and for recording its
  AWB capture device (card 0 device 1, "AWB Capture", 44.1 kHz stereo S16),
  which takes the same resampled stream into memory. The receiver works
  without them, but stays silent. `fm-up.sh -s` also needs the sound
  card's `Speaker Switch`, to keep the speaker on with the headphones in.

Build the kernel once (`make modules`) so `Module.symvers` exists.

## Building

```sh
make                    # expects the kernel tree at ../kernel/linux
make KDIR=/path/to/tree # or point at any prepared kernel tree
```

Produces `btif/mtk_btif_drv.ko`, `conn_soc/mtk_stp_wmt_soc.ko`,
`conn_soc/mtk_stp_bt_soc.ko`, `conn_soc/mtk_wmt_wifi_soc.ko`,
`conn_soc/mtk_stp_gps_soc.ko`, `wlan/wlan_gen2.ko`, `fmradio/mtk_fm_drv.ko`.

The wlan driver links against cfg80211; with `CONFIG_CFG80211=m` build it
first (`tools/build-staged-modules.sh` does this, and also stages the
BT/HID mainline modules the deploy ships) — the top Makefile picks up its
`Module.symvers` automatically.

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
arm-linux-gnueabihf-gcc -static -O2 -I tools/launcher -o tools/launcher/mtk_stp_launcher tools/launcher/stp_uart_launcher.c
arm-linux-gnueabihf-gcc -static -O2 -I fmradio/inc -o tools/fmctl tools/fmctl.c
```

## Firmware

All CONSYS firmware ships with the stock device image and is **not**
redistributed here. Run `tools/wifi-fw-extract.sh` on the device (booted
into the mainline rootfs, stock image still on eMMC) and it extracts and
installs every Wi-Fi and FM piece; or place them by hand:

- `/system/etc/firmware/`: `mt6572_82_patch_e1_{0,1}_hdr.bin` — **also
  copied as `ROMv1_patch_{0,1}_hdr.bin`**: outside Android the launcher
  cannot read the system properties it derives the patch prefix from and
  falls back to `ROMv<hwver>_patch`; with no match it reports zero patches,
  the chip runs bare ROM firmware, BT still works but the Wi-Fi RAM code
  dies at entry — and `WMT_SOC.cfg`;
- `/lib/firmware/`: `WIFI_RAM_CODE_MT6582` (the Wi-Fi RAM image, loaded by
  the wlan driver via request_firmware);
- `/lib/firmware/`: `mt6627_fm_v*_patch.bin` / `mt6627_fm_v*_coeff.bin`
  (FM DSP patch and coefficients, stock keeps them in
  `/system/etc/firmware/mt6627/`). The driver picks the pair for the chip's
  DSP ROM version - v1 on these boards, the other versions ship empty;
- `/etc/firmware/nvram/WIFI`: the device's Wi-Fi NVRAM (RF calibration,
  514 B, stock keeps it at `/data/nvram/APCFG/APRDEB/WIFI`). Without it
  unicast RX is broken — the per-BSS receive filter and the netdev address
  disagree and the firmware drops every unicast data frame, EAPOL included.
  It carries the factory MAC too, at offset 4 —
  `_MT6620_CFG_PARAM_STRUCT` starts with two version u16s and then
  `aucMacAddress[6]`. `wlan0` comes up with it, matching stock byte for
  byte.
- `/etc/firmware/nvram/BT_Addr`: the device's BT address (stock keeps it at
  `/data/nvram/APCFG/APRDEB/BT_Addr`; the BD_ADDR is the first six bytes).
  The bridge programs it with vendor opcode `0xfc1a` before the core sees
  the device. Without it the controller keeps a firmware default address.

## Bring-up

Run at boot (`tools/S99bt`) or by hand:

1. `tools/connsys-up.sh` — insmods `btif` → `wmt` → `bt`, creates the char
   device nodes, starts the resident `mtk_stp_launcher` (firmware download +
   WMT handshake), keeps PSM off during bring-up, and runs an HCI-reset
   smoke test. **If the smoke test fails, reboot; never retry or rmmod.**
2. `tools/bt-up.sh` — full stack: prerequisites (quick-sleep off, module
   sanity check), BT/HID kernel modules, the `stpbt-vhci-bridge` (creates
   `hci0` and governs PSM), D-Bus + bluetoothd, adapter power-on.
3. `tools/wifi-up.sh` — Wi-Fi: insmods `cfg80211` + `wlan_gen2`,
   function-on via `/dev/wmtWifi`, waits for `wlan0`. Then the usual
   `iw` / `wpa_supplicant -D nl80211` / `udhcpc` flow. PSM needs no
   manual handling: the driver holds a keep-awake reference whenever Wi-Fi
   is busy and lets the chip sleep when it is not.

4. `tools/fm-up.sh [-b] [-n] [-s] [MHz]` — FM radio: insmods `mtk_fm_drv`,
   turns the AFE FM route on and runs `fmctl`, in the foreground or (`-b`)
   in the background on the FIFO `/tmp/fmin`; `-s` plays on the speaker as
   well, with the headphones in. Headphones first - their
   cable is the antenna.
5. `tools/fm-record.sh <MHz> [seconds] [file|-]` — record a station as a
   44.1 kHz stereo WAV, or stream it (`-` = stdout, e.g. into `nc`).

Listening, the `fmctl` commands, recording, streaming and troubleshooting
are in `tools/fm.md`.

Pairing a classic HID keyboard end-to-end is documented in
`tools/kbd-pair.md`.

GPS runs on demand with `tools/gps-up.sh start|stop|status`; see [GPS](#gps)
and `tools/README.md`.

## GPS

- `conn_soc/mtk_stp_gps_soc.ko` provides `/dev/stpgps` (major 191).
  Opening it turns the WMT GPS function on, powering CONSYS up if nothing
  else is on, and closing it turns it off again. In between the node
  carries the raw stream between the GPS firmware and a positioning engine,
  which does all of the navigation; on its own the firmware stays silent.
  One opener at a time; a whole-chip reset shows up as `EIO` on read/write
  and `POLLERR` until the node is reopened.
- While the node is open it holds the chip awake (the keep-awake reference
  Wi-Fi uses), and PSM goes back to its policy on close: GPS streams once a
  second, which under the 30 ms idle timer would be a sleep/wake handshake
  every second for as long as the receiver is on. A whole-chip reset during
  a session drops the GPS LNA enable as well.
- The positioning engine is MediaTek's MNL, which is proprietary and not
  part of this repository. The one verified is the device's own stock-ROM
  engine (`/system/xbin/libmnlp_mt6572`, Android 4.2.2), a bionic binary run
  directly in a chroot of the stock `/system` from the eMMC. It needs
  `/dev/stpgps`, a writable `/data/misc` for its aiding data and the GPS
  calibration record from the stock `/data/nvram/APCFG/APRDEB/GPS`; it
  writes NMEA to `/dev/gps` (a plain file or a pipe will do) and logs through
  liblog to `/dev/log/main`. `mnld` and the GPS HAL are not needed: started
  without arguments, the engine positions straight away.
- An external LNA has to be powered while GPS is on: that is what the
  `gps_lna_state_*` pinctrl states on the consys node are for. On the
  PAP5500 DUO (enable on GPIO138) the receiver tracks no satellites without
  it.
- Measured on the PAP5500 DUO: a first fix from an empty aiding store in
  about 50 s, warm starts in about 10 s, up to 10 satellites used at HDOP
  below 1, and reported satellite elevations and azimuths within a few
  degrees of the published orbits. BT keeps working alongside GPS: BT
  commands and inquiry scans during a GPS session all succeeded.
- Quirks of this 2013 engine:
  - NMEA dates are exactly 1024 weeks back (GPS week rollover; add 7168
    days). Time of day and position are not affected.
  - A stationary receiver's position and speed are held (static
    navigation).
  - Ioctl 7 (RTC power-loss flag) answers 0, as stock does. Answering 1
    makes every start a no-time start: 40-70 s to a fix instead of 10 s.
- `tools/gps-up.sh` runs the engine on demand: engine → FIFO →
  `tools/gps-nmea` (the RMC date moved forward 1024 weeks, spoofed fixes
  withheld; an unset clock taken from the first fix that does not look
  foreign) → gpsd on localhost:2947. Details in `tools/README.md`.

## Power management (PSM)

PSM (the firmware's sleep mode) works, but only with all four of these in
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

4. **Wi-Fi holds a keep-awake reference while it needs one.** The bridge's
   governor watches the HCI stream and is blind to Wi-Fi — the Wi-Fi
   datapath runs over the CONSYS AHB HIF and never touches BTIF, so an
   actively transferring link looks idle to it. `wlan_gen2` keeps its own
   idle notion and holds a reference while the datapath moved a frame
   recently, while a scan is outstanding, and until the link is
   established; `wmt_lib_ps_enable()` refuses to enable sleep while any
   reference is held. The chip therefore sleeps on an associated but quiet
   link and the BT governor stays safe unmodified. `wifi_psm_idle_ms`
   (module parameter, writable at runtime) sets the idle window, default
   500 ms; `0` restores the old always-awake behaviour.
   Do **not** force `'0 0'` by hand for Wi-Fi any more: that clears
   `gPsEnable` globally and stops BT sleeping for the rest of the session.

Additionally, chip-initiated wakes (BGF EINT) must be answered with the
`HOST_AWAKE` command exchange, not the `WAKEUP` pulse — the stock remap in
`wmt_lib_ps_do_host_awake()` wedges this firmware on the first inbound
event; this tree carries the fix.

Two races in the stock sleep/wake handling are fixed as well. Both ended in
a PSM wait timeout and a whole-chip reset:

- **The wrong wake exchange.** A chip that raised its wake interrupt waits
  for `HOST_AWAKE`; a sleeping chip that did not answers only the `WAKEUP`
  pulse, so `HOST_AWAKE` sent to it times out (`read HOST_AWAKE_EVT fail`).
  The PSM queue folds the two requests into one, and stock armed the wake
  interrupt as soon as a sleep was queued, before the chip had acknowledged
  it, so a request near a sleep/wake transition could get the wrong
  exchange. wmtd now picks the exchange when it sends it, from whether the
  chip asked (the interrupt fired, or the line is still asserted); the
  interrupt is armed once the chip has acknowledged the sleep; and
  power-save events get the 2 s wait stock uses for every event (20 s
  here), inside the PSM's 6 s window.
- **PSM switched on under a WMT command.** WMT commands run with the PSM
  monitor stopped, but a PSM policy change in the middle of one (the BT
  governor's `0 1e`, the last keep-awake dropped, the enable at the end of
  chip init) restarted it: the chip went to sleep under the command, wmtd
  waited out the whole RX timeout, and the chip's own wake request queued
  behind it. A policy change during a command is now recorded and applied
  when the command ends.

`tools/psm-sleep-probe.sh` verifies wake-from-sleep across ascending idle
windows (10 s → 360 s) and stops at the first failure.

## Tools

How to run and build each one, and the rules they share, are in
[tools/README.md](tools/README.md).

| Tool | Purpose |
|---|---|
| `connsys-up.sh` | one-shot CONSYS bring-up + HCI smoke test |
| `bt-up.sh` / `S99bt` | full BT stack bring-up (boot service) |
| `wifi-up.sh` | Wi-Fi bring-up: wlan modules, func-on, waits for `wlan0` |
| `fm-up.sh` | FM bring-up: driver, AFE route, `fmctl` in the foreground or (`-b`) in the background on `/tmp/fmin` |
| `fm-record.sh` | record or stream a station from the AFE capture device |
| `fm.md` | FM radio runbook: listening, `fmctl` commands, recording, streaming, troubleshooting |
| `fmctl.c` | FM receiver control over `/dev/fm`: tune, soft-mute seek/scan, volume, RDS station name/text, register access; mutes while the headphones are out |
| `stpbt-vhci-bridge.c` | `/dev/stpbt` ↔ `/dev/vhci` pump (creates `hci0`), H4 reframing, firmware quirk shims, PSM governor |
| `launcher/stp_uart_launcher.c` | resident WMT launcher (`-m 3` = BTIF mode): firmware download + handshake |
| `btif-lpbk-test.c` | BTIF DMA loopback test (non-blocking) |
| `stpbt-hci-test.c` | HCI reset smoke test over `/dev/stpbt` |
| `stpgps-probe.c` | `/dev/stpgps` smoke test: GPS function on/off, the ioctls, single-opener check |
| `hci-localver.c` | HCI Read Local Version over `/dev/stpbt` |
| `btup-scan.c` | minimal inquiry scan over `hci0` |
| `fbcursor.c` | draws a cursor on `/dev/fb0` from an evdev pointer - checks a BT mouse end to end with no display server |
| `evrep.c` | sets the evdev autorepeat delay/period (BT HID keyboards need a longer delay, see [kbd-pair.md](tools/kbd-pair.md)) |
| `psm-sleep-probe.sh` | PSM deep-sleep wake threshold probe |
| `connsys-regdump.sh` | CONSYS-related register dump (devmem) |
| `build-staged-modules.sh` | rebuild the staged mainline modules (cfg80211, BT/HID set) from a kernel tree |
| `deploy-modules-sd.sh` | sync modules + tools + scripts onto an SD-card rootfs |
| `kbd-pair.md` | classic BT HID keyboard pairing runbook |

## AP / P2P

Not built today, but nothing in the hardware or firmware stands in the way -
this is a porting job, not a capability question.

What already exists:

- The MAC/baseband is the same CONSYS core the stock MT6582-class driver
  drives, and the RAM firmware we load (`WIFI_RAM_CODE_MT6582`) is the stock
  image, so the firmware-side AP and P2P command interfaces are already
  there. Stock Android on this board advertised `android.hardware.wifi.direct`
  and shipped a p2p_supplicant configuration, so the combination shipped on
  this exact hardware.
- The driver's own P2P/AP stack exists in this repository's history. It is
  compiled out (`-DCFG_ENABLE_WIFI_DIRECT=0` in `wlan/Kbuild`), and the
  sources were deleted as never-compiled dead code in `6b5107f` - 40 files,
  27k lines, 15 of them `.c`: the `p2p_*` FSM/assoc/scan/IE/RLM set under
  `wlan/mgmt/`, `wlan/nic/p2p_nic.c`, `wlan/common/wlan_p2p.c`, and the
  Linux glue `gl_p2p{,_cfg80211,_init,_kal}.c`. Recover any of them with
  `git show 6b5107f^:wlan/mgmt/p2p_fsm.c`.

What it would take:

1. Restore those sources and add them back to `wlan/Kbuild`.
2. Build with `CFG_ENABLE_WIFI_DIRECT=1` (and
   `CFG_ENABLE_WIFI_DIRECT_CFG_80211` for the cfg80211 path).
3. Register the second (P2P) net device and its cfg80211 ops alongside
   `wlan0`.
4. Userspace: wpa_supplicant built with AP/P2P support - the rootfs here
   currently builds it without (`BR2_PACKAGE_WPA_SUPPLICANT_AP_SUPPORT` is
   unset) - or hostapd for plain AP.

The real work is step 2-3: `gl_p2p_cfg80211.c` targets the vendor's
cfg80211 vintage, so it needs the same adaptation to current kernel APIs
that the station path already went through. Treat the recovered files as a
reference implementation rather than something that will compile as-is.

## Operational notes

- `/dev/stpbt` may be opened more than once — the BT function is
  refcounted, so it stays on until the last close. A blocking read from a
  shell still wedges the console; use the provided non-blocking tools.
- Never `rmmod mtk_stp_wmt_soc` — it hangs uninterruptibly; reboot instead.
- A crashed radio ("evt err trigger assert fail, do chip reset to recovery"
  in dmesg) comes back half-alive: HCI answers but outbound paging is dead.
  Reboot; don't trust any test result after that line.
- Do not access BTIF registers with `devmem` while the block is idle — the
  bus hangs and the watchdog reboots the board.
- The WMT firmware patches and the Wi-Fi NVRAM are read with
  `kernel_read_file_from_path_initns()`, not a plain `filp_open()`. The
  patch download runs on the `mtk_wmtd` kernel thread, whose fs root is the
  boot root — and an initramfs that `switch_root`s to a rootfs on storage
  leaves that root empty, so any absolute path fails with `-ENOENT` while
  the same file reads fine from userspace. Symptom if this regresses:
  `wmt_dev_patch_get ... fail, iRet(-2)` then `BT_open: WMT turn on BT
  fail!`, or `[nvram_read] : failed to open!!` followed by a random
  `00:08:22:xx:xx:xx` MAC — with the files plainly present on disk.
- Wi-Fi throughput is ~34 Mbit/s TCP down and ~22 Mbit/s up (2.4 GHz HT20,
  65 Mbit/s PHY, -56 dBm), at 22% CPU across both cores - so the radio is
  the limit, not the SoC. Uplink was measured with a raw TCP sender because
  busybox wget's --post-file sends a zero-length body in this build. The
  idle-gated keep-awake costs nothing measurable: interleaved runs with
  `wifi_psm_idle_ms` 0 and 500 are within noise of each other.
- Closing `/dev/fm` powers the receiver down, so whatever drives it has to
  keep it open - `fmctl` stays in the foreground for that reason, or in the
  background on a FIFO under `fm-up.sh -b`. The driver
  has no hardware seek: `fmctl` steps the band with soft-mute tunes and the
  chip's per-channel "valid station" verdict, as MediaTek's own FM service
  does.
- While FM plays it owns the downlink rate (44.1 kHz); DL1 playback at
  another rate is refused until FM is switched off.
- RDS needs a steady signal: with the headphone cable as antenna a station
  around -75 dBm delivers PI, station name and radio text, but not all the
  time. The MT6627's error correction lets some miscorrected blocks pass its
  CRC, and its corrected-bit count is not reliable enough to reject them, so
  the parser takes a new PI and each radio text segment only after they
  arrive twice alike. `fmctl`'s `b` counts the groups the parser accepted;
  the chip's own block counters only run during a block-error measurement
  and stay at 0.
- BT and Wi-Fi do run together (verified: a full BT inquiry alongside a
  25-packet ping, 0% loss, no assert), but the inquiry monopolises the
  shared MT6627N front end in bursts — round-trip average went from ~20 ms
  to ~100 ms with a ~390 ms worst case, measured with the chip still
  sleeping 19 times during the inquiry. That cost is the hardware, not this
  stack: stock Android on the same board shows ~517 ms average and 1.1 s
  worst case under the same load.
- Wi-Fi power save and Bluetooth HID traffic do not mix. BT and Wi-Fi share
  one antenna (`WMT_SOC.cfg`: `coex_wmt_ant_mode=1`). With Wi-Fi associated
  in 802.11 power save, a classic keyboard and a BLE mouse in use make the
  BT firmware assert (`bluetooth/core/ll/ll_lc/ll_lc_log.c #223`) and WMT
  resets the whole chip - 9 times in about 500 s in one run. With the
  interface in CAM there were none in the following 36 minutes, so `wlan0`
  comes up in CAM. Power save is still available per interface
  (`iw dev wlan0 set power_save on`); turn it on only while no Bluetooth
  device is connected.
- Even in CAM, a BLE mouse intermittently drops on LE supervision timeout
  (`0x08`) while a classic keyboard and Wi-Fi are both active, and
  reconnecting it while the keyboard stays connected has been unreliable.
  With the Wi-Fi function off neither happens. The chip's own coexistence
  controls are not wired up: the `coex_*` keys in `WMT_SOC.cfg` are parsed
  but never sent (`CFG_SUBSYS_COEX_NEED` is 0), and the Wi-Fi driver's
  BWCS path is compiled out (`CFG_SUPPORT_BCM` is 0).

## Origins and license

GPL-2.0. Derived from:

- MediaTek downstream ALPS kernel (`conn_soc` stack, MT6572/MT6582
  platform glue) — 3.4 era, via `l33tnoob/MT65x2_kernel_lk`;
- `frank-w/BPI-Router-Linux` 4.14 forward-port of the same stack (used as
  the API-churn roadmap);
- the `combo_tool` STP launcher from the AOSP MediaTek vendor tree;
- the MediaTek FM driver (`connectivity/fmradio`) as packaged out of tree in
  [mt8768-modules](https://codeberg.org/lowendlibre/mt8768-modules), MT6627
  chip support only.
