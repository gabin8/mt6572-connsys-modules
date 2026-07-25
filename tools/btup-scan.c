// Bring hci0 up and run a BT inquiry using raw kernel HCI ioctls
// (no BlueZ userspace needed). Prints discovered bdaddrs.
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o btup-scan btup-scan.c

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#define AF_BLUETOOTH_	31
#define BTPROTO_HCI	1
#define HCIDEVUP	_IOW('H', 201, int)
#define HCIINQUIRY	_IOR('H', 240, int)

struct hci_inquiry_req {
	uint16_t dev_id;
	uint16_t flags;
	uint8_t  lap[3];
	uint8_t  length;
	uint8_t  num_rsp;
};

struct inquiry_info {
	uint8_t  bdaddr[6];
	uint8_t  pscan_rep_mode;
	uint8_t  pscan_period_mode;
	uint8_t  pscan_mode;
	uint8_t  dev_class[3];
	uint16_t clock_offset;
} __attribute__((packed));

int main(int argc, char **argv)
{
	int dev = 0, sk, ret, i, n;
	uint8_t ibuf[sizeof(struct hci_inquiry_req) + 32 * sizeof(struct inquiry_info)];
	struct hci_inquiry_req *ir = (struct hci_inquiry_req *)ibuf;
	struct inquiry_info *info = (struct inquiry_info *)(ibuf + sizeof(*ir));

	sk = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI);
	if (sk < 0) {
		perror("hci socket (bluetooth.ko loaded?)");
		return 1;
	}

	ret = ioctl(sk, HCIDEVUP, dev);
	if (ret < 0 && errno != EALREADY)
		perror("HCIDEVUP");
	else
		printf("hci%d UP\n", dev);

	sleep(2);	/* controller init (reset, buffer sizes, bdaddr...) */

	memset(ibuf, 0, sizeof(ibuf));
	ir->dev_id = dev;
	ir->flags = 0x0001;	/* IREQ_CACHE_FLUSH */
	ir->lap[0] = 0x33; ir->lap[1] = 0x8b; ir->lap[2] = 0x9e; /* GIAC */
	ir->length = 8;		/* x 1.28 s */
	ir->num_rsp = 32;

	printf("inquiry (%.1fs)...\n", ir->length * 1.28);
	ret = ioctl(sk, HCIINQUIRY, (unsigned long)ibuf);
	if (ret < 0) {
		perror("HCIINQUIRY");
		close(sk);
		return 1;
	}

	n = ir->num_rsp;
	printf("inquiry complete: %d device(s)\n", n);
	for (i = 0; i < n; i++) {
		uint8_t *b = info[i].bdaddr;
		printf("  %02X:%02X:%02X:%02X:%02X:%02X  class %02x%02x%02x\n",
		       b[5], b[4], b[3], b[2], b[1], b[0],
		       info[i].dev_class[2], info[i].dev_class[1], info[i].dev_class[0]);
	}
	close(sk);
	return 0;
}
