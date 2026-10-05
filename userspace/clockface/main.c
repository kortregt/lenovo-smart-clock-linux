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
 * While Music Assistant plays on the clock, a now-playing view shows the cover, title,
 * artist and progress with previous / play-pause / next buttons, from /run/clock/media
 * (written by ha-media, which also sends the button presses to Home Assistant). Tapping
 * the cover goes back to the clock; it also goes back 30 seconds after playback stops.
 * While music plays behind the clock, a pill at the bottom shows the track; tapping it
 * brings the now-playing view back.
 *
 * Alarms (clock-alarm): the next one shows by a bell on the weather line; while one rings,
 * a full-screen Snooze / Stop view covers everything; while snoozed, tapping the bell
 * stops it.
 *
 * When /run/clock/volume changes (clock-volume, from the buttons or elsewhere), a volume bar
 * shows for 2 seconds.
 *
 * Settings in /etc/clock/clockface.conf:
 *   TIME_FORMAT=24          "12" or "24" hour time
 *   ICON_STYLE=ha           the weather icons: "ha", Home Assistant's own weather card
 *                           icons (ha-icons/, static; default), or animated "meteocons"
 *   ICON_ANIMATION=minute   with meteocons, the icon under the date: "always" animated, one
 *                           play-through every "minute" (default), or "still". Animating
 *                           it all the time costs ~20% of one CPU core. The forecast's
 *                           icons always animate while it's shown.
 *
 *   clockface [-f font.ttf]
 */
#include <fcntl.h>
#include <stdint.h>
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
#include <signal.h>

#include "lvgl.h"
#include "src/libs/tjpgd/tjpgd.h"	/* LVGL's copy of TJpgDec, for the cover art */

#define SCREEN_W	800
#define SCREEN_H	480
#define DRAW_LINES	60

#define WEATHER_FILE	"/run/clock/weather"
#define WEATHER_MAX_AGE	(15 * 60)
#define FORECAST_FILE	"/run/clock/forecast"
#define FORECAST_DAYS	5
#define FORECAST_MS	15000
#define TOUCH_DEV	"/dev/input/event1"
#define MEDIA_FILE	"/run/clock/media"
#define MEDIA_CTL	"/usr/local/bin/ha-media"
#define MEDIA_LINGER_S	30
#define ALARM_FILE	"/run/clock/alarm"
#define ALARM_NEXT_FILE	"/run/clock/alarm-next"
#define ALARM_CTL	"/usr/local/bin/clock-alarm"
#define VOLUME_FILE	"/run/clock/volume"
#define VOLUME_MAX	100
#define VOLUME_SHOW_MS	2000

#define CONF_FILE	"/etc/clock/clockface.conf"

static const char *font_path = "/usr/share/fonts/inter/InterVariable.ttf";

static enum { STYLE_METEOCONS, STYLE_HA } icon_style = STYLE_HA;
static enum { ANIM_ALWAYS, ANIM_MINUTE, ANIM_STILL } icon_animation = ANIM_MINUTE;
static bool icon_playing;
static bool time_12h;
static lv_obj_t *np_time, *alarm_time;
static void play_icon_once(void);

static int fb_fd;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
static unsigned char *fb;

static lv_obj_t *time_label, *date_label, *weather_label;
static lv_obj_t *forecast_panel, *fc_day[FORECAST_DAYS];
static lv_obj_t *fc_high[FORECAST_DAYS], *fc_low[FORECAST_DAYS];
static lv_obj_t *fc_loading;	/* "Forecast loading..." until ha-poll has one */
static lv_timer_t *forecast_timer;
static lv_obj_t *vol_panel, *vol_icon, *vol_bar;
static char alarm_state[64];	/* clock-alarm's state: idle, ringing, snoozed */
static lv_timer_t *vol_hide_timer;

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

/*
 * Home Assistant's weather card icons (ha-icons/, generated by ha-icons/generate.py from
 * the frontend, Apache 2.0) as PNGs at two sizes: under the date and in the forecast.
 */
