/*
 * afewatch: debugging aid for the Smart Clock's speaker freeze (docs/10-bluetooth.md).
 * Polls MT8167 AFE registers every ~2 ms and logs every change, so the moment the
 * HDMI/I2S8CH memif stops shows which register changed. If the memif's current pointer
 * stops moving for 20 ms, dumps all watched registers once.
 *
 *   insmod afewatch.ko        (dmesg | grep afewatch)
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/module.h>

#define AFE_BASE	0x11140000
#define AFE_SIZE	0x1000
#define HDMI_OUT_CUR	0x0378

static const struct { unsigned int off; const char *name; } regs[] = {
	{ 0x0000, "AUDIO_TOP_CON0" },	{ 0x0004, "AUDIO_TOP_CON1" },
	{ 0x000c, "AUDIO_TOP_CON3" },	{ 0x0010, "AFE_DAC_CON0" },
	{ 0x0370, "AFE_HDMI_OUT_CON0" },	{ 0x0374, "AFE_HDMI_OUT_BASE" },
	{ 0x037c, "AFE_HDMI_OUT_END" },	{ 0x0390, "AFE_HDMI_CONN0" },
	{ 0x03a0, "AFE_IRQ_MCU_CON" },	{ 0x03b4, "AFE_IRQ_MCU_EN" },
	{ 0x03f8, "AFE_IRQ_MCU_CON2" },	{ 0x03ac, "AFE_IRQ_CNT1" },
	{ 0x03b0, "AFE_IRQ_CNT2" },	{ 0x03bc, "AFE_IRQ_CNT5" },
	{ 0x0548, "AFE_TDM_CON1" },	{ 0x054c, "AFE_TDM_CON2" },
};

static void __iomem *afe;
static struct task_struct *thread;

static int afewatch(void *unused)
{
	u32 last[ARRAY_SIZE(regs)], cur, prev_cur = 0, v;
	unsigned long still_since = jiffies;
	bool dumped = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		last[i] = readl(afe + regs[i].off);
		pr_info("afewatch: %-18s = 0x%08x\n", regs[i].name, last[i]);
	}
	while (!kthread_should_stop()) {
		usleep_range(2000, 2500);
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			v = readl(afe + regs[i].off);
			if (v != last[i]) {
				pr_info("afewatch: %-18s 0x%08x -> 0x%08x (cur 0x%08x)\n",
					regs[i].name, last[i], v, readl(afe + HDMI_OUT_CUR));
				last[i] = v;
			}
		}
		cur = readl(afe + HDMI_OUT_CUR);
		if (cur != prev_cur) {
			prev_cur = cur;
			still_since = jiffies;
			dumped = false;
		} else if (!dumped && time_after(jiffies, still_since + msecs_to_jiffies(20))) {
			pr_info("afewatch: HDMI_OUT_CUR stuck at 0x%08x\n", cur);
			for (i = 0; i < ARRAY_SIZE(regs); i++)
				pr_info("afewatch:   %-18s = 0x%08x\n", regs[i].name, last[i]);
			dumped = true;
		}
	}
	return 0;
}

static int __init afewatch_init(void)
{
	afe = ioremap(AFE_BASE, AFE_SIZE);
	if (!afe)
		return -ENOMEM;
	thread = kthread_run(afewatch, NULL, "afewatch");
	if (IS_ERR(thread)) {
		iounmap(afe);
		return PTR_ERR(thread);
	}
	return 0;
}

static void __exit afewatch_exit(void)
{
	kthread_stop(thread);
	iounmap(afe);
}

module_init(afewatch_init);
module_exit(afewatch_exit);
MODULE_LICENSE("GPL");
