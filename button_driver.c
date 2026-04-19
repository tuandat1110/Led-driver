#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/atomic.h>
#include "bbb_glue.h"

#define DRIVER_NAME     "bbb-button-driver"
#define DEVICE_NAME     "button"
#define DEBOUNCE_MS     200UL

struct button_dev {
    struct gpio_desc  *btn_gpiod;
    struct cdev        cdev;
    dev_t              devno;
    int                irq;
    int                index;
    unsigned long      last_jiffies;
    atomic_t           pressed;
    wait_queue_head_t  wait;
    atomic_t           event_ready;
};

static struct class      *button_class;
static dev_t              button_devno_base;
static struct button_dev *btn_devs[MAX_DEVICES];

extern void glue_handle_button_event(int idx);

static irqreturn_t button_isr(int irq, void *dev_id)
{
    struct button_dev *bdev = dev_id;
    unsigned long now = jiffies;
    int val;

    if (time_before(now, bdev->last_jiffies + msecs_to_jiffies(DEBOUNCE_MS)))
        return IRQ_HANDLED;

    bdev->last_jiffies = now;
    val = gpiod_get_value(bdev->btn_gpiod);

    if (val == 1) {
        pr_info("[BUTTON] BTN%d PRESSED\n", bdev->index);
        /* Dùng glue_handle_button_event thay vì glue_toggle_led
         * để tôn trọng mode hiện tại (TOGGLE / BLINK / OFF) */
        glue_handle_button_event(bdev->index);
    } else {
        pr_info("[BUTTON] BTN%d RELEASED\n", bdev->index);
    }

    atomic_set(&bdev->event_ready, 1);
    wake_up_interruptible(&bdev->wait);

    return IRQ_HANDLED;
}

static int button_open(struct inode *inode, struct file *filp)
{
    struct button_dev *bdev =
        container_of(inode->i_cdev, struct button_dev, cdev);
    filp->private_data = bdev;
    return 0;
}

static ssize_t button_read(struct file *filp, char __user *buf,
                           size_t count, loff_t *ppos)
{
    struct button_dev *bdev = filp->private_data;
    char tmp[3];
    int len, val;

    if (*ppos > 0)
        return 0;

    wait_event_interruptible(bdev->wait, atomic_read(&bdev->event_ready));
    atomic_set(&bdev->event_ready, 0);

    val = gpiod_get_value(bdev->btn_gpiod);
    len = snprintf(tmp, sizeof(tmp), "%d\n", val);

    if (copy_to_user(buf, tmp, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open  = button_open,
    .read  = button_read,
};

static int button_probe(struct platform_device *pdev)
{
    int i, ret, num;

    num = gpiod_count(&pdev->dev, "button");
    if (num <= 0 || num > MAX_DEVICES) {
        dev_err(&pdev->dev, "Invalid button-gpios count: %d\n", num);
        return -EINVAL;
    }

    ret = alloc_chrdev_region(&button_devno_base, 0, num, DEVICE_NAME);
    if (ret) {
        dev_err(&pdev->dev, "alloc_chrdev_region failed\n");
        return ret;
    }

    for (i = 0; i < num; i++) {
        btn_devs[i] = devm_kzalloc(&pdev->dev,
                                   sizeof(*btn_devs[i]), GFP_KERNEL);
        if (!btn_devs[i]) {
            ret = -ENOMEM;
            goto err_chrdev;
        }

        btn_devs[i]->index        = i;
        btn_devs[i]->devno        = MKDEV(MAJOR(button_devno_base), i);
        btn_devs[i]->last_jiffies = 0;

        init_waitqueue_head(&btn_devs[i]->wait);
        atomic_set(&btn_devs[i]->event_ready, 0);
        atomic_set(&btn_devs[i]->pressed, 0);

        btn_devs[i]->btn_gpiod =
            devm_gpiod_get_index(&pdev->dev, "button", i, GPIOD_IN);
        if (IS_ERR(btn_devs[i]->btn_gpiod)) {
            dev_err(&pdev->dev, "Failed to get button%d gpio\n", i);
            ret = PTR_ERR(btn_devs[i]->btn_gpiod);
            goto err_chrdev;
        }

        btn_devs[i]->irq = gpiod_to_irq(btn_devs[i]->btn_gpiod);
        if (btn_devs[i]->irq < 0) {
            dev_err(&pdev->dev, "Failed to get IRQ for button%d\n", i);
            ret = btn_devs[i]->irq;
            goto err_chrdev;
        }

        ret = devm_request_irq(&pdev->dev,
                               btn_devs[i]->irq,
                               button_isr,
                               IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                               "btn_irq",
                               btn_devs[i]);
        if (ret) {
            dev_err(&pdev->dev, "Failed to request IRQ for button%d\n", i);
            goto err_chrdev;
        }

        cdev_init(&btn_devs[i]->cdev, &fops);
        ret = cdev_add(&btn_devs[i]->cdev, btn_devs[i]->devno, 1);
        if (ret) {
            dev_err(&pdev->dev, "cdev_add failed for button%d\n", i);
            goto err_chrdev;
        }

        device_create(button_class, NULL,
                      btn_devs[i]->devno, NULL, "button%d", i);

        dev_info(&pdev->dev, "Button%d ready (irq=%d)\n",
                 i, btn_devs[i]->irq);
    }

    dev_info(&pdev->dev, "Button driver loaded (%d buttons)\n", num);
    return 0;

err_chrdev:
    unregister_chrdev_region(button_devno_base, num);
    return ret;
}

static void button_remove(struct platform_device *pdev)
{
    int i;

    for (i = 0; i < MAX_DEVICES; i++) {
        if (!btn_devs[i])
            continue;
        device_destroy(button_class, btn_devs[i]->devno);
        cdev_del(&btn_devs[i]->cdev);
    }

    unregister_chrdev_region(button_devno_base, MAX_DEVICES);
    dev_info(&pdev->dev, "Button driver removed\n");
}

static const struct of_device_id match[] = {
    { .compatible = "bbb,button-driver" },
    {}
};
MODULE_DEVICE_TABLE(of, match);

static struct platform_driver drv = {
    .probe  = button_probe,
    .remove = button_remove,
    .driver = {
        .name           = DRIVER_NAME,
        .of_match_table = match,
    },
};

static int __init init(void)
{
    button_class = class_create("button_class");
    if (IS_ERR(button_class))
        return PTR_ERR(button_class);

    return platform_driver_register(&drv);
}

static void __exit exit_f(void)
{
    platform_driver_unregister(&drv);
    class_destroy(button_class);
    pr_info("[BUTTON] Module unloaded\n");
}

module_init(init);
module_exit(exit_f);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dat");
MODULE_DESCRIPTION("BBB Button Driver");