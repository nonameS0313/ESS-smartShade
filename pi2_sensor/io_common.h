#ifndef _IO_COMMON_H_
#define _IO_COMMON_H_

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define STATE_NORMAL   0
#define STATE_MANUAL   1
#define STATE_DISABLED 2
#define STATE_ALERT    3

#define SYS_IOCTL_START_NUM  0x80
#define SYS_IOCTL_NUM1       (SYS_IOCTL_START_NUM + 1)
#define SYS_IOCTL_NUM2       (SYS_IOCTL_START_NUM + 2)
#define SYS_IOCTL_NUM3       (SYS_IOCTL_START_NUM + 3)

#define SYS_IOCTL_NUM        'z'
#define CMD_ULTRA_TRIG       _IO(SYS_IOCTL_NUM, SYS_IOCTL_NUM1)
#define CMD_SET_SYSTEM_STATE _IOW(SYS_IOCTL_NUM, SYS_IOCTL_NUM2, int)
#define CMD_RELEASE_ALERT    _IO(SYS_IOCTL_NUM, SYS_IOCTL_NUM3)

#endif
