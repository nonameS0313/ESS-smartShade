/* MCP3208 SPI ADC 기반 조도 센서 커널 드라이버

사용법
ioctl(fd, LIGHT_SET_CHANNEL, 0) = CH0 선택
ioctl(fd, LIGHT_SET_CHANNEL, 1) = CH1 선택
read(fd, buf, len)              = 선택된 채널 조도값 반환

*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spi/spidev.h>
#include <linux/file.h>

// ioctl 명령 정의
#define LIGHT_IOC_MAGIC 'L'
#define LIGHT_SET_CHANNEL _IOW(LIGHT_IOC_MAGIC, 1, int)
#define LIGHT_GET_CHANNEL _IOR(LIGHT_IOC_MAGIC, 2, int)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MCP3208 SPI Light Sensor Driver for Smart Window Shade");

#define DEVICE_NAME	"light_sensor"
#define CLASS_NAME	"light"
#define SPIDEV_PATH	"/dev/spidev0.0"
#define SPI_SPEED_HZ	1000000  // SPI 통신 속도
#define SPI_BITS	8        // 한 번에 전송하는 비트 수

static int major_number;
static struct class *light_class  = NULL;
static struct device *light_device = NULL;
static struct cdev light_cdev;
static DEFINE_MUTEX(spi_lock);

// open()마다 독립적인 채널 상태 저장
struct light_dev_state {
	int channel; // 현재 선택된 MCP3208 채널 (0~7)
};

// /dev/spidev0.0 열어서 MCP3208 SPI 통신 (3바이트 전송)
// TX: start bit + SGL + D2/D1/D0 / RX: 12비트 ADC 결과
static int mcp3208_read(int channel)
{
	struct file *spi_file;
	unsigned char tbuf[3], rbuf[3];
	struct spi_ioc_transfer spi_msg;
	int ret;
	unsigned char spi_mode = SPI_MODE_0;
	unsigned char spi_bpw  = SPI_BITS;
	unsigned int  spi_speed = SPI_SPEED_HZ;
	mm_segment_t old_fs;

	if (channel < 0 || channel > 7) {
		pr_err("[light_sensor] Invalid channel: %d\n", channel);

		return -EINVAL;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	spi_file = filp_open(SPIDEV_PATH, O_RDWR, 0);
	if (IS_ERR(spi_file)) {
		pr_err("[light_sensor] filp_open failed: %ld\n", PTR_ERR(spi_file));
		set_fs(old_fs);
		
		return PTR_ERR(spi_file);
	}

	// SPI 설정
	vfs_ioctl(spi_file, SPI_IOC_WR_MODE, (unsigned long)&spi_mode);
	vfs_ioctl(spi_file, SPI_IOC_WR_BITS_PER_WORD, (unsigned long)&spi_bpw);
	vfs_ioctl(spi_file, SPI_IOC_WR_MAX_SPEED_HZ, (unsigned long)&spi_speed);

	tbuf[0] = 0x06 | ((channel & 0x07) >> 2);	// start bit + SGL + D2
	tbuf[1] = (channel & 0x07) << 6;		// D1, D0
	tbuf[2] = 0x00;					// dummy

	memset(&spi_msg, 0, sizeof(spi_msg));
	spi_msg.tx_buf = (unsigned long)tbuf;
	spi_msg.rx_buf = (unsigned long)rbuf;
	spi_msg.len = 3;

	ret = vfs_ioctl(spi_file, SPI_IOC_MESSAGE(1), (unsigned long)&spi_msg);

	filp_close(spi_file, NULL);
	set_fs(old_fs);

	if (ret < 0) {
		pr_err("[light_sensor] SPI transfer failed: %d\n", ret);

		return ret;
	}

	return ((rbuf[1] & 0x0F) << 8) | rbuf[2]; // 12비트 결과 파싱
}

static int light_open(struct inode *inode, struct file *file)
{
	struct light_dev_state *state;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->channel = 0; // 기본값 CH0
	file->private_data = state;

	pr_info("[light_sensor] Device opened (default ch=0)\n");
	
	return 0;
}

static int light_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	pr_info("[light_sensor] Device closed\n");

	return 0;
}

// read()로 현재 선택된 채널의 조도값을 "2048\n" 형태 문자열로 반환
static ssize_t light_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct light_dev_state *state = file->private_data;
	int adc_val;
	char kbuf[16];
	int len;

	if (*ppos > 0)
		return 0;

	mutex_lock(&spi_lock);
	adc_val = mcp3208_read(state->channel);
	mutex_unlock(&spi_lock);

	if (adc_val < 0) {
		pr_err("[light_sensor] Failed to read ADC (ch=%d)\n", state->channel);
		return adc_val;
	}

	len = snprintf(kbuf, sizeof(kbuf), "%d\n", adc_val);

	if (copy_to_user(buf, kbuf, len)) {
		pr_err("[light_sensor] copy_to_user failed\n");

		return -EFAULT;
	}
	*ppos += len;
	
	return len;
}

// LIGHT_SET_CHANNEL: 채널 번호 설정 (0~7)
// LIGHT_GET_CHANNEL: 현재 채널 번호 반환
static long light_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct light_dev_state *state = file->private_data;
	int channel;

	switch (cmd) {
		case LIGHT_SET_CHANNEL:
			if (copy_from_user(&channel, (int __user *)arg, sizeof(int)))
				return -EFAULT;
			if (channel < 0 || channel > 7) {
				pr_err("[light_sensor] ioctl: invalid channel %d\n", channel);

				return -EINVAL;
			}
			state->channel = channel;
			pr_info("[light_sensor] Channel set to %d\n", channel);

			break;

		case LIGHT_GET_CHANNEL:
			if (copy_to_user((int __user *)arg, &state->channel, sizeof(int)))
				return -EFAULT;

			break;

		default:
			return -ENOTTY;
	}

	return 0;
}

static const struct file_operations light_fops = {
	.owner		= THIS_MODULE,
	.open		= light_open,
	.release	= light_release,
	.read		= light_read,
	.unlocked_ioctl	= light_ioctl,
};

static int __init light_sensor_init(void)
{
	int ret;
	dev_t dev;

	pr_info("[light_sensor] Initializing driver\n");

	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err("[light_sensor] alloc_chrdev_region failed: %d\n", ret);

		return ret;
	}
	major_number = MAJOR(dev);

	cdev_init(&light_cdev, &light_fops);
	light_cdev.owner = THIS_MODULE;
	ret = cdev_add(&light_cdev, dev, 1);
	if (ret < 0) {
		pr_err("[light_sensor] cdev_add failed: %d\n", ret);
		
		goto err_cdev;
	}

	light_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(light_class)) {
		ret = PTR_ERR(light_class);

		goto err_class;
	}

	light_device = device_create(light_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(light_device)) {
		ret = PTR_ERR(light_device);

		goto err_device;
	}

	pr_info("[light_sensor] Driver loaded. major=%d device=/dev/%s\n", major_number, DEVICE_NAME);

	return 0;

err_device:
	class_destroy(light_class);
err_class:
	cdev_del(&light_cdev);
err_cdev:
	unregister_chrdev_region(MKDEV(major_number, 0), 1);

	return ret;
}

static void __exit light_sensor_exit(void)
{
	device_destroy(light_class, MKDEV(major_number, 0));
	class_destroy(light_class);
	cdev_del(&light_cdev);
	unregister_chrdev_region(MKDEV(major_number, 0), 1);
	pr_info("[light_sensor] Driver unloaded\n");
}

module_init(light_sensor_init);
module_exit(light_sensor_exit);
