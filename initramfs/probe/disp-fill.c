/*
 * disp-fill: draw a test image the way Android's hwcomposer does, bypassing /dev/fb0.
 * Allocates an ION buffer (MTK multimedia heap), fills it with four horizontal bands
 * (red, green, blue, white as bytes R,G,B,A), and submits it to the primary display session
 * through /dev/mtk_disp_mgr: PREPARE_INPUT_BUFFER -> SET_INPUT_BUFFER -> TRIGGER_SESSION.
 * Keeps the buffer alive for <seconds> (default 20), then exits.
 * Freestanding static binary; built against the kernel's real disp_session.h (see Makefile
 * notes in docs/08-display.md).
 */
#include <linux/types.h>
#include "disp_session.h"

#define AT_FDCWD	-100
#define O_RDWR		2
#define __NR_ioctl	29
#define __NR_openat	56
#define __NR_write	64
#define __NR_exit	93
#define __NR_nanosleep	101
#define __NR_mmap	222

/* drivers/staging/android/uapi/ion.h */
struct ion_allocation_data { size_t len, align; unsigned int heap_id_mask, flags; int handle; };
struct ion_fd_data { int handle, fd; };
#define ION_IOC_ALLOC	_IOWR('I', 0, struct ion_allocation_data)
#define ION_IOC_SHARE	_IOWR('I', 4, struct ion_fd_data)
#define ION_HEAP_MULTIMEDIA_MASK (1U << 10)

#define W 480
#define H 800

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	register long x4 asm("x4") = e;
	register long x5 asm("x5") = f;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static void say(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sc(__NR_write, 1, (long)s, n, 0, 0, 0);
}

static void num(const char *label, long v)
{
	char b[24];
	int i = 23;
	int neg = v < 0;

	b[i] = 0;
	if (neg)
		v = -v;
	do { b[--i] = '0' + v % 10; v /= 10; } while (v);
	if (neg)
		b[--i] = '-';
	say(label); say(b + i); say("\n");
}

static void __attribute__((noreturn)) die(const char *what, long ret)
{
	say("FAIL: "); num(what, ret);
	sc(__NR_exit, 1, 0, 0, 0, 0, 0);
	for (;;)
		;
}

static struct disp_session_input_config input;

void __attribute__((noreturn)) main_c(long argc, char **argv)
{
	struct ion_allocation_data alloc = { .len = W * H * 4, .heap_id_mask = ION_HEAP_MULTIMEDIA_MASK };
	struct ion_fd_data share;
	struct disp_buffer_info prep = { 0 };
	struct disp_session_config trig = { 0 };
	static const unsigned char band[4][4] = {
		{ 0xff, 0, 0, 0xff }, { 0, 0xff, 0, 0xff }, { 0, 0, 0xff, 0xff }, { 0xff, 0xff, 0xff, 0xff } };
	long ion, disp, ret, secs = 20, i;
	unsigned char *px;

	if (argc > 1)
		for (secs = 0, i = 0; argv[1][i]; i++)
			secs = secs * 10 + argv[1][i] - '0';

	ion = sc(__NR_openat, AT_FDCWD, (long)"/dev/ion", O_RDWR, 0, 0, 0);
	if (ion < 0) die("open /dev/ion", ion);
	ret = sc(__NR_ioctl, ion, ION_IOC_ALLOC, (long)&alloc, 0, 0, 0);
	if (ret) die("ION_IOC_ALLOC", ret);
	share.handle = alloc.handle;
	ret = sc(__NR_ioctl, ion, ION_IOC_SHARE, (long)&share, 0, 0, 0);
	if (ret) die("ION_IOC_SHARE", ret);
	px = (unsigned char *)sc(__NR_mmap, 0, W * H * 4, 3 /* RW */, 1 /* SHARED */, share.fd, 0);
	if ((long)px < 0 && (long)px > -4096) die("mmap", (long)px);
	for (i = 0; i < W * H; i++) {
		const unsigned char *c = band[i / W * 4 / H];

		px[i * 4 + 0] = c[0]; px[i * 4 + 1] = c[1]; px[i * 4 + 2] = c[2]; px[i * 4 + 3] = c[3];
	}
	say("ion buffer allocated and filled\n");

	disp = sc(__NR_openat, AT_FDCWD, (long)"/dev/mtk_disp_mgr", O_RDWR, 0, 0, 0);
	if (disp < 0) die("open /dev/mtk_disp_mgr", disp);

	prep.session_id = MAKE_DISP_SESSION(DISP_SESSION_PRIMARY, 0);
	prep.layer_id = 0;
	prep.layer_en = 1;
	prep.ion_fd = share.fd;
	prep.cache_sync = 1;
	ret = sc(__NR_ioctl, disp, DISP_IOCTL_PREPARE_INPUT_BUFFER, (long)&prep, 0, 0, 0);
	if (ret) die("PREPARE_INPUT_BUFFER", ret);
	num("prepared: index=", prep.index);
	num("          fence_fd=", prep.fence_fd);

	input.setter = SESSION_USER_HWC;
	input.session_id = prep.session_id;
	input.config_layer_num = 1;
	input.config[0].buffer_source = DISP_BUFFER_ION;
	input.config[0].security = DISP_NORMAL_BUFFER;
	input.config[0].src_fmt = DISP_FORMAT_RGBA8888;
	input.config[0].next_buff_idx = prep.index;
	input.config[0].src_fence_fd = -1;
	input.config[0].src_pitch = W;
	input.config[0].src_width = W;
	input.config[0].src_height = H;
	input.config[0].tgt_width = W;
	input.config[0].tgt_height = H;
	input.config[0].alpha = 0xff;
	input.config[0].layer_id = 0;
	input.config[0].layer_enable = 1;
	ret = sc(__NR_ioctl, disp, DISP_IOCTL_SET_INPUT_BUFFER, (long)&input, 0, 0, 0);
	if (ret) die("SET_INPUT_BUFFER", ret);
	say("input set\n");

	trig.type = DISP_SESSION_PRIMARY;
	trig.session_id = prep.session_id;
	ret = sc(__NR_ioctl, disp, DISP_IOCTL_TRIGGER_SESSION, (long)&trig, 0, 0, 0);
	if (ret) die("TRIGGER_SESSION", ret);
	say("triggered; holding the buffer\n");

	{
		long ts[2] = { secs, 0 };

		sc(__NR_nanosleep, (long)ts, 0, 0, 0, 0, 0);
	}
	say("done\n");
	sc(__NR_exit, 0, 0, 0, 0, 0, 0);
	for (;;)
		;
}

asm(".global _start\n_start:\n  ldr x0, [sp]\n  add x1, sp, #8\n  b main_c\n");
