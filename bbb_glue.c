#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>

#include "bbb_glue.h"
#include "bbb_glue_ioctl.h"

#define GLUE_DEVICE_NAME "bbb_glue"
#define BLINK_DELAY msecs_to_jiffies(500)

struct glue_context {
    enum bbb_mode modes[MAX_DEVICES];
    struct timer_list blink_timers[MAX_DEVICES];
    int blink_state[MAX_DEVICES];
    struct mutex lock;
    dev_t devno;
    struct cdev cdev;
    struct class *class;
};

static struct glue_context *g_ctx;

void glue_register_led(int led_index, struct gpio_desc *gpio)
{
    if (led_index < 0 || led_index >= MAX_DEVICES)
        return;
    if (!g_ctx) {
        pr_warn("[GLUE] glue_register_led called before module init\n");
        return;
    }

    mutex_lock(&g_ctx->lock);
    led_gpios[led_index] = gpio;
    mutex_unlock(&g_ctx->lock);

    pr_info("[GLUE] LED%d registered\n", led_index);
}
EXPORT_SYMBOL_GPL(glue_register_led);

void glue_unregister_led(int led_index)
{
    if (led_index < 0 || led_index >= MAX_DEVICES)
        return;
    if (!g_ctx) {
        pr_warn("[GLUE] glue_unregister_led called before module init\n");
        return;
    }

    /* Stop blink timer before clearing GPIO — use timer_delete_sync
     * to guarantee the callback has finished before we NULL the pointer */
    timer_delete_sync(&g_ctx->blink_timers[led_index]);

    mutex_lock(&g_ctx->lock);
    led_gpios[led_index] = NULL;
    g_ctx->blink_state[led_index] = 0;
    mutex_unlock(&g_ctx->lock);

    pr_info("[GLUE] LED%d unregistered\n", led_index);
}
EXPORT_SYMBOL_GPL(glue_unregister_led);


static void blink_timer_callback(struct timer_list *t)
{
    /* Pointer arithmetic to recover index — safe because blink_timers
     * is a plain array inside g_ctx (no heap allocation per timer) */
    int idx = t - g_ctx->blink_timers;
    struct gpio_desc *gpio;

    if (idx < 0 || idx >= MAX_DEVICES)
        return;

    mutex_lock(&g_ctx->lock);
    gpio = led_gpios[idx];

    if (!gpio || g_ctx->modes[idx] != MODE_BLINK) {
        mutex_unlock(&g_ctx->lock);
        return;
    }

    g_ctx->blink_state[idx] = !g_ctx->blink_state[idx];
    gpiod_set_value(gpio, g_ctx->blink_state[idx]);
    mutex_unlock(&g_ctx->lock);

    /* Reschedule — only if still in BLINK mode */
    mod_timer(&g_ctx->blink_timers[idx], jiffies + BLINK_DELAY);
}

void glue_handle_button_event(int idx)
{
    struct gpio_desc *gpio;
    enum bbb_mode mode;

    if (idx < 0 || idx >= MAX_DEVICES || !g_ctx)
        return;

    mutex_lock(&g_ctx->lock);
    gpio = led_gpios[idx];
    mode = g_ctx->modes[idx];
    mutex_unlock(&g_ctx->lock);

    if (!gpio) {
        pr_warn("[GLUE] Button%d event ignored: LED not registered\n", idx);
        return;
    }

    switch (mode) {
    case MODE_TOGGLE:
        gpiod_set_value(gpio, !gpiod_get_value(gpio));
        pr_info("[GLUE] Button%d: LED%d toggled\n", idx, idx);
        break;

    case MODE_BLINK:
        if (timer_pending(&g_ctx->blink_timers[idx])) {
            /* Stop blink — timer_delete_sync waits for any running callback */
            timer_delete_sync(&g_ctx->blink_timers[idx]);
            mutex_lock(&g_ctx->lock);
            gpiod_set_value(gpio, 0);
            g_ctx->blink_state[idx] = 0;
            mutex_unlock(&g_ctx->lock);
            pr_info("[GLUE] Button%d: LED%d blink stopped\n", idx, idx);
        } else {
            mod_timer(&g_ctx->blink_timers[idx], jiffies + BLINK_DELAY);
            pr_info("[GLUE] Button%d: LED%d blink started\n", idx, idx);
        }
        break;

    case MODE_OFF:
        timer_delete_sync(&g_ctx->blink_timers[idx]);
        mutex_lock(&g_ctx->lock);
        gpiod_set_value(gpio, 0);
        g_ctx->blink_state[idx] = 0;
        mutex_unlock(&g_ctx->lock);
        pr_info("[GLUE] Button%d: LED%d forced OFF\n", idx, idx);
        break;

    default:
        pr_info("[GLUE] Button%d: no action for mode %d\n", idx, mode);
        break;
    }
}
EXPORT_SYMBOL_GPL(glue_handle_button_event);

