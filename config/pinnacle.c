#include <zephyr/types.h>
#include <drivers/spi.h>
#include <drivers/gpio.h>
#include <device.h>

// Grundbefehle für den Cirque Pinnacle
#define PINNACLE_READ 0xA0
#define PINNACLE_WRITE 0x80

void pinnacle_init_custom(const struct device *spi_dev) {
    // Hier würde die Initialisierung der Register stehen
    // Da wir aber erst mal schauen wollen, ob es baut, 
    // lassen wir es minimalistisch.
}
