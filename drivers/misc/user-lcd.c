#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/pwm.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>

#define MODULE_DEFAULT_NAME "user-lcd"

#define IO_DELAY_TIME 1
#define RESET_DELAY_TIME 500
#define vlcd_bias_data 0xA0 //0~255  ,0x00~0xff
#define bias_ratio 1 //0: 1/5Bias,  1: 1/10B  ,2: 1/11B  ,3: 1/12B
#define temp_comp  3
#define LCD_ROW_SIZE 160
#define LCD_COLUMN_SIZE 160
#define COLUMN_START 112
#define ROW_START 1
#define LCD_BUFFER_SIZE (((LCD_ROW_SIZE) * (LCD_COLUMN_SIZE)) / 8)
#define LCD_MAX_LINE_BYTES 160
#define LCD_MAX_COLE_BYTES 20

#define CONTRAST_DEFAULT_75160 162

enum LCD_LIST {
	IO_LCD_RESET = 0,
	IO_LCD_BLIGHT,
	IO_LCD_A0_RS, // rs
	IO_LCD_WR,
	IO_LCD_RD,
	IO_LCD_CS,
	IO_LCD_D7,
	IO_LCD_D6,
	IO_LCD_D5,
	IO_LCD_D4,
	IO_LCD_D3,
	IO_LCD_D2,
	IO_LCD_D1,
	IO_LCD_D0,
	IO_LCD_MAX,
};

enum LCD_CTRL_LIST {
	LCD_CTRL_BL_ON = 0,
	LCD_CTRL_BL_OFF,
	LCD_CTRL_POWER_ON,
	LCD_CTRL_POWER_OFF,
	LCD_CTRL_RESET,
	LCD_CTRL_READ_STATE,
	LCD_CTRL_SET_RADIO,
	LCD_CTRL_SET_RADIO_MODE,
	LCD_CTRL_SET_TEMP,
	LCD_CTRL_MAX,
};

struct wr_slines_t {
	unsigned short x;
	unsigned short len;
	unsigned char __user *buffer;
};

static struct gpio_desc *lcd_gpios[IO_LCD_MAX];

static unsigned char lcd_showbuffer[3280];
static unsigned char lcd_dispbuffer[LCD_MAX_LINE_BYTES][LCD_MAX_COLE_BYTES] = {
	{ 0 },
	{ 0 }
};

#define LCD_PIN_NUM IO_LCD_MAX

static struct cdev cdev;
static dev_t devno;
static struct class *cdev_class;
static struct miscdevice *lcd_dev __maybe_unused;

// lcd power
static void lcd_poweron(void)
{
	// gpio_set_value(lcd_gpios[IO_LCD_POWER],0);
}
static void lcd_poweroff(void)
{
	// gpio_set_value(lcd_gpios[IO_LCD_POWER],1);
}
// lcd reset
static void lcd_rston(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_RESET], 1);
}
static void lcd_rstoff(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_RESET], 0);
}
// lcd blight
static void lcd_baklighton(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_BLIGHT], 1);
}
static void lcd_baklightoff(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_BLIGHT], 0);
}

static void lcd_hw_rst(void)
{
	lcd_rston();
	mdelay(2);
	lcd_rstoff();
	mdelay(5);
	lcd_rston();
	mdelay(10);
}

static void lcd_cs_0(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_CS], 0);
}
static void lcd_cs_1(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_CS], 1);
}

static void lcd_a0_rs_0(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_A0_RS], 0);
}
static void lcd_a0_rs_1(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_A0_RS], 1);
}

static void lcd_wr_0(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_WR], 0);
}
static void lcd_wr_1(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_WR], 1);
}

static void lcd_rd_0(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_RD], 0);
}
static void lcd_rd_1(void)
{
	gpiod_set_value_cansleep(lcd_gpios[IO_LCD_RD], 1);
}

static inline void delay_databus(void)
{
	int i;
	for (i = 0; i < IO_DELAY_TIME; i++)
		;
}

static void set_databus(unsigned char ch)
{
	int i;

	for (i = IO_LCD_D7; i < IO_LCD_MAX; i++, ch <<= 1) {
		gpiod_direction_output(lcd_gpios[i], (ch & 0x80) ? 1 : 0);
	}
}

