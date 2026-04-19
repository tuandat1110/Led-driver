#include "linux/device/devres.h"
#include "linux/gfp_types.h"
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#define DRIVER_NAME     "bbb-led-driver"
#define MAX_LEDS        2
#define DEVICE_NAME     "led"

/* Khai báo extern symbol từ button_driver */
extern void glue_register_led(int led_index, struct gpio_desc *gpio);
extern void glue_unregister_led(int led_index);

struct led_dev {
    struct gpio_desc *gpios[MAX_LEDS];
    int               num_leds;
    struct cdev       cdev;
    dev_t             devno;
    struct mutex      lock;
};

static struct class   *led_class;
///static dev_t           led_devno_base;
static struct led_dev *global_ldev;
//static struct led_dev *led_devs[MAX_LEDS];

/* ================= FILE OPS ================= */
static int led_open(struct inode *inode, struct file *filp)
{
    filp->private_data = container_of(inode->i_cdev, struct led_dev, cdev);
    return 0;
}

static int led_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t led_write(struct file *filp, const char __user *buf, size_t count, loff_t *ppos)
{
    struct led_dev *ldev = filp->private_data;
    char kbuf[16];
    int led_idx, val;
    size_t copy_len = min(count, sizeof(kbuf) - 1);
    if (copy_from_user(kbuf, buf, copy_len))
        return -EFAULT;

    kbuf[copy_len] = '\0';

    if (sscanf(kbuf, "%d:%d", &led_idx, &val) != 2) {
        pr_warn(DRIVER_NAME "[LED] Invalid format. Use 'index:value' (e.g., '0:1')\n");
        return -EINVAL;
    }

    if (led_idx < 0 || led_idx >= ldev->num_leds) {
        pr_err(DRIVER_NAME "[LED] LED index %d out of range\n", led_idx);
        return -EINVAL;
    }
    mutex_lock(&ldev->lock);
    gpiod_set_value(ldev->gpios[led_idx], val ? 1 : 0);
    mutex_unlock(&ldev->lock);

    pr_info(DRIVER_NAME ": LED %d set to %s\n", led_idx, val ? "ON" : "OFF");

    return count;
}

static ssize_t led_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    struct led_dev *ldev = filp->private_data;
    char kbuf[64]; 
    int len = 0;
    int i;

    if (*ppos > 0)
        return 0;

    mutex_lock(&ldev->lock);

    for (i = 0; i < ldev->num_leds; i++) {
        int val = gpiod_get_value(ldev->gpios[i]);
        
        len += snprintf(kbuf + len, sizeof(kbuf) - len, "LED%d: %d\n", i, val);
    }

    mutex_unlock(&ldev->lock);

    if (count < len) {
        pr_warn(DRIVER_NAME ": User buffer too small for status report\n");
        return -EINVAL;
    }

    if (copy_to_user(buf, kbuf, len)) {
        pr_err(DRIVER_NAME ": Failed to copy data to user space\n");
        return -EFAULT;
    }

    *ppos = len;

    pr_info(DRIVER_NAME ": Status report generated (%d bytes)\n", len);
    return len;
}

static const struct file_operations led_fops = {
    .owner   = THIS_MODULE,
    .open    = led_open,
    .release = led_release,
    .read    = led_read,
    .write   = led_write,
};

static int led_probe(struct platform_device *pdev)
{
    int i, ret, num_leds;
    void (*reg_fn)(int, struct gpio_desc *);

    num_leds = gpiod_count(&pdev->dev, "led");
    if (num_leds <= 0 || num_leds > MAX_LEDS) {
        dev_err(&pdev->dev, "[LED] Invalid number of LEDs: %d\n", num_leds);
        return -EINVAL;
    }

    global_ldev = devm_kzalloc(&pdev->dev, sizeof(*global_ldev), GFP_KERNEL);
    if (!global_ldev)
        return -ENOMEM;

    global_ldev->num_leds = num_leds;

    mutex_init(&global_ldev->lock);

    ret = alloc_chrdev_region(&global_ldev->devno, 0, 1, DEVICE_NAME);

    if (ret < 0) {
        dev_err(&pdev->dev, "[LED] Failed to allocate chrdev region\n");
        return ret;
    }

    for (int i = 0; i < num_leds; i++) {
        global_ldev->gpios[i] = devm_gpiod_get_index(&pdev->dev, "led", i, GPIOD_OUT_LOW);
        if (IS_ERR(global_ldev->gpios[i])) {
            ret = PTR_ERR(global_ldev->gpios[i]);
            goto err_unregister;
        }

        reg_fn = symbol_get(glue_register_led);
        if (reg_fn) {
            reg_fn(i, global_ldev->gpios[i]);
            symbol_put(glue_register_led);
            dev_info(&pdev->dev, "[LED] LED %d linked to button_driver\n", i);
        }
    }

    cdev_init(&global_ldev->cdev, &led_fops);
    global_ldev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&global_ldev->cdev, global_ldev->devno, 1);
    if (ret < 0) {
        dev_err(&pdev->dev, "LED] Failed to add cdev\n");
        goto err_unregister;
    }
    if (IS_ERR(device_create(led_class, &pdev->dev, global_ldev->devno, NULL, DEVICE_NAME))) {
        dev_err(&pdev->dev, "Failed to create device file\n");
        ret = -ENOMEM;
        goto err_cdev_del;
    }

    dev_info(&pdev->dev, "Driver initialized. Control via /dev/%s\n", DEVICE_NAME);
    return 0;
err_cdev_del:
    cdev_del(&global_ldev->cdev);
err_unregister:
    unregister_chrdev_region(global_ldev->devno, 1);
    return ret;
}

static void led_remove(struct platform_device *pdev)
{
    int i;
    void (*unreg_fn)(int);

    for (i = 0; i < global_ldev->num_leds; i++) {
        unreg_fn = symbol_get(glue_unregister_led);
        if (unreg_fn) {
            unreg_fn(i);
            symbol_put(glue_unregister_led);
        }
    }

    device_destroy(led_class, global_ldev->devno);
    cdev_del(&global_ldev->cdev);
    unregister_chrdev_region(global_ldev->devno, 1);
    
    dev_info(&pdev->dev, "Driver removed successfully\n");
}

/* ================= MATCH ================= */
static const struct of_device_id led_of_match[] = {
    { .compatible = "bbb,led-driver" },
    { }
};
MODULE_DEVICE_TABLE(of, led_of_match);

static struct platform_driver led_platform_driver = {
    .probe  = led_probe,
    .remove = led_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = led_of_match,
    },
};

/* ================= INIT ================= */
static int __init led_driver_init(void)
{
    led_class = class_create("led_driver");
    if (IS_ERR(led_class))
        return PTR_ERR(led_class);

    return platform_driver_register(&led_platform_driver);
}

static void __exit led_driver_exit(void)
{
    platform_driver_unregister(&led_platform_driver);
    class_destroy(led_class);
}

module_init(led_driver_init);
module_exit(led_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dat");
MODULE_DESCRIPTION("BBB LED Driver - GPIO + userspace + button toggle");
MODULE_VERSION("2.0");