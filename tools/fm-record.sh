#!/bin/sh
# fm-record.sh <MHz> [seconds] [file|-] - record an FM station from the AFE's
# capture device: 44.1 kHz stereo S16 WAV, the FM stream after its resampler.
#   fm-record.sh 105.0 60 /tmp/fm.wav         60 s to a file
#   fm-record.sh 105.0 0 - | nc HOST 9000     WAV on stdout until stopped
# seconds 0 (the default) records until interrupted; the file defaults to
# /tmp/fm-<MHz>.wav. A radio already running in the background (fm-up.sh -b)
# is retuned and left running; otherwise one is started, without the speaker
# route, and stopped afterwards. Headphones in - their cable is the antenna.
cd /root/connsys || exit 1
FIFO=/tmp/fmin

MHZ="${1:?usage: fm-record.sh <MHz> [seconds] [file|-]}"
SECS="${2:-0}"
OUT="${3:-/tmp/fm-$MHZ.wav}"

own=
stop_own() {
	[ -n "$own" ] && [ -p "$FIFO" ] && echo q > "$FIFO"
	own=
}
trap 'stop_own; exit 130' INT TERM

if pidof fmctl > /dev/null; then
	if [ ! -p "$FIFO" ]; then
		echo "fm-record: fmctl runs in the foreground - tune it there or quit it" >&2
		exit 1
	fi
	echo "t $MHZ" > "$FIFO"
else
	./fm-up.sh -b -n "$MHZ" || exit 1
	own=1
fi
sleep 1		# receiver tuned and the I2S stream running

[ "$OUT" = "-" ] || echo "fm-record: $MHZ MHz -> $OUT" >&2
arecord -q -D hw:0,1 -f S16_LE -r 44100 -c 2 -d "$SECS" "$OUT"
rc=$?
stop_own
exit $rc