static unsigned char get_databus(void)
{
	int i;
	int temp;
	unsigned char value;
	unsigned char ch = 0;
	for (i = 0; i < 8; i++) {
		gpiod_direction_input(lcd_gpios[IO_LCD_D7 + i]);
		temp = gpiod_get_value_cansleep(lcd_gpios[IO_LCD_D7 + i]);
		value = temp ? 1 : 0;
		ch |= value;
		ch <<= 1;
	}
	return ch;
}

static void lcd_75160_writedata(unsigned char ch)
{
	lcd_cs_1();
	lcd_a0_rs_1();
	lcd_cs_0();
	lcd_wr_1();
	lcd_rd_1();

	set_databus(ch);
	lcd_wr_0();
	//delay_databus();
	lcd_wr_1();

	lcd_cs_1();
	lcd_a0_rs_0();
}
static void read_status(unsigned char *buf)
{
	int i;

	lcd_a0_rs_0();
	lcd_wr_1();
	lcd_cs_0();

	for (i = 0; i < 3; i++) {
		lcd_rd_0();
		delay_databus();
		buf[i] = get_databus();
		lcd_rd_1();
	}

	lcd_cs_1();
	lcd_a0_rs_1();

	buf[3] = 0;
}
static void lcd_75160_setcmd(unsigned char ch)
{
	lcd_cs_1();
	lcd_a0_rs_0();
	lcd_cs_0();
	lcd_wr_1();
	lcd_rd_1();

	set_databus(ch);
	lcd_wr_0();
	//delay_databus();
	lcd_wr_1();

	lcd_cs_1();
	lcd_a0_rs_1();
}
// 8080

static unsigned char lcd_75160_read_byte(unsigned char cmd)
{
	unsigned char byte = 0;
	lcd_75160_setcmd(cmd);
	mdelay(5);

	lcd_a0_rs_0();
	lcd_wr_1();
	lcd_cs_0();

	lcd_rd_0();
	byte = get_databus();
	lcd_rd_1();

	lcd_cs_1();
	lcd_a0_rs_1();
	return byte;
}
static void __maybe_unused lcd_75160_dispbyte(unsigned char ch)
{
	lcd_75160_setcmd(0x30);
	lcd_75160_setcmd(0x5c);
	lcd_75160_writedata(ch);
}
static void __maybe_unused lcd_75160_read_status(unsigned char *buf)
{
}

static void lcd_75160_read_id(void)
{
	lcd_75160_setcmd(0x38); // Extension Command 3
	lcd_75160_setcmd(0x7F); // Enable Read ID
	lcd_75160_setcmd(0x30); // Extension Command 1
	lcd_75160_read_byte(0xFE); //Read ID Value
	mdelay(20);
	lcd_75160_setcmd(0x38); // Extension Command 3
	lcd_75160_setcmd(0x7E); // Disable Read ID
	lcd_75160_setcmd(0x30); // Extension Command 1
}

static void lcd_75160_set_ratio_data(int ratio)
{
	int contrast;
	contrast = ratio;

	if (contrast < 150) {
		contrast = 175;
	}
	if (contrast > 189) {
		contrast = 150;
	}

	lcd_75160_setcmd(0x30); // Extension Command 1
	lcd_75160_setcmd(0x81); // Set Vop = 13V

	lcd_75160_writedata((contrast / 3) & 0x00ff);
	lcd_75160_writedata(0x04);
}

