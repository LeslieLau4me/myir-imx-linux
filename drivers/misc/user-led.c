#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/slab.h>

#define DEVICE_LED_NAME "drv_leds"

// 控制命令定义
#define LED_MAGIC     'L'
#define LED_ON        _IO(LED_MAGIC, 0)                          // 常亮
#define LED_OFF       _IO(LED_MAGIC, 1)                          // 关闭
#define LED_BLINK     _IOW(LED_MAGIC, 2, struct led_blink_param) // 闪烁
#define LED_GET_STATE _IOR(LED_MAGIC, 3, struct led_state)       // 获取状态

enum LED_LIST {
    LED_RUN = 0,
    LED_ALERT,
    LED_LIST_MAX,
};

// LED模式定义
enum LED_MODE {
    LED_MODE_OFF = 0,
    LED_MODE_ON,
    LED_MODE_BLINK,
};

struct led_blink_param {
    int led_index;   // LED索引
    int blink_count; // 闪烁次数(0表示持续闪烁)
    int on_time_ms;  // 亮灯时间(ms)
    int off_time_ms; // 灭灯时间(ms)
};

struct led_state {
    int led_index;
    int mode;
    int blink_count;
    int max_blinks;
    int on_time_ms;
    int off_time_ms;
};

// LED控制结构
struct led_control {
    unsigned char enabled;
    unsigned char value;
    enum LED_MODE mode;        // 当前模式
    unsigned int  blink_count; // 已闪烁次数
    unsigned int  max_blinks;  // 最大闪烁次数
    unsigned int  on_time;     // 亮灯时间(jiffies)
    unsigned int  off_time;    // 灭灯时间(jiffies)
    unsigned long next_change; // 下次状态改变时间
    spinlock_t    lock;        // 自旋锁保护
};

#define TIMER_INTERVAL 10 // 10ms定时器间隔
static struct timer_list led_timer;

static struct cdev   c_leds_dev;
static dev_t         leds_devno;
static struct class *c_leds_dev_class;

/* GPIO pins parsed from device tree */
static int *led_gpio_pins;
static int led_pin_num;

static struct led_control *led_ctrls;

static void gpio_free_safe(unsigned int gpio)
{
    if (gpio_is_valid(gpio)) {
        gpio_free(gpio);
    }
}

// 应用新的LED设置（抢占式）
static void apply_led_settings(
    int led_index, enum LED_MODE new_mode, int max_blinks, int on_time_ms, int off_time_ms)
{
    struct led_control *led = &led_ctrls[led_index];
    unsigned long       flags;

    // 获取自旋锁保护
    spin_lock_irqsave(&led->lock, flags);

    // 立即应用新设置
    led->mode        = new_mode;
    led->enabled     = (new_mode != LED_MODE_OFF);
    led->value       = (new_mode == LED_MODE_ON) ? 1 : 0;
    led->blink_count = 0;
    led->max_blinks  = max_blinks;
    led->on_time     = msecs_to_jiffies(on_time_ms);
    led->off_time    = msecs_to_jiffies(off_time_ms);

    // 设置GPIO状态
    gpio_set_value(led_gpio_pins[led_index], led->value);

    // 如果是闪烁模式，设置下次改变时间
    if (new_mode == LED_MODE_BLINK && led->enabled) {
        led->next_change = jiffies + led->on_time;
    }

    spin_unlock_irqrestore(&led->lock, flags);

    printk(KERN_DEBUG "LED %d: mode=%d, blinks=%d, on=%dms, off=%dms\n",
           led_index,
           new_mode,
           max_blinks,
           on_time_ms,
           off_time_ms);
}

