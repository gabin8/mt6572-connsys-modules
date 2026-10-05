#!/bin/sh
# gps-up.sh start|stop|status - GPS on demand (run from /root/connsys).
#
# The positioning engine is the device's own stock-ROM MediaTek MNL
# (/system/xbin/libmnlp_mt6572, Android 4.2.2), run in a chroot of the
# stock /system; it is proprietary and not part of this repository.
#
#   engine -> /dev/gps, a FIFO in the chroot -> gps-nmea (dates out of the
#   1024-week rollover, spoofed fixes withheld) -> $RUN/nmea, a FIFO -> gpsd
#
# start   sets up the chroot (first time: /data/nvram copied from the stock
#         userdata, read-only), loads mtk_stp_gps_soc.ko, then starts gpsd,
#         gps-nmea and the engine. A supervisor restarts the engine when it
#         exits or a whole-chip reset takes the GPS function away.
# stop    stops all of it; closing /dev/stpgps turns GPS and its LNA off.
# status  what runs, the filter's last verdict, gpsd's current fix.
#
# Clients talk to gpsd on localhost:2947 (gpspipe -w, cgps, libgps).
# Board settings, from the environment (defaults: PAP5500 DUO):
#   GPS_SYSTEM_DEV  stock /system partition       /dev/mmcblk1p4
#   GPS_DATA_DEV    stock userdata (/data/nvram)  /dev/mmcblk1p6
# Log: /var/log/gps-up.log
cd /root/connsys || exit 1
R=/root/andr
RUN=/run/gps
LOG=/var/log/gps-up.log
ENGINE=/system/xbin/libmnlp_mt6572
SYSTEM_DEV=${GPS_SYSTEM_DEV:-/dev/mmcblk1p4}
DATA_DEV=${GPS_DATA_DEV:-/dev/mmcblk1p6}
RESET_MSG='whole chip reset, the GPS function is off'