static void lcd_75160_reset(void)
{
	printk(KERN_ALERT "init st75160 ...\n");

	lcd_75160_setcmd(0x30); // Extension Command 1
	lcd_75160_setcmd(0x94); // Sleep Out
	lcd_75160_setcmd(0xd1);
	lcd_75160_setcmd(0x20); // Power Control

	lcd_75160_writedata(0x08); // VB, VR ON

	mdelay(10);

	lcd_75160_setcmd(0x20); // Power Control

	lcd_75160_writedata(0x0B); // VB, VR, VF All ON

	mdelay(10);

#if 0
	lcd_75160_setcmd(0x81);  // Set Vop = 13V

	lcd_75160_writedata((Contrast/3)&0x00ff);
	lcd_75160_writedata(0x04);
#else
	lcd_75160_set_ratio_data(CONTRAST_DEFAULT_75160);
#endif

	lcd_75160_setcmd(0xca); // Display Control

	lcd_75160_writedata(0x00); // CL Dividing Ratio? Not Divide
	lcd_75160_writedata(0x9f); //Duty Set 160 Duty
	lcd_75160_writedata(0x0f); //Frame Inversion  Nline

	/*
	lcd_75160_setcmd(0xa8);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x08);*/

	lcd_75160_setcmd(0xa6); //Inverse Display
	lcd_75160_setcmd(0x08); // DO

	lcd_75160_setcmd(0xbc); // Data Scan Direction
	lcd_75160_writedata(0x00); //0x00

	lcd_75160_setcmd(0xf0); // Display Mode

	lcd_75160_writedata(0x10); // Monochrome Mode  0x11 4 gray ; 0x10 Mono

	lcd_75160_setcmd(0x76); // Disable ICON RAM

	lcd_75160_setcmd(0x31); // Extension Command 2
	lcd_75160_setcmd(0x32); // Analog Circuit Set
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x01); // Booster Efficiency =6KHz
	lcd_75160_writedata(0x00); //Bias=1/14; 00-05h=1/duty=14/13/12/11/10/9

	lcd_75160_setcmd(0x20);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x0f);
	lcd_75160_writedata(0x0f);
	lcd_75160_writedata(0x0f);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x13); //1d
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x13);
	lcd_75160_writedata(0x13);
	lcd_75160_writedata(0x13);
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x00);

	lcd_75160_setcmd(0x51); // Booster Level x8
	lcd_75160_writedata(0xfb); //6x

	lcd_75160_setcmd(0x40); // Internal Power Supply
	lcd_75160_setcmd(0x49); //0x48

	lcd_75160_setcmd(0xF0); //SET FRAM IN DIFFERENT TEMPERATURE RANGE
	lcd_75160_writedata(0x02);
	lcd_75160_writedata(0x03);
	lcd_75160_writedata(0x0d);
	lcd_75160_writedata(0x1f);

	lcd_75160_setcmd(0xF2); //Frame Rate Temp. Range
	lcd_75160_writedata(0x07);
	lcd_75160_writedata(0x17);
	lcd_75160_writedata(0x6b);

	lcd_75160_setcmd(0xF4); //Frame Rate Temp. Range
	lcd_75160_writedata(0x5e);
	lcd_75160_writedata(0x20);
	lcd_75160_writedata(0x22);
	lcd_75160_writedata(0x21);
	lcd_75160_writedata(0x85);
	lcd_75160_writedata(0x09);
	lcd_75160_writedata(0x39);
	lcd_75160_writedata(0xff);

	lcd_75160_setcmd(0x39);
	lcd_75160_setcmd(0xF3); //Temp Hysteresis
	lcd_75160_writedata(0x01);

	lcd_75160_setcmd(0xF7); //
	lcd_75160_writedata(0x04);

	lcd_75160_setcmd(0x30);
	lcd_75160_setcmd(0x15); // Column Address Setting
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x9f); //160 seg

	lcd_75160_setcmd(0x75); // Page Address Setting
	lcd_75160_writedata(0x00);
	lcd_75160_writedata(0x20); //160 col

	lcd_75160_setcmd(0xaf); // Display ON
}

static void britch(unsigned char c)
{
	//unsigned char i=0;

	//if(lcd_invs)i = ~i;
	//lcdphy_dispbyte(i);
	lcd_75160_writedata(c);
}

static void disp_onebyte(unsigned char c)
{
	//britch((c>>6)&0x0f);
	lcd_75160_writedata(c);
}

static void display_lcdbuffer(int row_start, int rows)
{
	int i, j;
	unsigned char *pdata = lcd_dispbuffer[row_start];

	for (i = 0; i < rows; i++) {
		//lcdphy_setx(row_start+i);
		//lcdphy_sety(0);

		for (j = 0; j < LCD_MAX_COLE_BYTES; j++)
			disp_onebyte(*pdata++);
	}
}

static void refurbish_screen(unsigned int row_start, unsigned int row_end,
			     const unsigned char *buf)
{
	unsigned int row, col;

	if (row_start >= (LCD_ROW_SIZE / 8) || row_end > (LCD_ROW_SIZE / 8))
		return;

	for (row = row_start; row < row_end; row++) {
		lcd_75160_setcmd(0x75); // Page Address Setting
		lcd_75160_writedata(row);
		lcd_75160_writedata(row); //160 col
		lcd_75160_setcmd(0x30);
		lcd_75160_setcmd(0x5c);
		//for(col=0; col<(LCD_ROW_SIZE/8); col++)
		for (col = 0; col < (LCD_ROW_SIZE); col++) {
			//disp_onebyte(*buf++);
			lcd_75160_writedata(*buf++);
		}
	}
	//britch(0);
}

