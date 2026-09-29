# FM radio runbook

**VERIFIED WORKING** on the Prestigio PAP5500 DUO: listening through the
headphones or the speaker, seek and scan, RDS (PI, station name, radio
text), recording to WAV and streaming it over the network.

The MT6572's FM receiver sits in CONSYS with the MT6627N as its RF front
end. `mtk_fm_drv.ko` controls it over WMT/STP and exposes `/dev/fm`; the
audio leaves CONSYS as I2S into the AFE, which resamples it to 44.1 kHz and
sends it to the codec and, on request, into memory.

## Before you start

- **Headphones in.** Their cable is the antenna: without it a station reads
  around -108 dBm, with it -65 to -80 dBm. The board has no internal
  antenna (`a 1` changes nothing). `fmctl` mutes the receiver while the
  headphone jack reports them out, instead of playing noise on the speaker.
- The connsys stack is up (`connsys-up.sh`, or `S99bt` at boot).
- `/lib/firmware/mt6627_fm_v*_{patch,coeff}.bin` are installed (see the
  README's Firmware section; `wifi-fw-extract.sh` pulls them from stock).
- The kernel has the AFE FM path: `amixer controls | grep FM` lists
  `FM Playback Switch` and `FM Playback Volume`, and `arecord -l` shows
  card 0 device 1, "AWB Capture", for recording. `Speaker Switch` is there
  too for `-s`.

## Listening

```sh
/root/connsys/fm-up.sh 105.0
```

`fm-up.sh` loads the driver, turns the FM route on (FM gain 0 dB) and runs
`fmctl` in the foreground. Type one command per line:

| Command | Does |
|---|---|
| `t <MHz>` | tune, e.g. `t 99.0` |
| `s+` / `s-` | seek to the next station up / down |
| `S` | scan the band: every station with its level in dBm |
| `v <0-31>` | chip volume |
| `m` / `u` | mute / unmute |
| `r` | signal level and mono/stereo |
| `i` | chip, ROM and patch version |
| `b` | RDS groups decoded so far and the chip's RDS FIFO status |
| `q` | power the receiver down and exit (the route is switched off) |
| `?` | list the commands |

RDS arrives on its own as `rds PI …`, `rds PS "…"` (station name) and
`rds RT "…"` (radio text) lines.

The overall level is the shared downlink gain, the same one music playback
uses: `amixer cset name="Playback Volume" 63311` is loud, the default
(~-18 dB) is quiet. `FM Playback Volume` sets the FM stream's own gain in
front of it.

### In the background

```sh
/root/connsys/fm-up.sh -b 105.0      # returns at once
echo "t 99.0" > /tmp/fmin            # same commands, through the FIFO
echo S > /tmp/fmin                   # output goes to the console
echo q > /tmp/fmin                   # stop: receiver off, route off, FIFO gone
```

`-n` leaves the speaker/headphone route off (receiver on, nothing heard);
turn it on or off at any time with
`amixer cset name="FM Playback Switch" on|off`.

### On the speaker with the headphones in

```sh
/root/connsys/fm-up.sh -s 105.0                  # or -b -s
amixer cset name='Speaker Switch' on|off         # the same by hand, at any time
```

Plugging the headphones in turns the speaker off; `Speaker Switch` turns it
back on while their cable stays the antenna. The headphones keep playing:
the speaker amplifier is fed from the headphone output, so both play at one
level (`Playback Volume`), and there is no speaker-only setting with the
headphones in. A replug turns the speaker off again. When `-s` ends, the
speaker goes back to what the jack says.

## Recording

```sh
/root/connsys/fm-record.sh 105.0 60 /tmp/fm.wav    # 60 s to a file
/root/connsys/fm-record.sh 105.0                   # /tmp/fm-105.0.wav until Ctrl-C
```

The capture device records the FM stream after the AFE's resampler:
44.1 kHz stereo S16 WAV, about 10 MB a minute. If a radio runs under
`fm-up.sh -b`, `fm-record.sh` retunes it and leaves it running; otherwise
it starts one with the route off and stops it afterwards, Ctrl-C included.
Listening is independent - switch `FM Playback Switch` on to hear what is
being recorded.

The same by hand, with a radio already on the right station:

```sh
arecord -D hw:0,1 -f S16_LE -r 44100 -c 2 -d 60 /tmp/fm.wav
```

The device only offers 44.1 kHz stereo S16; `plughw:0,1` converts to
other formats in software. Play a recording back with
`aplay -D hw:0,0 /tmp/fm.wav`.

## Streaming over the network

`-` sends the WAV to stdout, so any pipe works. Raw PCM needs about
1.4 Mbit/s and costs the phone next to nothing; encode on the receiving
side.

```sh
# on the phone
/root/connsys/fm-record.sh 105.0 0 - | nc <pc-address> 9000

# on the PC, listen or keep (traditional netcat: nc -l -p 9000)
nc -l 9000 | ffplay -nodisp pipe:0
nc -l 9000 > fm.wav
```

Encoding on the phone (lame, opus, ffmpeg) works too but is the one part
that costs CPU.

## Notes and troubleshooting

- **Silence with the radio on:** the route is off (`fm-up.sh -n`, or after
  `q`) - `amixer cget name="FM Playback Switch"`; or the headphones are out
  and `fmctl` has muted the receiver.
- **Noise instead of stations:** the antenna. Seek and scan only report
  channels the chip rates as valid, so a weak position also shrinks the
  station list.
- **`fm-up: fmctl is already running`:** another radio holds `/dev/fm`.
  `echo q > /tmp/fmin` stops a background one; a foreground one is stopped
  with `q` in its terminal. `fm-record.sh` refuses to retune a foreground
  radio for the same reason.
- **RDS missing or partial:** RDS needs a steadier signal than audio. At
  around -75 dBm PI, name and text come through, but not all the time;
  `b` shows 0 groups and FIFO `0000` while the chip decodes none.
- **Music playback fails with EBUSY while FM plays:** FM holds the downlink
  at 44.1 kHz; play 44.1 kHz material, or switch the FM route off first.
  Recording is not affected.
- **Closing `/dev/fm` powers the receiver down.** Whatever drives it has to
  keep it open, which is why `fmctl` stays in the foreground or on the FIFO.
- **Registers:** `g <reg>` / `w <reg> <val>` read and write FM core
  registers; `h <addr> [val]` reads and writes CONSYS host registers once
  `echo 0xfffffff7 > /proc/fm` has enabled that; `fmctl -d` dumps all FM
  registers of a receiver another process keeps powered.
