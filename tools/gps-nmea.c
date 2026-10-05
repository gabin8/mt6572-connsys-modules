// NMEA filter between the MediaTek MNL positioning engine and gpsd.
// Reads the engine's NMEA (a FIFO in its chroot) and writes the corrected
// stream (a FIFO that gpsd reads):
//  - the RMC date moves forward 1024 weeks: the 2013 engine resolves the
//    GPS week number into the 1999-2019 era, so today comes out in 2007;
//  - $GPACCURACY, the engine's own sentence, is dropped;
//  - with the system clock set (not before this tool was built), an epoch
//    whose satellite date or time is more than MAX_SKEW s off the clock has
//    its position sentences withheld: such a signal carries a foreign time,
//    which is what a spoofer sends. Satellite views (GSV) still pass.
//  - with -c STAMP and the clock unset (after a battery pull), the first fix
//    sets the clock and the RTC, unless it looks foreign: more than
//    MAX_INVIEW satellites in view, or a time before the clock last seen set
//    (STAMP, else the build date) or more than MAX_GAP after it. A foreign-
//    looking fix is withheld instead. STAMP records each clock taken.
//    Without -c an unset clock passes fixes unchecked.
// The input or output going away (engine restart, gpsd closing the device)
// just reopens it. Only state changes are logged, to stderr.
//
// Usage: gps-nmea [-c STAMP] IN OUT    ("-" = stdin / stdout)
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o gps-nmea gps-nmea.c

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_SKEW	120			// s; real satellites agree within seconds
#define ROLLOVER	(1024 * 7 * 86400L)	// one GPS week-number era
#define LINE_MAX_LEN	256			// the engine caps its sentences here
#define EPOCH_LINES	32
#define MAX_INVIEW	16	// a GPS-only sky shows about 14 at most
#define MAX_GAP		(90 * 86400L)	// s past the clock last seen set

enum check { CHECK_NONE, CHECK_OK, CHECK_FOREIGN, CHECK_UNSET, CHECK_REFUSED };

static const char *in_path, *out_path, *stamp_path;
static int inview;
static FILE *in, *out;
static time_t built;
static enum check state = CHECK_NONE;
static char pending[EPOCH_LINES][LINE_MAX_LEN + 2];
static int npending, decided, withhold;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...)
{
	char stamp[32];
	time_t now = time(NULL);
	va_list ap;

	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", gmtime(&now));
	fprintf(stderr, "%s gps-nmea: ", stamp);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

// the build date, from __DATE__ ("Oct  5 2026"): any clock before it is unset
static time_t build_time(void)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	struct tm tm = { 0 };
	char mon[4];

	if (sscanf(__DATE__, "%3s %d %d", mon, &tm.tm_mday, &tm.tm_year) != 3)
		return 0;
	tm.tm_mon = (int)((strstr(months, mon) - months) / 3);
	tm.tm_year -= 1900;
	return timegm(&tm);
}

static FILE *open_in(void)
{
	FILE *f;

	if (!strcmp(in_path, "-"))
		return stdin;
	// blocks until a writer (the engine) opens the FIFO
	while (!(f = fopen(in_path, "r"))) {
		if (errno != EINTR) {
			logmsg("cannot open %s: %s", in_path, strerror(errno));
			sleep(1);
		}
	}
	return f;
}

static FILE *open_out(void)
{
	FILE *f;

	if (!strcmp(out_path, "-"))
		return stdout;
	// blocks until a reader (gpsd) opens the FIFO
	while (!(f = fopen(out_path, "w"))) {
		if (errno != EINTR) {
			logmsg("cannot open %s: %s", out_path, strerror(errno));
			sleep(1);
		}
	}
	setvbuf(f, NULL, _IOLBF, 0);
	return f;
}

static int is_fifo(FILE *f)
{
	struct stat st;

	return !fstat(fileno(f), &st) && S_ISFIFO(st.st_mode);
}

static void emit(const char *line)
{
	while (fputs(line, out) == EOF || fflush(out) == EOF) {
		if (out == stdout)
			exit(1);
		// the reader went away (EPIPE): wait for the next one
		fclose(out);
		out = open_out();
	}
}

static unsigned char checksum(const char *s, size_t n)
{
	unsigned char c = 0;

	while (n--)
		c ^= (unsigned char)*s++;
	return c;
}

static int is_type(const char *line, const char *type)
{
	// $GPRMC, $GNRMC, ...: the talker is two letters after the '$'
	return line[0] == '$' && strlen(line) > 6 && !strncmp(line + 3, type, 3) &&
	       line[6] == ',';
}

