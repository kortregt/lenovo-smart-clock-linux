/*
 * clockface: a clock on the Smart Clock's screen, with LVGL on /dev/fb0.
 *
 * LVGL renders the 800x480 landscape screen; the flush callback rotates each area into
 * the 480x800 framebuffer (the panel is mounted sideways) and pans after the last area of a
 * frame, because the display only composes a new frame on a pan (docs/08-display.md).
 *
 * Under the date it shows the weather from /run/clock/weather (written by ha-poll from
 * Home Assistant): the condition on the first line, drawn as a Material Design Icons
 * weather icon (fonts/mdi-weather.ttf, built into the binary), and the text on the second.
 * Hidden when that file is missing or older than 15 minutes.
 *
 * Tapping the screen shows the forecast from /run/clock/forecast (also from ha-poll) for
 * 15 seconds; tapping again hides it. The touchscreen reports panel coordinates, so touches
 * are rotated the same way as the display.
 *
 *   clockface [-f font.ttf]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>

#include "lvgl.h"

#define SCREEN_W	800
#define SCREEN_H	480
#define DRAW_LINES	60

#define WEATHER_FILE	"/run/clock/weather"
#define WEATHER_MAX_AGE	(15 * 60)
#define FORECAST_FILE	"/run/clock/forecast"
#define FORECAST_DAYS	5
#define FORECAST_MS	15000
#define TOUCH_DEV	"/dev/input/event1"

static const char *font_path = "/usr/share/fonts/inter/InterVariable.ttf";

static int fb_fd;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb;

static lv_obj_t *time_label, *date_label, *weather_icon, *weather_label;
static lv_obj_t *forecast_panel, *fc_day[FORECAST_DAYS], *fc_icon[FORECAST_DAYS];
static lv_obj_t *fc_high[FORECAST_DAYS], *fc_low[FORECAST_DAYS];
static lv_timer_t *forecast_timer;

static int touch_fd = -1;
static int touch_x, touch_y, touch_down, touch_tapped;

/* fonts/mdi-weather.ttf: a subset of Material Design Icons (Apache 2.0) */
extern const unsigned char mdi_weather_ttf[], mdi_weather_ttf_end[];
__asm__(".section .rodata\n"
	".global mdi_weather_ttf\n.global mdi_weather_ttf_end\n"
	"mdi_weather_ttf:\n.incbin \"fonts/mdi-weather.ttf\"\nmdi_weather_ttf_end:\n"
	".previous\n");

/* Home Assistant weather conditions -> Material Design Icons codepoints */
static const struct { const char *condition; uint32_t codepoint; } weather_icons[] = {
	{ "sunny", 0xF0599 },		/* weather-sunny */
	{ "clear-night", 0xF0594 },	/* weather-night */
	{ "partlycloudy", 0xF0595 },	/* weather-partly-cloudy */
	{ "partlycloudy-night", 0xF0F31 }, /* weather-night-partly-cloudy */
	{ "cloudy", 0xF0590 },		/* weather-cloudy */
	{ "fog", 0xF0591 },		/* weather-fog */
	{ "hail", 0xF0592 },		/* weather-hail */
	{ "lightning", 0xF0593 },	/* weather-lightning */
	{ "lightning-rainy", 0xF067E },	/* weather-lightning-rainy */
	{ "pouring", 0xF0596 },		/* weather-pouring */
	{ "rainy", 0xF0597 },		/* weather-rainy */
	{ "snowy", 0xF0598 },		/* weather-snowy */
	{ "snowy-rainy", 0xF067F },	/* weather-snowy-rainy */
	{ "windy", 0xF059D },		/* weather-windy */
	{ "windy-variant", 0xF059E },	/* weather-windy-variant */
	{ "exceptional", 0xF05D6 },	/* alert-circle-outline */
};

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

