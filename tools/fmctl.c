// SPDX-License-Identifier: GPL-2.0
/*
 * fmctl - minimal FM receiver control for the MediaTek FM driver (/dev/fm).
 *
 * The driver powers the receiver down when the last /dev/fm opener closes,
 * so fmctl stays in the foreground holding the fd. It powers up on the given
 * frequency, turns RDS on, then takes one command per line on stdin while it
 * prints RDS station name / radio text as they are decoded.
 *
 *   fmctl [MHz]            default 100.0
 *   fmctl -d               dump the FM registers of a receiver that some
 *                          other process keeps powered (no power up/down)
 *
 *   t <MHz>   tune             s+ / s-   seek up / down
 *   S         scan the band    v <0-31>  chip volume
 *   m / u     mute / unmute    r         RSSI + stereo
 *   i         chip / patch info
 *   b         RDS block counters
 *   a <0|1>   antenna: 0 long (headset cable), 1 short
 *   g <reg> / w <reg> <val>     read / write an FM core register
 *   h <addr> [val]              read / write a CONSYS host register
 *                               (needs "echo 0xfffffff7 > /proc/fm" first)
 *   q         power down, exit
 *
 * The driver has no hardware seek: seek and scan step the band with
 * soft-mute tunes and take the chip's per-channel "valid station" verdict,
 * as MediaTek's own FM service does.
 *
 * The headphone cable is the antenna. While the headphone jack switch reports
 * them out the receiver is muted, instead of playing noise on the speaker.
 *
 * Build: arm-linux-gnueabihf-gcc -O2 -static -I../fmradio/inc -o fmctl fmctl.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fm_rds.h"	/* struct rds_t, RDS_EVENT_*: plain C, shared with the driver */

/* ioctl ABI, mirrored from the driver's fm_main.h / fm_ioctl.h / fm_interface.h */

/*
 * Tune request, laid out so both driver generations read it: older drivers
 * take freq at offset 4; newer ones put deemphasis there and freq at offset 6,
 * and fall back to the old field when theirs is 0. The answer comes back in
 * whichever field the driver owns.
 */
struct fm_tune_parm {
	unsigned char err;
	unsigned char band;
	unsigned char space;
	unsigned char hilo;
	unsigned short freq_old;
	unsigned short freq_new;
};

struct fm_softmute_tune_t {
	int rssi;
	unsigned short freq;	/* 10 kHz units */
	int valid;		/* chip's CQI verdict: a station */
};

struct fm_ctl_parm {
	unsigned char err;
	unsigned char addr;
	unsigned short val;
	unsigned short rw_flag;	/* 1: read */
};

struct fm_host_rw_parm {
	unsigned char err;
	unsigned char rw_flag;	/* 1: read */
	unsigned int addr;
	unsigned int val;
};

struct fm_hw_info {
	int chip_id;
	int eco_ver;
	int rom_ver;
	int patch_ver;
	int reserve;
};