static long glue_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct bbb_ioctl_data data;
    struct gpio_desc *gpio;

    if (_IOC_TYPE(cmd) != BBB_IOC_MAGIC)
        return -ENOTTY;
    if (cmd != BBB_IOC_SET_MODE)
        return -ENOTTY;

    if (copy_from_user(&data, (struct bbb_ioctl_data __user *)arg, sizeof(data)))
        return -EFAULT;

    if (data.index < 0 || data.index >= MAX_DEVICES)
        return -EINVAL;

    /* Stop any running blink timer before changing mode.
     * Must be outside the mutex to avoid deadlock with the callback. */
    timer_delete_sync(&g_ctx->blink_timers[data.index]);

    mutex_lock(&g_ctx->lock);
    g_ctx->modes[data.index] = data.mode;
    gpio = led_gpios[data.index];

    switch (data.mode) {
        case MODE_BLINK:
            g_ctx->blink_state[data.index] = 0;
            mutex_unlock(&g_ctx->lock);
            mod_timer(&g_ctx->blink_timers[data.index], jiffies + BLINK_DELAY);
            break;

        case MODE_OFF:
            if (gpio) {
                gpiod_set_value(gpio, 0);
                g_ctx->blink_state[data.index] = 0;
            }
            mutex_unlock(&g_ctx->lock);
            break;

        default: /* MODE_TOGGLE and anything else */
            mutex_unlock(&g_ctx->lock);
            break;
    }

    pr_info("[GLUE] IOCTL: LED%d set to mode %d\n", data.index, data.mode);
    return 0;
}

static const struct file_operations glue_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = glue_ioctl,
};

static int __init bbb_glue_init(void)
{
    int i, ret;

    for (i = 0; i < MAX_DEVICES; i++)
        led_gpios[i] = NULL;

    g_ctx = kzalloc(sizeof(*g_ctx), GFP_KERNEL);
    if (!g_ctx)
        return -ENOMEM;

    mutex_init(&g_ctx->lock);

    ret = alloc_chrdev_region(&g_ctx->devno, 0, 1, GLUE_DEVICE_NAME);
    if (ret < 0) {
        pr_err("[GLUE] alloc_chrdev_region failed: %d\n", ret);
        goto err_free;
    }

    g_ctx->class = class_create(GLUE_DEVICE_NAME);
    if (IS_ERR(g_ctx->class)) {
        ret = PTR_ERR(g_ctx->class);
        pr_err("[GLUE] class_create failed: %d\n", ret);
        goto err_unregister;
    }

    cdev_init(&g_ctx->cdev, &glue_fops);
    ret = cdev_add(&g_ctx->cdev, g_ctx->devno, 1);
    if (ret < 0) {
        pr_err("[GLUE] cdev_add failed: %d\n", ret);
        goto err_class;
    }

    if (IS_ERR(device_create(g_ctx->class, NULL, g_ctx->devno,
                             NULL, GLUE_DEVICE_NAME))) {
        ret = -ENOMEM;
        pr_err("[GLUE] device_create failed\n");
        goto err_cdev;
    }

    for (i = 0; i < MAX_DEVICES; i++) {
        timer_setup(&g_ctx->blink_timers[i], blink_timer_callback, 0);
        g_ctx->modes[i] = MODE_TOGGLE;
    }

    pr_info("[GLUE] Module loaded at /dev/%s\n", GLUE_DEVICE_NAME);
    return 0;

err_cdev:
    cdev_del(&g_ctx->cdev);
err_class:
    class_destroy(g_ctx->class);
err_unregister:
    unregister_chrdev_region(g_ctx->devno, 1);
err_free:
    kfree(g_ctx);
    return ret;
}

static void __exit bbb_glue_exit(void)
{
    int i;

    /* timer_delete_sync ensures no callback runs after this loop */
    for (i = 0; i < MAX_DEVICES; i++)
        timer_delete_sync(&g_ctx->blink_timers[i]);

    device_destroy(g_ctx->class, g_ctx->devno);
    class_destroy(g_ctx->class);
    cdev_del(&g_ctx->cdev);
    unregister_chrdev_region(g_ctx->devno, 1);
    kfree(g_ctx);
    pr_info("[GLUE] Module unloaded\n");
}

module_init(bbb_glue_init);
module_exit(bbb_glue_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tuan Dat");
MODULE_DESCRIPTION("Glue module: connects button and LED drivers via ioctl");