/* The icon for a condition, as a UTF-8 string; empty if unknown */
static void weather_icon_text(const char *condition, char out[5])
{
	uint32_t cp = 0;
	size_t i;

	for (i = 0; i < sizeof(weather_icons) / sizeof(weather_icons[0]); i++)
		if (!strcmp(condition, weather_icons[i].condition))
			cp = weather_icons[i].codepoint;
	if (!cp) {
		out[0] = 0;
		return;
	}
	out[0] = 0xf0 | (cp >> 18);
	out[1] = 0x80 | ((cp >> 12) & 0x3f);
	out[2] = 0x80 | ((cp >> 6) & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	out[4] = 0;
}

static void update_weather(lv_timer_t *t)
{
	char condition[32] = "", text[96] = "", icon[5];
	struct stat st;
	FILE *f;

	(void)t;
	if (stat(WEATHER_FILE, &st) == 0 && time(NULL) - st.st_mtime < WEATHER_MAX_AGE &&
	    (f = fopen(WEATHER_FILE, "r"))) {
		if (fgets(condition, sizeof(condition), f) && fgets(text, sizeof(text), f)) {
			condition[strcspn(condition, "\n")] = 0;
			text[strcspn(text, "\n")] = 0;
		} else {
			condition[0] = text[0] = 0;
		}
		fclose(f);
	}
	weather_icon_text(condition, icon);
	if (strcmp(icon, lv_label_get_text(weather_icon)))
		lv_label_set_text(weather_icon, icon);
	if (strcmp(text, lv_label_get_text(weather_label)))
		lv_label_set_text(weather_label, text);
}

/*
 * Touch: the FT6336U driver (mtk-tpd) sends multitouch positions and BTN_TOUCH. A quick tap
 * can press and release between two reads, so a press is latched until it's been reported.
 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	struct input_event ev[16];
	ssize_t n;
	int i, sx, sy;

	(void)indev;
	while ((n = read(touch_fd, ev, sizeof(ev))) > 0) {
		for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
			if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_POSITION_X)
				touch_x = ev[i].value;
			else if (ev[i].type == EV_ABS && ev[i].code == ABS_MT_POSITION_Y)
				touch_y = ev[i].value;
			else if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) {
				touch_down = ev[i].value;
				if (touch_down)
					touch_tapped = 1;
			}
		}
	}
	/* panel (x, y) is screen (y, 479 - x), as for the display */
	sx = touch_y;
	sy = SCREEN_H - 1 - touch_x;
	data->point.x = sx < 0 ? 0 : sx >= SCREEN_W ? SCREEN_W - 1 : sx;
	data->point.y = sy < 0 ? 0 : sy >= SCREEN_H ? SCREEN_H - 1 : sy;
	data->state = touch_down || touch_tapped ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	touch_tapped = 0;
}

/* Fill the forecast panel from FORECAST_FILE; returns the number of days */
static int load_forecast(void)
{
	char line[128], icon[5], *f[4], *p;
	FILE *fp = fopen(FORECAST_FILE, "r");
	int n = 0, i;

	if (!fp)
		return 0;
	while (n < FORECAST_DAYS && fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = 0;
		for (i = 0, p = line; i < 4; i++) {
			f[i] = p;
			p += strcspn(p, "|");
			if (*p)
				*p++ = 0;
		}
		weather_icon_text(f[0], icon);
		lv_label_set_text(fc_icon[n], icon);
		lv_label_set_text(fc_day[n], f[1]);
		lv_label_set_text_fmt(fc_high[n], "%s°", f[2]);
		lv_label_set_text_fmt(fc_low[n], *f[3] ? "%s°" : "", f[3]);
		n++;
	}
	fclose(fp);
	return n;
}

static void hide_forecast(lv_timer_t *t)
{
	(void)t;
	lv_obj_set_hidden(forecast_panel, true);
	lv_timer_pause(forecast_timer);
}

static void on_tap(lv_event_t *e)
{
	if (lv_event_get_current_target(e) == forecast_panel) {
		hide_forecast(NULL);
	} else if (load_forecast() > 0) {
		lv_obj_set_hidden(forecast_panel, false);
		lv_timer_reset(forecast_timer);
		lv_timer_resume(forecast_timer);
	}
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, "");
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

