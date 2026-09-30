/*
 * btrelay: make the MT7668's Bluetooth a normal Linux (BlueZ) adapter.
 *
 * MediaTek's driver (btmtksdio.ko) doesn't register an HCI device; it exposes the controller
 * as /dev/stpbt, carrying H4 packets (type byte + HCI packet) for Android's Bluetooth stack.
 * This relays those packets to a virtual controller (/dev/vhci, CONFIG_BT_HCIVHCI), which
 * BlueZ sees as hci0. On the way it resets the controller and gives it an address (the
 * clock's has none: Android set one from a factory partition that's blank here), by
 * default the Wi-Fi address + 1.
 *
 *   btrelay [-a XX:XX:XX:XX:XX:XX] [-v]
 *
 * Build (static, so it runs on Alpine's musl):
 *   aarch64-linux-gnu-gcc -O2 -static -Wall -o btrelay userspace/btrelay.c
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STPBT		"/dev/stpbt"
#define VHCI		"/dev/vhci"
#define WIFI_MAC	"/sys/class/net/wlan0/address"

#define H4_CMD		0x01
#define H4_ACL		0x02
#define H4_SCO		0x03
#define H4_EVT		0x04
#define H4_ISO		0x05
#define HCI_VENDOR_PKT	0xff
#define HCI_PRIMARY	0x00		/* vhci: create a BR/EDR + LE controller */

static int verbose;