log() { echo "$(date -u '+%Y-%m-%d %H:%M:%S') gps-up: $*" >> $LOG; }
alive() {
	local pid

	pid=$(cat "$RUN/$1.pid" 2>/dev/null)
	[ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

setup() {
	lsmod | grep -q mtk_stp_gps_soc || insmod mtk_stp_gps_soc.ko || return 1
	mkdir -p /mnt/sys $R/system $R/dev $R/proc $R/data/misc || return 1
	mountpoint -q /mnt/sys || mount -t ext4 -o ro "$SYSTEM_DEV" /mnt/sys || return 1
	mountpoint -q $R/system || mount --bind /mnt/sys $R/system || return 1
	mountpoint -q $R/proc || mount -t proc proc $R/proc || return 1
	if ! mountpoint -q $R/dev; then
		mount -t tmpfs -o mode=755 tmpfs $R/dev || return 1
		mknod -m 666 $R/dev/null c 1 3
		mknod -m 666 $R/dev/zero c 1 5
		mknod -m 666 $R/dev/urandom c 1 9
		mknod -m 666 $R/dev/random c 1 8
		mknod -m 666 $R/dev/tty c 5 0
		mknod -m 660 $R/dev/stpgps c 191 0
		mkdir $R/dev/log	# liblog writes plain files here
	fi
	for l in main system radio events; do : > $R/dev/log/$l; done
	# the engine's configuration (26 MHz TCXO etc.) lives in the stock
	# nvram; take a copy once, never write the stock partition
	if [ ! -f $R/data/nvram/APCFG/APRDEB/GPS ]; then
		mkdir -p /mnt/gps-data
		mount -t ext4 -o ro "$DATA_DEV" /mnt/gps-data || return 1
		cp -a /mnt/gps-data/nvram $R/data/
		umount /mnt/gps-data
		[ -f $R/data/nvram/APCFG/APRDEB/GPS ] || return 1
		log "copied /data/nvram from $DATA_DEV"
	fi
}

# TERM, and KILL after 3 s: the engine's own shutdown can hang in a futex
# wait with /dev/stpgps still open
end() {
	local pid i=0

	alive "$1" || return 0
	pid=$(cat "$RUN/$1.pid")
	kill "$pid"
	while [ $i -lt 30 ] && kill -0 "$pid" 2>/dev/null; do usleep 100000; i=$((i + 1)); done
	kill -9 "$pid" 2>/dev/null
	rm -f "$RUN/$1.pid"
}

# restart the engine when it exits or a whole-chip reset takes GPS away;
# an engine that keeps dying young (the stack still down) backs off up to
# 5 minutes
supervise() {
	delay=10
	while [ ! -f $RUN/stop ]; do
		resets=$(dmesg | grep -c "$RESET_MSG")
		started=$(cut -d. -f1 /proc/uptime)
		reset=
		chroot $R $ENGINE > /dev/null 2>&1 &
		echo $! > $RUN/engine.pid
		log "engine started (pid $!)"
		while alive engine && [ ! -f $RUN/stop ]; do
			sleep 5
			# compared with the last look, so a cleared dmesg is no blind spot
			n=$(dmesg | grep -c "$RESET_MSG")
			if [ "$n" -gt "$resets" ]; then
				log "whole-chip reset: restarting the engine"
				reset=1
				end engine
			fi
			resets=$n
		done
		[ -f $RUN/stop ] && break
		if [ -z "$reset" ] && [ $(($(cut -d. -f1 /proc/uptime) - started)) -lt 30 ]; then
			[ $delay -lt 300 ] && delay=$((delay * 2))
		else
			delay=10
		fi
		log "engine gone; restarting in $delay s"
		sleep $delay
	done
}

start() {
	if alive super; then
		echo "gps-up: already running" >&2
		return 1
	fi
	if pidof libmnlp_mt6572 > /dev/null; then
		echo "gps-up: an engine is already running (gps-test.sh?)" >&2
		return 1
	fi
	setup || { echo "gps-up: setup failed" >&2; log "setup failed"; return 1; }
	mkdir -p $RUN
	rm -f $RUN/stop $RUN/nmea $R/dev/gps
	mkfifo $RUN/nmea $R/dev/gps || return 1
	# -n: read the source at once, without waiting for a client; gpsd
	# listens on localhost only
	gpsd -n -P $RUN/gpsd.pid $RUN/nmea || { echo "gps-up: gpsd failed" >&2; return 1; }
	./gps-nmea $R/dev/gps $RUN/nmea 2>> $LOG &
	echo $! > $RUN/nmea.pid
	supervise &
	echo $! > $RUN/super.pid
	log "started"
	echo "gps-up: started; gpsd on localhost:2947, log $LOG"
}

stop() {
	[ -d $RUN ] || { echo "gps-up: not running"; return 0; }
	touch $RUN/stop
	for p in super engine nmea gpsd; do
		end $p
	done
	rm -f $RUN/nmea $R/dev/gps
	: > $R/dev/gps 2>/dev/null	# a plain file again, as gps-test.sh expects
	log "stopped"
	echo "gps-up: stopped"
}

status() {
	for p in super engine nmea gpsd; do
		if alive $p; then s="running (pid $(cat $RUN/$p.pid))"; else s="not running"; fi
		echo "$p: $s"
	done
	v=$(grep 'gps-nmea:' $LOG 2>/dev/null | tail -1 | cut -d' ' -f3-)
	echo "filter: ${v:-no verdict yet}"
	alive gpsd || return 0
	tpv=$(gpspipe -w -x 3 2>/dev/null | grep -m1 '"class":"TPV"')
	echo "gpsd: ${tpv:-no TPV report within 3 s}"
}

case "$1" in
start) start ;;
stop) stop ;;
status) status ;;
*) echo "usage: $0 start|stop|status" >&2; exit 2 ;;
esac
