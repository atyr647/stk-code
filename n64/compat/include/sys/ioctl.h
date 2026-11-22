/* N64: ioctl() comes from libdragon (<ioctl.h>); socket ioctls are stubs. */
#ifndef STK_N64_SYS_IOCTL_H
#define STK_N64_SYS_IOCTL_H
#include <ioctl.h>
#ifndef FIONBIO
#define FIONBIO  0x5421
#endif
#ifndef FIONREAD
#define FIONREAD 0x541B
#endif
#endif
