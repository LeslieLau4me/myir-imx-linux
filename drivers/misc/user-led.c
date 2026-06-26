#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/gpio.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#define DEVICE_LED_NAME "drv_leds"

#define LEDS_EN_SHIFT 31
#define LEDS_MODE_SHIFT 30
#define LEDS_NAME_SHIFT 24
#define LEDS_NUM_SHIFT 16
#define LEDS_TIMER_SHIFT 0

enum LED_LIST {
	LED_RUN = 0,
	LED_ALERT,
	LED_LIST_MAX,
};

struct _led_run {
	unsigned char en; // 0: closeled 1:open led
	unsigned char value;
	unsigned char mode;
	unsigned char maxnum;
	unsigned char curnum;
	unsigned int maxtime;
	unsigned int curtime;
};

#define pin_num(group, number) ((group - 1) * 32 + number)

struct _pin {
	int index;
	int reg;
};

#define PTIME 1
static struct timer_list s_timer;

static struct cdev c_leds_dev;
static dev_t leds_devno;
static struct class *c_leds_dev_class;

struct _pin led_pin[] = {
	{ LED_RUN, pin_num(4, 7) },
	{ LED_ALERT, pin_num(4, 13) },
};

struct LedLock {
	spinlock_t lock; // 自旋锁保护
};

#define LED_PIN_NUM (sizeof(led_pin) / sizeof(struct _pin))

struct _led_run led_run[LED_PIN_NUM];
unsigned int len_max = LED_PIN_NUM;
struct LedLock led_lock[LED_PIN_NUM];

static void gpioFree(unsigned int pin)
{
	if (gpio_is_valid(pin) && pin != 0) {
		gpio_free(pin);
	}
}

void leds_timer_task(void)
{
	int i;
	unsigned long flags;

	for (i = 0; i < LED_PIN_NUM; ++i) {
		spin_lock_irqsave(&led_lock[i].lock, flags);
		if (led_run[i].en) {
			switch (led_run[i].mode) {
			case 0:
				if (!led_run[i].maxtime)
					break;

				led_run[i].curtime++;
				if (led_run[i].curtime < led_run[i].maxtime)
					break;
				gpio_set_value(led_pin[i].reg, 0);
				memset(&led_run[i], 0, sizeof(struct _led_run));
				break;

			case 1:
				led_run[i].curtime++;
				if (led_run[i].curtime < led_run[i].maxtime)
					break;
				led_run[i].curtime = 0;
				if (led_run[i].value) {
					led_run[i].value = 0;
					gpio_set_value(led_pin[i].reg, 0);
					break;
				}

				led_run[i].value = 1;
				gpio_set_value(led_pin[i].reg, 1);
				if (!led_run[i].maxnum)
					break;
				led_run[i].curnum++;
				if (led_run[i].curnum < led_run[i].maxnum)
					break;
				memset(&led_run[i], 0, sizeof(struct _led_run));
				gpio_set_value(led_pin[i].reg, 0);
				break;

			default:
				break;
			}
		}
		spin_unlock_irqrestore(&led_lock[i].lock, flags);
	}
}

EXPORT_SYMBOL(leds_timer_task);

// 修改：Linux 5.4内核定时器回调函数使用新的参数类型
static void s_timer_fn(struct timer_list *t)
{
	leds_timer_task();
	mod_timer(&s_timer, jiffies + PTIME);
}

void user_timer_init(void)
{
	int i;

	for (i = 0; i < LED_PIN_NUM; i++) {
		spin_lock_init(&led_lock[i].lock);
	}

	// 修改：使用timer_setup替代init_timer
	timer_setup(&s_timer, s_timer_fn, 0);
	s_timer.expires = jiffies + PTIME;
	add_timer(&s_timer);
}

void user_timer_del(void)
{
	// 修改：使用del_timer_sync确保安全删除
	del_timer_sync(&s_timer);
}

static int leds_open(struct inode *inode, struct file *file)
{
	printk(KERN_ALERT "open leds driver ...\n");
	return 0;
}

static ssize_t leds_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	return 1;
}

static long leds_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	unsigned int i;
	unsigned long flags;

	if (cmd != 0) {
		printk(KERN_ALERT "cmd para error!!\n");
		return -1;
	}

	i = 0x3f & (arg >> 24);
	if (i >= LED_PIN_NUM) {
		printk(KERN_ALERT "led input para error!!\n");
		return -1;
	}

	spin_lock_irqsave(&led_lock[i].lock, flags);
	if (arg & (1 << 31)) {
		led_run[i].en = 1;
		led_run[i].value = 1;
		led_run[i].mode = (unsigned char)(1 & (arg >> 30));
		led_run[i].curnum = 0;
		led_run[i].maxnum = (unsigned char)(arg >> 16);
		led_run[i].curtime = 0;
		led_run[i].maxtime = 0xffff & arg;

		gpio_set_value(led_pin[i].reg, 1);
	} else {
		memset(&led_run[i], 0, sizeof(struct _led_run));
		gpio_set_value(led_pin[i].reg, 0);
	}
	spin_unlock_irqrestore(&led_lock[i].lock, flags);

	return 0;
}

