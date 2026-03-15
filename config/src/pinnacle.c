// Minimaler Dummy-Treiber
#include <zephyr/kernel.h>
#include <zephyr/device.h>

static int pinnacle_init(const struct device *dev) { return 0; }
DEVICE_DT_DEFINE(ANY, pinnacle_init, NULL, NULL, NULL, POST_KERNEL, 90, NULL);