#define FM_IOC_MAGIC		0xf5
#define FM_IOCTL_POWERUP	_IOWR(FM_IOC_MAGIC, 0, struct fm_tune_parm *)
#define FM_IOCTL_POWERDOWN	_IOWR(FM_IOC_MAGIC, 1, int32_t *)
#define FM_IOCTL_TUNE		_IOWR(FM_IOC_MAGIC, 2, struct fm_tune_parm *)
#define FM_IOCTL_SETVOL		_IOWR(FM_IOC_MAGIC, 4, uint32_t *)
#define FM_IOCTL_MUTE		_IOWR(FM_IOC_MAGIC, 6, uint32_t *)
#define FM_IOCTL_GETRSSI	_IOWR(FM_IOC_MAGIC, 7, int32_t *)
#define FM_IOCTL_RW_REG		_IOWR(FM_IOC_MAGIC, 12, struct fm_ctl_parm *)
#define FM_IOCTL_HOST_RDWR	_IOWR(FM_IOC_MAGIC, 44, struct fm_host_rw_parm *)
#define FM_IOCTL_GETMONOSTERO	_IOWR(FM_IOC_MAGIC, 13, uint16_t *)
#define FM_IOCTL_GETGOODBCNT	_IOWR(FM_IOC_MAGIC, 15, uint16_t *)
#define FM_IOCTL_GETBADBNT	_IOWR(FM_IOC_MAGIC, 16, uint16_t *)
#define FM_IOCTL_GETBLERRATIO	_IOWR(FM_IOC_MAGIC, 17, uint16_t *)
#define FM_IOCTL_RDS_ONOFF	_IOWR(FM_IOC_MAGIC, 18, uint16_t *)
#define FM_IOCTL_ANA_SWITCH	_IOWR(FM_IOC_MAGIC, 30, int32_t *)
#define FM_IOCTL_GET_HW_INFO	_IOWR(FM_IOC_MAGIC, 40, struct fm_hw_info *)
#define FM_IOCTL_PRE_SEARCH	_IOWR(FM_IOC_MAGIC, 45, int32_t)
#define FM_IOCTL_RESTORE_SEARCH	_IOWR(FM_IOC_MAGIC, 46, int32_t)
#define FM_IOCTL_SOFT_MUTE_TUNE	_IOWR(FM_IOC_MAGIC, 63, struct fm_softmute_tune_t *)

#define FM_BAND_UE		1	/* 87.5 - 108 MHz */
#define FM_SPACE_100K		10
#define FM_AUTO_HILO_OFF	0
#define FM_RX			0

/* band and channel step, 10 kHz units */
#define BAND_LOW		8750
#define BAND_HIGH		10800
#define STEP			10
#define MAX_STATIONS		64

/* RDS is fetched with a non-blocking read(); the driver has no poll(). */
#define RDS_POLL_MS		200

static int fd = -1;
static unsigned short cur_freq;	/* 10 kHz units */
static bool user_mute;		/* "m" / "u" */
static bool antenna = true;	/* headphones plugged in */

/* The driver answers in 100 kHz or 10 kHz units depending on the request. */
static unsigned short to_10k(unsigned short f)
{
	return f < 2000 ? f * 10 : f;
}

static unsigned short parse_mhz(const char *s)
{
	return (unsigned short)(strtod(s, NULL) * 100.0 + 0.5);
}

static void print_freq(const char *what, unsigned short f)
{
	printf("%s %u.%02u MHz\n", what, f / 100, f % 100);
}

static int tune_req(unsigned long req, unsigned short freq, const char *what)
{
	struct fm_tune_parm p = {
		.band = FM_BAND_UE, .space = FM_SPACE_100K,
		.hilo = FM_AUTO_HILO_OFF,
		.freq_old = freq / 10,	/* 100 kHz units: understood by both */
	};

	if (ioctl(fd, req, &p) < 0) {
		fprintf(stderr, "%s: %s (err %u)\n", what, strerror(errno), p.err);
		return -1;
	}
	cur_freq = to_10k(p.freq_new ? p.freq_new : p.freq_old);
	print_freq("tuned", cur_freq);
	return 0;
}

static int power_up(unsigned short freq)
{
	return tune_req(FM_IOCTL_POWERUP, freq, "power up");
}

static void tune(unsigned short freq)
{
	tune_req(FM_IOCTL_TUNE, freq, "tune");
}

/* Soft-mute tune to f; returns the RSSI if the chip calls it a station. */
static bool probe(unsigned short f, int *rssi)
{
	struct fm_softmute_tune_t p = { .freq = f };

	if (ioctl(fd, FM_IOCTL_SOFT_MUTE_TUNE, &p) < 0)
		return false;
	*rssi = p.rssi * 6 / 16;	/* raw -> dBm, as the driver's GETRSSI */
	return p.valid;
}