static int __maybe_unused write_single_lines(struct wr_slines_t *pwr,
			      unsigned char *lcd_buffer)
{
	unsigned short rowend;

	if (pwr->x >= LCD_ROW_SIZE)
		return 1;
	if (pwr->len == 0 || pwr->len > LCD_BUFFER_SIZE)
		return 1;
	if (copy_from_user(lcd_buffer, pwr->buffer, pwr->len))
		return 1;

	rowend = pwr->len / (LCD_COLUMN_SIZE / 8);
	rowend += pwr->x;

	refurbish_screen(pwr->x, rowend, lcd_buffer);

	return 0;
}

unsigned char mod_yinhe[320] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x3F,
	0x3F, 0x20, 0x00, 0x00, 0x3F, 0x3F, 0x1C, 0x0E, 0x07, 0x3F, 0x3F, 0x00,
	0x3F, 0x3F, 0x02, 0x02, 0x02, 0x3F, 0x3F, 0x00, 0x20, 0x3F, 0x3F, 0x22,
	0x27, 0x30, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	0x3F, 0x3F, 0x1C, 0x0E, 0x1C, 0x3F, 0x3F, 0x00, 0x20, 0x3F, 0x3F, 0x22,
	0x27, 0x30, 0x38, 0x00, 0x00, 0x38, 0x30, 0x3F, 0x3F, 0x30, 0x38, 0x00,
	0x20, 0x3F, 0x3F, 0x22, 0x27, 0x30, 0x38, 0x00, 0x20, 0x3F, 0x3F, 0x22,
	0x23, 0x3F, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xF0,
	0xF0, 0x10, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0xF0, 0x00,
	0xF0, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x10, 0xF0, 0xF0, 0x10,
	0x10, 0x30, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	0xF0, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0xF0, 0x00, 0x10, 0xF0, 0xF0, 0x10,
	0x10, 0x30, 0x70, 0x00, 0x00, 0x00, 0x10, 0xF0, 0xF0, 0x10, 0x00, 0x00,
	0x10, 0xF0, 0xF0, 0x10, 0x10, 0x30, 0x70, 0x00, 0x10, 0xF0, 0xF0, 0x00,
	0x00, 0xF0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

};
static void disp_boot(void)
{
	int i;
	for (i = 0; i < 3200; ++i) {
		lcd_showbuffer[i] = 0;
	}

	for (i = 0; i < 320; ++i) {
		lcd_showbuffer[1440 + i] = mod_yinhe[i];
	}
	refurbish_screen(0, 20, lcd_showbuffer);
}

static int lcd_open(struct inode *inode, struct file *file)
{
	printk(KERN_ALERT "open lcds driver ...\n");

	return 0;
}

static int lcd_close(struct inode *inode, struct file *file)
{
	return 0;
}
static ssize_t lcd_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	//printk(KERN_ALERT "write lcds buf ...\n");
	if ((count > 0xCD0))
		return -EFAULT;

	if (copy_from_user(lcd_showbuffer, buf, count)) {
		return -EFAULT;
	}

	refurbish_screen(0, 20, lcd_showbuffer);

	return count;
}
static ssize_t lcd_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	return 1;
}

static long lcd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ratio, tc;
	int mode;

	switch (cmd) {
	case LCD_CTRL_BL_ON:
		lcd_baklighton();
		break;
	case LCD_CTRL_BL_OFF:
		lcd_baklightoff();
		break;
	case LCD_CTRL_POWER_ON:
		lcd_poweron();
		break;
	case LCD_CTRL_POWER_OFF:
		lcd_poweroff();
		break;
	case LCD_CTRL_RESET:
		lcd_hw_rst();
		//lcd_cmd_reset();
		//lcd_dev_p->reset();
		lcd_75160_reset();
		break;
	case LCD_CTRL_READ_STATE: {
		unsigned char uc[16] = { 0x00, 0x00, 0x00, 0x00 };
		read_status(uc);
		if (copy_to_user((void __user *)arg, uc, 4))
			return -EFAULT;
	} break;
	case LCD_CTRL_SET_RADIO_MODE:
		mode = (int)arg;
		if (mode > 3)
			return -EFAULT;

		//lcdphy_set_ratio_mode(mode);
		break;
	case LCD_CTRL_SET_RADIO:
		ratio = (int)arg;
		if (ratio > 255)
			return -EFAULT;

		//lcd_75160_set_ratio_data(ratio);
		break;
	case LCD_CTRL_SET_TEMP:
		tc = (int)arg;
		if (tc > 3)
			return -EFAULT;

		//lcdphy_set_temp_compensation(tc);
		break;
	case 20: //write single lines
	{
		struct wr_slines_t wrconf;

		if (copy_from_user(&wrconf, (void __user *)arg, sizeof(wrconf)))
			return -EFAULT;

		//if(write_single_lines(&wrconf,lcd_showbuffer)) return -EFAULT;
	} break;
	default:
		return -EFAULT;
	}

	return 0;
}

