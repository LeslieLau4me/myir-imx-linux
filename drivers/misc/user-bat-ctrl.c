// SPDX-License-Identifier: GPL-2.0
/*
 * Battery Backup Control Driver
 * Adapted for Linux 6.x kernel (platform driver model)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/slab.h>

#define BATCTRL_CNT  1         /* 设备号个数 */
#define BATCTRL_NAME "drv_bat" /* 名字 */
#define BATOFF       0         /* 关电池 */
#define BATON        1         /* 开电池 */

/* batctrl设备结构体 */
struct batctrl_dev {
	dev_t       devid;            /* 设备号 	 */
	struct cdev cdev;             /* cdev 	*/
	struct class *class;          /* 类 		*/
	struct device *device;        /* 设备 	 */
	int         major;            /* 主设备号	  */
	int         minor;            /* 次设备号   */
	int         bat_gpio;         /* 电池控制所使用的GPIO编号 */
};

/*
 * @description		: 打开设备
 * @param - inode 	: 传递给驱动的inode
 * @param - filp 	: 设备文件，file结构体有个叫做private_data的成员变量
 * 					  一般在open的时候将private_data指向设备结构体。
 * @return 			: 0 成功;其他 失败
 */
static int batctrl_open(struct inode *inode, struct file *filp)
{
	struct batctrl_dev *dev = container_of(inode->i_cdev,
					       struct batctrl_dev, cdev);
	filp->private_data = dev; /* 设置私有数据 */
	return 0;
}

/*
 * @description		: 从设备读取数据，返回当前电池控制引脚状态
 * @param - filp 	: 要打开的设备文件(文件描述符)
 * @param - buf 	: 返回给用户空间的数据缓冲区
 * @param - cnt 	: 要读取的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 读取的字节数，如果为负值，表示读取失败
 */
static ssize_t batctrl_read(struct file *filp, char __user *buf,
			    size_t cnt, loff_t *offt)
{
	int retvalue;
	unsigned char databuf[1];
	unsigned char batstat;
	struct batctrl_dev *dev = filp->private_data;

	/* 获取当前GPIO状态 */
	batstat    = gpio_get_value(dev->bat_gpio);
	databuf[0] = batstat;

	retvalue = copy_to_user(buf, databuf, cnt);
	if (retvalue < 0) {
		pr_err("kernel read failed!\n");
		return -EFAULT;
	}

	return cnt;
}

/*
 * @description		: 向设备写数据，控制电池开关
 * @param - filp 	: 设备文件，表示打开的文件描述符
 * @param - buf 	: 要写给设备写入的数据
 * @param - cnt 	: 要写入的数据长度
 * @param - offt 	: 相对于文件首地址的偏移
 * @return 			: 写入的字节数，如果为负值，表示写入失败
 */
static ssize_t batctrl_write(struct file *filp, const char __user *buf,
			     size_t cnt, loff_t *offt)
{
	int retvalue;
	unsigned char databuf[1];
	unsigned char batstat;
	struct batctrl_dev *dev = filp->private_data;

	pr_info("batctrl: write called, count=%zu\n", cnt);
	if (cnt != 1) {
		pr_err("write size error! only support 1 byte\n");
		return -EINVAL;
	}

	retvalue = copy_from_user(databuf, buf, cnt);
	if (retvalue < 0) {
		pr_err("kernel write failed!\n");
		return -EFAULT;
	}

	batstat = databuf[0]; /* 获取状态值 */

	if (batstat == BATON) {
		gpio_set_value(dev->bat_gpio, 1); /* 输出高电平，打开电池 */
		pr_info("Battery backup turned ON (GPIO = HIGH)\n");
	} else if (batstat == BATOFF) {
		gpio_set_value(dev->bat_gpio, 0); /* 输出低电平，关闭电池 */
		pr_info("Battery backup turned OFF (GPIO = LOW)\n");
	} else {
		pr_err("Invalid value! Please write 1(ON) or 0(OFF)\n");
		return -EINVAL;
	}

	return cnt;
}

/*
 * @description		: 关闭/释放设备
 * @param - filp 	: 要关闭的设备文件(文件描述符)
 * @return 			: 0 成功;其他 失败
 */
static int batctrl_release(struct inode *inode, struct file *filp)
{
	pr_info("batctrl device released\n");
	return 0;
}

/* 设备操作函数 */
static struct file_operations batctrl_fops = {
	.owner   = THIS_MODULE,
	.open    = batctrl_open,
	.read    = batctrl_read,
	.write   = batctrl_write,
	.release = batctrl_release,
};

