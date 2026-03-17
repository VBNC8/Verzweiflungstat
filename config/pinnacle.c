#include <zephyr/types.h>
#include <drivers/spi.h>
#include <device.h>
#include <init.h>
#include <logging/log.h>

LOG_MODULE_REGISTER(pinnacle_custom, LOG_LEVEL_INF);

#define PINNACLE_ADDR_HW_ID 0x00
#define PINNACLE_READ 0xA0

static int pinnacle_init(const struct device *dev) {
    // Wir suchen uns das SPI-Gerät "SPI_3" aus dem Devicetree
    const struct device *spi_dev = device_get_binding("SPI_3");
    
    if (!spi_dev) {
        // Wenn das hier im Log erscheint, stimmt der Name "SPI_3" nicht
        return -ENODEV;
    }

    struct spi_config config = {
        .frequency = 1000000,
        .operation = SPI_WORD_SET(8) | SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB,
        .slave = 0,
    };

    // Test-Read: Wir wollen wissen, wer da am anderen Ende der Leitung ist
    uint8_t tx_buf[2] = { PINNACLE_READ | PINNACLE_ADDR_HW_ID, 0xFC }; 
    uint8_t rx_buf[2] = { 0 };

    struct spi_buf tx = { .buf = tx_buf, .len = sizeof(tx_buf) };
    struct spi_buf rx = { .buf = rx_buf, .len = sizeof(rx_buf) };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

    int ret = spi_transceive(spi_dev, &config, &tx_set, &rx_set);

    if (ret == 0) {
        // Erfolg! Das Trackpad hat geantwortet.
        // Die HW_ID sollte bei Cirque normalerweise 0x02 sein.
    }

    return 0;
}

SYS_INIT(pinnacle_init, APPLICATION, 90);
