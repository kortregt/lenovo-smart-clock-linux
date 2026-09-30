/*
 * clockface: a clock on the Smart Clock's screen, with LVGL on /dev/fb0.
 *
 * LVGL renders the 800x480 landscape screen; the flush callback rotates each area into
 * the 480x800 framebuffer (the panel is mounted sideways) and pans after the last area of a
 * frame, because the display only composes a new frame on a pan (docs/08-display.md).
 *
 * Under the date it shows the weather from /run/clock/weather (written by ha-poll from
 * Home Assistant): the condition on the first line, drawn as an animated Meteocons icon
 * (Lottie files in meteocons/, built into the binary), and the text on the second. Hidden
 * when that file is missing or older than 15 minutes.
 *
 * Tapping the screen shows the forecast from /run/clock/forecast (also from ha-poll) for
 * 15 seconds; tapping again hides it. The touchscreen reports panel coordinates, so touches
 * are rotated the same way as the display.
 *
 * Settings in /etc/clock/clockface.conf:
 *   ICON_ANIMATION=minute   the weather icon under the date: "always" animated, one
 *                           play-through every "minute" (default), or "still". Animating
 *                           it all the time costs ~20% of one CPU core. The forecast's
 *                           icons always animate while it's shown.
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

#define CONF_FILE	"/etc/clock/clockface.conf"

static const char *font_path = "/usr/share/fonts/inter/InterVariable.ttf";

static enum { ANIM_ALWAYS, ANIM_MINUTE, ANIM_STILL } icon_animation = ANIM_MINUTE;
static bool icon_playing;
static void play_icon_once(void);

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

/* Meteocons Lottie animations (meteocons/, MIT), built into the binary, NUL-terminated */
#define METEOCON(sym, file)							\
	extern const char sym[], sym##_end[];					\
	__asm__(".section .rodata\n.global " #sym "\n.global " #sym "_end\n"	\
		#sym ":\n.incbin \"meteocons/" file ".json\"\n"		\
		#sym "_end:\n.byte 0\n.previous\n");
METEOCON(mc_clear_day, "clear-day")
METEOCON(mc_clear_night, "clear-night")
METEOCON(mc_partly_cloudy_day, "partly-cloudy-day")
METEOCON(mc_partly_cloudy_night, "partly-cloudy-night")
METEOCON(mc_cloudy, "cloudy")
METEOCON(mc_fog, "fog")
METEOCON(mc_hail, "hail")
METEOCON(mc_thunderstorms, "thunderstorms")
METEOCON(mc_thunderstorms_rain, "thunderstorms-rain")
METEOCON(mc_extreme_rain, "extreme-rain")
METEOCON(mc_rain, "rain")
METEOCON(mc_snow, "snow")
METEOCON(mc_sleet, "sleet")
METEOCON(mc_wind, "wind")
METEOCON(mc_code_orange, "code-orange")
METEOCON(mc_not_available, "not-available")

/* Home Assistant weather conditions -> Meteocons */
static const struct { const char *condition, *data, *end; } weather_icons[] = {
	{ "sunny", mc_clear_day, mc_clear_day_end },
	{ "clear-night", mc_clear_night, mc_clear_night_end },
	{ "partlycloudy", mc_partly_cloudy_day, mc_partly_cloudy_day_end },
	{ "partlycloudy-night", mc_partly_cloudy_night, mc_partly_cloudy_night_end },
	{ "cloudy", mc_cloudy, mc_cloudy_end },
	{ "fog", mc_fog, mc_fog_end },
	{ "hail", mc_hail, mc_hail_end },
	{ "lightning", mc_thunderstorms, mc_thunderstorms_end },
	{ "lightning-rainy", mc_thunderstorms_rain, mc_thunderstorms_rain_end },
	{ "pouring", mc_extreme_rain, mc_extreme_rain_end },
	{ "rainy", mc_rain, mc_rain_end },
	{ "snowy", mc_snow, mc_snow_end },
	{ "snowy-rainy", mc_sleet, mc_sleet_end },
	{ "windy", mc_wind, mc_wind_end },
	{ "windy-variant", mc_wind, mc_wind_end },
	{ "exceptional", mc_code_orange, mc_code_orange_end },
};

#define ICON_SIZE	72
#define FC_ICON_SIZE	120
static uint32_t icon_buf[ICON_SIZE * ICON_SIZE];
static uint32_t fc_icon_buf[FORECAST_DAYS][FC_ICON_SIZE * FC_ICON_SIZE];

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
		if (icon_animation == ANIM_MINUTE)
			play_icon_once();
	}
}

/* Show the animation for a condition (Meteocons' "not available" if unknown) */
static bool set_icon(lv_obj_t *icon, const char *condition)
{
	const char *data = mc_not_available, *end = mc_not_available_end;
	size_t i;

	for (i = 0; i < sizeof(weather_icons) / sizeof(weather_icons[0]); i++) {
		if (!strcmp(condition, weather_icons[i].condition)) {
			data = weather_icons[i].data;
			end = weather_icons[i].end;
		}
	}
	if (lv_obj_get_user_data(icon) == data)
		return false;
	lv_obj_set_user_data(icon, (void *)data);
	lv_lottie_set_src_data(icon, data, end - data + 1);
	return true;
}

