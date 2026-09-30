/*
 * fbtest [1|2]: draw a test image into /dev/fb0 through mmap, then pan so it shows.
 * A white border, a blue gradient down the framebuffer rows, and a yellow square in one
 * corner (1) or the opposite one (2). Prints a pixel read back through read() as a check.
 *
 * Build (static, so it runs on Alpine's musl):
 *   aarch64-linux-gnu-gcc -O2 -static -Wall -o fbtest userspace/fbtest.c
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int frame = argc > 1 ? atoi(argv[1]) : 1;
	unsigned int x, y, sx = frame == 1 ? 40 : 320, sy = frame == 1 ? 40 : 640;
	unsigned char *fb, c[4];
	int fd;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("/dev/fb0");
		return 1;
	}
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (y = 0; y < var.yres; y++) {
		for (x = 0; x < var.xres; x++) {
			unsigned char *p = fb + y * fix.line_length + x * 4;
			unsigned char r = 0, g = 0, b = 255 * y / var.yres;

			if (x < 8 || x >= var.xres - 8 || y < 8 || y >= var.yres - 8)
				r = g = b = 255;
			else if (x >= sx && x < sx + 120 && y >= sy && y < sy + 120)
				r = 255, g = 200, b = 0;
			/* R, G, B, X byte order (blue.offset 16) */
			p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
		}
	}
	if (pread(fd, c, 4, var.yres / 2 * fix.line_length + var.xres / 2 * 4) == 4)
		printf("read back at the centre: %02x %02x %02x %02x\n", c[0], c[1], c[2], c[3]);
	/* the display only composes a new frame on a pan */
	var.yoffset = 0;
	if (ioctl(fd, FBIOPAN_DISPLAY, &var)) {
		perror("FBIOPAN_DISPLAY");
		return 1;
	}
	return 0;
}
