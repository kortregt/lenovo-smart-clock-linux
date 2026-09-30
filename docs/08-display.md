# 08 — Display

**Status: working, as a normal framebuffer (`/dev/fb0`), with kernel patch 0004.**

## Using it

`/dev/fb0` is 480×800, 32 bits per pixel, bytes in the order **R, G, B, X**
(`blue.offset` = 16), with three pages (`yres_virtual` = 2400).

Two things differ from a PC framebuffer:

- **The screen only updates on a pan.** The display runs in MediaTek's "decouple" mode:
  the overlay engine composes a frame into an intermediate buffer only when it's told to,
  and the panel is refreshed from that buffer. Writing to `/dev/fb0` alone changes nothing
  on screen; follow each drawn frame with `FBIOPAN_DISPLAY` (the same `yoffset` is fine).
  LVGL's fbdev driver can do this after every flush (`lv_linux_fbdev_set_force_refresh`).
- **The panel is mounted rotated 90°.** The visible screen is 800×480 landscape:

  | Screen | Framebuffer |
  |---|---|
  | left → right | rows, top → bottom (y = 0 → 799) |
  | top → bottom | columns, right → left (x = 479 → 0) |

  So the screen's top-left corner is framebuffer pixel (479, 0). Let the UI toolkit rotate.

[`userspace/fbtest.c`](../userspace/fbtest.c) draws a test image and pans.

## Why `/dev/fb0` was black (the bug patch 0004 fixes)

With no framebuffer handed over by the bootloader, the driver (`mtkfb`, in its
`MTK_ONLY_KERNEL_DISP` path) allocates the framebuffer from ION. That returns a kernel
pointer to the buffer and its **IOMMU address** (MVA, `0xff800000`). The driver then threw
the pointer away and passed the IOMMU address to `disp_hal_allocate_framebuffer()`, which
treats it as a *physical* address: it `ioremap`s it (there's no RAM there) and maps it into
the IOMMU again. So:

- everything written to `/dev/fb0`, via `write()` or `mmap`, went nowhere (reading it back
  gave zeros), and
- a pan pointed the display at that second, empty mapping.

The real buffer was never written, and the display never read it. Patch 0004 keeps the ION
kernel pointer and the IOMMU address as they are, and gives `/dev/fb0` an `mmap` that maps
the ION buffer's pages (they aren't physically contiguous, so the generic `fb_mmap`, which
maps `smem_start` as a physical range, can't be used; `smem_start` still reports the
IOMMU address).

### Without the patch: drawing like Android's hwcomposer

Before the fix was found, buffers allocated from ION by a program and submitted through
`/dev/mtk_disp_mgr` (the way Android's hwcomposer draws) were already shown correctly.
[`initramfs/probe/disp-fill.c`](../initramfs/probe/disp-fill.c) does this:

1. Allocate a buffer from `/dev/ion`: `ION_IOC_ALLOC` with heap mask `1 << 10` (MediaTek's
   multimedia heap), then `ION_IOC_SHARE` for a dma-buf fd, and `mmap` it.
2. Fill it: 480×800, bytes **R, G, B, A** (`DISP_FORMAT_RGBA8888`).
3. On `/dev/mtk_disp_mgr`, for the primary session `0x10000` (`MAKE_DISP_SESSION(1, 0)`):
   - `DISP_IOCTL_PREPARE_INPUT_BUFFER`: layer 0, the ION fd, `cache_sync = 1`. It returns
     a buffer index and a release fence.
   - `DISP_IOCTL_SET_INPUT_BUFFER`: setter `SESSION_USER_HWC`, source `DISP_BUFFER_ION`,
     `next_buff_idx` = that index, pitch **in pixels** (480), 480×800 source and target.
   - `DISP_IOCTL_TRIGGER_SESSION`.

Build against the kernel's own `drivers/misc/mediatek/video/include/disp_session.h`, so
the structures (and so the ioctl numbers) match exactly. `disp-fill` is freestanding (no
libc) and uses a tiny `linux/types.h` shim:

```sh
aarch64-linux-android-gcc -Os -static -nostdlib -ffreestanding -fno-stack-protector \
  -I initramfs/probe/shim -I $KERNEL/drivers/misc/mediatek/video/include \
  -o disp-fill initramfs/probe/disp-fill.c
```

That this worked while `/dev/fb0` didn't is what pointed at the framebuffer's own memory.

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

The notes below are the bring-up history, written before the cause above was found. The log warnings
in them weren't the cause: the fix works without touching them.

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
