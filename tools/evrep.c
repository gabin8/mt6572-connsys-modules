// Set the kernel autorepeat delay/period on an evdev device.
//
// A Bluetooth HID keyboard on a sniffing link can deliver a key-up several
// hundred milliseconds late - the peripheral only transmits on sniff anchor
// points and may skip many of them. The input layer cannot tell that from a
// held key, so past its 250 ms autorepeat delay it starts repeating and
// "cat" arrives as "caaaaaaat".
//
// Refusing sniff also fixes it, but pins the link in active mode, where the
// controller polls it every other slot and starves any concurrent LE link -
// on this single-radio chip a connected keyboard then triples LE mouse
// latency. Raising the autorepeat delay past the worst observed stall costs
// nothing on the air instead.
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o evrep evrep.c
// Run:   evrep /dev/input/eventN [delay_ms] [period_ms]
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/ioctl.h>

#define DEFAULT_DELAY_MS	800	/* worst stall measured was 575 ms */
#define DEFAULT_PERIOD_MS	33	/* kernel default */

int main(int argc, char **argv)
{
	unsigned int rep[2];
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s /dev/input/eventN [delay_ms] [period_ms]\n",
			argv[0]);
		return 2;
	}
	rep[0] = argc > 2 ? (unsigned)atoi(argv[2]) : DEFAULT_DELAY_MS;
	rep[1] = argc > 3 ? (unsigned)atoi(argv[3]) : DEFAULT_PERIOD_MS;

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	if (ioctl(fd, EVIOCSREP, rep) < 0) {
		perror("EVIOCSREP");
		close(fd);
		return 1;
	}
	printf("%s: autorepeat delay=%ums period=%ums\n", argv[1], rep[0], rep[1]);
	close(fd);
	return 0;
}
