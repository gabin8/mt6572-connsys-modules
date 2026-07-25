// Minimal HCI smoke test over /dev/stpbt (MTK STP BT chardev).
// Opens non-blocking, sends HCI_Reset (01 03 0c 00), poll-reads the
// Command Complete event with a hard timeout. Never blocks the console.
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o stpbt-hci-test stpbt-hci-test.c

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>

int main(void)
{
	unsigned char hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };
	unsigned char buf[64];
	int fd, ret, got = 0, i;

	fd = open("/dev/stpbt", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open /dev/stpbt");
		return 1;
	}
	printf("stpbt opened (BT func on)\n");

	ret = write(fd, hci_reset, sizeof(hci_reset));
	printf("HCI_Reset write: %d/4\n", ret);

	for (i = 0; i < 40 && got < 7; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
			ret = read(fd, buf + got, sizeof(buf) - got);
			if (ret > 0)
				got += ret;
		}
	}

	printf("read %d bytes:", got);
	for (i = 0; i < got; i++)
		printf(" %02x", buf[i]);
	printf("\n");

	if (got >= 7 && buf[0] == 0x04 && buf[1] == 0x0e && buf[6] == 0x00) {
		printf("HCI RESET OK - radio answers, BT link is LIVE\n");
		close(fd);
		return 0;
	}
	printf("HCI RESET: no/short/unexpected response\n");
	close(fd);
	return 1;
}
