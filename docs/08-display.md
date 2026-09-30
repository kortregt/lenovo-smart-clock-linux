# 08 — Display

**Status: working, through the display manager (the way Android's hwcomposer draws).**
`/dev/fb0` itself shows only black, but a buffer from ION submitted to `/dev/mtk_disp_mgr`
displays correctly. [`initramfs/probe/disp-fill.c`](../initramfs/probe/disp-fill.c) draws
a test pattern this way.

## Drawing through the display manager

1. Allocate a buffer from `/dev/ion`: `ION_IOC_ALLOC` with heap mask `1 << 10` (MediaTek's
   multimedia heap), then `ION_IOC_SHARE` for a dma-buf fd, and `mmap` it.
2. Fill it: 480×800, 4 bytes per pixel, in byte order **R, G, B, A**
   (`DISP_FORMAT_RGBA8888`).
3. On `/dev/mtk_disp_mgr`, for the primary session `0x10000` (`MAKE_DISP_SESSION(1, 0)`):
   - `DISP_IOCTL_PREPARE_INPUT_BUFFER`: layer 0, the ION fd, `cache_sync = 1`. It returns
     a buffer index and a fence.
   - `DISP_IOCTL_SET_INPUT_BUFFER`: setter `SESSION_USER_HWC`, source `DISP_BUFFER_ION`,
     `next_buff_idx` = that index, pitch **in pixels** (480), 480×800 source and target.
   - `DISP_IOCTL_TRIGGER_SESSION`.

Build against the kernel's own `drivers/misc/mediatek/video/include/disp_session.h`, so
the structures (and so the ioctl numbers) match exactly. The tool here is freestanding
(no libc) and uses a tiny `linux/types.h` shim:

```sh
aarch64-linux-android-gcc -Os -static -nostdlib -ffreestanding -fno-stack-protector \
  -I initramfs/probe/shim -I $KERNEL/drivers/misc/mediatek/video/include \
  -o disp-fill initramfs/probe/disp-fill.c
```

**The panel is mounted rotated 90°.** The visible screen is 800×480 landscape; buffer
rows (top to bottom) show up as columns (left to right). Draw rotated.

## Why `/dev/fb0` stays black

The display pipeline runs in MediaTek's "decouple" mode, and this kernel is hard-wired to
it (`DISP_HW_MODE_CAP = DISP_OUTPUT_CAP_DECOUPLE`), so Android uses the very same pipeline.
What differs is the overlay's input: Android's (and `disp-fill`'s) buffers are ION buffers
mapped when they're submitted, while mtkfb allocates `/dev/fb0`'s buffer at probe time
(0.46 s into boot). The likely cause, not yet confirmed, is that this happens while the
pseudo-M4U IOMMU is still initialising (the log shows `pseudo alloc_iova failed
pseudo_reserve_dm` at that moment), leaving that one buffer badly mapped. The history of
how this was narrowed down is below.

## The hardware

- Panel: **ST7701S**, 480×800, MIPI DSI video mode, 2 lanes, RGB888; kernel LCM driver
  `st7701s_t400_wvga_dsi_vdo`. Supply `vgp2` (2.8 V), reset on GPIO 66.
- Backlight: **SGM37603A** on i2c-2 `0x36`, as `/sys/class/leds/lcd-backlight`. Its enable
  pin (pin 22) is shared with the LCM driver.
- The device tree's `gpio_lcd_pwr` (pin 50) is a red herring: that pin is muxed to SPI.

## Who initializes the panel

The bootloader **doesn't**: on the fastboot path it reports `lcm=0-<null>`, and on normal
boots it passes no `atag,videolfb` at all. The stock Lenovo logo only appears ~16 s in, from
Android. So the kernel does it: with no `videolfb`, the driver's `MTK_ONLY_KERNEL_DISP` path
finds the panel by its device-tree node and runs the full power-up at boot (regulator,
reset, and a short init table; the panel's settings are in its OTP).

With `LOG_BUF_SHIFT=20`, the boot log shows it:

```
[DISP][DT][videolfb] lcmfound=1, fps=0, fb_base=tobe, vram=8388608, lcmname=st7701s_t400_wvga_dsi_vdo
[Kernel/LCM] lcm_init() enter
[Kernel/LCM] lcm_resume() enter
LCM: lcm_vgp_supply_enable
```

## What's proven

- `echo -n dsi_bist:0xff0000 > /sys/kernel/debug/dispsys` (our patch 0003) switches the
  DSI block to its self-test pattern. **Red, green, blue and white all display.** So the
  panel is initialized and the DSI link is good.
- The pipeline is configured and running (`cat /sys/kernel/debug/disp/dump`). It's
  MediaTek's "decouple" mode:

  ```
  OVL0 (reads fb @0xff800000) → WDMA0 → buffer @0xff400000      [on each update]
  RDMA0 (reads @0xff400000) → COLOR → CCORR → AAL → GAMMA → DITHER → UFOE → DSI0   [60 Hz]
  ```

  RDMA0 reports no underflows, and the compose counters go up on every framebuffer pan.
- `pq_bypass:0x1f` (patch 0003: all five picture-quality stages in relay) **doesn't**
  help, so the pixels are lost before those stages.

## The investigation (for reference)

### Memory path

`0xff800000` and `0xff400000` aren't RAM (RAM is `0x40000000`–`0x80000000`). They're
IOMMU addresses (M4U) of ION buffers, and the display engines only see the right memory if
their larb0 ports are in *virtual* mode. On this kernel (`CONFIG_MTK_PSEUDO_M4U` +
`MTK_IOMMU` + MediaTek's in-house TEE), that's done through a secure-world call when the
larb powers up. The boot log has suspicious lines:

```
pseudo alloc_iova failed pseudo_reserve_dm, ... dm->start 0x0, dm->length 0x800000
cmdq_session_handle() failed to create session, ret=-65528
[CMDQ][ERR]DEV: byName: cannot get module clock: smi-common / smi-larb0 / mtcmos-dis
```

Yet **the same kernel shows pictures when stock Android runs on top of it**, so Android's
graphics stack (hwcomposer/gralloc through `/dev/mtk_disp_mgr` and ION) does something the
bare framebuffer path doesn't.

An attempt to capture Android's display state (booting stock Android userspace on our
kernel with a kernel-side snapshot task) never produced data: the task, which works under
Alpine, didn't run under that boot. Reproducing hwcomposer's calls from Alpine worked
first time instead.

## Warnings

- **Don't use `dispsys` `regr`/`regw` on arbitrary blocks.** Reading a clock-gated display
  block hung the SoC and the watchdog rebooted it. There's no `/dev/mem` either.
- After blanking the framebuffer (`echo 4 > /sys/class/graphics/fb0/blank`) and unblanking,
  the backlight chip has been reset and stays dark. Write `0` then `255` to
  `/sys/class/leds/lcd-backlight/brightness`; writing 255 twice is a no-op.
- `echo 1 > …/blank` doesn't power-cycle the panel; `4` does.
