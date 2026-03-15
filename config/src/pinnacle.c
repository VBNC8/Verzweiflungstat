#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

// Wir prüfen, ob das Label "trackpad" überhaupt existiert
#define HAS_TRACKPAD DT_NODE_EXISTS(DT_NODELABEL(trackpad))

static int pinnacle_init(const struct device *dev) {
    return 0;
}

#if HAS_TRACKPAD
// Nur wenn das Trackpad im DeviceTree (rechte Seite) gefunden wird, 
// wird dieser Teil mitkompiliert.
DEVICE_DT_DEFINE(DT_NODELABEL(trackpad), pinnacle_init, NULL, NULL, NULL, POST_KERNEL, 90, NULL);
#endif
