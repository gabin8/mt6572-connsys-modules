// Smoke test for /dev/stpgps (the CONSYS GPS channel over STP).
// Opens the node (GPS function on), queries the ioctls mnld uses, checks
// that a second opener is refused, then poll-reads for a few seconds and
// reports what the GPS firmware sent. Never blocks the console.
//
// Usage: stpgps-probe [seconds]   (default 10)
// Exit:  0 bytes arrived, 1 open/ioctl failed, 2 chip reset, 3 silent
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o stpgps-probe stpgps-probe.c

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define GPS_DEV			"/dev/stpgps"
#define COMBO_IOC_GPS_HWVER	6
#define COMBO_IOC_RTC_FLAG	7
#define COMBO_IOC_CO_CLOCK_FLAG	8
#define DUMP_BYTES		64

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	unsigned char buf[4096], head[DUMP_BYTES];
	long long start, first = -1, end;
	int fd, fd2, hwver = -1, ret, i;
	int reads = 0, total = 0, kept = 0;

	fd = open(GPS_DEV, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		printf("GPS_PROBE=BUSTED open: %s\n", strerror(errno));
		return 1;
	}
	printf("open ok (GPS function on)\n");

	if (ioctl(fd, COMBO_IOC_GPS_HWVER, &hwver) < 0) {
		printf("GPS_PROBE=BUSTED HWVER ioctl: %s\n", strerror(errno));
		return 1;
	}
	printf("HWVER=%d RTC_FLAG=%d CO_CLOCK=%d\n", hwver,
	       ioctl(fd, COMBO_IOC_RTC_FLAG), ioctl(fd, COMBO_IOC_CO_CLOCK_FLAG));

	fd2 = open(GPS_DEV, O_RDWR | O_NONBLOCK);
	printf("second open: %s\n", fd2 < 0 ? strerror(errno) : "ALLOWED (unexpected)");
	if (fd2 >= 0)
		close(fd2);

	start = now_ms();
	end = start + secs * 1000LL;
	while (now_ms() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };

		if (poll(&p, 1, 200) <= 0)
			continue;
		if (p.revents & POLLERR) {
			printf("GPS_PROBE=BUSTED chip reset after %lld ms\n", now_ms() - start);
			close(fd);
			return 2;
		}
		ret = read(fd, buf, sizeof(buf));
		if (ret <= 0)
			continue;
		if (first < 0)
			first = now_ms() - start;
		reads++;
		total += ret;
		for (i = 0; i < ret && kept < DUMP_BYTES; i++)
			head[kept++] = buf[i];
	}

	printf("%d bytes in %d reads over %d s", total, reads, secs);
	if (first >= 0)
		printf(", first after %lld ms", first);
	printf("\nhead:");
	for (i = 0; i < kept; i++)
		printf(" %02x", head[i]);
	printf("\n");

	close(fd);
	printf("closed (GPS function off)\n");

	if (total) {
		printf("GPS_PROBE=CONFIRMED firmware streams on the GPS channel\n");
		return 0;
	}
	printf("GPS_PROBE=SILENT no bytes; the firmware may wait for the engine\n");
	return 3;
}
