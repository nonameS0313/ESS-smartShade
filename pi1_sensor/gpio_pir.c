/* PIR 센서 GPIO IRQ 기반 커널 드라이버

cat /dev/pir_sensor  -> 0(평시) 또는 1(침입)

*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PIR Sensor GPIO IRQ Driver for Smart Window Shade");

#define DEVICE_NAME	"pir_sensor"
#define CLASS_NAME	"pir"
#define PIR_GPIO	17
#define PIR_GPIO_LABEL	"pir_gpio"

static int major_number;
static struct class *pir_class  = NULL;
static struct device *pir_device = NULL;
static struct cdev pir_cdev;
static int pir_irq_number;

// pir_state: 0 = 평시, 1 = 침입 감지
// IRQ 핸들러와 read() 양쪽에서 접근하므로 spin_lock_irqsave로 보호
static int pir_state = 0;
static spinlock_t pir_lock;

// Top-half: GPIO 핀 상태 읽어서 pir_state 갱신만 수행
static irqreturn_t pir_irq_handler(int irq, void *dev_id)
{
	unsigned long flags;

	spin_lock_irqsave(&pir_lock, flags);
	pir_state = gpio_get_value(PIR_GPIO); // High = 1(침입), Low = 0(평시)
	spin_unlock_irqrestore(&pir_lock, flags);

	return IRQ_HANDLED;
}

static int pir_open(struct inode *inode, struct file *file)
{
	pr_info("[pir_sensor] Device opened\n");

	return 0;
}

static int pir_release(struct inode *inode, struct file *file)
{
	pr_info("[pir_sensor] Device closed\n");

	return 0;
}

// read()로 현재 PIR 상태를 "0\n" 또는 "1\n" 문자열로 반환
static ssize_t pir_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	unsigned long flags;
	int state;
	char kbuf[4];
	int len;

	if (*ppos > 0)
		return 0;

	spin_lock_irqsave(&pir_lock, flags); // IRQ 핸들러와 동시 접근 보호
	state = pir_state;
	spin_unlock_irqrestore(&pir_lock, flags);

	len = snprintf(kbuf, sizeof(kbuf), "%d\n", state);

	if (copy_to_user(buf, kbuf, len)) {
		pr_err("[pir_sensor] copy_to_user failed\n");

		return -EFAULT;
	}

	*ppos += len;

	return len;
}

static const struct file_operations pir_fops = {
	.owner		= THIS_MODULE,
	.open		= pir_open,
	.release	= pir_release,
	.read		= pir_read,
};

static int __init pir_sensor_init(void)
{
	int ret;
	dev_t dev;

	pr_info("[pir_sensor] Initializing driver\n");

	spin_lock_init(&pir_lock);

	// GPIO 요청 + 입력 방향 설정
	ret = gpio_request_one(PIR_GPIO, GPIOF_IN, PIR_GPIO_LABEL);
	if (ret < 0) {
		pr_err("[pir_sensor] gpio_request_one failed: %d\n", ret);

		return ret;
	}

	// IRQ 번호 획득
	pir_irq_number = gpio_to_irq(PIR_GPIO);
	if (pir_irq_number < 0) {
		pr_err("[pir_sensor] gpio_to_irq failed: %d\n", pir_irq_number);
		ret = pir_irq_number;

		goto err_gpio;
	}

	// IRQ 등록 (전압 상승/하강 양쪽 엣지 감지)
	ret = request_irq(pir_irq_number,
			pir_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			"pir_irq",
			THIS_MODULE);
	if (ret < 0) {
		pr_err("[pir_sensor] request_irq failed: %d\n", ret);

		goto err_gpio;
	}

	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err("[pir_sensor] alloc_chrdev_region failed: %d\n", ret);

		goto err_irq;
	}
	major_number = MAJOR(dev);

	cdev_init(&pir_cdev, &pir_fops);
	pir_cdev.owner = THIS_MODULE;
	ret = cdev_add(&pir_cdev, dev, 1);
	if (ret < 0) {
		pr_err("[pir_sensor] cdev_add failed: %d\n", ret);

		goto err_cdev;
	}

	pir_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(pir_class)) {
		ret = PTR_ERR(pir_class);

		goto err_class;
	}

	pir_device = device_create(pir_class, NULL, MKDEV(major_number, 0),
                                NULL, DEVICE_NAME);
	if (IS_ERR(pir_device)) {
		ret = PTR_ERR(pir_device);

		goto err_device;
	}

	pr_info("[pir_sensor] Driver loaded. GPIO=%d IRQ=%d major=%d device=/dev/%s\n", PIR_GPIO, pir_irq_number, major_number, DEVICE_NAME);

	return 0;

err_device:
	class_destroy(pir_class);
err_class:
	cdev_del(&pir_cdev);
err_cdev:
	unregister_chrdev_region(MKDEV(major_number, 0), 1);
err_irq:
	free_irq(pir_irq_number, THIS_MODULE);
err_gpio:
	gpio_free(PIR_GPIO);

	return ret;
}

static void __exit pir_sensor_exit(void)
{
	device_destroy(pir_class, MKDEV(major_number, 0));
	class_destroy(pir_class);
	cdev_del(&pir_cdev);
	unregister_chrdev_region(MKDEV(major_number, 0), 1);
	free_irq(pir_irq_number, THIS_MODULE);
	gpio_free(PIR_GPIO);
	pr_info("[pir_sensor] Driver unloaded\n");
}

module_init(pir_sensor_init);
module_exit(pir_sensor_exit);
