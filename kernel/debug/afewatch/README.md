# afewatch

A debugging module that found the speaker freeze ([docs/10](../../../docs/10-bluetooth.md)):
it polls MT8167 AFE registers (memif enables, the HDMI/I2S8CH memif's buffer and pointer,
IRQ setup, AUDIO_TOP power bits, TDM out) every ~2 ms and logs every change to the kernel
log, and dumps them all if the memif's current pointer stops moving.

Build against the kernel tree (same config as the running kernel) and load it while
reproducing a problem:

```sh
make -C $KERNEL O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- M=$PWD modules
insmod afewatch.ko && dmesg -w | grep afewatch
```

Only read while the AFE is clocked: reading a clock-gated block can hang the SoC.