// LED定时器处理函数（Linux 5.4内核版本）
static void led_timer_handler(struct timer_list *t)
{
    int           i;
    unsigned long flags;
    unsigned long current_jiffies = jiffies;

    for (i = 0; i < led_pin_num; i++) {
        struct led_control *led = &led_ctrls[i];

        // 获取锁检查状态
        spin_lock_irqsave(&led->lock, flags);

        if (led->enabled && led->mode == LED_MODE_BLINK) {
            if (time_after_eq(current_jiffies, led->next_change)) {
                // 切换LED状态
                led->value = !led->value;
                gpio_set_value(led_gpio_pins[i], led->value);

                // 设置下次状态改变时间
                if (led->value) {
                    led->next_change = current_jiffies + led->on_time;
                    // 计数闪烁次数(从亮到灭算一次)
                    if (led->max_blinks > 0) {
                        led->blink_count++;
                        if (led->blink_count >= led->max_blinks) {
                            // 达到闪烁次数，关闭LED
                            led->enabled = 0;
                            led->mode    = LED_MODE_OFF;
                            gpio_set_value(led_gpio_pins[i], 0);
                            printk(KERN_DEBUG "LED %d: blink completed\n", i);
                        }
                    }
                } else {
                    led->next_change = current_jiffies + led->off_time;
                }
            }
        }

        spin_unlock_irqrestore(&led->lock, flags);
    }

    // 重新设置定时器
    mod_timer(&led_timer, jiffies + msecs_to_jiffies(TIMER_INTERVAL));
}

static void led_timer_init(void)
{
    int i;

    // 初始化自旋锁
    for (i = 0; i < led_pin_num; i++) { 
        spin_lock_init(&led_ctrls[i].lock); 
    }

    // 使用Linux 5.4内核的新定时器API
    timer_setup(&led_timer, led_timer_handler, 0);
    led_timer.expires = jiffies + msecs_to_jiffies(TIMER_INTERVAL);
    add_timer(&led_timer);
}

static void led_timer_cleanup(void)
{
    del_timer_sync(&led_timer);  // 使用同步删除，确保定时器不会在删除后再次执行
}

static int leds_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "LED driver opened\n");
    return 0;
}

static long leds_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int                    ret = 0;
    int                    led_index;
    struct led_blink_param blink_param;
    struct led_state       state;

    switch (cmd) {
        case LED_ON:
            led_index = (int)arg;
            if (led_index < 0 || led_index >= led_pin_num) {
                ret = -EINVAL;
                break;
            }

            // 立即切换到常亮模式（抢占之前的任何状态）
            apply_led_settings(led_index, LED_MODE_ON, 0, 0, 0);
            break;

        case LED_OFF:
            led_index = (int)arg;
            if (led_index < 0 || led_index >= led_pin_num) {
                ret = -EINVAL;
                break;
            }

            // 立即关闭LED（抢占之前的任何状态）
            apply_led_settings(led_index, LED_MODE_OFF, 0, 0, 0);
            break;

        case LED_BLINK:
            if (copy_from_user(&blink_param, (void __user *)arg, sizeof(blink_param))) {
                ret = -EFAULT;
                break;
            }

            if (blink_param.led_index < 0 || blink_param.led_index >= led_pin_num ||
                blink_param.on_time_ms <= 0 || blink_param.off_time_ms <= 0) {
                ret = -EINVAL;
                break;
            }

            // 立即切换到闪烁模式（抢占之前的任何状态）
            apply_led_settings(blink_param.led_index,
                               LED_MODE_BLINK,
                               blink_param.blink_count,
                               blink_param.on_time_ms,
                               blink_param.off_time_ms);
            break;

        case LED_GET_STATE:
            if (copy_from_user(&state, (void __user *)arg, sizeof(state))) {
                ret = -EFAULT;
                break;
            }

            if (state.led_index < 0 || state.led_index >= led_pin_num) {
                ret = -EINVAL;
                break;
            }

            // 读取LED状态（需要锁保护）
            {
                struct led_control *led = &led_ctrls[state.led_index];
                unsigned long       flags;

                spin_lock_irqsave(&led->lock, flags);
                state.mode        = led->mode;
                state.blink_count = led->blink_count;
                state.max_blinks  = led->max_blinks;
                state.on_time_ms  = jiffies_to_msecs(led->on_time);
                state.off_time_ms = jiffies_to_msecs(led->off_time);
                spin_unlock_irqrestore(&led->lock, flags);
            }

            if (copy_to_user((void __user *)arg, &state, sizeof(state))) {
                ret = -EFAULT;
            }
            break;

        default:
            ret = -ENOTTY;
            break;
    }

    return ret;
}

// 文件操作结构
static struct file_operations leds_fops = {
    .owner          = THIS_MODULE,
    .open           = leds_open,
    .unlocked_ioctl = leds_ioctl,
};

