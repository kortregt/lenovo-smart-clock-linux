/*
 * clockkeys: the Smart Clock's buttons and mic switch.
 *
 * None of them is an input device on this kernel (Android Things read them as GPIOs):
 *   volume up    SoC GPIO 42 = gpio 429, low while pressed
 *   volume down  PMIC home key, exported as gpio 576, high while pressed
 *   mic switch   SoC GPIO 23 = gpio 410, low when the mics are switched off
 * (pins from the stock Lenovo app's resources; see docs/hardware.md).
 *
 * Gestures, each running the shell command set for it in /etc/clock/keys.conf:
 *   VOLUP, VOLDOWN   press; repeats every 200 ms after holding 500 ms
 *   BOTH             both buttons pressed together
 *   BOTH_LONG        both held for 2 seconds
 *   MIC_OFF, MIC_ON  the mic switch moved (also written to /run/clock/mic)
 * A press waits 150 ms for the other button, so pressing both doesn't also change volume.
 *
 *   clockkeys [-v]
 *
 * Build (static, so it runs on Alpine's musl):
 *   aarch64-linux-gnu-gcc -O2 -static -Wall -o clockkeys userspace/clockkeys.c
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CONF		"/etc/clock/keys.conf"
#define MIC_STATE	"/run/clock/mic"

#define COMBO_MS	150
#define REPEAT_DELAY_MS	500
#define REPEAT_MS	200
#define BOTH_LONG_MS	2000

enum { UP, DOWN, MIC, NPINS };
static const struct { int gpio, active; const char *name; } pins[NPINS] = {
	[UP] = { 429, 0, "volume up" },
	[DOWN] = { 576, 1, "volume down" },
	[MIC] = { 410, 0, "mic switch" },	/* "active" = mics off */
};

enum { EV_VOLUP, EV_VOLDOWN, EV_BOTH, EV_BOTH_LONG, EV_MIC_OFF, EV_MIC_ON, NEVENTS };
static const char *event_names[NEVENTS] = {
	"VOLUP", "VOLDOWN", "BOTH", "BOTH_LONG", "MIC_OFF", "MIC_ON",
};
static char *actions[NEVENTS];
static int verbose;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void write_file(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");

	if (f) {
		fputs(s, f);
		fclose(f);
	}
}