static int is_position(const char *line)
{
	return is_type(line, "GGA") || is_type(line, "GSA") || is_type(line, "RMC") ||
	       is_type(line, "VTG") || is_type(line, "GLL") || is_type(line, "GNS");
}

// field i (0 = the sentence type) of a comma-separated sentence, copied out
static int field(const char *line, int i, char *buf, size_t len)
{
	const char *p = line, *e;

	while (i-- > 0) {
		p = strchr(p, ',');
		if (!p)
			return -1;
		p++;
	}
	e = p + strcspn(p, ",*\r\n");
	if ((size_t)(e - p) >= len)
		return -1;
	memcpy(buf, p, e - p);
	buf[e - p] = '\0';
	return 0;
}

// RMC date ddmmyy + time hhmmss[.sss] -> UTC seconds with the rollover
// undone; 0 while the engine has no date yet (it reports 1980 until then)
static time_t rmc_time(const char *date, const char *hms)
{
	struct tm tm = { 0 };
	int d, m, y, hh, mm, ss;

	if (strlen(date) != 6 || strlen(hms) < 6 ||
	    sscanf(date, "%2d%2d%2d", &d, &m, &y) != 3 ||
	    sscanf(hms, "%2d%2d%2d", &hh, &mm, &ss) != 3)
		return 0;
	y += y >= 80 ? 1900 : 2000;
	if (y < 1999)
		return 0;
	tm.tm_year = y - 1900;
	tm.tm_mon = m - 1;
	tm.tm_mday = d;
	tm.tm_hour = hh;
	tm.tm_min = mm;
	tm.tm_sec = ss;
	return timegm(&tm) + ROLLOVER;
}

// rewrite the RMC date field to `date` and redo the checksum
static void rmc_set_date(char *line, const char *date)
{
	char out_line[LINE_MAX_LEN + 2], *p = line, *q, *star;
	size_t head;
	int i;

	for (i = 0; i < 9; i++) {
		p = strchr(p, ',');
		if (!p)
			return;
		p++;
	}
	q = p + strcspn(p, ",*\r\n");
	head = p - line;
	star = strchr(q, '*');
	if (!star || head + strlen(date) + strlen(q) + 4 > sizeof(out_line))
		return;
	snprintf(out_line, sizeof(out_line), "%.*s%s%.*s", (int)head, line, date,
		 (int)(star - q), q);
	snprintf(out_line + strlen(out_line), sizeof(out_line) - strlen(out_line),
		 "*%02X\r\n", checksum(out_line + 1, strlen(out_line) - 1));
	strcpy(line, out_line);
}

static void set_state(enum check s, time_t gps, long skew, const char *why)
{
	char g[32];

	if (s == state)
		return;
	state = s;
	strftime(g, sizeof(g), "%Y-%m-%d %H:%M:%S", gmtime(&gps));
	if (s == CHECK_OK)
		logmsg("time checked: the satellites agree with the clock (%+ld s)", skew);
	else if (s == CHECK_FOREIGN)
		logmsg("foreign time: satellites say %s UTC, %+ld s off the clock; "
		       "withholding fixes", g, skew);
	else if (s == CHECK_UNSET)
		logmsg("system clock not set: fixes pass unchecked%s%s",
		       why ? "; " : "", why ? why : "");
	else if (s == CHECK_REFUSED)
		logmsg("clock not set: satellites say %s UTC, %s; withholding fixes "
		       "(set the clock by hand: date -u -s ...; hwclock -w -u)", g, why);
}

// the clock last seen set: the stamp, or this tool's build date
static time_t reference(void)
{
	long long t;
	time_t ref = built;
	FILE *f = fopen(stamp_path, "r");

	if (f) {
		if (fscanf(f, "%lld", &t) == 1 && (time_t)t > ref)
			ref = (time_t)t;
		fclose(f);
	}
	return ref;
}

static void save_stamp(time_t t)
{
	char tmp[256];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.new", stamp_path);
	f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "%lld\n", (long long)t);
	if (fclose(f) == 0)
		rename(tmp, stamp_path);
}