static unsigned short wrap(int f)
{
	if (f > BAND_HIGH)
		return BAND_LOW;
	if (f < BAND_LOW)
		return BAND_HIGH;
	return f;
}

/* Next station from cur_freq in direction dir (+1/-1), once round the band. */
static void seek(int dir)
{
	unsigned short f = cur_freq;
	int n, rssi;

	ioctl(fd, FM_IOCTL_PRE_SEARCH, 0);
	for (n = 0; n < (BAND_HIGH - BAND_LOW) / STEP; n++) {
		f = wrap(f + dir * STEP);
		if (probe(f, &rssi))
			break;
	}
	ioctl(fd, FM_IOCTL_RESTORE_SEARCH, 0);
	if (n == (BAND_HIGH - BAND_LOW) / STEP) {
		printf("no station found\n");
		tune(cur_freq);
		return;
	}
	tune(f);
}

/* Whole band; of two adjacent valid channels only the stronger is a station. */
static void scan(void)
{
	unsigned short st[MAX_STATIONS];
	int st_rssi[MAX_STATIONS];
	int n = 0, i, rssi;
	bool last = false;
	unsigned short f;

	ioctl(fd, FM_IOCTL_PRE_SEARCH, 0);
	for (f = BAND_LOW; f <= BAND_HIGH; f += STEP) {
		if (!probe(f, &rssi)) {
			last = false;
			continue;
		}
		if (last && n) {
			if (rssi > st_rssi[n - 1]) {
				st[n - 1] = f;
				st_rssi[n - 1] = rssi;
			}
		} else if (n < MAX_STATIONS) {
			st[n] = f;
			st_rssi[n++] = rssi;
		}
		last = true;
	}
	ioctl(fd, FM_IOCTL_RESTORE_SEARCH, 0);

	for (i = 0; i < n; i++)
		printf("  %3u.%02u MHz  %d dBm\n", st[i] / 100, st[i] % 100, st_rssi[i]);
	printf("%d stations\n", n);
	tune(cur_freq);
}

static void show_signal(void)
{
	int32_t rssi = 0;
	uint16_t stereo = 0;

	if (ioctl(fd, FM_IOCTL_GETRSSI, &rssi) < 0)
		fprintf(stderr, "rssi: %s\n", strerror(errno));
	if (ioctl(fd, FM_IOCTL_GETMONOSTERO, &stereo) < 0)
		fprintf(stderr, "stereo: %s\n", strerror(errno));
	printf("rssi %d dBm, %s\n", rssi, stereo ? "stereo" : "mono");
}

static void show_info(void)
{
	struct fm_hw_info hw = { 0 };

	if (ioctl(fd, FM_IOCTL_GET_HW_INFO, &hw) < 0) {
		fprintf(stderr, "hw info: %s\n", strerror(errno));
		return;
	}
	printf("chip 0x%04x eco %d rom %d patch 0x%08x\n",
	       hw.chip_id, hw.eco_ver, hw.rom_ver, hw.patch_ver);
}

static void show_rds_stats(void)
{
	uint16_t good = 0, bad = 0, bler = 0;

	if (ioctl(fd, FM_IOCTL_GETGOODBCNT, &good) < 0 ||
	    ioctl(fd, FM_IOCTL_GETBADBNT, &bad) < 0 ||
	    ioctl(fd, FM_IOCTL_GETBLERRATIO, &bler) < 0) {
		fprintf(stderr, "rds stats: %s\n", strerror(errno));
		return;
	}
	printf("rds blocks good %u bad %u, bler %u%%\n", good, bad, bler);
}

static void set_u32(unsigned long req, uint32_t val, const char *what)
{
	if (ioctl(fd, req, &val) < 0)
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
}

static void apply_mute(void)
{
	set_u32(FM_IOCTL_MUTE, user_mute || !antenna, "mute");
}

static bool test_bit(unsigned int nr, const unsigned long *bits)
{
	return bits[nr / (8 * sizeof(long))] & (1UL << (nr % (8 * sizeof(long))));
}

