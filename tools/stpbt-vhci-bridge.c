// Bridge the MTK STP BT chardev (/dev/stpbt, H4-framed) to the kernel's
// virtual HCI controller (/dev/vhci) so the BT core exposes a real hci0.
//
// Opening /dev/stpbt triggers WMT BT function-on (CONSYS boot + patch);
// opening /dev/vhci creates hci0 after the driver's 1s auto-setup, then
// every H4 packet is pumped verbatim in both directions.
//
// The bridge also governs WMT PSM via /proc/driver/wmt_dbg: the CONSYS
// firmware dies if a WMT SLEEP lands mid-inquiry/page, and those RF ops
// carry no STP traffic for the PSM idle timer to notice. PSM is held off
// while HCI commands are outstanding, a long RF op is in flight, an ACL
// link is up, or <1 s since last traffic; enabled (30 ms idle) otherwise.
// bt-up.sh must set quick-sleep off ('1 0' > wmt_dbg) before the governor
// first enables PSM.
//
// Build: arm-linux-gnueabihf-gcc -static -O2 -o stpbt-vhci-bridge stpbt-vhci-bridge.c
// Run:   ./stpbt-vhci-bridge &   (stays resident; kill to tear down hci0)

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <time.h>

/* ---------------- PSM governor ---------------- */
static int psm_state = -1;	/* -1 unknown, 0 held off, 1 on (sleep allowed) */
static int outstanding;		/* HCI cmds without Command Complete/Status */
static int acl_links;		/* open ACL connections */
static long rf_deadline;	/* ms clock: long RF op assumed busy until then */
static long last_traffic;	/* ms clock of last packet either direction */

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static void psm_set(int on)
{
	int fd;

	if (psm_state == on)
		return;
	fd = open("/proc/driver/wmt_dbg", O_WRONLY);
	if (fd < 0)
		return;
	if (write(fd, on ? "0 1e" : "0 0", 4) < 0)
		fprintf(stderr, "psm toggle write failed: %s\n", strerror(errno));
	close(fd);
	psm_state = on;
	fprintf(stderr, "psm %s (out=%d acl=%d rf=%ldms)\n", on ? "on" : "off",
		outstanding, acl_links, rf_deadline ? rf_deadline - now_ms() : 0);
}

static void governor(void)
{
	long now = now_ms();
	int hold;

	/* self-heal: a lost Command Complete must not pin the radio awake */
	if (outstanding > 0 && now - last_traffic > 5000) {
		fprintf(stderr, "governor: clearing stale outstanding=%d\n",
			outstanding);
		outstanding = 0;
	}
	if (rf_deadline && now >= rf_deadline)
		rf_deadline = 0;

	hold = outstanding > 0 || acl_links > 0 || rf_deadline ||
	       now - last_traffic < 1000;
	psm_set(!hold);
}

/* host -> radio: HCI command tracking */
static void classify_tx(const unsigned char *b, int n)
{
	unsigned op;

	last_traffic = now_ms();
	if (n < 3 || b[0] != 0x01)
		return;
	outstanding++;
	op = b[1] | (b[2] << 8);
	switch (op) {
	case 0x0401:				/* Inquiry: <= 10.24s + margin */
		rf_deadline = now_ms() + 15000;
		break;
	case 0x0405:				/* Create Connection (page) */
	case 0x0409:				/* Accept Connection Request */
	case 0x0419:				/* Remote Name Request */
		rf_deadline = now_ms() + 8000;
		break;
	}
}

/* radio -> host: event tracking (b = one whole H4 packet) */
static void classify_rx(const unsigned char *b, int n)
{
	last_traffic = now_ms();
	if (n < 3 || b[0] != 0x04)
		return;
	switch (b[1]) {
	case 0x0e:				/* Command Complete */
	case 0x0f:				/* Command Status */
		if (outstanding > 0)
			outstanding--;
		break;
	case 0x01:				/* Inquiry Complete */
		rf_deadline = 0;
		break;
	case 0x03:				/* Connection Complete */
		rf_deadline = 0;
		if (n >= 4 && b[3] == 0x00)
			acl_links++;
		break;
	case 0x04:				/* Connection Request (inbound page) */
		rf_deadline = now_ms() + 8000;
		break;
	case 0x05:				/* Disconnection Complete */
		if (acl_links > 0)
			acl_links--;
		break;
	case 0x07:				/* Remote Name Request Complete */
		rf_deadline = 0;
		break;
	}
}

/* ---------------- H4 reframing ---------------- */