// The clock is unset: take it from this fix, unless the fix looks foreign.
// Returns 1 once the clock is set.
static int take_clock(time_t gps)
{
	struct timeval tv = { .tv_sec = gps };
	time_t ref = reference();
	char why[96], r[32], g[32];

	strftime(r, sizeof(r), "%Y-%m-%d %H:%M", gmtime(&ref));
	if (inview > MAX_INVIEW)
		snprintf(why, sizeof(why), "%d in view, more than a GPS-only sky shows", inview);
	else if (gps < ref)
		snprintf(why, sizeof(why), "earlier than the clock last seen set (%s)", r);
	else if (gps > ref + MAX_GAP)
		snprintf(why, sizeof(why), "over 90 days after the clock last seen set (%s)", r);
	else
		why[0] = '\0';
	if (why[0]) {
		withhold = 1;
		set_state(CHECK_REFUSED, gps, 0, why);
		return 0;
	}
	if (settimeofday(&tv, NULL)) {
		withhold = 0;
		snprintf(why, sizeof(why), "cannot set it: %s", strerror(errno));
		set_state(CHECK_UNSET, gps, 0, why);
		return 0;
	}
	if (system("hwclock -w -u") != 0)
		logmsg("hwclock -w -u failed: the RTC keeps its old time");
	save_stamp(gps);
	strftime(g, sizeof(g), "%Y-%m-%d %H:%M:%S", gmtime(&gps));
	logmsg("clock set from the satellites: %s UTC (%d in view), RTC written", g, inview);
	return 1;
}

// decide the epoch on its RMC, and move the RMC date out of the old era
static void judge_rmc(char *line)
{
	char date[16], hms[16], status[4], fixed[8];
	time_t gps, now = time(NULL);
	struct tm tm;

	if (field(line, 9, date, sizeof(date)) || field(line, 1, hms, sizeof(hms)) ||
	    field(line, 2, status, sizeof(status)))
		return;
	gps = rmc_time(date, hms);
	if (!gps)
		return;		// no date yet: nothing to correct or check
	gmtime_r(&gps, &tm);
	strftime(fixed, sizeof(fixed), "%d%m%y", &tm);
	rmc_set_date(line, fixed);
	if (now < built) {
		if (!stamp_path) {
			withhold = 0;
			set_state(CHECK_UNSET, gps, 0, NULL);
			return;
		}
		// the clock comes from a fix, not from a time alone
		if (status[0] != 'A' || !take_clock(gps))
			return;
		now = time(NULL);
	}
	// checked with or without a fix: gpsd takes the time from either
	if (labs((long)(gps - now)) > MAX_SKEW) {
		withhold = 1;
		set_state(CHECK_FOREIGN, gps, (long)(gps - now), NULL);
	} else {
		withhold = 0;
		set_state(CHECK_OK, gps, (long)(gps - now), NULL);
	}
}

static void pass(const char *line)
{
	if (!(withhold && is_position(line)))
		emit(line);
}

static void flush_pending(void)
{
	int i;

	for (i = 0; i < npending; i++)
		pass(pending[i]);
	npending = 0;
}

static void handle(char *line)
{
	if (!strncmp(line, "$GPACCURACY", 11))
		return;
	if (is_type(line, "GGA")) {
		// a new epoch: whatever is still held goes out on the last decision
		flush_pending();
		decided = 0;
	}
	if (is_type(line, "GSV")) {
		char n[8];

		if (!field(line, 3, n, sizeof(n)))
			inview = atoi(n);	// satellites in view, in every GSV
	}
	if (is_type(line, "RMC")) {
		judge_rmc(line);
		flush_pending();
		decided = 1;
		pass(line);
		return;
	}
	if (decided) {
		pass(line);
		return;
	}
	// before this epoch's RMC: hold the line until the date is known
	if (npending == EPOCH_LINES)
		flush_pending();
	snprintf(pending[npending++], sizeof(pending[0]), "%s", line);
}

int main(int argc, char **argv)
{
	char line[LINE_MAX_LEN + 2];
	int opt;

	while ((opt = getopt(argc, argv, "c:")) != -1) {
		if (opt != 'c')
			goto usage;
		stamp_path = optarg;
	}
	if (argc - optind != 2) {
usage:
		fprintf(stderr, "usage: %s [-c STAMP] IN OUT   (\"-\" = stdin / stdout)\n",
			argv[0]);
		return 2;
	}
	in_path = argv[optind];
	out_path = argv[optind + 1];
	built = build_time();
	signal(SIGPIPE, SIG_IGN);	// a vanished reader shows up as EPIPE
	in = open_in();
	out = open_out();
	for (;;) {
		if (!fgets(line, sizeof(line), in)) {
			flush_pending();
			// a file or stdin just ends; a FIFO's writer (the engine)
			// went away: wait for the next one
			if (in == stdin || !is_fifo(in))
				return 0;
			fclose(in);
			in = open_in();
			continue;
		}
		if (line[0] == '$')
			handle(line);
	}
}
