// Draw a cursor on the framebuffer from a relative pointing device.
// Proves an evdev mouse end to end without a display server.
//   fbcursor /dev/input/eventN [/dev/fb0]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define CUR 10				/* cursor square, pixels */

static unsigned char *fb;
static struct fb_var_screeninfo v;
static unsigned bpp, stride;
static unsigned int save[CUR * CUR];

static void put(unsigned x, unsigned y, unsigned int c)
{
	unsigned char *p;

	if (x >= v.xres || y >= v.yres)
		return;
	p = fb + y * stride + x * (bpp / 8);
	if (bpp == 32)
		*(unsigned int *)p = c;
	else
		*(unsigned short *)p = c;
}

static unsigned int get(unsigned x, unsigned y)
{
	unsigned char *p;

	if (x >= v.xres || y >= v.yres)
		return 0;
	p = fb + y * stride + x * (bpp / 8);
	return bpp == 32 ? *(unsigned int *)p : *(unsigned short *)p;
}

static void cursor(int x, int y, unsigned int col, int restore)
{
	int i, j;

	for (j = 0; j < CUR; j++)
		for (i = 0; i < CUR; i++) {
			if (restore)
				put(x + i, y + j, save[j * CUR + i]);
			else {
				save[j * CUR + i] = get(x + i, y + j);
				put(x + i, y + j, col);
			}
		}
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/input/event0";
	const char *fbdev = argc > 2 ? argv[2] : "/dev/fb0";
	struct input_event e;
	int fd, ffd, x, y, px, py;
	unsigned int col, red, green, blue, white;

	fd = open(dev, O_RDONLY);
	if (fd < 0) { perror(dev); return 1; }
	ffd = open(fbdev, O_RDWR);
	if (ffd < 0) { perror(fbdev); return 1; }
	if (ioctl(ffd, FBIOGET_VSCREENINFO, &v) < 0) { perror("vscreeninfo"); return 1; }

	bpp = v.bits_per_pixel;
	if (bpp == 32) {
		red = 0x00ff0000; green = 0x0000ff00;
		blue = 0x000000ff; white = 0x00ffffff;
	} else {
		red = 0xf800; green = 0x07e0; blue = 0x001f; white = 0xffff;
	}
	col = red;
	stride = v.xres_virtual * (bpp / 8);
	fb = mmap(NULL, stride * v.yres_virtual, PROT_READ | PROT_WRITE,
		  MAP_SHARED, ffd, 0);
	if (fb == MAP_FAILED) { perror("mmap"); return 1; }
	fprintf(stderr, "fb %ux%u %ubpp, cursor on %s\n", v.xres, v.yres, bpp, dev);

	x = v.xres / 2; y = v.yres / 2;
	px = x; py = y;
	cursor(x, y, col, 0);

	while (read(fd, &e, sizeof(e)) == sizeof(e)) {
		if (e.type == EV_REL) {
			if (e.code == REL_X) x += e.value;
			else if (e.code == REL_Y) y += e.value;
		} else if (e.type == EV_KEY) {
			if (e.code == BTN_LEFT)   col = e.value ? green : red;
			else if (e.code == BTN_RIGHT)  col = e.value ? blue : red;
			else if (e.code == BTN_MIDDLE) col = e.value ? white : red;
		} else if (e.type == EV_SYN) {
			if (x < 0) x = 0;
			if (y < 0) y = 0;
			if (x > (int)v.xres - CUR) x = v.xres - CUR;
			if (y > (int)v.yres - CUR) y = v.yres - CUR;
			if (x != px || y != py) {
				cursor(px, py, col, 1);
				cursor(x, y, col, 0);
				px = x; py = y;
			} else {
				cursor(x, y, col, 0);
			}
		}
	}
	return 0;
}