static lv_obj_t *weather_lottie(lv_obj_t *parent, int size, uint32_t *buf)
{
	lv_obj_t *icon = lv_lottie_create(parent);

	lv_lottie_set_buffer(icon, size, size, buf);
	return icon;
}

static void pause_icon(lv_obj_t *icon, bool pause)
{
	lv_anim_t *a = lv_lottie_get_anim(icon);

	if (!a)
		return;
	if (pause)
		lv_anim_pause(a);
	else
		lv_anim_resume(a);
}

static void update_weather(lv_timer_t *t)
{
	char condition[32] = "", text[96] = "";
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
	/* a new animation starts playing; hold it still unless it should be */
	if (*condition && set_icon(weather_icon, condition) &&
	    icon_animation != ANIM_ALWAYS && !icon_playing)
		pause_icon(weather_icon, true);
	lv_obj_set_hidden(weather_icon, !*condition);
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
	char line[128], *f[4], *p;
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
		set_icon(fc_icon[n], f[0]);
		lv_label_set_text(fc_day[n], f[1]);
		lv_label_set_text_fmt(fc_high[n], "%s°", f[2]);
		lv_label_set_text_fmt(fc_low[n], *f[3] ? "%s°" : "", f[3]);
		n++;
	}
	fclose(fp);
	return n;
}

static void stop_icon(lv_timer_t *t)
{
	(void)t;
	pause_icon(weather_icon, true);
	icon_playing = false;
}

/* ICON_ANIMATION=minute: play the weather icon's animation through once */
static void play_icon_once(void)
{
	lv_anim_t *a = lv_lottie_get_anim(weather_icon);
	lv_timer_t *t;

	if (!a || icon_playing || lv_obj_is_hidden(weather_icon))
		return;
	icon_playing = true;
	pause_icon(weather_icon, false);
	t = lv_timer_create(stop_icon, lv_anim_get_time(a), NULL);
	lv_timer_set_repeat_count(t, 1);
}

/* The forecast's animations only run while it's on screen */
static void pause_forecast_icons(bool pause)
{
	int i;

	for (i = 0; i < FORECAST_DAYS; i++)
		pause_icon(fc_icon[i], pause);
}

static void hide_forecast(lv_timer_t *t)
{
	(void)t;
	lv_obj_set_hidden(forecast_panel, true);
	lv_timer_pause(forecast_timer);
	pause_forecast_icons(true);
}

static void on_tap(lv_event_t *e)
{
	if (lv_event_get_current_target(e) == forecast_panel) {
		hide_forecast(NULL);
	} else if (load_forecast() > 0) {
		lv_obj_set_hidden(forecast_panel, false);
		pause_forecast_icons(false);
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

static void create_forecast_panel(lv_obj_t *scr, const lv_font_t *text)
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
		lv_obj_set_style_pad_row(col, 4, 0);
		lv_obj_set_clickable(col, false);
		fc_day[i] = label(col, text, 0x909090);
		fc_icon[i] = weather_lottie(col, FC_ICON_SIZE, fc_icon_buf[i]);
		fc_high[i] = label(col, text, 0xe8e8e8);
		fc_low[i] = label(col, text, 0x707070);
	}
	forecast_timer = lv_timer_create(hide_forecast, FORECAST_MS, NULL);
	lv_timer_pause(forecast_timer);
}

static void load_conf(void)
{
	char line[128];
	FILE *f = fopen(CONF_FILE, "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strcmp(line, "ICON_ANIMATION=always"))
			icon_animation = ANIM_ALWAYS;
		else if (!strcmp(line, "ICON_ANIMATION=minute"))
			icon_animation = ANIM_MINUTE;
		else if (!strcmp(line, "ICON_ANIMATION=still"))
			icon_animation = ANIM_STILL;
		else if (!strncmp(line, "ICON_ANIMATION=", 15))
			fprintf(stderr, "clockface: unknown %s\n", line);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	static uint32_t draw_buf[2][SCREEN_W * DRAW_LINES];
	lv_display_t *disp;
	lv_font_t *big, *medium, *small;
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

	load_conf();
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
	lv_obj_set_style_pad_column(weather_row, 6, 0);
	lv_obj_align(weather_row, LV_ALIGN_CENTER, 0, 165);

	weather_icon = weather_lottie(weather_row, ICON_SIZE, icon_buf);
	lv_obj_set_hidden(weather_icon, true);

	weather_label = lv_label_create(weather_row);
	lv_label_set_text(weather_label, "");
	lv_obj_set_style_text_font(weather_label, small, 0);
	lv_obj_set_style_text_color(weather_label, lv_color_hex(0x707070), 0);

	create_forecast_panel(scr, small);
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
