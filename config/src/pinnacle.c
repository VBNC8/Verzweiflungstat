#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

// Wir holen uns die ID direkt aus dem DeviceTree Label
#define TRACKPAD_NODE DT_NODELABEL(trackpad)

static int pinnacle_init(const struct device *dev) {
    return 0;
}

// Hier nutzen wir jetzt die korrekte DT-Instanz
DEVICE_DT_DEFINE(TRACKPAD_NODE, pinnacle_init, NULL, NULL, NULL, POST_KERNEL, 90, NULL);
