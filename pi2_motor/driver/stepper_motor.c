#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "stepper_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dual stepper motor char device driver");

#define STEPPER_A_PIN1  6
#define STEPPER_A_PIN2  13
#define STEPPER_A_PIN3  19
#define STEPPER_A_PIN4  26

#define STEPPER_B_PIN1  5
#define STEPPER_B_PIN2  12
#define STEPPER_B_PIN3  16
#define STEPPER_B_PIN4  20

#define STEPS          8
#define ONEROUND       512
#define FULLDEGREE     360
#define STEP_DELAY_US  1000
#define MAX_STEPPER_DEG 90
#define STEPPER_COUNT  2

static int blue[8] = { 1, 1, 0, 0, 0, 0, 0, 1 };
static int pink[8] = { 0, 1, 1, 1, 0, 0, 0, 0 };
static int yellow[8] = { 0, 0, 0, 1, 1, 1, 0, 0 };
static int orange[8] = { 0, 0, 0, 0, 0, 1, 1, 1 };

struct stepper_dev {
	int pin[4];
	struct cdev cdev;
	struct device *device;
	const char *name;
	int current_deg;	//현재 각도 저장
	bool at_zero;	//0도로 돌았는지를 저장
	struct mutex lock;	//MQTT와 Poll이 다른 쓰레드로 돌기 때문에 필요
};

static struct stepper_dev stepper_a;
static struct stepper_dev stepper_b;
static dev_t stepper_devno;
static struct class *stepper_class;

static void setstep(struct stepper_dev *dev, int p1, int p2, int p3, int p4)
{
	gpio_set_value(dev->pin[0], p1);
	gpio_set_value(dev->pin[1], p2);
	gpio_set_value(dev->pin[2], p3);
	gpio_set_value(dev->pin[3], p4);
}

static void backward(struct stepper_dev *dev, int degree)
{
	int i, j;

	for (i = 0; i < ONEROUND * degree / FULLDEGREE; i++) {
		for (j = STEPS-1; j >= 0; j--) {
			setstep(dev, blue[j], pink[j], yellow[j], orange[j]);
			udelay(STEP_DELAY_US);
		}
	}
}

static void forward(struct stepper_dev *dev, int degree)
{
	int i, j;

	for (i = 0; i < ONEROUND * degree / FULLDEGREE; i++) {
		for (j = 0; j < STEPS; j++) {
			setstep(dev, blue[j], pink[j], yellow[j], orange[j]);
			udelay(STEP_DELAY_US);
		}
	}
}

static void stepper_do_move(struct stepper_dev *dev, int degree, int direction)
{
	int actual = degree;	//실제로 돌 각도(0도와 90도를 하한선과 상한선으로 지정)

	if (direction == 0) {
		if (dev->current_deg + degree > MAX_STEPPER_DEG)
			actual = MAX_STEPPER_DEG - dev->current_deg;
		if (actual <= 0)
			return;
		forward(dev, actual);
		dev->current_deg += actual;
	} else {
		if (dev->current_deg - degree < 0)
			actual = dev->current_deg;
		if (actual <= 0)
			return;
		backward(dev, actual);
		dev->current_deg -= actual;
	}

	dev->at_zero = (dev->current_deg == 0);
}

static void stepper_do_zero(struct stepper_dev *dev)
{
	if (!dev->at_zero && dev->current_deg > 0)
		backward(dev, dev->current_deg);
	dev->at_zero = true;
	setstep(dev, 0, 0, 0, 0);
}	//초기 각도(0도)로 조정

static void stepper_do_restore(struct stepper_dev *dev)
{
	if (dev->at_zero && dev->current_deg > 0)
		forward(dev, dev->current_deg);
	dev->at_zero = false;
	setstep(dev, 0, 0, 0, 0);
}	//0도 조정 이후 다시 복구

static int stepper_open(struct inode *inode, struct file *filp)
{
	struct stepper_dev *dev;

	dev = container_of(inode->i_cdev, struct stepper_dev, cdev);
	filp->private_data = dev;

	return 0;
}

