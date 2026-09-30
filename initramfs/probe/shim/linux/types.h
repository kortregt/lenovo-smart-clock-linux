/* Minimal freestanding stand-in for <linux/types.h> + _IO* macros, enough for the MTK
 * display headers (drivers/misc/mediatek/video/include/disp_session.h). arm64 LP64. */
#ifndef SHIM_LINUX_TYPES_H
#define SHIM_LINUX_TYPES_H
typedef unsigned char __u8;
typedef signed char __s8;
typedef unsigned short __u16;
typedef unsigned int __u32;
typedef int __s32;
typedef unsigned long long __u64;
typedef unsigned long size_t;
typedef _Bool bool;
#define _IOC(dir, type, nr, size) (((dir) << 30) | ((size) << 16) | ((type) << 8) | (nr))
#define _IOW(type, nr, t)  _IOC(1U, (type), (nr), sizeof(t))
#define _IOR(type, nr, t)  _IOC(2U, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t) _IOC(3U, (type), (nr), sizeof(t))
#define _IO(type, nr)      _IOC(0U, (type), (nr), 0)
#endif
