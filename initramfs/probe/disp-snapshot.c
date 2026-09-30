/*
 * disp-snapshot: runs in the background from the initramfs while stock Android boots on our
 * kernel, and records the display pipeline state once Android has drawn something.
 *
 *   disp-snapshot <block device> <debugfs mount>
 *
 * The block device is opened immediately (busybox switch_root deletes the initramfs, device
 * node included, before Android's init runs). Into boot_b's unused gap:
 *   22 MiB  kernel log only, rewritten every 5 s for 150 s (safe; shows how far boot got)
 *   24 MiB  at 60 s: kernel log, then /disp/dump and /mtkfb from the debugfs mount
 *   26 MiB  at 120 s: the same
 * The display dumps read hardware registers, which can hang the SoC if a block is
 * clock-gated, so each snapshot's kernel log is written before they are read.
 * The debugfs mount stays reachable through this process's root after switch_root.
 *
 * Freestanding (no libc) so it doesn't depend on any file that switch_root removes.
 */
#define AT_FDCWD	-100
#define O_RDONLY	0
#define O_WRONLY	1
#define __NR_openat	56
#define __NR_close	57
#define __NR_read	63
#define __NR_pwrite64	68
#define __NR_nanosleep	101
#define __NR_syslog	116
#define __NR_exit	93

static long sc(long n, long a, long b, long c, long d)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static char buf[4 << 20];
static long len;
static char path[256];

static void put(const char *s)
{
	while (*s && len < (long)sizeof(buf) - 1)
		buf[len++] = *s++;
}

static void put_file(const char *dir, const char *name)
{
	long fd, n, i = 0;

	while (dir[i] && i < 200) { path[i] = dir[i]; i++; }
	while (*name && i < 250) path[i++] = *name++;
	path[i] = 0;
	put("\n=== FILE "); put(path); put("\n");
	fd = sc(__NR_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
	if (fd < 0) { put("(open failed)\n"); return; }
	while ((n = sc(__NR_read, fd, (long)(buf + len), sizeof(buf) - 1 - len, 0)) > 0)
		len += n;
	sc(__NR_close, fd, 0, 0, 0);
}

static void put_dmesg(void)
{
	long n;

	put("\n=== DMESG\n");
	n = sc(__NR_syslog, 3 /* READ_ALL */, (long)(buf + len), sizeof(buf) - 1 - len - 16, 0);
	if (n > 0)
		len += n;
}

static void flush(long out, long offset)
{
	long l = len;

	put("\n=== END\n");
	while (len % 4096)
		buf[len++] = 0;
	sc(__NR_pwrite64, out, (long)buf, len, offset);
	len = l;	/* allow appending more before the next flush */
}

static void snapshot(long out, long offset, const char *dbg, const char *label, int full)
{
	len = 0;
	put("=== CLOCK-DIAG android-snapshot "); put(label); put("\n");
	put_dmesg();
	flush(out, offset);
	if (!full)
		return;
	put_file(dbg, "/mtkfb");
	flush(out, offset);
	put_file(dbg, "/disp/dump");
	flush(out, offset);
}

static void sleep_s(long s)
{
	long ts[2] = { s, 0 };

	sc(__NR_nanosleep, (long)ts, 0, 0, 0);
}

void __attribute__((noreturn)) main_c(long argc, char **argv)
{
	long out;

	if (argc < 3)
		sc(__NR_exit, 1, 0, 0, 0);
	out = sc(__NR_openat, AT_FDCWD, (long)argv[1], O_WRONLY, 0);
	if (out < 0)
		sc(__NR_exit, 2, 0, 0, 0);
	{
		long t;
		static char label[] = "t+000s";

		for (t = 5; t <= 150; t += 5) {
			sleep_s(5);
			label[2] = '0' + t / 100;
			label[3] = '0' + t / 10 % 10;
			label[4] = '0' + t % 10;
			snapshot(out, 22L << 20, argv[2], label, 0);
			if (t == 60)
				snapshot(out, 24L << 20, argv[2], label, 1);
			if (t == 120)
				snapshot(out, 26L << 20, argv[2], label, 1);
		}
	}
	sc(__NR_exit, 0, 0, 0, 0);
	for (;;)
		;
}

/* argc/argv come from the initial stack: [sp] = argc, [sp+8] = argv[0], ... */
asm(".global _start\n_start:\n  ldr x0, [sp]\n  add x1, sp, #8\n  b main_c\n");
