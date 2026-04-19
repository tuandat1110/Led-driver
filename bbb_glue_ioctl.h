#ifndef BBB_GLUE_IOCTL_H
#define BBB_GLUE_IOCTL_H

#include <linux/ioctl.h>

enum bbb_mode {
    MODE_TOGGLE = 0,
    MODE_BLINK,
    MODE_OFF
};

struct bbb_ioctl_data {
    int index;
    enum bbb_mode mode;
};

#define BBB_IOC_MAGIC       'G'
#define BBB_IOC_SET_MODE    _IOW(BBB_IOC_MAGIC, 1, struct bbb_ioctl_data)

#endif