#define HA_ICON(sym, file)							\
	extern const unsigned char ha_##sym##_s[], ha_##sym##_s_end[],		\
				   ha_##sym##_l[], ha_##sym##_l_end[];		\
	__asm__(".section .rodata\n"						\
		".global ha_" #sym "_s\n.global ha_" #sym "_s_end\n"		\
		".global ha_" #sym "_l\n.global ha_" #sym "_l_end\n"		\
		"ha_" #sym "_s:\n.incbin \"ha-icons/" file "-56.png\"\n"	\
		"ha_" #sym "_s_end:\n"						\
		"ha_" #sym "_l:\n.incbin \"ha-icons/" file "-96.png\"\n"	\
		"ha_" #sym "_l_end:\n.previous\n");
HA_ICON(sunny, "sunny")
HA_ICON(clear_night, "clear-night")
HA_ICON(partlycloudy, "partlycloudy")
HA_ICON(partlycloudy_night, "partlycloudy-night")
HA_ICON(cloudy, "cloudy")
HA_ICON(fog, "fog")
HA_ICON(hail, "hail")
HA_ICON(lightning, "lightning")
HA_ICON(lightning_rainy, "lightning-rainy")
HA_ICON(pouring, "pouring")
HA_ICON(rainy, "rainy")
HA_ICON(snowy, "snowy")
HA_ICON(snowy_rainy, "snowy-rainy")
HA_ICON(windy, "windy")
HA_ICON(windy_variant, "windy-variant")
HA_ICON(exceptional, "exceptional")
#define HA_SMALL	56
#define HA_LARGE	96

/* Home Assistant weather conditions -> Meteocons and HA icons */
#define ICON(cond, mc, ha) \
	{ cond, mc, mc##_end, { ha_##ha##_s, ha_##ha##_l }, { ha_##ha##_s_end, ha_##ha##_l_end } }
static const struct {
	const char *condition, *lottie, *lottie_end;
	const unsigned char *png[2], *png_end[2];	/* small, large */
} weather_icons[] = {
	ICON("sunny", mc_clear_day, sunny),
	ICON("clear-night", mc_clear_night, clear_night),
	ICON("partlycloudy", mc_partly_cloudy_day, partlycloudy),
	ICON("partlycloudy-night", mc_partly_cloudy_night, partlycloudy_night),
	ICON("cloudy", mc_cloudy, cloudy),
	ICON("fog", mc_fog, fog),
	ICON("hail", mc_hail, hail),
	ICON("lightning", mc_thunderstorms, lightning),
	ICON("lightning-rainy", mc_thunderstorms_rain, lightning_rainy),
	ICON("pouring", mc_extreme_rain, pouring),
	ICON("rainy", mc_rain, rainy),
	ICON("snowy", mc_snow, snowy),
	ICON("snowy-rainy", mc_sleet, snowy_rainy),
	ICON("windy", mc_wind, windy),
	ICON("windy-variant", mc_wind, windy_variant),
	ICON("exceptional", mc_code_orange, exceptional),
};
#define N_ICONS		(sizeof(weather_icons) / sizeof(weather_icons[0]))
#define ICON_UNKNOWN	N_ICONS		/* Meteocons "not available" / HA "exceptional" */
static lv_image_dsc_t ha_dsc[N_ICONS][2];

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

/*
 * The big time: hours, colon and minutes as three labels, so the colon can sit at the
 * digits' middle. Inter's own colon sits at lowercase height and only rises between
 * digits through an OpenType substitution, which TinyTTF doesn't apply.
 */
#define COLON_RAISE	26	/* px, for the 240 px font */

static lv_obj_t *big_time_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
	lv_obj_t *row = lv_obj_create(parent), *l;
	int i;

	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_clickable(row, false);	/* taps go through to the screen, as on a label */
	for (i = 0; i < 3; i++) {
		l = lv_label_create(row);
		lv_label_set_text(l, i == 1 ? ":" : "");
		lv_obj_set_style_text_font(l, font, 0);
		lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	}
	lv_obj_set_style_translate_y(lv_obj_get_child(row, 1), -COLON_RAISE, 0);
	return row;
}

static void big_time_set(lv_obj_t *row, const char *hm)	/* "10:16" */
{
	const char *colon = strchr(hm, ':');

	if (!colon)
		return;
	lv_label_set_text_fmt(lv_obj_get_child(row, 0), "%.*s", (int)(colon - hm), hm);
	lv_label_set_text(lv_obj_get_child(row, 2), colon + 1);
}

static void update_time(lv_timer_t *t)
{
	static char last[64];
	char buf[64];
	time_t now = time(NULL);
	struct tm tm;

	(void)t;
	localtime_r(&now, &tm);
	strftime(buf, sizeof(buf), time_12h ? "%-I:%M" : "%H:%M", &tm);
	if (strcmp(buf, last)) {
		strcpy(last, buf);
		big_time_set(time_label, buf);
		if (np_time)
			lv_label_set_text(np_time, buf);
		if (alarm_time)
			big_time_set(alarm_time, buf);
		strftime(buf, sizeof(buf), "%A %e %B", &tm);
		lv_label_set_text(date_label, buf);
		if (icon_animation == ANIM_MINUTE)
			play_icon_once();
	}
}

/* A weather icon: a Lottie animation (meteocons) or a PNG image (ha) */
struct icon {
	lv_obj_t *obj;
	int large;		/* forecast size */
	int shown;		/* index into weather_icons, ICON_UNKNOWN, or -1 for none */
};
static struct icon weather_icon, fc_icon[FORECAST_DAYS];

static void icon_create(struct icon *icon, lv_obj_t *parent, int large, uint32_t *lottie_buf)
{
	int size = large ? FC_ICON_SIZE : ICON_SIZE;

	icon->large = large;
	icon->shown = -1;
	if (icon_style == STYLE_HA) {
		icon->obj = lv_image_create(parent);
	} else {
		icon->obj = lv_lottie_create(parent);
		lv_lottie_set_buffer(icon->obj, size, size, lottie_buf);
	}
}

/* Show the icon for a condition; returns whether it changed */
static bool set_icon(struct icon *icon, const char *condition)
{
	int i, n = ICON_UNKNOWN;

	for (i = 0; i < (int)N_ICONS; i++)
		if (!strcmp(condition, weather_icons[i].condition))
			n = i;
	if (icon_style == STYLE_HA && n == ICON_UNKNOWN)
		n = N_ICONS - 1;			/* "exceptional" */
	if (n == icon->shown)
		return false;
	icon->shown = n;
	if (icon_style == STYLE_HA)
		lv_image_set_src(icon->obj, &ha_dsc[n][icon->large]);
	else if (n == ICON_UNKNOWN)
		lv_lottie_set_src_data(icon->obj, mc_not_available,
				       mc_not_available_end - mc_not_available + 1);
	else
		lv_lottie_set_src_data(icon->obj, weather_icons[n].lottie,
				       weather_icons[n].lottie_end - weather_icons[n].lottie + 1);
	return true;
}

/* PNG image descriptors for the ha style, decoded by LVGL's lodepng */
static void init_ha_icons(void)
{
	size_t i;
	int l;

	for (i = 0; i < N_ICONS; i++) {
		for (l = 0; l < 2; l++) {
			lv_image_dsc_t *d = &ha_dsc[i][l];

			d->header.magic = LV_IMAGE_HEADER_MAGIC;
			d->header.cf = LV_COLOR_FORMAT_RAW_ALPHA;
			d->header.w = d->header.h = l ? HA_LARGE : HA_SMALL;
			d->data = weather_icons[i].png[l];
			d->data_size = weather_icons[i].png_end[l] - weather_icons[i].png[l];
		}
	}
}

static void pause_icon(struct icon *icon, bool pause)
{
	lv_anim_t *a;

	if (icon_style != STYLE_METEOCONS || !(a = lv_lottie_get_anim(icon->obj)))
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
	if (*condition && set_icon(&weather_icon, condition) &&
	    icon_animation != ANIM_ALWAYS && !icon_playing)
		pause_icon(&weather_icon, true);
	lv_obj_set_hidden(weather_icon.obj, !*condition);
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
		set_icon(&fc_icon[n], f[0]);
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
	pause_icon(&weather_icon, true);
	icon_playing = false;
}

/* ICON_ANIMATION=minute: play the weather icon's animation through once */
static void play_icon_once(void)
{
	lv_anim_t *a;
	lv_timer_t *t;

	if (icon_style != STYLE_METEOCONS || icon_playing || lv_obj_is_hidden(weather_icon.obj) ||
	    !(a = lv_lottie_get_anim(weather_icon.obj)))
		return;
	icon_playing = true;
	pause_icon(&weather_icon, false);
	t = lv_timer_create(stop_icon, lv_anim_get_time(a), NULL);
	lv_timer_set_repeat_count(t, 1);
}

/* The forecast's animations only run while it's on screen */
static void pause_forecast_icons(bool pause)
{
	int i;

	for (i = 0; i < FORECAST_DAYS; i++)
		pause_icon(&fc_icon[i], pause);
}

static void hide_forecast(lv_timer_t *t)
{
	(void)t;
	lv_obj_set_hidden(forecast_panel, true);
	lv_timer_pause(forecast_timer);
	pause_forecast_icons(true);
}

/* Show the days, or "loading" while there's no forecast yet (just after boot) */
static void fill_forecast(void)
{
	int n = load_forecast(), i;

	lv_obj_set_hidden(fc_loading, n > 0);
	for (i = 0; i < FORECAST_DAYS; i++)
		lv_obj_set_hidden(lv_obj_get_parent(fc_day[i]), i >= n);
}

static void retry_forecast(lv_timer_t *t)
{
	(void)t;
	if (!lv_obj_is_hidden(forecast_panel) && !lv_obj_is_hidden(fc_loading))
		fill_forecast();
}

static void on_tap(lv_event_t *e)
{
	if (lv_event_get_current_target(e) == forecast_panel) {
		hide_forecast(NULL);
	} else {
		fill_forecast();
		lv_obj_set_hidden(forecast_panel, false);
		pause_forecast_icons(false);
		lv_timer_reset(forecast_timer);
		lv_timer_resume(forecast_timer);
	}
}

static void hide_volume(lv_timer_t *t)
{
	(void)t;
	lv_obj_set_hidden(vol_panel, true);
	lv_timer_pause(vol_hide_timer);
}

/* Show the volume bar when VOLUME_FILE changes (not for its state at startup) */
static void check_volume(lv_timer_t *t)
{
	static struct timespec last;
	static bool started;
	struct stat st;
	FILE *f;
	int level = -1;

	(void)t;
	if (stat(VOLUME_FILE, &st))
		return;
	if (st.st_mtim.tv_sec == last.tv_sec && st.st_mtim.tv_nsec == last.tv_nsec)
		return;
	last = st.st_mtim;
	if (!started) {
		started = true;
		return;
	}
	if ((f = fopen(VOLUME_FILE, "r"))) {
		if (fscanf(f, "%d", &level) != 1)
			level = -1;
		fclose(f);
	}
	/* while an alarm rings, its fade-up would cover the Snooze / Stop buttons */
	if (level < 0 || !strcmp(alarm_state, "ringing"))
		return;
	lv_bar_set_value(vol_bar, level, LV_ANIM_OFF);
	lv_label_set_text(vol_icon, level == 0 ? LV_SYMBOL_MUTE :
			  level < VOLUME_MAX / 2 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
	lv_obj_set_hidden(vol_panel, false);
	lv_obj_move_foreground(vol_panel);	/* over the alarm editor */
	lv_timer_reset(vol_hide_timer);
	lv_timer_resume(vol_hide_timer);
}

static void create_volume_panel(lv_obj_t *scr)
{
	vol_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(vol_panel);
	lv_obj_set_size(vol_panel, 520, 90);
	lv_obj_align(vol_panel, LV_ALIGN_BOTTOM_MID, 0, -30);
	lv_obj_set_style_bg_color(vol_panel, lv_color_hex(0x181818), 0);
	lv_obj_set_style_bg_opa(vol_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(vol_panel, 45, 0);
	lv_obj_set_style_pad_hor(vol_panel, 32, 0);
	lv_obj_set_style_pad_column(vol_panel, 24, 0);
	lv_obj_set_flex_flow(vol_panel, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(vol_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_hidden(vol_panel, true);

	vol_icon = lv_label_create(vol_panel);
	lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(vol_icon, lv_color_hex(0xe8e8e8), 0);
	lv_obj_set_width(vol_icon, 40);
	lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);

	vol_bar = lv_bar_create(vol_panel);
	lv_obj_set_flex_grow(vol_bar, 1);
	lv_obj_set_height(vol_bar, 14);
	lv_bar_set_range(vol_bar, 0, VOLUME_MAX);
	lv_obj_set_style_bg_color(vol_bar, lv_color_hex(0x404040), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(vol_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(vol_bar, lv_color_hex(0xe8e8e8), LV_PART_INDICATOR);
	lv_obj_set_style_radius(vol_bar, 7, LV_PART_MAIN);
	lv_obj_set_style_radius(vol_bar, 7, LV_PART_INDICATOR);

	vol_hide_timer = lv_timer_create(hide_volume, VOLUME_SHOW_MS, NULL);
	lv_timer_pause(vol_hide_timer);
	check_volume(NULL);
	lv_timer_create(check_volume, 100, NULL);
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
		icon_create(&fc_icon[i], col, 1, fc_icon_buf[i]);
		fc_high[i] = label(col, text, 0xe8e8e8);
		fc_low[i] = label(col, text, 0x707070);
	}
	fc_loading = label(forecast_panel, text, 0x707070);
	lv_label_set_text(fc_loading, "Forecast loading...");
	lv_obj_set_floating(fc_loading, true);
	lv_obj_center(fc_loading);
	lv_obj_set_hidden(fc_loading, true);
	lv_timer_create(retry_forecast, 1000, NULL);

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
		if (!strcmp(line, "TIME_FORMAT=12"))
			time_12h = true;
		else if (!strcmp(line, "TIME_FORMAT=24"))
			time_12h = false;
		else if (!strcmp(line, "ICON_ANIMATION=always"))
			icon_animation = ANIM_ALWAYS;
		else if (!strcmp(line, "ICON_ANIMATION=minute"))
			icon_animation = ANIM_MINUTE;
		else if (!strcmp(line, "ICON_ANIMATION=still"))
			icon_animation = ANIM_STILL;
		else if (!strcmp(line, "ICON_STYLE=meteocons"))
			icon_style = STYLE_METEOCONS;
		else if (!strcmp(line, "ICON_STYLE=ha"))
			icon_style = STYLE_HA;
		else if (!strncmp(line, "ICON_ANIMATION=", 15) || !strncmp(line, "ICON_STYLE=", 11))
			fprintf(stderr, "clockface: unknown %s\n", line);
	}
	fclose(f);
}

/* ---- now playing ------------------------------------------------------------------ */

static struct media {
	char state[16], title[160], artist[160], album[160], cover[128];
	long duration, position, position_at;
} media;
static lv_obj_t *np_panel, *np_cover, *np_title, *np_artist, *np_bar, *np_elapsed, *np_total;
static lv_obj_t *np_play, *np_pill, *np_pill_text;
static bool np_dismissed;		/* the cover was tapped: stay on the clock */
static time_t np_inactive_since;

static bool media_active(void)
{
	return !strcmp(media.state, "playing") || !strcmp(media.state, "paused");
}

static void set_text_if(lv_obj_t *label, const char *text)
{
	if (strcmp(lv_label_get_text(label), text))
		lv_label_set_text(label, text);
}

static void fmt_time(char *buf, size_t len, long s)
{
	snprintf(buf, len, "%ld:%02ld", s / 60, s % 60);
}

static void media_command(const char *cmd)
{
	if (fork() == 0) {
		execl(MEDIA_CTL, MEDIA_CTL, cmd, (char *)NULL);
		_exit(127);
	}
}

static void on_media_button(lv_event_t *e)
{
	const char *cmd = lv_event_get_user_data(e);

	media_command(cmd);
	if (!strcmp(cmd, "play_pause")) {	/* show it straight away; ha-media confirms */
		bool playing = !strcmp(media.state, "playing");

		lv_label_set_text(np_play, playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
	}
}

static void on_cover_tap(lv_event_t *e)
{
	(void)e;
	np_dismissed = true;
	lv_obj_set_hidden(np_panel, true);
}

static void on_pill_tap(lv_event_t *e)
{
	(void)e;
	np_dismissed = false;
	lv_obj_set_hidden(forecast_panel, true);
	lv_obj_set_hidden(np_panel, false);
}

/* The pill shows while music plays behind the clock; the clock moves up to make room */
static void update_pill(void)
{
	bool show = media_active() && lv_obj_is_hidden(np_panel);
	int32_t dy = show ? -36 : 0;

	if (show == !lv_obj_is_hidden(np_pill))
		return;
	lv_obj_set_hidden(np_pill, !show);
	lv_obj_set_style_translate_y(time_label, dy, 0);
	lv_obj_set_style_translate_y(date_label, dy, 0);
	lv_obj_set_style_translate_y(lv_obj_get_parent(weather_icon.obj), dy, 0);
}

static void update_progress(void)
{
	long pos = media.position;
	char buf[48];

	if (!strcmp(media.state, "playing") && media.position_at > 0)
		pos += time(NULL) - media.position_at;
	if (media.duration > 0 && pos > media.duration)
		pos = media.duration;
	if (pos < 0)
		pos = 0;
	lv_bar_set_range(np_bar, 0, media.duration > 0 ? media.duration : 1);
	lv_bar_set_value(np_bar, media.duration > 0 ? pos : 0, LV_ANIM_OFF);
	fmt_time(buf, sizeof(buf), pos);
	set_text_if(np_elapsed, media.duration > 0 ? buf : "");
	fmt_time(buf, sizeof(buf), media.duration);
	set_text_if(np_total, media.duration > 0 ? buf : "");
}

/* Reload MEDIA_FILE when it changes; returns whether it did */
static bool load_media(void)
{
	static struct timespec last;
	struct stat st;
	char line[256], *v;
	FILE *f;

	if (stat(MEDIA_FILE, &st) ||
	    (st.st_mtim.tv_sec == last.tv_sec && st.st_mtim.tv_nsec == last.tv_nsec))
		return false;
	last = st.st_mtim;
	if (!(f = fopen(MEDIA_FILE, "r")))
		return false;
	/* position and position_at are kept when Home Assistant briefly has none (it drops
	 * them while playback changes state), so the progress bar doesn't jump to 0:00 */
	media.state[0] = media.title[0] = media.artist[0] = media.album[0] = media.cover[0] = 0;
	media.duration = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!(v = strchr(line, '=')))
			continue;
		*v++ = 0;
		if (!strcmp(line, "state"))
			snprintf(media.state, sizeof(media.state), "%s", v);
		else if (!strcmp(line, "title"))
			snprintf(media.title, sizeof(media.title), "%s", v);
		else if (!strcmp(line, "artist"))
			snprintf(media.artist, sizeof(media.artist), "%s", v);
		else if (!strcmp(line, "album"))
			snprintf(media.album, sizeof(media.album), "%s", v);
		else if (!strcmp(line, "cover"))
			snprintf(media.cover, sizeof(media.cover), "%s", v);
		else if (!strcmp(line, "duration"))
			media.duration = atol(v);
		else if (!strcmp(line, "position") && *v)
			media.position = atol(v);
		else if (!strcmp(line, "position_at") && *v)
			media.position_at = atol(v);
	}
	fclose(f);
	return true;
}

/*
 * Cover art: decoded here in full with TJpgDec (LVGL's own decoder works in strips, which it
 * can't scale) and box-filtered to exactly COVER_SIZE square, so LVGL just draws it. Two
 * buffers alternate, so a new cover never replaces one that's still on screen.
 */
#define COVER_SIZE	320

struct jpeg_src {
	FILE *f;
	uint8_t *rgb;		/* full image, 3 bytes per pixel (B, G, R from LVGL's TJpgDec) */
	unsigned w;
};

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t len)
{
	struct jpeg_src *src = jd->device;

	if (!buf)
		return fseek(src->f, (long)len, SEEK_CUR) ? 0 : len;
	return fread(buf, 1, len, src->f);
}

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *r)
{
	struct jpeg_src *src = jd->device;
	unsigned w = r->right - r->left + 1, y;

	for (y = r->top; y <= r->bottom; y++)
		memcpy(src->rgb + ((size_t)y * src->w + r->left) * 3,
		       (uint8_t *)bitmap + (size_t)(y - r->top) * w * 3, w * 3);
	return 1;
}

/* Decode a JPEG file into out (COVER_SIZE^2 pixels, LVGL XRGB8888); returns 0 if it worked */
static int decode_cover(const char *path, uint32_t *out)
{
	static uint8_t pool[16384];
	struct jpeg_src src = { 0 };
	JDEC jd;
	unsigned x, y, iw, ih;
	int ret = -1;

	if (!(src.f = fopen(path, "rb")))
		return -1;
	if (jd_prepare(&jd, jpeg_in, pool, sizeof(pool), &src) != JDR_OK)
		goto out;
	iw = jd.width;
	ih = jd.height;
	src.w = iw;
	if (!(src.rgb = malloc((size_t)iw * ih * 3)))
		goto out;
	if (jd_decomp(&jd, jpeg_out, 0) != JDR_OK)
		goto out;
	/* box filter: each output pixel averages the input pixels it covers */
	for (y = 0; y < COVER_SIZE; y++) {
		unsigned y0 = y * ih / COVER_SIZE, y1 = (y + 1) * ih / COVER_SIZE;

		if (y1 <= y0)
			y1 = y0 + 1;
		for (x = 0; x < COVER_SIZE; x++) {
			unsigned x0 = x * iw / COVER_SIZE, x1 = (x + 1) * iw / COVER_SIZE;
			unsigned r = 0, g = 0, b = 0, n, xx, yy;

			if (x1 <= x0)
				x1 = x0 + 1;
			n = (x1 - x0) * (y1 - y0);
			for (yy = y0; yy < y1; yy++) {
				const uint8_t *p = src.rgb + ((size_t)yy * iw + x0) * 3;

				for (xx = x0; xx < x1; xx++, p += 3) {
					r += p[0];
					g += p[1];
					b += p[2];
				}
			}
			out[y * COVER_SIZE + x] = 0xff000000 | (b / n) << 16 | (g / n) << 8 | (r / n);
		}
	}
	ret = 0;
out:
	free(src.rgb);
	fclose(src.f);
	return ret;
}

static void set_cover(const char *path)
{
	static uint32_t *buf[2];
	static lv_image_dsc_t dsc[2];
	static int cur;

	if (!*path || (!buf[0] && (!(buf[0] = malloc(COVER_SIZE * COVER_SIZE * 4)) ||
				   !(buf[1] = malloc(COVER_SIZE * COVER_SIZE * 4))))) {
		lv_image_set_src(np_cover, NULL);
		return;
	}
	cur = !cur;
	if (decode_cover(path, buf[cur])) {
		fprintf(stderr, "clockface: can't decode %s\n", path);
		lv_image_set_src(np_cover, NULL);
		return;
	}
	dsc[cur].header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc[cur].header.cf = LV_COLOR_FORMAT_XRGB8888;
	dsc[cur].header.w = dsc[cur].header.h = COVER_SIZE;
	dsc[cur].header.stride = COVER_SIZE * 4;
	dsc[cur].data = (const uint8_t *)buf[cur];
	dsc[cur].data_size = COVER_SIZE * COVER_SIZE * 4;
	lv_image_set_src(np_cover, &dsc[cur]);
}

static void update_media(lv_timer_t *t)
{
	static char last_title[160], last_state[16], cover_src[140];
	bool changed = load_media();

	(void)t;
	if (changed) {
		char src[140] = "";

		/* a new track, or starting to play, brings the view back */
		if (strcmp(media.title, last_title) ||
		    (!strcmp(media.state, "playing") && strcmp(last_state, "playing")))
			np_dismissed = false;
		snprintf(last_title, sizeof(last_title), "%s", media.title);
		snprintf(last_state, sizeof(last_state), "%s", media.state);

		set_text_if(np_title, media.title);
		set_text_if(np_artist, media.artist);
		{
			char pill[340];

			snprintf(pill, sizeof(pill), *media.artist ? "%s  ·  %s" : "%s", media.title,
				 media.artist);
			set_text_if(np_pill_text, pill);
		}
		set_text_if(np_play, !strcmp(media.state, "playing") ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
		snprintf(src, sizeof(src), "%s", media.cover);
		if (strcmp(src, cover_src)) {
			snprintf(cover_src, sizeof(cover_src), "%s", src);
			set_cover(src);
		}
	}

	if (media_active()) {
		np_inactive_since = 0;
		if (!np_dismissed && lv_obj_is_hidden(np_panel)) {
			lv_obj_set_hidden(forecast_panel, true);
			lv_obj_set_hidden(np_panel, false);
		}
	} else if (!lv_obj_is_hidden(np_panel)) {
		if (!np_inactive_since)
			np_inactive_since = time(NULL);
		else if (time(NULL) - np_inactive_since >= MEDIA_LINGER_S)
			lv_obj_set_hidden(np_panel, true);
	}
	if (!lv_obj_is_hidden(np_panel))
		update_progress();
	update_pill();
}

static lv_obj_t *media_button(lv_obj_t *parent, const char *symbol, const char *cmd)
{
	lv_obj_t *b = lv_obj_create(parent), *l;

	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, 110, 90);
	lv_obj_set_clickable(b, true);
	lv_obj_set_style_radius(b, 45, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x303030), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, on_media_button, LV_EVENT_CLICKED, (void *)cmd);
	l = lv_label_create(b);
	lv_label_set_text(l, symbol);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0xe8e8e8), 0);
	lv_obj_center(l);
	return l;
}

