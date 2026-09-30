/*
 * Smallest possible /init: reboot immediately. Used to tell "init runs" (fast reset, ~20 s
 * to the Lenovo logo) from "init never runs" (~60-70 s watchdog reset) without any output.
 * Freestanding, raw syscalls only, so it adds almost nothing to the kernel image size.
 */
#define __NR_sync   81
#define __NR_reboot 142

static long sys3(long n, long a, long b, long c, long d)
{
	register long x8 asm("x8") = n;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;
	register long x3 asm("x3") = d;
	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

void _start(void)
{
	sys3(__NR_sync, 0, 0, 0, 0);
	/* LINUX_REBOOT_MAGIC1, MAGIC2, LINUX_REBOOT_CMD_RESTART */
	sys3(__NR_reboot, 0xfee1dead, 672274793, 0x01234567, 0);
	for (;;)
		;
}