static int create_leds_device(const char *name)
{
    int ret;

    ret = alloc_chrdev_region(&leds_devno, 0, 1, name);
    if (ret) {
        printk(KERN_ERR "Failed to alloc chrdev region\n");
        return ret;
    }

    cdev_init(&c_leds_dev, &leds_fops);
    c_leds_dev.owner = THIS_MODULE;

    ret = cdev_add(&c_leds_dev, leds_devno, 1);
    if (ret) {
        printk(KERN_ERR "Failed to add cdev\n");
        unregister_chrdev_region(leds_devno, 1);
        return ret;
    }

    c_leds_dev_class = class_create(name);
    if (IS_ERR(c_leds_dev_class)) {
        printk(KERN_ERR "Failed to create class\n");
        cdev_del(&c_leds_dev);
        unregister_chrdev_region(leds_devno, 1);
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
    int                 ret, i;
    struct device_node *dev_node;
    struct device      *dev;
    const char         *name;

    printk(KERN_INFO "Probing LED device\n");

    dev = &pdev->dev;
    dev_node = dev->of_node;

    /* 从设备树中获取GPIO引脚数量 */
    led_pin_num = 0;
    while (of_get_named_gpio(dev_node, "leds-gpios", led_pin_num) >= 0)
        led_pin_num++;

    if (led_pin_num <= 0) {
        printk(KERN_ERR "No leds-gpios found in device tree\n");
        return -ENODEV;
    }

    /* 动态分配内存 */
    led_gpio_pins = devm_kcalloc(dev, led_pin_num, sizeof(int), GFP_KERNEL);
    if (!led_gpio_pins)
        return -ENOMEM;

    led_ctrls = devm_kcalloc(dev, led_pin_num, sizeof(struct led_control), GFP_KERNEL);
    if (!led_ctrls)
        return -ENOMEM;

    name = of_get_property(dev_node, "drv_leds-names", NULL) ?: dev_node->name;
    if (!name)
        name = DEVICE_LED_NAME;

    ret = create_leds_device(name);
    if (ret)
        return ret;

    // 初始化GPIO
    for (i = 0; i < led_pin_num; i++) {
        led_gpio_pins[i] = of_get_named_gpio(dev_node, "leds-gpios", i);

        if (!gpio_is_valid(led_gpio_pins[i])) {
            printk(KERN_ERR "Invalid GPIO for LED %d: %d\n", i, led_gpio_pins[i]);
            goto err_cleanup;
        }

        ret = gpio_direction_output(led_gpio_pins[i], 0);
        if (ret) {
            printk(KERN_ERR "Failed to set GPIO direction for LED %d\n", i);
            goto err_cleanup;
        }
    }

    // 启动运行指示灯
    if (led_pin_num > LED_RUN) {
        led_ctrls[LED_RUN].enabled = 1;
        led_ctrls[LED_RUN].mode    = LED_MODE_ON;
        led_ctrls[LED_RUN].value   = 1;
        gpio_set_value(led_gpio_pins[LED_RUN], 1);
    }

    led_timer_init();
    printk(KERN_INFO "LED driver initialized successfully\n");
    return 0;

err_cleanup:
    for (i = 0; i < led_pin_num; i++) {
        if (gpio_is_valid(led_gpio_pins[i])) {
            gpio_free_safe(led_gpio_pins[i]);
        }
    }
    destroy_leds_device();
    return ret;
}

static void leds_drv_remove(struct platform_device *pdev)
{
    int i;

    printk(KERN_INFO "Removing LED driver\n");

    // 关闭所有LED
    for (i = 0; i < led_pin_num; i++) {
        gpio_set_value(led_gpio_pins[i], 0);
        gpio_free_safe(led_gpio_pins[i]);
    }

    led_timer_cleanup();
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
    if (ret) {
        printk(KERN_ERR "Failed to register LED driver\n");
        return ret;
    }

    return 0;
}

static void __exit drv_led_exit(void)
{
    platform_driver_unregister(&leds_drv_driver);
    printk(KERN_INFO "LED driver exited\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Liu Sheng");
MODULE_DESCRIPTION("LED Driver with Watchdog Feeding for Linux 5.4");

module_init(drv_led_init);
module_exit(drv_led_exit);