static struct file_operations leds_fops = {
	.owner = THIS_MODULE,
	.open = leds_open,
	.read = leds_read,
	.unlocked_ioctl = leds_ioctl,
};

static int gener_leds_device(const char *name)
{
	int ret = 0;

	ret = alloc_chrdev_region(&leds_devno, 0, 1, name);
	if (ret) {
		printk("alloc_chrdev_region leds fail!\n");
		return ret;
	}

	cdev_init(&c_leds_dev, &leds_fops);
	c_leds_dev.owner = THIS_MODULE;

	ret = cdev_add(&c_leds_dev, leds_devno, 1);
	if (ret) {
		printk("c_leds_dev add fail.\n");
		unregister_chrdev_region(leds_devno, 1);
		return ret;
	}

	c_leds_dev_class = class_create(name);
	if (IS_ERR(c_leds_dev_class)) {
		printk("create leds class fail.\n");
		unregister_chrdev_region(leds_devno, 1);
		cdev_del(&c_leds_dev);
		return PTR_ERR(c_leds_dev_class);
	}

	device_create(c_leds_dev_class, NULL, leds_devno, NULL, name);

	return 0;
}

static void destroy_leds_device(void)
{
	device_destroy(c_leds_dev_class, leds_devno);
	class_destroy(c_leds_dev_class);
	cdev_del(&c_leds_dev);
	unregister_chrdev_region(leds_devno, 1);
}

static int leds_drv_probe(struct platform_device *pdev)
{
	int i;
	struct device_node *dev_node;
	struct device *dev;
	const char *name;

	printk("probe device %s\n", dev_name(&pdev->dev));

	memset(&led_run, 0, sizeof(led_run));

	dev = &pdev->dev;
	dev_node = dev->of_node;

	// 修改：简化设备树节点查找逻辑
	dev_node = of_find_compatible_node(NULL, NULL, "fsl,imx6ul-gpio");
	if (!dev_node) {
		printk("get gpio device node error!\n");
		return -EINVAL;
	}

	dev_node = of_find_compatible_node(dev_node, NULL, "leds_drv");
	if (!dev_node) {
		printk("failure to find leds device node!\n");
		return -EINVAL;
	}

	name = of_get_property(dev_node, "drv_leds-names", NULL) ?:
								   dev_node->name;
	if (name == NULL)
		name = DEVICE_LED_NAME;

	gener_leds_device(name);

	// 初始化leds为输出
	for (i = 0; i < LED_PIN_NUM; ++i) {
		led_pin[i].reg = of_get_named_gpio(dev_node, "leds-gpios", i);

		if (!gpio_is_valid(led_pin[i].reg)) {
			printk("invalid leds[%d].pin: %d \n", i,
			       led_pin[i].reg);
			goto err1;
		}

		// 使能GPIO
		gpio_direction_output(led_pin[i].reg, 0);
	}

	gpio_set_value(led_pin[LED_RUN].reg, 1);

	user_timer_init();

	return 0;

err1:
	for (i = 0; i < LED_PIN_NUM; ++i) {
		gpioFree(led_pin[i].reg);
	}
	destroy_leds_device();
	return -1;
}

static void leds_drv_remove(struct platform_device *pdev)
{
	int i;

	// 关闭所有LED
	for (i = 0; i < LED_PIN_NUM; ++i) {
		gpio_set_value(led_pin[i].reg, 0);
		gpioFree(led_pin[i].reg);
	}

	user_timer_del();
	destroy_leds_device();
}

static const struct of_device_id of_leds_drv_match[] = {
	{
		.compatible = "leds_drv",
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_leds_drv_match);

static struct platform_driver leds_drv_driver = {
    .probe  = leds_drv_probe,
    .remove = leds_drv_remove,
    .driver = {
        .name           = "leds-drv",
        .owner          = THIS_MODULE,
        .of_match_table = of_leds_drv_match,
    },
};

static int __init drv_led_init(void)
{
	int ret;

	ret = platform_driver_register(&leds_drv_driver);
	if (ret < 0) {
		printk("leds_drv_driver init error!\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit drv_led_exit(void)
{
	printk(KERN_ALERT "exit led driver\n");
	platform_driver_unregister(&leds_drv_driver);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("LED Driver for Linux 5.4");

module_init(drv_led_init);
module_exit(drv_led_exit);