/* The input device carrying the headphone jack switch, or -1. */
static int open_jack(void)
{
	unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))];
	char path[32];
	int i, jfd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		jfd = open(path, O_RDONLY | O_NONBLOCK);
		if (jfd < 0)
			continue;
		memset(sw, 0, sizeof(sw));
		if (ioctl(jfd, EVIOCGBIT(EV_SW, sizeof(sw)), sw) >= 0 &&
		    test_bit(SW_HEADPHONE_INSERT, sw)) {
			if (ioctl(jfd, EVIOCGSW(sizeof(sw)), sw) >= 0)
				antenna = test_bit(SW_HEADPHONE_INSERT, sw);
			return jfd;
		}
		close(jfd);
	}
	return -1;
}

static void read_jack(int jfd)
{
	struct input_event ev;

	while (read(jfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_SW || ev.code != SW_HEADPHONE_INSERT)
			continue;
		antenna = ev.value;
		printf("headphones %s\n", antenna ? "in" : "out: muted, they are the antenna");
		apply_mute();
	}
}

/* Printable copy of a fixed-size, not necessarily terminated RDS text. */
static void print_text(const char *tag, const unsigned char *s, size_t n)
{
	char out[65];
	size_t i;

	if (n > sizeof(out) - 1)
		n = sizeof(out) - 1;
	for (i = 0; i < n && s[i]; i++)
		out[i] = (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : ' ';
	out[i] = '\0';
	printf("%s \"%s\"\n", tag, out);
}

static void read_rds(void)
{
	static struct rds_t rds;
	ssize_t n = read(fd, &rds, sizeof(rds));

	if (n != (ssize_t)sizeof(rds))
		return;
	if (rds.event_status & RDS_EVENT_PI_CODE)
		printf("rds PI %04X\n", rds.PI);
	if (rds.event_status & RDS_EVENT_PROGRAMNAME)
		print_text("rds PS", rds.PS_Data.PS[3], sizeof(rds.PS_Data.PS[3]));
	if (rds.event_status & RDS_EVENT_LAST_RADIOTEXT)
		print_text("rds RT", rds.RT_Data.TextData[3],
			   sizeof(rds.RT_Data.TextData[3]));
	fflush(stdout);
}

static void fm_reg(char *arg)
{
	char *end;
	struct fm_ctl_parm p = { .addr = strtoul(arg, &end, 16), .rw_flag = 1 };

	if (*end) {
		p.val = strtoul(end, NULL, 16);
		p.rw_flag = 0;
	}
	if (ioctl(fd, FM_IOCTL_RW_REG, &p) < 0) {
		fprintf(stderr, "reg 0x%02x: %s\n", p.addr, strerror(errno));
		return;
	}
	if (p.rw_flag)
		printf("reg %02x = %04x\n", p.addr, p.val);
}

static void host_reg(char *arg)
{
	char *end;
	struct fm_host_rw_parm h = { .addr = strtoul(arg, &end, 16), .rw_flag = 1 };

	if (*end) {
		h.val = strtoul(end, NULL, 16);
		h.rw_flag = 0;
	}
	if (ioctl(fd, FM_IOCTL_HOST_RDWR, &h) < 0) {
		fprintf(stderr, "host %08x: %s\n", h.addr, strerror(errno));
		return;
	}
	if (h.rw_flag)
		printf("host %08x = %08x\n", h.addr, h.val);
}

/* Returns false on "q". */
static bool command(char *line)
{
	char *arg = line + 1;

	while (*arg == ' ')
		arg++;
	switch (line[0]) {
	case 't':
		tune(parse_mhz(arg));
		break;
	case 's':
		seek(*arg == '-' ? -1 : 1);
		break;
	case 'S':
		scan();
		break;
	case 'v':
		set_u32(FM_IOCTL_SETVOL, strtoul(arg, NULL, 0), "volume");
		break;
	case 'm':
	case 'u':
		user_mute = line[0] == 'm';
		apply_mute();
		break;
	case 'r':
		show_signal();
		break;
	case 'i':
		show_info();
		break;
	case 'b':
		show_rds_stats();
		break;
	case 'a':
		set_u32(FM_IOCTL_ANA_SWITCH, strtoul(arg, NULL, 0), "antenna");
		show_signal();
		break;
	case 'g':
	case 'w':
		fm_reg(arg);
		break;
	case 'h':
		host_reg(arg);
		break;
	case 'q':
		return false;
	case '\0':
		break;
	default:
		printf("t <MHz> | s+ | s- | S | v <0-31> | m | u | r | i | b | a <0|1> | g/w/h ... | q\n");
	}
	fflush(stdout);
	return true;
}

/* FM core registers 0x00-0xff, plus the CONSYS host side of the audio link. */
static int dump_regs(void)
{
	static const unsigned int host[] = { 0x80101054, 0x80101030, 0x80000224 };
	struct fm_host_rw_parm h;
	struct fm_ctl_parm p;
	unsigned int i;

	for (i = 0; i <= 0xff; i++) {
		p = (struct fm_ctl_parm){ .addr = i, .rw_flag = 1 };
		if (ioctl(fd, FM_IOCTL_RW_REG, &p) < 0) {
			fprintf(stderr, "reg 0x%02x: %s\n", i, strerror(errno));
			return 1;
		}
		printf("%02x: %04x%c", i, p.val, (i & 7) == 7 ? '\n' : ' ');
	}
	for (i = 0; i < sizeof(host) / sizeof(host[0]); i++) {
		h = (struct fm_host_rw_parm){ .rw_flag = 1, .addr = host[i] };
		if (ioctl(fd, FM_IOCTL_HOST_RDWR, &h) < 0)
			printf("host %08x: %s\n", host[i], strerror(errno));
		else
			printf("host %08x: %08x\n", host[i], h.val);
	}
	return 0;
}

int main(int argc, char **argv)
{
	bool dump = argc > 1 && !strcmp(argv[1], "-d");
	unsigned short freq = argc > 1 && !dump ? parse_mhz(argv[1]) : 10000;
	struct pollfd pfd[2] = {
		{ .fd = STDIN_FILENO, .events = POLLIN },
		{ .fd = -1, .events = POLLIN },
	};
	uint16_t rds_on = 1;
	int32_t rx = FM_RX;
	char line[64];
	size_t len = 0;
	bool run = true;

	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = open("/dev/fm", O_RDWR);
	if (fd < 0) {
		perror("/dev/fm");
		return 1;
	}
	if (dump)
		return dump_regs();
	if (power_up(freq) < 0)
		return 1;
	if (ioctl(fd, FM_IOCTL_RDS_ONOFF, &rds_on) < 0)
		fprintf(stderr, "rds on: %s\n", strerror(errno));
	pfd[1].fd = open_jack();
	if (!antenna)
		printf("headphones out: muted, they are the antenna\n");
	apply_mute();
	show_signal();

	while (run) {
		int r = poll(pfd, 2, RDS_POLL_MS);
		char c;

		if (r < 0 && errno != EINTR)
			break;
		if (r > 0 && (pfd[1].revents & POLLIN))
			read_jack(pfd[1].fd);
		/* one byte per wakeup: no stdio buffer hiding queued input from poll() */
		if (r > 0 && (pfd[0].revents & POLLIN)) {
			if (read(STDIN_FILENO, &c, 1) != 1)
				break;
			if (c == '\n' || c == '\r') {
				line[len] = '\0';
				len = 0;
				run = command(line);
			} else if (len < sizeof(line) - 1) {
				line[len++] = c;
			}
		}
		read_rds();
	}

	if (ioctl(fd, FM_IOCTL_POWERDOWN, &rx) < 0)
		fprintf(stderr, "power down: %s\n", strerror(errno));
	close(fd);
	return 0;
}
