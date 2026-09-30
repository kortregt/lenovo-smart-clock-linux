/*
 * clockface: a clock on the Smart Clock's screen, with LVGL on /dev/fb0.
 *
 * LVGL renders the 800x480 landscape screen; the flush callback rotates each area into
 * the 480x800 framebuffer (the panel is mounted sideways) and pans after the last area of a
 * frame, because the display only composes a new frame on a pan (docs/08-display.md).
 *
 *   clockface [-f font.ttf]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>

#include "lvgl.h"

#define SCREEN_W	800
#define SCREEN_H	480
#define DRAW_LINES	60

static const char *font_path = "/usr/share/fonts/inter/InterVariable.ttf";

static int fb_fd;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb;

static lv_obj_t *time_label, *date_label;

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Screen (sx, sy) is framebuffer (479 - sy, sx): screen columns are framebuffer rows.
 * LVGL's XRGB8888 is bytes B, G, R, X; the framebuffer wants R, G, B, X.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	const uint32_t *src = (const uint32_t *)px_map;
	int32_t w = lv_area_get_width(area);
	int32_t sx, sy;

	for (sx = area->x1; sx <= area->x2; sx++) {
		uint32_t *row = (uint32_t *)(fb + (size_t)sx * fix.line_length);

		/* walk up the screen so the framebuffer writes are sequential */
		for (sy = area->y2; sy >= area->y1; sy--) {
			uint32_t s = src[(sy - area->y1) * w + (sx - area->x1)];

			row[var.xres - 1 - sy] = 0xff000000 | (s & 0x0000ff00) |
						 ((s >> 16) & 0xff) | ((s & 0xff) << 16);
		}
	}
	if (lv_display_flush_is_last(disp)) {
		var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &var);
	}
	lv_display_flush_ready(disp);
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	void *data = NULL;
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		data = malloc(n);
		if (data && fread(data, 1, n, f) != (size_t)n) {
			free(data);
			data = NULL;
		}
		*len = n;
	}
	fclose(f);
	return data;
}

static void update_time(lv_timer_t *t)
{
	static char last[64];
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;

	(void)t;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), "%H:%M", &tm);
	if (strcmp(buf, last)) {
		strcpy(last, buf);
		lv_label_set_text(time_label, buf);
		strftime(buf, sizeof(buf), "%A %e %B", &tm);
		lv_label_set_text(date_label, buf);
	}
}

int main(int argc, char **argv)
{
	static uint32_t draw_buf[2][SCREEN_W * DRAW_LINES];
	lv_display_t *disp;
	lv_font_t *big, *small;
	lv_obj_t *scr;
	void *ttf;
	size_t ttf_len = 0;
	int opt;

	while ((opt = getopt(argc, argv, "f:")) != -1) {
		if (opt == 'f') {
			font_path = optarg;
		} else {
			fprintf(stderr, "usage: clockface [-f font.ttf]\n");
			return 2;
		}
	}

	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &var) ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("/dev/fb0");
		return 1;
	}
	if (var.bits_per_pixel != 32 || var.xres != SCREEN_H || var.yres != SCREEN_W) {
		fprintf(stderr, "clockface: expected a 480x800 32 bpp framebuffer\n");
		return 1;
	}
	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap /dev/fb0");
		return 1;
	}
	ttf = read_file(font_path, &ttf_len);
	if (!ttf) {
		perror(font_path);
		return 1;
	}

	lv_init();
	lv_tick_set_cb(tick_ms);
	disp = lv_display_create(SCREEN_W, SCREEN_H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, draw_buf[0], draw_buf[1], sizeof(draw_buf[0]),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush_cb);

	big = lv_tiny_ttf_create_data(ttf, ttf_len, 240);
	small = lv_tiny_ttf_create_data(ttf, ttf_len, 44);

	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	time_label = lv_label_create(scr);
	lv_obj_set_style_text_font(time_label, big, 0);
	lv_obj_set_style_text_color(time_label, lv_color_hex(0xe8e8e8), 0);
	lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -40);

	date_label = lv_label_create(scr);
	lv_obj_set_style_text_font(date_label, small, 0);
	lv_obj_set_style_text_color(date_label, lv_color_hex(0x808080), 0);
	lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 130);

	update_time(NULL);
	lv_timer_create(update_time, 1000, NULL);

	for (;;) {
		uint32_t idle = lv_timer_handler();

		usleep((idle > 100 ? 100 : idle) * 1000);
	}
}
