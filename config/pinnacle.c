#include <zephyr/types.h>
#include <drivers/spi.h>
#include <device.h>
#include <init.h>

// Wir definieren unser Gerät, damit Zephyr es beim Start "sieht"
static int pinnacle_init(const struct device *dev) {
    // Hier wird später die SPI-Kommunikation gestartet
    // Für den ersten grünen Build reicht ein "Return 0"
    return 0;
}

// Das hier ist die Magie: Es meldet den Treiber beim Systemstart an
SYS_INIT(pinnacle_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
