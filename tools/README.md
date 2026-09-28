# Tools

Everything here runs on the device from `/root/connsys`, where
`deploy-modules-sd.sh` installs the modules, scripts and the resident
programs. The diagnostics are not deployed; copy them next to the rest when
needed. Two tasks have their own runbooks: [fm.md](fm.md) (FM radio) and
[kbd-pair.md](kbd-pair.md) (pairing a BT HID keyboard).

## Rules for all of them

- **Never `rmmod mtk_stp_wmt_soc`.** It hangs uninterruptibly; reboot
  instead.
- **A failed HCI smoke test or a chip reset means reboot.** After "evt err
  trigger assert fail, do chip reset to recovery" in dmesg the radio answers
  HCI but outbound paging is dead. Don't retry, and don't trust any result
  after that line.
- **No blocking reads of `/dev/stpbt` from a shell** (`cat`, `hexdump`):
  they wedge the console. The C tools below open it non-blocking with
  timeouts.
- **No `devmem` on BTIF registers while the block is idle.** The bus hangs
  and the watchdog reboots the board.
- **Modules match the running kernel exactly.** After a kernel rebuild,
  rebuild and redeploy all of them: the connsys modules, the staged
  mainline modules and `mtk_fm_drv.ko`.

## Building the C tools

Static ARM builds, from the repository root:

```sh
CC="arm-linux-gnueabihf-gcc -static -O2"
$CC -I tools/launcher -o tools/launcher/mtk_stp_launcher tools/launcher/stp_uart_launcher.c
$CC -o tools/stpbt-vhci-bridge tools/stpbt-vhci-bridge.c
$CC -o tools/stpbt-hci-test    tools/stpbt-hci-test.c
$CC -I fmradio/inc -o tools/fmctl tools/fmctl.c
# diagnostics, as needed
$CC -o tools/hci-localver   tools/hci-localver.c
$CC -o tools/btif-lpbk-test tools/btif-lpbk-test.c
$CC -o tools/btup-scan      tools/btup-scan.c
$CC -o tools/fbcursor       tools/fbcursor.c
$CC -o tools/evrep          tools/evrep.c
```

`deploy-modules-sd.sh` refuses to run without the launcher,
`stpbt-hci-test` and `fmctl`, since nothing else provides them.

## Bring-up

### `connsys-up.sh`

The CONSYS core: insmods `mtk_btif_drv` → `mtk_stp_wmt_soc` →
`mtk_stp_bt_soc` and creates `/dev/stpwmt` and `/dev/stpbt`. It then starts
the resident launcher (log in `/tmp/launcher.log`), turns PSM off for the
bring-up and runs `stpbt-hci-test`. The last line is `HCI_TEST_RC=<n>`;
anything but 0 means reboot. Every step checks before acting, so a second
run against a running stack is harmless. It skips the smoke test while the
bridge holds `/dev/stpbt`.

### `bt-up.sh`, `S99bt`

The full Bluetooth stack on top of `connsys-up.sh`:
1. PSM prerequisites: quick-sleep off, and a check that the WMT module
   forces `AP2CONN_OSC_EN`. Without it PSM stays off.
2. The BT/HID kernel modules from `bt/`.
3. `stpbt-vhci-bridge`.
4. D-Bus and `bluetoothd`.
5. Adapter power-on, with retries.

It uses a BlueZ under `/opt/bt` if present, otherwise the rootfs one. It
logs to `/root/bt-up.log`; the bridge and `bluetoothd` log to
`/root/bridge.log` and `/root/btd.log`.

`S99bt` is the init script that starts `bt-up.sh` at boot, in the
background (about 10 s).

### `wifi-up.sh`

Wi-Fi, after `connsys-up.sh`: insmods `mtk_wmt_wifi_soc`, `cfg80211` and
`wlan_gen2`, turns the function on through `/dev/wmtWifi` and waits up to
20 s for `wlan0`. It needs the `WIFI_RAM_CODE*` image in `/lib/firmware`.
Then use the usual `iw` / `wpa_supplicant -D nl80211` / `udhcpc`. Leave PSM
alone: `wlan_gen2` holds its own keep-awake reference, and forcing
`'0 0'` into `wmt_dbg` stops BT sleeping for the rest of the session.

### `fm-up.sh`, `fm-record.sh`, `fmctl`

FM radio: listen, seek, scan and read RDS with `fm-up.sh [-b] [-n] [MHz]`;
record or stream with `fm-record.sh <MHz> [seconds] [file|-]`. `fmctl` is
the `/dev/fm` client both scripts drive. Everything is in [fm.md](fm.md).

## Resident programs

### `mtk_stp_launcher` (`launcher/stp_uart_launcher.c`)

