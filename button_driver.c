#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include "bbb_glue.h"

#define DRIVER_NAME "bbb-button-driver"
#define DEVICE_NAME "all_buttons"
#define DEBOUNCE_MS 200UL

struct button_info {
    struct gpio_desc *gpiod;
    int irq;
    int index;
    unsigned long last_jiffies;
};

struct buttons_priv {
    struct cdev cdev;
    dev_t devno;
    struct button_info *btns;
    int num_btns;
    wait_queue_head_t wait;
    atomic_t event_ready;
};

static struct buttons_priv *priv_data;
static struct class *button_class;

extern void glue_handle_button_event(int idx);

//ham ngat xu ly khi nut duoc bam
static irqreturn_t button_isr(int irq, void *dev_id)
{
    struct button_info *bi = dev_id;
    unsigned long now = jiffies;
    int val;
    // dung debouncing tranh user nhan nut qua nhanh
    if (time_before(now, bi->last_jiffies + msecs_to_jiffies(DEBOUNCE_MS)))
        return IRQ_HANDLED;

    bi->last_jiffies = now;
    val = gpiod_get_value(bi->gpiod);

    if (val == 1) {
        pr_info("[BUTTON] BTN%d PRESSED\n", bi->index);
        glue_handle_button_event(bi->index);    //nut duoc bam nhay vao ham nay xu ly
    } else {
        pr_info("[BUTTON] BTN%d RELEASED\n", bi->index);
    }
    // set co event ready = 1
    atomic_set(&priv_data->event_ready, 1);
    wake_up_interruptible(&priv_data->wait);

    return IRQ_HANDLED;
}

static int button_open(struct inode *inode, struct file *filp)
{
    filp->private_data = priv_data;
    return 0;
}
// ham read doc trang thai cac button (vi du: 01 ,nghia la button0 dang mo, button1 dang nhan)
static ssize_t button_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
    struct buttons_priv *priv = filp->private_data;
    char *kbuf;
    int i, len;

    if (*ppos > 0)
        return 0;

    // cho cho nut nhan duoc nhan/nha
    if (wait_event_interruptible(priv->wait, atomic_read(&priv->event_ready)))
        return -ERESTARTSYS;

    atomic_set(&priv->event_ready, 0);
    kbuf = kmalloc(priv->num_btns + 2, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    for (i = 0; i < priv->num_btns; i++) {
        kbuf[i] = gpiod_get_value(priv->btns[i].gpiod) ? '1' : '0';
    }
    kbuf[priv->num_btns] = '\n';
    kbuf[priv->num_btns + 1] = '\0';
    
    len = priv->num_btns + 1;
    if (copy_to_user(buf, kbuf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }

    kfree(kbuf);
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
        dev_err(&pdev->dev, "Invalid button count: %d\n", num);
        return -EINVAL;
    }

    priv_data = devm_kzalloc(&pdev->dev, sizeof(*priv_data), GFP_KERNEL);
    if (!priv_data) return -ENOMEM;

    priv_data->btns = devm_kcalloc(&pdev->dev, num, sizeof(struct button_info), GFP_KERNEL);
    if (!priv_data->btns) return -ENOMEM;

    priv_data->num_btns = num;
    init_waitqueue_head(&priv_data->wait);
    atomic_set(&priv_data->event_ready, 0);

    /* 2. Đăng ký Device Number (Chỉ 1 cái duy nhất) */
    ret = alloc_chrdev_region(&priv_data->devno, 0, 1, DEVICE_NAME);
    if (ret) return ret;

    /* 3. Cấu hình từng GPIO và IRQ */
    for (i = 0; i < num; i++) {
        priv_data->btns[i].index = i;
        priv_data->btns[i].gpiod = devm_gpiod_get_index(&pdev->dev, "button", i, GPIOD_IN);
        
        if (IS_ERR(priv_data->btns[i].gpiod)) {
            ret = PTR_ERR(priv_data->btns[i].gpiod);
            goto err_unregister;
        }

        priv_data->btns[i].irq = gpiod_to_irq(priv_data->btns[i].gpiod);
        ret = devm_request_irq(&pdev->dev, priv_data->btns[i].irq, button_isr,
                               IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                               "btn_irq", &priv_data->btns[i]);
        if (ret) goto err_unregister;
    }

    /* 4. Đăng ký Character Device và Device File */
    cdev_init(&priv_data->cdev, &fops);
    ret = cdev_add(&priv_data->cdev, priv_data->devno, 1);
    if (ret) goto err_unregister;

    device_create(button_class, NULL, priv_data->devno, NULL, DEVICE_NAME);

    dev_info(&pdev->dev, "Driver loaded: 1 device file for %d buttons\n", num);
    return 0;

err_unregister:
    unregister_chrdev_region(priv_data->devno, 1);
    return ret;
}

static void button_remove(struct platform_device *pdev)
{
    device_destroy(button_class, priv_data->devno);
    cdev_del(&priv_data->cdev);
    unregister_chrdev_region(priv_data->devno, 1);
    dev_info(&pdev->dev, "Button driver removed\n");
}

static const struct of_device_id match[] = {
    { .compatible = "bbb,button-driver" },
    {}
};
MODULE_DEVICE_TABLE(of, match);

static struct platform_driver drv = {
    .probe = button_probe,
    .remove = button_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = match,
    },
};

static int __init button_init(void)
{
    button_class = class_create("button_class");
    if (IS_ERR(button_class))
        return PTR_ERR(button_class);

    return platform_driver_register(&drv);
}

static void __exit button_exit(void)
{
    platform_driver_unregister(&drv);
    class_destroy(button_class);
}

module_init(button_init);
module_exit(button_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dat");
MODULE_DESCRIPTION("BBB Button Driver - Unified Device File");