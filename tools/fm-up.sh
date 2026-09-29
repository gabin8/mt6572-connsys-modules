#!/bin/sh
# fm-up.sh [-b] [-n] [-s] [MHz] - FM radio (run from /root/connsys, after connsys-up.sh).
# Loads the FM driver, routes the receiver to the downlink and runs fmctl:
#   default  in the foreground: one command per line ("?" lists them), "q"
#            powers the receiver down;
#   -b       in the background, taking the same commands through the FIFO
#            /tmp/fmin (echo "t 99.0" > /tmp/fmin, echo q > /tmp/fmin);
#   -n       without turning the speaker/headphone route on (fm-record.sh);
#   -s       on the speaker as well as the headphones (until the next replug).
# Plug headphones in first - their cable is the antenna, and fmctl keeps the
# receiver muted while they are out.
cd /root/connsys || exit 1
FIFO=/tmp/fmin

bg=
route=on
spk=
while [ $# -gt 0 ]; do
	case "$1" in
	-b) bg=1 ;;
	-n) route=off ;;
	-s) spk=1 ;;
	*) break ;;
	esac
	shift
done
MHZ="${1:-100.0}"

if pidof fmctl > /dev/null; then
	echo "fm-up: fmctl is already running" >&2
	exit 1
fi
lsmod | grep -q mtk_fm_drv || insmod fm/mtk_fm_drv.ko || exit 1

# FM HW gain 0 dB; the shared "Playback Volume" (downlink gain) is left alone
amixer -q cset name='FM Playback Volume' 524288
amixer -q cset name='FM Playback Switch' "$route"
[ -n "$spk" ] && amixer -q cset name='Speaker Switch' on

# route off; a speaker -s turned on goes back to what the jack says
route_off() {
	amixer -q cset name='FM Playback Switch' off
	[ -n "$spk" ] || return 0
	amixer cget iface=CARD,name='Headphone Jack' | grep -q ': values=on' &&
		amixer -q cset name='Speaker Switch' off
}

if [ -z "$bg" ]; then
	trap : INT	# Ctrl-C ends fmctl, not this script: the route still goes off
	fm/fmctl "$MHZ"
	route_off
	exit 0
fi

rm -f "$FIFO"
mkfifo "$FIFO" || exit 1
sleep 999999999 > "$FIFO" &	# keeps the FIFO open between commands
keep=$!
(
	fm/fmctl "$MHZ" < "$FIFO" > /dev/console 2>&1
	kill "$keep" 2>/dev/null
	rm -f "$FIFO"
	route_off
) &