static void die(const char *what)
{
	fprintf(stderr, "btrelay: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void hexdump(const char *dir, const uint8_t *p, size_t n)
{
	size_t i;

	if (!verbose)
		return;
	fprintf(stderr, "%s", dir);
	for (i = 0; i < n && i < 32; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "%s\n", n > 32 ? " ..." : "");
}

/* Length of the complete H4 packet at p (n bytes available), 0 if more bytes are needed,
 * -1 if the type byte is unknown */
static long h4_len(const uint8_t *p, size_t n)
{
	if (n < 1)
		return 0;
	switch (p[0]) {
	case H4_EVT:	/* type, code, len */
		return n < 3 ? 0 : 3 + p[2];
	case H4_ACL:	/* type, handle(2), len(2) */
		return n < 5 ? 0 : 5 + (p[3] | p[4] << 8);
	case H4_SCO:	/* type, handle(2), len */
		return n < 4 ? 0 : 4 + p[3];
	case H4_ISO:	/* type, handle(2), len(2, 14 bits) */
		return n < 5 ? 0 : 5 + ((p[3] | p[4] << 8) & 0x3fff);
	default:
		return -1;
	}
}

/* Send an HCI command on /dev/stpbt and wait (up to 2 s) for its Command Complete */
static int stpbt_cmd(int fd, uint16_t opcode, const uint8_t *param, uint8_t plen)
{
	uint8_t buf[260];
	struct pollfd p = { .fd = fd, .events = POLLIN };
	size_t have = 0;
	long len;
	ssize_t n;

	buf[0] = H4_CMD;
	buf[1] = opcode & 0xff;
	buf[2] = opcode >> 8;
	buf[3] = plen;
	memcpy(buf + 4, param, plen);
	hexdump("setup >", buf, 4 + plen);
	if (write(fd, buf, 4 + plen) != 4 + plen)
		return -1;
	while (poll(&p, 1, 2000) == 1) {
		n = read(fd, buf + have, sizeof(buf) - have);
		if (n <= 0)
			return -1;
		have += n;
		while ((len = h4_len(buf, have)) > 0 && (size_t)len <= have) {
			hexdump("setup <", buf, len);
			/* Command Complete (0x0e) for our opcode: status is the byte after it */
			if (buf[0] == H4_EVT && buf[1] == 0x0e && len >= 7 &&
			    (buf[4] | buf[5] << 8) == opcode)
				return buf[6];
			memmove(buf, buf + len, have - len);
			have -= len;
		}
		if (len < 0)
			have = 0;
	}
	return -1;
}

static int parse_addr(const char *s, uint8_t a[6])
{
	unsigned v[6];
	int i;

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (i = 0; i < 6; i++)
		a[i] = v[i];
	return 0;
}

int main(int argc, char **argv)
{
	static uint8_t sbuf[4096];
	uint8_t addr[6], param[6], vbuf[1100];
	struct pollfd pfd[2];
	const char *addr_arg = NULL;
	size_t shave = 0;
	int stp, vhci, opt, i;

	while ((opt = getopt(argc, argv, "a:v")) != -1) {
		if (opt == 'a') {
			addr_arg = optarg;
		} else if (opt == 'v') {
			verbose = 1;
		} else {
			fprintf(stderr, "usage: btrelay [-a XX:XX:XX:XX:XX:XX] [-v]\n");
			return 2;
		}
	}

	/* the address: given, or the Wi-Fi MAC + 1 */
	if (addr_arg) {
		if (parse_addr(addr_arg, addr)) {
			fprintf(stderr, "btrelay: bad address %s\n", addr_arg);
			return 2;
		}
	} else {
		char line[32] = "";
		FILE *f = fopen(WIFI_MAC, "r");

		if (!f || !fgets(line, sizeof(line), f) || parse_addr(line, addr))
			die("reading the Wi-Fi address (use -a)");
		fclose(f);
		for (i = 5; i >= 0 && ++addr[i] == 0; i--)
			;
	}

	stp = open(STPBT, O_RDWR);
	if (stp < 0)
		die(STPBT " (is btmtksdio loaded?)");
	if (stpbt_cmd(stp, 0x0c03, NULL, 0) != 0)		/* HCI Reset */
		fprintf(stderr, "btrelay: controller reset failed\n");
	for (i = 0; i < 6; i++)					/* HCI wants it reversed */
		param[i] = addr[5 - i];
	if (stpbt_cmd(stp, 0xfc1a, param, 6) != 0)		/* MediaTek: set BD_ADDR */
		fprintf(stderr, "btrelay: setting the address failed\n");
	else
		fprintf(stderr, "btrelay: address %02x:%02x:%02x:%02x:%02x:%02x\n",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

	vhci = open(VHCI, O_RDWR);
	if (vhci < 0)
		die(VHCI);
	{
		uint8_t create[2] = { HCI_VENDOR_PKT, HCI_PRIMARY };

		if (write(vhci, create, 2) != 2)
			die("creating the vhci controller");
	}

	pfd[0].fd = vhci;
	pfd[0].events = POLLIN;
	pfd[1].fd = stp;
	pfd[1].events = POLLIN;
	for (;;) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}
		/* BlueZ -> controller: vhci gives one whole packet per read */
		if (pfd[0].revents & POLLIN) {
			ssize_t n = read(vhci, vbuf, sizeof(vbuf));

			if (n <= 0)
				die("reading vhci");
			if (vbuf[0] == HCI_VENDOR_PKT) {	/* vhci's reply to "create" */
				if (n >= 4)
					fprintf(stderr, "btrelay: hci%u\n", vbuf[2] | vbuf[3] << 8);
				continue;
			}
			hexdump(">", vbuf, n);
			if (write(stp, vbuf, n) != n)
				die("writing " STPBT);
		}
		/* controller -> BlueZ: the driver may hand over several packets at once */
		if (pfd[1].revents & POLLIN) {
			ssize_t n = read(stp, sbuf + shave, sizeof(sbuf) - shave);
			long len;

			if (n <= 0)
				die("reading " STPBT);
			shave += n;
			while ((len = h4_len(sbuf, shave)) > 0 && (size_t)len <= shave) {
				hexdump("<", sbuf, len);
				if (write(vhci, sbuf, len) != len)
					die("writing vhci");
				memmove(sbuf, sbuf + len, shave - len);
				shave -= len;
			}
			if (len < 0) {
				fprintf(stderr, "btrelay: unknown packet type %02x, dropping\n",
					sbuf[0]);
				shave = 0;
			}
			if (shave == sizeof(sbuf))	/* can't happen with sane packets */
				shave = 0;
		}
		if ((pfd[0].revents | pfd[1].revents) & (POLLHUP | POLLERR))
			die("device closed");
	}
}