/* stpbt read() chunks don't respect H4 packet boundaries, but /dev/vhci
 * requires exactly one complete H4 packet per write. Accumulate the radio
 * stream and emit whole packets. */
static unsigned char acc[8192];
static int acc_len;

static int h4_pkt_len(const unsigned char *p, int len)
{
	if (len < 1)
		return 0;
	switch (p[0]) {
	case 0x04:	/* event: type, evt, plen */
		return (len < 3) ? 0 : 3 + p[2];
	case 0x02:	/* ACL: type, handle16, len16 */
		return (len < 5) ? 0 : 5 + (p[3] | (p[4] << 8));
	case 0x03:	/* SCO: type, handle16, len8 */
		return (len < 4) ? 0 : 4 + p[3];
	default:
		fprintf(stderr, "bad pkt type 0x%02x, resync\n", p[0]);
		return -1;
	}
}

static void pump_to_vhci(int vh)
{
	int pl, w;

	for (;;) {
		pl = h4_pkt_len(acc, acc_len);
		if (pl < 0) {			/* resync: drop one byte */
			memmove(acc, acc + 1, --acc_len);
			continue;
		}
		if (pl == 0 || acc_len < pl)	/* incomplete */
			return;
		classify_rx(acc, pl);
		if (acc[0] == 0x04 && acc[1] == 0x10) {
			fprintf(stderr, "dropping hw-error evt 0x%02x (MTK quirk)\n",
				acc[3]);
			memmove(acc, acc + pl, acc_len - pl);
			acc_len -= pl;
			continue;
		}
		w = write(vh, acc, pl);
		if (w != pl)
			fprintf(stderr, "vhci short write %d/%d\n", w, pl);
		memmove(acc, acc + pl, acc_len - pl);
		acc_len -= pl;
	}
}

int main(void)
{
	unsigned char buf[2048];
	int bt, vh, n, w;

	bt = open("/dev/stpbt", O_RDWR | O_NONBLOCK);
	if (bt < 0) {
		perror("open /dev/stpbt");
		return 1;
	}
	fprintf(stderr, "stpbt open (BT func on)\n");
	last_traffic = now_ms();
	psm_set(0);	/* hold awake through hci0 creation + adapter setup */

	/* drain stale events (e.g. MTK vendor startup event) so the BT core
	 * starts from a clean stream */
	usleep(300 * 1000);
	while ((n = read(bt, buf, sizeof(buf))) > 0)
		fprintf(stderr, "drained %d stale bytes\n", n);

	vh = open("/dev/vhci", O_RDWR);
	if (vh < 0) {
		perror("open /dev/vhci");
		return 1;
	}
	sleep(2);	/* let vhci auto-create the BR/EDR hci device */
	fprintf(stderr, "vhci open, hci device created - pumping\n");

	for (;;) {
		struct pollfd p[2] = {
			{ .fd = bt, .events = POLLIN },
			{ .fd = vh, .events = POLLIN },
		};
		if (poll(p, 2, 300) < 0)
			break;

		if (p[0].revents & POLLIN) {		/* radio -> host */
			n = read(bt, buf, sizeof(buf));
			if (n > 0) {
				if (acc_len + n > (int)sizeof(acc))
					acc_len = 0;	/* overflow: reset */
				memcpy(acc + acc_len, buf, n);
				acc_len += n;
				pump_to_vhci(vh);
			}
		}
		if (p[1].revents & POLLIN) {		/* host -> radio */
			n = read(vh, buf, sizeof(buf));
			if (n <= 0)
				break;
			if (n >= 4 && buf[0] == 0x01 &&
			    buf[1] == 0x04 && buf[2] == 0x10) {
				/* Read Local Extended Features: fake an
				 * all-zero page (no ext features) */
				unsigned char cc[17] = {
					0x04, 0x0e, 0x0e, 0x01, 0x04, 0x10,
					0x00,			/* status */
					n >= 5 ? buf[4] : 0,	/* page */
					0x00			/* max page */
				};
				fprintf(stderr, "shimming ReadLocalExtFeatures p%d\n",
					cc[7]);
				w = write(vh, cc, sizeof(cc));
				continue;
			}
			classify_tx(buf, n);
			w = write(bt, buf, n);
			if (w != n)
				fprintf(stderr, "stpbt short write %d/%d\n", w, n);
		}
		if ((p[0].revents | p[1].revents) & (POLLERR | POLLHUP))
			break;

		governor();
	}
	fprintf(stderr, "bridge exit (%s)\n", strerror(errno));
	psm_set(1);	/* bridge down: nothing outstanding, let the radio sleep */
	close(vh);
	close(bt);
	return 0;
}