MediaTek's WMT launcher, run as `mtk_stp_launcher -m 3 -p
/system/etc/firmware/` (mode 3 = BTIF). It stays resident and serves the
firmware patch information WMT asks for at every function-on. Without it,
BT and Wi-Fi are both dead. Outside Android it cannot read the system
properties that give the patch name prefix. It falls back to
`ROMv<hwver>_patch`, which is why the patches are installed under both
names (see the Firmware section of the top-level README). Use this glibc
build: the stock Android binary needs the bionic linker and fails with a
bare "not found".

### `stpbt-vhci-bridge`

Connects `/dev/stpbt` to `/dev/vhci`, so the kernel's BT core gets a real
`hci0` for BlueZ. Kill it to tear `hci0` down. Besides pumping packets it:
- reframes the H4 stream: `/dev/stpbt` reads split packets anywhere, and
  vhci wants whole ones;
- primes STP with a throwaway HCI_Reset. The first command after
  function-on runs twice, and the late duplicate completion would desync
  the core's adapter setup;
- programs the factory BD_ADDR from `/etc/firmware/nvram/BT_Addr` with
  vendor opcode `0xfc1a`;
- widens LE Set Event Mask to the events the firmware needs to report LE
  connections;
- governs PSM through `/proc/driver/wmt_dbg`. It holds sleep off while
  commands are outstanding or an inquiry or page is in flight, and allows
  it after 1 s of quiet. A WMT sleep that lands mid-inquiry kills the
  firmware.

The PSM fix only works with the matching `mtk_stp_wmt_soc.ko` and
`bt-up.sh`, so deploy all three together.

## Diagnostics

| Tool | Run | Checks |
|---|---|---|
| `stpbt-hci-test` | `./stpbt-hci-test` | HCI_Reset over `/dev/stpbt`, waiting for Command Complete with a hard timeout; exit code 0 = alive. `connsys-up.sh` runs it. |
| `hci-localver` | `./hci-localver` | HCI Read Local Version over `/dev/stpbt`, printed as raw event bytes |
| `btif-lpbk-test` | `./btif-lpbk-test [len] [hold_s] [pio]` | The AP-side BTIF block alone: internal loopback, pattern out and back, DMA by default. `pio` forces PIO, `hold_s` keeps the port looped for inspection. Run it with only `mtk_btif_drv.ko` loaded, before `connsys-up.sh`. |
| `btup-scan` | `./btup-scan` | Brings `hci0` up and runs a ~10 s inquiry with raw HCI ioctls, no BlueZ needed; lists addresses and classes |
| `fbcursor` | `./fbcursor /dev/input/eventN [/dev/fb0]` | Draws a cursor from a relative pointer, to check a BT mouse end to end without a display server |
| `evrep` | `./evrep /dev/input/eventN [delay_ms] [period_ms]` | Sets the autorepeat delay and period (default 800/33 ms). A BT keyboard in sniff mode delivers key-ups late, see [kbd-pair.md](kbd-pair.md). |
| `psm-sleep-probe.sh` | `sh psm-sleep-probe.sh` | Wake from PSM after idle windows of 10 s up to 360 s, poking the adapter through `bluetoothctl`. It stops at the first failed wake, and the radio is wedged from then on. |
| `connsys-regdump.sh` | `sh connsys-regdump.sh [devmem]` | Dumps TOPCKGEN, INFRACFG, RGU, SPM and APMIXED, then CONSYS probes last, as `ADDR: VALUE`. Run it the same way on stock Android and on mainline, then diff the two. |

## Host side

### `build-staged-modules.sh [KDIR]`

Builds the mainline modules the deploy ships next to the connsys stack:
`cfg80211` into `wifi-stage/`, and the BT/HID set (`bluetooth`, `hidp`,
`hci_vhci`, `uhid`, plus `ecc`, `ecdh_generic` and `kpp` for SMP pairing)
into `kbd-stage/`. They must be `=m` in the kernel config. It prints each
module's vermagic, which must match the running kernel. Build these before
`make`: `wlan_gen2` links against this `cfg80211`.

### `deploy-modules-sd.sh <mountpoint>`

Installs everything onto the SD rootfs from the build machine (with the
card mounted):
- modules into `/root/connsys` and its `bt/`, `wifi/` and `fm/`
  subdirectories;
- scripts, launcher, bridge and `fmctl`;
- `S99bt` into `/etc/init.d`;
- the firmware and NVRAM blobs staged in `tools/wifi-fw/` into the paths
  the drivers read.

It checks for `/sbin/init` first, so pointing it at the wrong mount fails.
The NVRAM blobs are per device: `NVRAM_BLOB=` and `BDADDR_BLOB=` override
the staged ones for another board. Run it again after every full rootfs
deploy, because a fresh rootfs has no `/root/connsys`.

### `wifi-fw-extract.sh`

Runs on the device, booted into the mainline rootfs with the stock image
still on the eMMC. It finds the stock system and data partitions by
try-mounting (busybox `blkid` sees nothing on these eMMCs) and installs:
- the Wi-Fi RAM code;
- the WMT patches, under both names;
- `WMT_SOC.cfg`;
- the FM DSP firmware;
- the Wi-Fi NVRAM.

It also copies everything to `/root/fw-extract`; carry that to the build
machine's `tools/wifi-fw/` so later deploys can reinstall without the stock
image. The environment variables `DEVPFX`, `MNT`, `OUT`, `FWDIR`, `SYSFW`
and `NVDIR` override the paths for dry runs.