static void create_now_playing(lv_obj_t *scr, const lv_font_t *title_font,
			       const lv_font_t *text_font, const lv_font_t *small_font)
{
	lv_obj_t *col, *row;

	np_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(np_panel);
	lv_obj_set_size(np_panel, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(np_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(np_panel, LV_OPA_COVER, 0);
	lv_obj_set_clickable(np_panel, true);	/* don't let taps reach the clock */
	lv_obj_set_hidden(np_panel, true);

	np_cover = lv_image_create(np_panel);
	lv_obj_set_size(np_cover, COVER_SIZE, COVER_SIZE);
	lv_obj_align(np_cover, LV_ALIGN_LEFT_MID, 40, 0);
	lv_obj_set_style_bg_color(np_cover, lv_color_hex(0x202020), 0);
	lv_obj_set_style_bg_opa(np_cover, LV_OPA_COVER, 0);
	lv_obj_set_clickable(np_cover, true);
	lv_obj_add_event_cb(np_cover, on_cover_tap, LV_EVENT_CLICKED, NULL);

	np_time = lv_label_create(np_panel);
	lv_label_set_text(np_time, "");
	lv_obj_set_style_text_font(np_time, text_font, 0);
	lv_obj_set_style_text_color(np_time, lv_color_hex(0x707070), 0);
	lv_obj_align(np_time, LV_ALIGN_TOP_RIGHT, -40, 24);

	col = lv_obj_create(np_panel);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, 360, LV_SIZE_CONTENT);
	lv_obj_align(col, LV_ALIGN_TOP_LEFT, 400, 96);	/* below the small clock */
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 8, 0);
	lv_obj_set_clickable(col, false);

	np_title = lv_label_create(col);
	/* at most two lines, then "..." */
	lv_obj_set_size(np_title, lv_pct(100), 2 * lv_font_get_line_height(title_font));
	lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(np_title, "");
	lv_obj_set_style_text_font(np_title, title_font, 0);
	lv_obj_set_style_text_color(np_title, lv_color_hex(0xe8e8e8), 0);

	np_artist = lv_label_create(col);
	lv_obj_set_size(np_artist, lv_pct(100), lv_font_get_line_height(text_font));
	lv_label_set_long_mode(np_artist, LV_LABEL_LONG_MODE_DOTS);
	lv_label_set_text(np_artist, "");
	lv_obj_set_style_text_font(np_artist, text_font, 0);
	lv_obj_set_style_text_color(np_artist, lv_color_hex(0x909090), 0);

	np_bar = lv_bar_create(col);
	lv_obj_set_size(np_bar, lv_pct(100), 6);
	lv_obj_set_style_margin_top(np_bar, 28, 0);
	lv_obj_set_style_bg_color(np_bar, lv_color_hex(0x404040), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(np_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(np_bar, lv_color_hex(0xc0c0c0), LV_PART_INDICATOR);
	lv_obj_set_style_radius(np_bar, 3, LV_PART_MAIN);
	lv_obj_set_style_radius(np_bar, 3, LV_PART_INDICATOR);

	row = lv_obj_create(col);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_clickable(row, false);
	np_elapsed = lv_label_create(row);
	lv_label_set_text(np_elapsed, "");
	lv_obj_set_style_text_font(np_elapsed, small_font, 0);
	lv_obj_set_style_text_color(np_elapsed, lv_color_hex(0x707070), 0);
	lv_obj_align(np_elapsed, LV_ALIGN_LEFT_MID, 0, 0);
	np_total = lv_label_create(row);
	lv_label_set_text(np_total, "");
	lv_obj_set_style_text_font(np_total, small_font, 0);
	lv_obj_set_style_text_color(np_total, lv_color_hex(0x707070), 0);
	lv_obj_align(np_total, LV_ALIGN_RIGHT_MID, 0, 0);

	row = lv_obj_create(col);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_margin_top(row, 20, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(row, false);
	media_button(row, LV_SYMBOL_PREV, "previous");
	np_play = media_button(row, LV_SYMBOL_PLAY, "play_pause");
	media_button(row, LV_SYMBOL_NEXT, "next");

	/* the pill, on the clock face itself */
	np_pill = lv_obj_create(scr);
	lv_obj_remove_style_all(np_pill);
	lv_obj_set_size(np_pill, LV_SIZE_CONTENT, 56);
	lv_obj_align(np_pill, LV_ALIGN_BOTTOM_MID, 0, -14);
	lv_obj_set_style_bg_color(np_pill, lv_color_hex(0x181818), 0);
	lv_obj_set_style_bg_opa(np_pill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(np_pill, 28, 0);
	lv_obj_set_style_pad_hor(np_pill, 24, 0);
	lv_obj_set_style_pad_column(np_pill, 14, 0);
	lv_obj_set_flex_flow(np_pill, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(np_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(np_pill, true);
	lv_obj_add_event_cb(np_pill, on_pill_tap, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(np_pill, true);
	{
		lv_obj_t *note = lv_label_create(np_pill);

		lv_label_set_text(note, LV_SYMBOL_AUDIO);
		lv_obj_set_style_text_font(note, &lv_font_montserrat_28, 0);
		lv_obj_set_style_text_color(note, lv_color_hex(0x909090), 0);
	}
	np_pill_text = lv_label_create(np_pill);
	lv_obj_set_style_max_width(np_pill_text, 560, 0);
	lv_label_set_long_mode(np_pill_text, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(np_pill_text, lv_font_get_line_height(small_font));
	lv_label_set_text(np_pill_text, "");
	lv_obj_set_style_text_font(np_pill_text, small_font, 0);
	lv_obj_set_style_text_color(np_pill_text, lv_color_hex(0xc0c0c0), 0);

	update_media(NULL);
	lv_timer_create(update_media, 500, NULL);
}

/* ---- alarms --------------------------------------------------------------------------- */

static lv_obj_t *alarm_panel, *alarm_bell, *alarm_bell_text;

static void alarm_command(const char *cmd)
{
	if (fork() == 0) {
		execl(ALARM_CTL, ALARM_CTL, cmd, (char *)NULL);
		_exit(127);
	}
}

static void on_alarm_button(lv_event_t *e)
{
	alarm_command(lv_event_get_user_data(e));
	lv_obj_set_hidden(alarm_panel, true);	/* clock-alarm confirms through ALARM_FILE */
}

static void open_alarm_list(void);

/* The bell: stops a snoozed alarm, otherwise opens the alarm list */
static void on_bell_tap(lv_event_t *e)
{
	(void)e;
	if (!strcmp(alarm_state, "snoozed"))
		alarm_command("stop");
	else
		open_alarm_list();
}

/* "HH:MM" (24 h) in the clock face's format; alarm times get AM / PM in 12 h mode */
static void fmt_hm(char *out, size_t len, const char *hm)
{
	int h, m;

	if (sscanf(hm, "%d:%d", &h, &m) != 2) {
		snprintf(out, len, "%s", hm);
		return;
	}
	if (time_12h)
		snprintf(out, len, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
	else
		snprintf(out, len, "%02d:%02d", h, m);
}

static bool file_changed(const char *path, struct timespec *last)
{
	struct stat st;

	if (stat(path, &st))
		return false;
	if (st.st_mtim.tv_sec == last->tv_sec && st.st_mtim.tv_nsec == last->tv_nsec)
		return false;
	*last = st.st_mtim;
	return true;
}

static void update_alarm(lv_timer_t *t)
{
	static struct timespec last_state, last_next;
	static char next[32];
	static long snooze_until;
	bool changed = false;
	char line[64], text[128], hm[16];
	FILE *f;

	(void)t;
	if (file_changed(ALARM_FILE, &last_state) && (f = fopen(ALARM_FILE, "r"))) {
		alarm_state[0] = 0;
		snooze_until = 0;
		while (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\n")] = 0;
			if (!strncmp(line, "state=", 6))
				snprintf(alarm_state, sizeof(alarm_state), "%s", line + 6);
			else if (!strncmp(line, "snooze_until=", 13))
				snooze_until = atol(line + 13);
		}
		fclose(f);
		changed = true;
	}
	if (file_changed(ALARM_NEXT_FILE, &last_next) && (f = fopen(ALARM_NEXT_FILE, "r"))) {
		if (!fgets(next, sizeof(next), f))
			next[0] = 0;
		next[strcspn(next, "\n")] = 0;
		fclose(f);
		changed = true;
	}
	if (!changed)
		return;

	/* ringing: the full-screen view, above everything */
	if (!strcmp(alarm_state, "ringing")) {
		lv_obj_set_hidden(vol_panel, true);
		lv_obj_set_hidden(alarm_panel, false);
		lv_obj_move_foreground(alarm_panel);
	} else {
		lv_obj_set_hidden(alarm_panel, true);
	}

	/* the bell: snoozed until ..., or the next alarm ("Tue 07:00") */
	text[0] = 0;
	if (!strcmp(alarm_state, "snoozed") && snooze_until) {
		time_t u = snooze_until;
		struct tm tm;

		localtime_r(&u, &tm);
		strftime(hm, sizeof(hm), "%H:%M", &tm);
		fmt_hm(line, sizeof(line), hm);
		snprintf(text, sizeof(text), "Snoozed  ·  %s", line);
	} else if (*next) {
		char day[8] = "";

		if (sscanf(next, "%7s %15s", day, hm) == 2) {
			char today[8];
			time_t now = time(NULL);
			struct tm tm;

			localtime_r(&now, &tm);
			strftime(today, sizeof(today), "%a", &tm);
			fmt_hm(line, sizeof(line), hm);
			if (!strcmp(day, today))	/* today: just the time */
				snprintf(text, sizeof(text), "%s", line);
			else
				snprintf(text, sizeof(text), "%s %s", day, line);
		}
	}
	set_text_if(alarm_bell_text, text);
	lv_obj_set_hidden(alarm_bell, !*text);
}

/* ---- the alarm editor -------------------------------------------------------------------
 * Long-press the clock (or tap the bell) for the list of alarms: an on / off switch each,
 * and a tap on one to change its time and days. Changes go through `clock-alarm set`,
 * which keeps Home Assistant's helpers in step; the list reads clock-alarm's copy of
 * them (ALARMS_FILE), so changes made in Home Assistant show up here too.
 */
#define ALARMS_FILE	"/var/lib/clock/alarms"
#define N_ALARMS	3
#define EDITOR_IDLE_MS	30000		/* close the editor after this long untouched */
#define ACCENT		0x03a9f4	/* Home Assistant's blue, for "on" */

static const char *const day_names[] = { "Every day", "Weekdays", "Weekends", "Once" };
#define N_DAYS		(sizeof(day_names) / sizeof(day_names[0]))

static struct {
	bool on;
	int h, m, days;		/* 24 h; days indexes day_names */
} alarms[N_ALARMS];

static lv_obj_t *al_list, *al_time[N_ALARMS], *al_days[N_ALARMS], *al_switch[N_ALARMS];
static lv_obj_t *al_edit, *al_edit_title, *al_hour, *al_min, *al_ampm, *al_day[N_DAYS];
static int al_editing, al_edit_days;
static struct timespec al_file_time;

static void alarm_set(int n)	/* clock-alarm set N on|off HH:MM DAYS */
{
	char num[12], hm[8];

	snprintf(num, sizeof(num), "%d", n + 1);
	snprintf(hm, sizeof(hm), "%02d:%02d", alarms[n].h, alarms[n].m);
	if (fork() == 0) {
		execl(ALARM_CTL, ALARM_CTL, "set", num, alarms[n].on ? "on" : "off", hm,
		      day_names[alarms[n].days], (char *)NULL);
		_exit(127);
	}
}

/* ALARMS_FILE lines: alarmN=on|off HH:MM Every_day */
static void load_alarms(void)
{
	char line[128], onoff[8], days[32];
	FILE *f = fopen(ALARMS_FILE, "r");
	int n, h, m, i;

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "alarm%d=%7s %d:%d %31s", &n, onoff, &h, &m, days) != 5 ||
		    n < 1 || n > N_ALARMS)
			continue;
		n--;
		alarms[n].on = !strcmp(onoff, "on");
		alarms[n].h = h;
		alarms[n].m = m;
		for (i = 0; days[i]; i++)
			if (days[i] == '_')
				days[i] = ' ';
		for (i = 0; i < (int)N_DAYS; i++)
			if (!strcmp(days, day_names[i]))
				alarms[n].days = i;
	}
	fclose(f);
}

static void fill_alarm_list(void)
{
	char hm[8], text[16];
	int i;

	for (i = 0; i < N_ALARMS; i++) {
		snprintf(hm, sizeof(hm), "%02d:%02d", alarms[i].h, alarms[i].m);
		fmt_hm(text, sizeof(text), hm);
		set_text_if(al_time[i], text);
		set_text_if(al_days[i], day_names[alarms[i].days]);
		lv_obj_set_style_text_color(al_time[i], lv_color_hex(alarms[i].on ? 0xf0f0f0 : 0x606060), 0);
		lv_obj_set_state(al_switch[i], LV_STATE_CHECKED, alarms[i].on);
	}
}

static void open_alarm_list(void)
{
	load_alarms();
	fill_alarm_list();
	lv_obj_set_hidden(al_edit, true);
	lv_obj_set_hidden(al_list, false);
	hide_forecast(NULL);
}

static void close_alarm_editor(void)
{
	lv_obj_set_hidden(al_list, true);
	lv_obj_set_hidden(al_edit, true);
}

static void on_long_press(lv_event_t *e)
{
	(void)e;
	open_alarm_list();
}

static void on_alarm_done(lv_event_t *e)
{
	(void)e;
	close_alarm_editor();
}

static void on_alarm_switch(lv_event_t *e)
{
	int n = (int)(intptr_t)lv_event_get_user_data(e);

	alarms[n].on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	fill_alarm_list();
	alarm_set(n);
}

static void show_edit_days(void)
{
	int i;

	for (i = 0; i < (int)N_DAYS; i++) {
		lv_obj_t *l = lv_obj_get_child(al_day[i], 0);
		bool sel = i == al_edit_days;

		lv_obj_set_style_bg_color(al_day[i], lv_color_hex(sel ? 0xe8e8e8 : 0x202020), 0);
		lv_obj_set_style_text_color(l, lv_color_hex(sel ? 0x000000 : 0xb0b0b0), 0);
	}
}

static void on_alarm_row(lv_event_t *e)
{
	int n = (int)(intptr_t)lv_event_get_user_data(e), h = alarms[n].h;

	al_editing = n;
	al_edit_days = alarms[n].days;
	lv_label_set_text_fmt(al_edit_title, "Alarm %d", n + 1);
	if (time_12h) {
		lv_roller_set_selected(al_hour, (h % 12 ? h % 12 : 12) - 1, LV_ANIM_OFF);
		lv_roller_set_selected(al_ampm, h >= 12, LV_ANIM_OFF);
	} else {
		lv_roller_set_selected(al_hour, h, LV_ANIM_OFF);
	}
	lv_roller_set_selected(al_min, alarms[n].m, LV_ANIM_OFF);
	show_edit_days();
	lv_obj_set_hidden(al_list, true);
	lv_obj_set_hidden(al_edit, false);
}

static void on_edit_day(lv_event_t *e)
{
	al_edit_days = (int)(intptr_t)lv_event_get_user_data(e);
	show_edit_days();
}

static void on_edit_cancel(lv_event_t *e)
{
	(void)e;
	open_alarm_list();
}

/* Save: the new time and days, and the alarm switched on */
static void on_edit_save(lv_event_t *e)
{
	int n = al_editing, h = lv_roller_get_selected(al_hour);

	(void)e;
	if (time_12h)
		h = (h + 1) % 12 + (lv_roller_get_selected(al_ampm) ? 12 : 0);
	alarms[n].h = h;
	alarms[n].m = lv_roller_get_selected(al_min);
	alarms[n].days = al_edit_days;
	alarms[n].on = true;
	alarm_set(n);
	fill_alarm_list();
	lv_obj_set_hidden(al_edit, true);
	lv_obj_set_hidden(al_list, false);
}

/* Follow changes from Home Assistant; close when left alone */
static void check_alarm_editor(lv_timer_t *t)
{
	(void)t;
	if (lv_obj_is_hidden(al_list) && lv_obj_is_hidden(al_edit))
		return;
	if (lv_display_get_inactive_time(NULL) > EDITOR_IDLE_MS) {
		close_alarm_editor();
		return;
	}
	if (file_changed(ALARMS_FILE, &al_file_time) && !lv_obj_is_hidden(al_list)) {
		load_alarms();
		fill_alarm_list();
	}
}

static lv_obj_t *panel(lv_obj_t *scr)
{
	lv_obj_t *p = lv_obj_create(scr);

	lv_obj_remove_style_all(p);
	lv_obj_set_size(p, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(p, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(p, 28, 0);
	lv_obj_set_clickable(p, true);	/* nothing underneath gets the taps */
	lv_obj_set_hidden(p, true);
	return p;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, int w, int h,
			     const lv_font_t *font, lv_event_cb_t cb, void *data)
{
	lv_obj_t *b = lv_obj_create(parent), *l;

	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, w, h);
	lv_obj_set_clickable(b, true);
	lv_obj_set_style_radius(b, h / 2, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2a2a), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x505050), LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
	l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0xf0f0f0), 0);
	lv_obj_center(l);
	return b;
}

static lv_obj_t *roller(lv_obj_t *parent, const char *options, bool infinite, int w,
			const lv_font_t *font)
{
	lv_obj_t *r = lv_roller_create(parent);

	lv_roller_set_options(r, options, infinite ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(r, w);
	lv_obj_set_style_text_font(r, font, 0);
	lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(r, lv_color_hex(0x505050), 0);
	lv_obj_set_style_bg_color(r, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_text_line_space(r, 8, 0);
	lv_obj_set_style_bg_color(r, lv_color_hex(0x202020), LV_PART_SELECTED);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_text_color(r, lv_color_hex(0xf0f0f0), LV_PART_SELECTED);
	lv_obj_set_style_text_font(r, font, LV_PART_SELECTED);
	lv_obj_set_style_radius(r, 16, LV_PART_SELECTED);
	lv_roller_set_visible_row_count(r, 3);	/* after the font: it sets the height */
	return r;
}

static void create_alarm_editor(lv_obj_t *scr, const lv_font_t *wheel, const lv_font_t *medium,
				const lv_font_t *small, const lv_font_t *tiny)
{
	static char hours[24 * 3], minutes[60 * 3];
	lv_obj_t *row, *l, *sw;
	int i;

	/* the list */
	al_list = panel(scr);
	lv_obj_set_flex_flow(al_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(al_list, 14, 0);

	row = lv_obj_create(al_list);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	l = label(row, small, 0x909090);
	lv_label_set_text(l, "Alarms");
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_align(pill_button(row, "Done", 180, 70, small, on_alarm_done, NULL),
		     LV_ALIGN_RIGHT_MID, 0, 0);

	for (i = 0; i < N_ALARMS; i++) {
		row = lv_obj_create(al_list);
		lv_obj_remove_style_all(row);
		lv_obj_set_size(row, LV_PCT(100), 100);
		lv_obj_set_style_radius(row, 24, 0);
		lv_obj_set_style_bg_color(row, lv_color_hex(0x161616), 0);
		lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), LV_STATE_PRESSED);
		lv_obj_set_style_pad_hor(row, 32, 0);
		lv_obj_set_style_pad_column(row, 28, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		lv_obj_set_clickable(row, true);
		lv_obj_add_event_cb(row, on_alarm_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		al_time[i] = label(row, medium, 0xf0f0f0);
		lv_obj_set_width(al_time[i], 230);
		al_days[i] = label(row, tiny, 0x909090);
		lv_obj_set_flex_grow(al_days[i], 1);

		sw = al_switch[i] = lv_switch_create(row);
		lv_obj_set_size(sw, 110, 56);
		lv_obj_set_ext_click_area(sw, 20);
		lv_obj_set_style_bg_color(sw, lv_color_hex(0x404040), 0);
		lv_obj_set_style_bg_color(sw, lv_color_hex(ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
		lv_obj_set_style_bg_color(sw, lv_color_hex(0xf0f0f0), LV_PART_KNOB);
		lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
		lv_obj_add_event_cb(sw, on_alarm_switch, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
	}

	/* editing one: time wheels, days, Cancel / Save */
	al_edit = panel(scr);
	lv_obj_set_flex_flow(al_edit, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(al_edit, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_ver(al_edit, 16, 0);

	al_edit_title = label(al_edit, tiny, 0x909090);
	lv_obj_set_floating(al_edit_title, true);
	lv_obj_align(al_edit_title, LV_ALIGN_TOP_LEFT, 12, 8);

	row = lv_obj_create(al_edit);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 12, 0);
	hours[0] = minutes[0] = 0;
	for (i = 0; i < (time_12h ? 12 : 24); i++)	/* 1-12, or 00-23 */
		sprintf(hours + strlen(hours), "%s%.*d", i ? "\n" : "", time_12h ? 1 : 2,
			time_12h ? i + 1 : i);
	for (i = 0; i < 60; i++)
		sprintf(minutes + strlen(minutes), i ? "\n%02d" : "%02d", i);
	al_hour = roller(row, hours, true, 150, wheel);
	l = label(row, wheel, 0xf0f0f0);
	lv_label_set_text(l, ":");
	al_min = roller(row, minutes, true, 150, wheel);
	if (time_12h) {
		al_ampm = roller(row, "AM\nPM", false, 150, medium);
		lv_obj_set_style_margin_left(al_ampm, 20, 0);
	}

	row = lv_obj_create(al_edit);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	for (i = 0; i < (int)N_DAYS; i++)
		al_day[i] = pill_button(row, day_names[i], 172, 64, tiny, on_edit_day,
					(void *)(intptr_t)i);

	row = lv_obj_create(al_edit);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	pill_button(row, "Cancel", 300, 80, small, on_edit_cancel, NULL);
	pill_button(row, "Save", 300, 80, small, on_edit_save, NULL);

	lv_timer_create(check_alarm_editor, 1000, NULL);
}

static lv_obj_t *alarm_button(lv_obj_t *parent, const char *text, const char *cmd, int w,
			      uint32_t bg, const lv_font_t *font)
{
	lv_obj_t *b = lv_obj_create(parent), *l;

	lv_obj_remove_style_all(b);
	lv_obj_set_size(b, w, 110);
	lv_obj_set_clickable(b, true);
	lv_obj_set_style_radius(b, 55, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, lv_color_hex(0x505050), LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, on_alarm_button, LV_EVENT_CLICKED, (void *)cmd);
	l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0xf0f0f0), 0);
	lv_obj_center(l);
	return b;
}

static void create_alarm_views(lv_obj_t *scr, const lv_font_t *big, const lv_font_t *wheel,
			       const lv_font_t *medium, const lv_font_t *small, const lv_font_t *tiny)
{
	lv_obj_t *row, *l;

	/* the bell: on the weather line, after the weather */
	alarm_bell = lv_obj_create(lv_obj_get_parent(weather_icon.obj));
	lv_obj_remove_style_all(alarm_bell);
	lv_obj_set_size(alarm_bell, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_margin_left(alarm_bell, 28, 0);
	lv_obj_set_style_pad_column(alarm_bell, 10, 0);
	lv_obj_set_flex_flow(alarm_bell, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(alarm_bell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(alarm_bell, true);
	lv_obj_add_event_cb(alarm_bell, on_bell_tap, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(alarm_bell, true);
	l = lv_label_create(alarm_bell);
	lv_label_set_text(l, LV_SYMBOL_BELL);
	lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0x707070), 0);
	alarm_bell_text = lv_label_create(alarm_bell);
	lv_label_set_text(alarm_bell_text, "");
	lv_obj_set_style_text_font(alarm_bell_text, small, 0);
	lv_obj_set_style_text_color(alarm_bell_text, lv_color_hex(0x707070), 0);

	create_alarm_editor(scr, wheel, medium, small, tiny);

	/* ringing: above the editor */
	alarm_panel = lv_obj_create(scr);
	lv_obj_remove_style_all(alarm_panel);
	lv_obj_set_size(alarm_panel, SCREEN_W, SCREEN_H);
	lv_obj_set_style_bg_color(alarm_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(alarm_panel, LV_OPA_COVER, 0);
	lv_obj_set_clickable(alarm_panel, true);
	lv_obj_set_hidden(alarm_panel, true);

	alarm_time = big_time_create(alarm_panel, big, 0xf0f0f0);
	lv_obj_align(alarm_time, LV_ALIGN_CENTER, 0, -60);	/* where the clock face has it */

	row = lv_obj_create(alarm_panel);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, SCREEN_W - 80, LV_SIZE_CONTENT);
	lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -40);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(row, false);
	alarm_button(row, "Snooze", "snooze", 440, 0x2a2a2a, medium);
	alarm_button(row, "Stop", "stop", 220, 0x2a2a2a, medium);

	update_alarm(NULL);
	lv_timer_create(update_alarm, 500, NULL);
}

int main(int argc, char **argv)
{
	static uint32_t draw_buf[2][SCREEN_W * DRAW_LINES];
	lv_display_t *disp;
	lv_font_t *big, *wheel, *medium, *small, *tiny;
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
	signal(SIGCHLD, SIG_IGN);	/* ha-media commands: no zombies */
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
	init_ha_icons();
	disp = lv_display_create(SCREEN_W, SCREEN_H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
	lv_display_set_buffers(disp, draw_buf[0], draw_buf[1], sizeof(draw_buf[0]),
			       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush_cb);

	big = lv_tiny_ttf_create_data(ttf, ttf_len, 240);
	wheel = lv_tiny_ttf_create_data(ttf, ttf_len, 64);
	medium = lv_tiny_ttf_create_data(ttf, ttf_len, 44);
	small = lv_tiny_ttf_create_data(ttf, ttf_len, 36);
	tiny = lv_tiny_ttf_create_data(ttf, ttf_len, 26);

	scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	time_label = big_time_create(scr, big, 0xe8e8e8);
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
	lv_obj_set_clickable(weather_row, false);	/* the screen's tap; the bell has its own */
	lv_obj_align(weather_row, LV_ALIGN_CENTER, 0, 165);

	icon_create(&weather_icon, weather_row, 0, icon_buf);
	lv_obj_set_hidden(weather_icon.obj, true);

	weather_label = lv_label_create(weather_row);
	lv_label_set_text(weather_label, "");
	lv_obj_set_style_text_font(weather_label, small, 0);
	lv_obj_set_style_text_color(weather_label, lv_color_hex(0x707070), 0);

	create_forecast_panel(scr, small);
	create_now_playing(scr, medium, small, tiny);
	create_volume_panel(scr);
	create_alarm_views(scr, big, wheel, medium, small, tiny);
	/* tap: the forecast; long-press: the alarms (a long press isn't also a tap) */
	lv_obj_add_event_cb(scr, on_tap, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(scr, on_long_press, LV_EVENT_LONG_PRESSED, NULL);

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