static struct file_operations lcd_fops = {
	.owner = THIS_MODULE,
	.open = lcd_open,
	.read = lcd_read,
	.write = lcd_write,
	.unlocked_ioctl = lcd_ioctl,
	.release = lcd_close,
};

static int gener_device(const char *name)
{
	int ret = 0;

	ret = alloc_chrdev_region(&devno, 0, 1, name);
	if (ret) {
		printk("alloc_chrdev_region fail!\n");
		unregister_chrdev_region(devno, 1);
		return ret;
	}

	cdev_init(&cdev, &lcd_fops);
	cdev.owner = THIS_MODULE;

	ret = cdev_add(&cdev, devno, 1);
	if (ret) {
		printk("cdev add fail.\n");
		unregister_chrdev_region(devno, 1);
		return ret;
	}

	cdev_class = class_create(name);
	if (IS_ERR(cdev_class)) {
		printk("create class fail.\n");
		unregister_chrdev_region(devno, 1);
		cdev_del(&cdev);
	}

	device_create(cdev_class, NULL, devno, NULL, name);

	return 0;
}
static void destroy_device(void)
{
	device_destroy(cdev_class, devno);
	class_destroy(cdev_class);
	cdev_del(&cdev);
	unregister_chrdev_region(devno, 1);
}
// extern void gpioFree(unsigned int pin);
static int lcd_drv_probe(struct platform_device *pdev)
{
	int ret;
	int i;
	struct device_node *dev_node;
	const char *name;

	printk("probe device %s\n", dev_name(&pdev->dev));

	dev_node = pdev->dev.of_node;
	if (!dev_node || !of_device_is_compatible(dev_node, "lcd_drv")) {
		printk("failure to find lcd device node!\n");
		return -EINVAL;
	}

	name = of_get_property(dev_node, "drv_lcd-names", NULL) ?:
								  dev_node->name;
	if (name == NULL)
		name = MODULE_DEFAULT_NAME;

	ret = gener_device(name);
	if (ret)
		return ret;

	for (i = 0; i < IO_LCD_MAX; i++) {
		lcd_gpios[i] = devm_gpiod_get_index(&pdev->dev, "lcd", i,
						    GPIOD_OUT_LOW);
		if (IS_ERR(lcd_gpios[i])) {
			printk("failed to get lcd gpio %d: %ld\n", i,
			       PTR_ERR(lcd_gpios[i]));
			return PTR_ERR(lcd_gpios[i]);
		}
	}

	lcd_poweron();
	lcd_hw_rst();
	lcd_75160_reset();
	lcd_baklightoff();

	disp_boot();
	return 0;
}

static void lcd_drv_remove(struct platform_device *pdev)
{
	destroy_device();
}

static const struct of_device_id of_lcd_drv_match[] = {
	{
		.compatible = "lcd_drv",
	},
	{},
};
MODULE_DEVICE_TABLE(of, of_lcd_drv_match);

static struct platform_driver lcd_drv_driver = {
	.probe	= lcd_drv_probe,
	.remove	    = lcd_drv_remove,
	.driver	= {
		.name   = "lcd-drv",
		.owner  = THIS_MODULE,
		.of_match_table = of_lcd_drv_match,
	},
};

static int __init lcd_drv_module_init(void)
{
	int ret;
	ret = platform_driver_register(&lcd_drv_driver);
	if (ret < 0) {
		printk("lcd_drv_driver init error!\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit lcd_drv_module_exit(void)
{
	platform_driver_unregister(&lcd_drv_driver);
}

module_init(lcd_drv_module_init);

module_exit(lcd_drv_module_exit);

MODULE_LICENSE("GPL");
