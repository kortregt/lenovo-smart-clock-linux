/*
 * disp-mode <mode>: set the MediaTek primary display session mode through /dev/mtk_disp_mgr
 * (DISP_IOCTL_SET_SESSION_MODE), the way Android's hwcomposer does.
 *   1 = direct link (OVL0 -> PQ -> DSI), 2 = decouple (OVL0 -> WDMA0 -> buffer -> RDMA0 -> DSI)
 * Freestanding static binary (no libc).
 */
#define AT_FDCWD	-100
#define O_RDWR		2
#define __NR_ioctl	29
#define __NR_openat	56
#define __NR_write	64
#define __NR_exit	93

/* _IOW('O', 209, struct disp_session_config) with sizeof == 36 on arm64 */
#define DISP_IOCTL_SET_SESSION_MODE	0x40244fd1
#define DISP_SESSION_PRIMARY		1

struct disp_session_config {
	unsigned int type, device_id, mode, session_id, user;
	unsigned int present_fence_idx, dc_type;
	int need_merge;
	unsigned int tigger_mode;
};

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

void __attribute__((noreturn)) main_c(long argc, char **argv)
{
	static const char usage[] = "usage: disp-mode <1=direct link|2=decouple>\n";
	static struct disp_session_config cfg;
	long fd, ret;

	if (argc < 2 || argv[1][0] < '1' || argv[1][0] > '4') {
		sc(__NR_write, 2, (long)usage, sizeof(usage) - 1, 0);
		sc(__NR_exit, 1, 0, 0, 0);
	}
	fd = sc(__NR_openat, AT_FDCWD, (long)"/dev/mtk_disp_mgr", O_RDWR, 0);
	if (fd < 0)
		sc(__NR_exit, 2, 0, 0, 0);
	cfg.type = DISP_SESSION_PRIMARY;
	cfg.mode = argv[1][0] - '0';
	cfg.session_id = DISP_SESSION_PRIMARY << 16;
	ret = sc(__NR_ioctl, fd, DISP_IOCTL_SET_SESSION_MODE, (long)&cfg, 0);
	sc(__NR_exit, ret ? 3 : 0, 0, 0, 0);
	for (;;)
		;
}

asm(".global _start\n_start:\n  ldr x0, [sp]\n  add x1, sp, #8\n  b main_c\n");
