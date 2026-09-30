/*
 * reboot-to <target>: reboot with a command string, like Android's `adb reboot bootloader`.
 * On the Smart Clock, `reboot-to bootloader` lands in fastboot mode without the volume-key
 * dance. Freestanding static binary (no libc), for Alpine or the initramfs.
 */
#define __NR_sync	81
#define __NR_reboot	142
#define __NR_write	64
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

void __attribute__((noreturn)) main_c(long argc, char **argv)
{
	static const char usage[] = "usage: reboot-to <bootloader|recovery|...>\n";

	if (argc < 2) {
		sc(__NR_write, 2, (long)usage, sizeof(usage) - 1, 0);
		sc(__NR_exit, 1, 0, 0, 0);
	}
	sc(__NR_sync, 0, 0, 0, 0);
	/* LINUX_REBOOT_MAGIC1, MAGIC2, LINUX_REBOOT_CMD_RESTART2, arg */
	sc(__NR_reboot, 0xfee1dead, 672274793, 0xa1b2c3d4, (long)argv[1]);
	sc(__NR_exit, 2, 0, 0, 0);
	for (;;)
		;
}

asm(".global _start\n_start:\n  ldr x0, [sp]\n  add x1, sp, #8\n  b main_c\n");