/* Export a GPIO as an input with interrupts on both edges; returns its value fd */
static int gpio_open(int gpio, int *edges)
{
	char path[64], num[16];
	struct stat st;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
	if (stat(path, &st)) {
		snprintf(num, sizeof(num), "%d", gpio);
		write_file("/sys/class/gpio/export", num);
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/edge", gpio);
	write_file(path, "both");
	*edges = 0;
	{
		FILE *f = fopen(path, "r");
		char buf[16] = "";

		if (f) {
			if (fgets(buf, sizeof(buf), f))
				*edges = !strncmp(buf, "both", 4);
			fclose(f);
		}
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	return open(path, O_RDONLY);
}

static int gpio_read(int fd)
{
	char c = '0';

	if (lseek(fd, 0, SEEK_SET) < 0 || read(fd, &c, 1) != 1)
		return -1;
	return c == '1';
}

static void load_conf(void)
{
	char line[512], *eq, *val;
	FILE *f = fopen(CONF, "r");
	int i;

	actions[EV_VOLUP] = strdup("/usr/local/bin/clock-volume up");
	actions[EV_VOLDOWN] = strdup("/usr/local/bin/clock-volume down");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (line[0] == '#' || !(eq = strchr(line, '=')))
			continue;
		*eq = 0;
		val = eq + 1;
		if (*val == '"' && val[strlen(val) - 1] == '"') {
			val[strlen(val) - 1] = 0;
			val++;
		}
		for (i = 0; i < NEVENTS; i++) {
			if (!strcmp(line, event_names[i])) {
				free(actions[i]);
				actions[i] = *val ? strdup(val) : NULL;
			}
		}
	}
	fclose(f);
}

static void fire(int ev)
{
	if (verbose)
		fprintf(stderr, "clockkeys: %s -> %s\n", event_names[ev],
			actions[ev] ? actions[ev] : "(nothing)");
	if (!actions[ev])
		return;
	if (fork() == 0) {
		execl("/bin/sh", "sh", "-c", actions[ev], (char *)NULL);
		_exit(127);
	}
}

int main(int argc, char **argv)
{
	struct pollfd pfd[NPINS];
	int state[NPINS], edges[NPINS], all_edges = 1, i;
	long pressed_at[2] = { 0, 0 }, next_repeat = 0;
	int pending = -1;		/* button waiting out the combo window */
	int repeating = -1;		/* button whose volume action repeats */
	int both = 0, both_long_done = 0;
	long both_at = 0;

	if (argc > 1 && !strcmp(argv[1], "-v"))
		verbose = 1;
	signal(SIGCHLD, SIG_IGN);	/* don't leave zombie action processes */
	load_conf();

	for (i = 0; i < NPINS; i++) {
		pfd[i].fd = gpio_open(pins[i].gpio, &edges[i]);
		pfd[i].events = POLLPRI | POLLERR;
		if (pfd[i].fd < 0) {
			fprintf(stderr, "clockkeys: gpio %d (%s): %s\n", pins[i].gpio, pins[i].name,
				strerror(errno));
			return 1;
		}
		all_edges &= edges[i];
		state[i] = gpio_read(pfd[i].fd) == pins[i].active;
	}
	mkdir("/run/clock", 0755);
	write_file(MIC_STATE, state[MIC] ? "off\n" : "on\n");
	if (verbose)
		fprintf(stderr, "clockkeys: %s, mics %s\n",
			all_edges ? "edge interrupts" : "polling", state[MIC] ? "off" : "on");

	for (;;) {
		long t = now_ms(), due = -1, timeout;

		/* wake up for whichever timer is due first */
		if (pending >= 0)
			due = pressed_at[pending] + COMBO_MS;
		else if (repeating >= 0)
			due = next_repeat;
		else if (both && !both_long_done)
			due = both_at + BOTH_LONG_MS;
		timeout = due < 0 ? -1 : due > t ? due - t : 0;
		if (!all_edges && (timeout < 0 || timeout > 50))
			timeout = 50;	/* no interrupts: poll the pins */
		poll(pfd, NPINS, timeout);
		t = now_ms();

		for (i = 0; i < NPINS; i++) {
			int s = gpio_read(pfd[i].fd) == pins[i].active;

			if (s == state[i])
				continue;
			state[i] = s;
			if (i == MIC) {
				write_file(MIC_STATE, s ? "off\n" : "on\n");
				fire(s ? EV_MIC_OFF : EV_MIC_ON);
			} else if (s) {
				pressed_at[i] = t;
				if (state[!i]) {		/* the other one is down too */
					pending = repeating = -1;
					both = 1;
					both_long_done = 0;
					both_at = t;
					fire(EV_BOTH);
				} else {
					pending = i;
				}
			} else {
				if (pending == i)		/* a tap shorter than the window */
					fire(i == UP ? EV_VOLUP : EV_VOLDOWN);
				if (pending == i)
					pending = -1;
				if (repeating == i)
					repeating = -1;
				if (!state[UP] && !state[DOWN])
					both = 0;
			}
		}

		if (pending >= 0 && t >= pressed_at[pending] + COMBO_MS) {
			fire(pending == UP ? EV_VOLUP : EV_VOLDOWN);
			repeating = pending;
			next_repeat = pressed_at[pending] + REPEAT_DELAY_MS;
			pending = -1;
		}
		if (repeating >= 0 && t >= next_repeat) {
			fire(repeating == UP ? EV_VOLUP : EV_VOLDOWN);
			next_repeat = t + REPEAT_MS;
		}
		if (both && !both_long_done && state[UP] && state[DOWN] &&
		    t >= both_at + BOTH_LONG_MS) {
			both_long_done = 1;
			fire(EV_BOTH_LONG);
		}
	}
}