static int gener_batctrl_device(struct batctrl_dev *batctrl, const char *name)
{
	int ret = 0;

	ret = alloc_chrdev_region(&batctrl->devid, 0, BATCTRL_CNT, name);
	if (ret) {
		pr_err("alloc_chrdev_region %s fail!\n", name);
		return ret;
	}
	batctrl->major = MAJOR(batctrl->devid);
	batctrl->minor = MINOR(batctrl->devid);
	pr_info("batctrl major=%d, minor=%d\n", batctrl->major, batctrl->minor);

	cdev_init(&batctrl->cdev, &batctrl_fops);
	batctrl->cdev.owner = THIS_MODULE;

	ret = cdev_add(&batctrl->cdev, batctrl->devid, BATCTRL_CNT);
	if (ret) {
		pr_err("cdev add fail.\n");
		unregister_chrdev_region(batctrl->devid, BATCTRL_CNT);
		return ret;
	}

	batctrl->class = class_create(name);
	if (IS_ERR(batctrl->class)) {
		ret = PTR_ERR(batctrl->class);
		cdev_del(&batctrl->cdev);
		unregister_chrdev_region(batctrl->devid, BATCTRL_CNT);
		return ret;
	}

	batctrl->device = device_create(batctrl->class, NULL,
					batctrl->devid, NULL, name);
	if (IS_ERR(batctrl->device)) {
		ret = PTR_ERR(batctrl->device);
		class_destroy(batctrl->class);
		cdev_del(&batctrl->cdev);
		unregister_chrdev_region(batctrl->devid, BATCTRL_CNT);
		return ret;
	}

	return 0;
}

static void destroy_batctrl_device(struct batctrl_dev *batctrl)
{
	device_destroy(batctrl->class, batctrl->devid);
	class_destroy(batctrl->class);
	cdev_del(&batctrl->cdev);
	unregister_chrdev_region(batctrl->devid, BATCTRL_CNT);
}

static int batctrl_drv_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dev_node = dev->of_node;
	struct batctrl_dev *batctrl;
	const char *name;
	int ret;

	pr_info("batctrl: probe device %s\n", dev_name(dev));

	batctrl = devm_kzalloc(dev, sizeof(*batctrl), GFP_KERNEL);
	if (!batctrl)
		return -ENOMEM;

	platform_set_drvdata(pdev, batctrl);

	/* 读取dts字符串属性 batctrl-names */
	if (of_property_read_string(dev_node, "batctrl-names", &name)) {
		/* 读不到属性则取节点短名 */
		name = of_node_full_name(dev_node);
	}

	if (!name)
		name = BATCTRL_NAME;

	/* 获取GPIO编号（使用设备树中的 bat-gpios 属性） */
	batctrl->bat_gpio = of_get_named_gpio(dev_node, "bat-gpios", 0);
	if (!gpio_is_valid(batctrl->bat_gpio)) {
		dev_err(dev, "failed to get GPIO from DT property 'bat-gpios': %d\n",
			batctrl->bat_gpio);
		return batctrl->bat_gpio;
	}

	/* 请求GPIO */
	ret = devm_gpio_request(dev, batctrl->bat_gpio, "batctrl");
	if (ret) {
		dev_err(dev, "failed to request GPIO %d: %d\n",
			batctrl->bat_gpio, ret);
		return ret;
	}

	/* 设置GPIO方向为输出，初始值高电平（电池打开） */
	ret = gpio_direction_output(batctrl->bat_gpio, 1);
	if (ret) {
		dev_err(dev, "failed to set GPIO %d direction to output: %d\n",
			batctrl->bat_gpio, ret);
		return ret;
	}

	pr_info("batctrl: got GPIO %d, default state: ON (HIGH)\n", batctrl->bat_gpio);

	/* 注册字符设备 */
	ret = gener_batctrl_device(batctrl, name);
	if (ret)
		return ret;

	pr_info("batctrl driver initialized successfully!\n");
	pr_info("Device node: /dev/%s\n", name);

	return 0;
}

static void batctrl_drv_remove(struct platform_device *pdev)
{
	struct batctrl_dev *batctrl = platform_get_drvdata(pdev);

	/* 退出时确保电池打开 */
	if (gpio_is_valid(batctrl->bat_gpio))
		gpio_set_value(batctrl->bat_gpio, 1);

	destroy_batctrl_device(batctrl);

	pr_info("batctrl driver exited, battery backup set to ON state\n");
}

static const struct of_device_id of_batctrl_drv_match[] = {
	{
		.compatible = "batctrl",
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_batctrl_drv_match);

static struct platform_driver batctrl_drv_driver = {
	.probe  = batctrl_drv_probe,
	.remove = batctrl_drv_remove,
	.driver = {
		.name           = "batctrl-drv",
		.owner          = THIS_MODULE,
		.of_match_table = of_batctrl_drv_match,
	},
};

static int __init drv_batctrl_init(void)
{
	int ret;

	ret = platform_driver_register(&batctrl_drv_driver);
	if (ret < 0) {
		pr_err("batctrl: platform driver init error!\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit drv_batctrl_exit(void)
{
	platform_driver_unregister(&batctrl_drv_driver);
}

module_init(drv_batctrl_init);
module_exit(drv_batctrl_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Battery Backup Control Driver");
MODULE_VERSION("2.0");
MODULE_AUTHOR("Adapted for Linux 6.x");
