/*
 * autobright: set the backlight from the ambient light sensor.
 *
 * The sensor is a Lite-On LTR-578ALS on i2c-0 at 0x53, with no kernel driver, so it's read
 * through /dev/i2c-0. Readings are smoothed and mapped through a curve to a backlight
 * level, and the backlight fades towards it ~30 times a second on a log scale (4 -> 8
 * looks as big a change as 100 -> 200), so changes are smooth at every brightness.
 *
 * The curve is read from /etc/clock/autobright.conf:
 *   CURVE="3:6 20:60 100:130 300:190 3000:255"      (sensor reading:backlight level, log-interpolated)
 *
 *   autobright [-v]
 *
 * Build (static, so it runs on Alpine's musl):
 *   aarch64-linux-gnu-gcc -O2 -static -Wall -o autobright userspace/autobright.c -lm
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define I2C_DEV		"/dev/i2c-0"
#define LTR578_ADDR	0x53
#define LED		"/sys/class/leds/lcd-backlight/brightness"
#define CONF		"/etc/clock/autobright.conf"

#define TICK_MS		33	/* fade step */
#define READ_MS		500	/* sensor period (400 ms integration) */
#define FADE_TAU	0.7	/* seconds; settles in ~2 s */
#define SMOOTH		0.25	/* weight of each new reading */
#define DEADBAND	0.06	/* ignore target changes under 6% */
#define MAX_POINTS	16

static int npoints;
static double curve_x[MAX_POINTS], curve_y[MAX_POINTS];	/* log(reading), level */
static int verbose;

static int i2c_fd;

static int reg_write(unsigned char reg, unsigned char val)
{
	unsigned char buf[2] = { reg, val };

	return write(i2c_fd, buf, 2) == 2 ? 0 : -1;
}

/* ALS_DATA: 20 bits at 0x0d..0x0f, little-endian */
static int read_als(double *out)
{
	unsigned char reg = 0x0d, d[3];
	struct i2c_msg msgs[2] = {
		{ .addr = LTR578_ADDR, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = LTR578_ADDR, .flags = I2C_M_RD, .len = 3, .buf = d },
	};
	struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };

	if (ioctl(i2c_fd, I2C_RDWR, &xfer) != 2)
		return -1;
	*out = d[0] | (d[1] << 8) | ((d[2] & 0x0f) << 16);
	return 0;
}

static void parse_curve(const char *s)
{
	double x, y;
	int n;

	npoints = 0;
	while (npoints < MAX_POINTS && sscanf(s, " %lf:%lf%n", &x, &y, &n) == 2) {
		curve_x[npoints] = log(x < 1 ? 1 : x);
		curve_y[npoints] = y;
		npoints++;
		s += n;
	}
}

static void load_conf(void)
{
	char line[256], *p;
	FILE *f = fopen(CONF, "r");

	parse_curve("3:6 20:60 100:130 300:190 3000:255");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "CURVE=", 6))
			continue;
		p = line + 6;
		if (*p == '"')		/* the value may be quoted */
			p++;
		p[strcspn(p, "\"\n")] = 0;
		parse_curve(p);
	}
	fclose(f);
	if (npoints < 2) {
		fprintf(stderr, "autobright: can't read CURVE in %s, using the default\n", CONF);
		parse_curve("3:6 20:60 100:130 300:190 3000:255");
	}
}

static double curve(double reading)
{
	double v = log(reading < 1 ? 1 : reading);
	int i;

	if (v <= curve_x[0])
		return curve_y[0];
	for (i = 1; i < npoints; i++)
		if (v <= curve_x[i])
			return curve_y[i - 1] + (curve_y[i] - curve_y[i - 1]) *
			       (v - curve_x[i - 1]) / (curve_x[i] - curve_x[i - 1]);
	return curve_y[npoints - 1];
}

static int led_get(void)
{
	FILE *f = fopen(LED, "r");
	int v = 128;

	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = 128;
		fclose(f);
	}
	return v;
}

static void led_set(int v)
{
	FILE *f = fopen(LED, "w");

	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

int main(int argc, char **argv)
{
	double avg, raw, target, level;
	const double k = 1 - exp(-TICK_MS / 1000.0 / FADE_TAU);
	struct timespec tick = { 0, TICK_MS * 1000000L };
	int since_read = 0, shown;

	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	load_conf();
	if (verbose) {
		int i;

		fprintf(stderr, "autobright: curve");
		for (i = 0; i < npoints; i++)
			fprintf(stderr, " %.0f:%.0f", exp(curve_x[i]), curve_y[i]);
		fprintf(stderr, "\n");
	}

	i2c_fd = open(I2C_DEV, O_RDWR);
	if (i2c_fd < 0 || ioctl(i2c_fd, I2C_SLAVE_FORCE, LTR578_ADDR) < 0) {
		perror(I2C_DEV);
		return 1;
	}
	/* ALS off; 20-bit / 400 ms, measured every 500 ms; gain 18x; ALS on */
	if (reg_write(0x00, 0x00) || reg_write(0x04, 0x05) || reg_write(0x05, 0x04) ||
	    reg_write(0x00, 0x02)) {
		perror("autobright: LTR-578 setup");
		return 1;
	}
	sleep(1);
	if (read_als(&avg)) {
		perror("autobright: LTR-578 read");
		return 1;
	}

	shown = led_get();
	if (shown < 1)
		shown = 1;
	level = shown;
	target = curve(avg);

	for (;;) {
		since_read += TICK_MS;
		if (since_read >= READ_MS) {
			since_read = 0;
			if (read_als(&raw) == 0) {
				double t;

				avg += (raw - avg) * SMOOTH;
				t = curve(avg);
				if (fabs(t - target) > DEADBAND * target)
					target = t;
				if (verbose)
					fprintf(stderr, "raw %.0f avg %.1f target %.1f level %.1f\n",
						raw, avg, target, level);
			}
		}
		/* fade in log space */
		level = exp(log(level) + (log(target) - log(level)) * k);
		if ((int)lround(level) != shown) {
			shown = lround(level);
			if (shown < 1)
				shown = 1;
			if (shown > 255)
				shown = 255;
			led_set(shown);
		}
		nanosleep(&tick, NULL);
	}
}