static long stepper_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct stepper_dev *dev = filp->private_data;
	struct stepper_move move;

	switch (cmd) {
	case STEPPER_IOC_MOVE:
		if (copy_from_user(&move, (void __user *)arg, sizeof(move)))
			return -EFAULT;

		if (move.degree <= 0 || move.degree > 360)
			return -EINVAL;

		if (move.direction != 0 && move.direction != 1)
			return -EINVAL;

		mutex_lock(&dev->lock);
		stepper_do_move(dev, move.degree, move.direction);
		mutex_unlock(&dev->lock);
		return 0;

	case STEPPER_IOC_ZERO:
		mutex_lock(&dev->lock);
		stepper_do_zero(dev);
		mutex_unlock(&dev->lock);
		return 0;

	case STEPPER_IOC_RESTORE:
		mutex_lock(&dev->lock);
		stepper_do_restore(dev);
		mutex_unlock(&dev->lock);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations stepper_fops = {
	.owner          = THIS_MODULE,
	.open           = stepper_open,
	.unlocked_ioctl = stepper_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = stepper_ioctl,
#endif
};

static int stepper_gpio_init(struct stepper_dev *dev)
{
	int i, ret;
	char label[16];

	for (i = 0; i < 4; i++) {
		snprintf(label, sizeof(label), "%s_p%d", dev->name, i + 1);
		ret = gpio_request_one(dev->pin[i], GPIOF_OUT_INIT_LOW, label);
		if (ret) {
			while (--i >= 0)
				gpio_free(dev->pin[i]);
			return ret;
		}
	}

	setstep(dev, 0, 0, 0, 0);
	return 0;
}

static void stepper_gpio_free(struct stepper_dev *dev)
{
	int i;

	for (i = 0; i < 4; i++)
		gpio_free(dev->pin[i]);
}

static int register_stepper(struct stepper_dev *dev, int minor)
{
	int ret;

	cdev_init(&dev->cdev, &stepper_fops);

	ret = cdev_add(&dev->cdev, MKDEV(MAJOR(stepper_devno), minor), 1);
	if (ret)
		return ret;

	dev->device = device_create(stepper_class, NULL,
				    MKDEV(MAJOR(stepper_devno), minor),
				    NULL, dev->name);
	if (IS_ERR(dev->device)) {
		ret = PTR_ERR(dev->device);
		cdev_del(&dev->cdev);
		return ret;
	}

	return 0;
}

static void unregister_stepper(struct stepper_dev *dev, int minor)
{
	device_destroy(stepper_class, MKDEV(MAJOR(stepper_devno), minor));
	cdev_del(&dev->cdev);
}

static void stepper_dev_init(struct stepper_dev *dev, const char *name)
{
	dev->name = name;
	dev->current_deg = 0;
	dev->at_zero = true;
	mutex_init(&dev->lock);
}

static int __init stepper_init(void)
{
	int ret;

	stepper_dev_init(&stepper_a, "stepper_a");
	stepper_a.pin[0] = STEPPER_A_PIN1;
	stepper_a.pin[1] = STEPPER_A_PIN2;
	stepper_a.pin[2] = STEPPER_A_PIN3;
	stepper_a.pin[3] = STEPPER_A_PIN4;

	stepper_dev_init(&stepper_b, "stepper_b");
	stepper_b.pin[0] = STEPPER_B_PIN1;
	stepper_b.pin[1] = STEPPER_B_PIN2;
	stepper_b.pin[2] = STEPPER_B_PIN3;
	stepper_b.pin[3] = STEPPER_B_PIN4;

	ret = alloc_chrdev_region(&stepper_devno, 0, STEPPER_COUNT, "stepper");
	if (ret)
		return ret;

	stepper_class = class_create(THIS_MODULE, "stepper");
	if (IS_ERR(stepper_class)) {
		ret = PTR_ERR(stepper_class);
		goto err_chrdev;
	}

	ret = stepper_gpio_init(&stepper_a);
	if (ret)
		goto err_class;

	ret = stepper_gpio_init(&stepper_b);
	if (ret)
		goto err_gpio_a;

	ret = register_stepper(&stepper_a, 0);
	if (ret)
		goto err_gpio_b;

	ret = register_stepper(&stepper_b, 1);
	if (ret)
		goto err_reg_a;

	pr_info("stepper_motor: loaded (%s, %s)\n", stepper_a.name, stepper_b.name);
	return 0;

err_reg_a:
	unregister_stepper(&stepper_a, 0);
err_gpio_b:
	stepper_gpio_free(&stepper_b);
err_gpio_a:
	stepper_gpio_free(&stepper_a);
err_class:
	class_destroy(stepper_class);
err_chrdev:
	unregister_chrdev_region(stepper_devno, STEPPER_COUNT);
	return ret;
}

static void __exit stepper_exit(void)
{
	unregister_stepper(&stepper_b, 1);
	unregister_stepper(&stepper_a, 0);
	stepper_gpio_free(&stepper_b);
	stepper_gpio_free(&stepper_a);
	class_destroy(stepper_class);
	unregister_chrdev_region(stepper_devno, STEPPER_COUNT);
	pr_info("stepper_motor: unloaded\n");
}

module_init(stepper_init);
module_exit(stepper_exit);
