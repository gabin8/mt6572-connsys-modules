#!/bin/sh
# CONSYS clock-hunt register dump — run IDENTICALLY on stock Android (adb,
# with BT enabled) and on mainline (UART shell, after connsys-spike.sh).
# Diff the two outputs to find the register that makes the conn2ap bridge run.
#
# Usage: sh connsys-regdump.sh [path-to-devmem]   (default: devmem in PATH)
# Output: "ADDR: VALUE" lines on stdout.
#
# CONN-space probes are LAST: if they bus-hang, everything else is already out.

DM="${1:-devmem}"

dump_range() {  # dump_range START END_INCLUSIVE
	a=$1
	while [ "$a" -le "$2" ]; do
		printf '0x%08X: %s\n' "$a" "$($DM "$a" 2>/dev/null || echo ERR)"
		a=$((a + 4))
	done
}

echo "== TOPCKGEN 0x10000000 (CG STA, DCM, FREDIV, CLK_CFG, muxes) =="
dump_range $((0x10000000)) $((0x100000FC))

echo "== INFRACFG 0x10001000 head =="
dump_range $((0x10001000)) $((0x1000107C))
echo "== INFRACFG bus-prot / 0x1310 area / OSC_EN =="
dump_range $((0x10001220)) $((0x10001230))
dump_range $((0x10001300)) $((0x10001330))
printf '0x10001800: %s\n' "$($DM 0x10001800)"

echo "== RGU 0x10007000 =="
dump_range $((0x10007000)) $((0x10007030))

echo "== SPM 0x10006000 =="
dump_range $((0x10006000)) $((0x100062FC))
dump_range $((0x10006400)) $((0x1000641C))
dump_range $((0x10006600)) $((0x1000661C))

echo "== APMIXED 0x10205000 (all PLLs) =="
dump_range $((0x10205000)) $((0x102051FC))

echo "== CONN probes (LAST - may hang if conn dead) =="
printf 'CHIP_ID  0x18070008: %s\n' "$($DM 0x18070008)"
printf 'CPUPCR   0x18070160: %s\n' "$($DM 0x18070160)"
printf 'BTIF_TRI 0x1100C060: %s\n' "$($DM 0x1100C060)"
echo "== done =="
