#ifndef STEPPER_IOCTL_H
#define STEPPER_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define STEPPER_IOCTL_START_NUM  0x80
#define STEPPER_IOCTL_NUM1       (STEPPER_IOCTL_START_NUM + 1)
#define STEPPER_IOCTL_NUM2       (STEPPER_IOCTL_START_NUM + 2)
#define STEPPER_IOCTL_NUM3       (STEPPER_IOCTL_START_NUM + 3)

#define STEPPER_IOC_NUM          'S'
#define STEPPER_IOC_MOVE     _IOW(STEPPER_IOC_NUM, STEPPER_IOCTL_NUM1, struct stepper_move)
#define STEPPER_IOC_ZERO     _IO(STEPPER_IOC_NUM, STEPPER_IOCTL_NUM2)
#define STEPPER_IOC_RESTORE  _IO(STEPPER_IOC_NUM, STEPPER_IOCTL_NUM3)

struct stepper_move {
	int degree;
	int direction; /* 0=clockwise, 1=counter-clockwise */
};

#endif /* STEPPER_IOCTL_H */