static void create_forecast_panel(lv_obj_t *scr, const lv_font_t *text, const lv_font_t *icons)
{
	int i;

	forecast_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(forecast_panel);
	lv_obj_set_size(forecast_panel, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(forecast_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(forecast_panel, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(forecast_panel, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(forecast_panel, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(forecast_panel, true);
	lv_obj_set_hidden(forecast_panel, true);
	lv_obj_add_event_cb(forecast_panel, on_tap, LV_EVENT_CLICKED, NULL);

	for (i = 0; i < FORECAST_DAYS; i++) {
		lv_obj_t *col = lv_obj_create(forecast_panel);

		lv_obj_remove_style_all(col);
		lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 14, 0);
		lv_obj_set_clickable(col, false);
		fc_day[i] = label(col, text, 0x909090);
		fc_icon[i] = label(col, icons, 0xc0c0c0);
		fc_high[i] = label(col, text, 0xe8e8e8);
		fc_low[i] = label(col, text, 0x707070);
	}
	forecast_timer = lv_timer_create(hide_forecast, FORECAST_MS, NULL);
	lv_timer_pause(forecast_timer);
}

int main(int argc, char **argv)
{
	static uint32_t draw_buf[2][SCREEN_W * DRAW_LINES];
	lv_display_t *disp;
	lv_font_t *big, *medium, *small, *icons, *icons_big;
	lv_obj_t *scr, *weather_row;
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
	medium = lv_tiny_ttf_create_data(ttf, ttf_len, 44);
	small = lv_tiny_ttf_create_data(ttf, ttf_len, 36);
	icons = lv_tiny_ttf_create_data(mdi_weather_ttf, mdi_weather_ttf_end - mdi_weather_ttf, 48);
	icons_big = lv_tiny_ttf_create_data(mdi_weather_ttf, mdi_weather_ttf_end - mdi_weather_ttf, 96);

	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	time_label = lv_label_create(scr);
	lv_obj_set_style_text_font(time_label, big, 0);
	lv_obj_set_style_text_color(time_label, lv_color_hex(0xe8e8e8), 0);
	lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -60);

	date_label = lv_label_create(scr);
	lv_obj_set_style_text_font(date_label, medium, 0);
	lv_obj_set_style_text_color(date_label, lv_color_hex(0x909090), 0);
	lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 100);

	/* weather: icon and text side by side, centred as a pair */
	weather_row = lv_obj_create(scr);
	lv_obj_remove_style_all(weather_row);
	lv_obj_set_size(weather_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(weather_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(weather_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(weather_row, 14, 0);
	lv_obj_align(weather_row, LV_ALIGN_CENTER, 0, 165);

	weather_icon = lv_label_create(weather_row);
	lv_label_set_text(weather_icon, "");
	lv_obj_set_style_text_font(weather_icon, icons, 0);
	lv_obj_set_style_text_color(weather_icon, lv_color_hex(0x909090), 0);

	weather_label = lv_label_create(weather_row);
	lv_label_set_text(weather_label, "");
	lv_obj_set_style_text_font(weather_label, small, 0);
	lv_obj_set_style_text_color(weather_label, lv_color_hex(0x707070), 0);

	create_forecast_panel(scr, small, icons_big);
	lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);

	touch_fd = open(TOUCH_DEV, O_RDONLY | O_NONBLOCK);
	if (touch_fd >= 0) {
		lv_indev_t *touch = lv_indev_create();

		lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(touch, touch_read_cb);
	} else {
		perror(TOUCH_DEV);
	}

	update_time(NULL);
	lv_timer_create(update_time, 1000, NULL);
	update_weather(NULL);
	lv_timer_create(update_weather, 5000, NULL);

	for (;;) {
		uint32_t idle = lv_timer_handler();

		usleep((idle > 100 ? 100 : idle) * 1000);
	}
}
