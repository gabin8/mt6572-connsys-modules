// BTIF internal-loopback self-test for the mt6572-connsys-modules btif driver.
// Drives /dev/btif: open -> loopback on -> write pattern -> read back -> compare.
// Exercises the AP-side BTIF block + IRQ path without any CONSYS involvement.
//
// Cross-build: arm-linux-gnueabihf-gcc -static -O2 -o btif-lpbk-test btif-lpbk-test.c
// Run on device: ./btif-lpbk-test [len]   (default 64 bytes, max 1024)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>

#define BTIF_IOC_MAGIC        0xb0
#define BTIF_IOCTL_OPEN       _IOW(BTIF_IOC_MAGIC, 1, int)
#define BTIF_IOCTL_CLOSE      _IOW(BTIF_IOC_MAGIC, 2, int)
#define BTIF_IOCTL_LPBK_CTRL  _IOWR(BTIF_IOC_MAGIC, 3, int)
#define BTIF_IOCTL_REG_DUMP   _IOWR(BTIF_IOC_MAGIC, 7, int)
#define BTIF_IOCTL_DMA_CTRL   _IOWR(BTIF_IOC_MAGIC, 8, int)

/*
 * Stock /proc/interrupts (BT on) shows BT runs on the BTIF *DMA* path
 * (tx/rx dma irq active) while the PIO core btif irq stays 0. So DMA mode
 * is the real transport; default to it. Pass "pio" as arg to force PIO.
 * Args: [len] [hold_secs] [pio]
 */
int main(int argc, char **argv)
{
	unsigned char tx[1024], rx[1024];
	int len = argc > 1 ? atoi(argv[1]) : 64;
	int fd, i, got, ret, failed = 0;
	int use_dma = !(argc > 3 && !strcmp(argv[3], "pio"));

	if (len < 1 || len > (int)sizeof(tx))
		len = 64;

	fd = open("/dev/btif", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("open /dev/btif");
		return 1;
	}

	ret = ioctl(fd, BTIF_IOCTL_OPEN, 0);
	printf("BTIF open: %d\n", ret);
	if (ret) goto out;

	ret = ioctl(fd, BTIF_IOCTL_DMA_CTRL, use_dma ? 1 : 0);
	printf("%s mode: %d\n", use_dma ? "DMA" : "PIO", ret);

	ret = ioctl(fd, BTIF_IOCTL_LPBK_CTRL, 1);
	printf("loopback enable: %d\n", ret);
	if (ret) goto close_out;

	for (i = 0; i < len; i++)
		tx[i] = (unsigned char)(i ^ 0xA5);

	ret = write(fd, tx, len);
	printf("write: %d/%d\n", ret, len);
	if (ret != len) { failed = 1; goto dump_out; }

	usleep(50 * 1000);
	ioctl(fd, BTIF_IOCTL_REG_DUMP, 0); /* state snapshot to dmesg */

	got = 0;
	for (i = 0; i < 40 && got < len; i++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ret = poll(&pfd, 1, 50);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			ret = read(fd, rx + got, sizeof(rx) - got);
			if (ret > 0)
				got += ret;
		}
	}
	printf("read: %d/%d\n", got, len);

	if (got == len && !memcmp(tx, rx, len)) {
		printf("BTIF LOOPBACK PASS (%d bytes)\n", len);
	} else {
		failed = 1;
		printf("BTIF LOOPBACK FAIL (got %d, cmp %d)\n", got,
		       got == len ? memcmp(tx, rx, len) : -1);
		for (i = 0; i < (got > 16 ? 16 : got); i++)
			printf(" %02x", rx[i]);
		printf("\n");
	}

dump_out:
	if (failed)
		ioctl(fd, BTIF_IOCTL_REG_DUMP, 0); /* dump to dmesg */
	if (argc > 2) { /* hold: keep port open+looped for external inspection */
		printf("holding open %ss...\n", argv[2]);
		sleep(atoi(argv[2]));
	}
close_out:
	ioctl(fd, BTIF_IOCTL_CLOSE, 0);
out:
	close(fd);
	return failed;
}
