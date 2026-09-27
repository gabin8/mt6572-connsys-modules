#!/bin/sh
# fm-up.sh [MHz] - FM radio (run from /root/connsys, after connsys-up.sh).
# Loads the FM driver, routes the receiver to the downlink and runs fmctl in
# the foreground: one command per line ("?" lists them), "q" powers the
# receiver down. Plug headphones in first - their cable is the antenna, and
# fmctl keeps the receiver muted while they are out.
cd /root/connsys || exit 1

lsmod | grep -q mtk_fm_drv || insmod fm/mtk_fm_drv.ko || exit 1

# FM HW gain 0 dB; the shared "Playback Volume" (downlink gain) is left alone
amixer -q cset name='FM Playback Volume' 524288
amixer -q cset name='FM Playback Switch' on

fm/fmctl "${1:-100.0}"

amixer -q cset name='FM Playback Switch' off
