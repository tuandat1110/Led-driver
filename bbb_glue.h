#ifndef BBB_GLUE_H
#define BBB_GLUE_H

#include <linux/gpio/consumer.h>
#define DRIVER_NAME "bbb-glue"
#define MAX_DEVICES 2
struct gpio_desc *led_gpios[MAX_DEVICES];

void glue_register_led(int idx, struct gpio_desc *gpio);
void glue_unregister_led(int idx);
struct gpio_desc *glue_get_led_gpio(int idx);
void glue_toggle_led(int idx);

#endif