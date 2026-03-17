#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>

LOG_MODULE_REGISTER(pinnacle, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT cirque_pinnacle

struct pinnacle_config {
    struct spi_dt_spec bus;
};

struct pinnacle_data {
    struct k_timer timer;
    struct k_work work;
    const struct device *dev;
};

/* Die Abfrage-Logik (läuft jetzt per Timer) */
static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    const struct pinnacle_config *config = data->dev->config;

    uint8_t read_cmd = 0xFC; 
    uint8_t report[4] = {0}; 
    
    struct spi_buf tx_buf = { .buf = &read_cmd, .len = 1 };
    struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf rx_buf[] = {
        { .buf = NULL, .len = 1 }, 
        { .buf = report, .len = sizeof(report) }
    };
    struct spi_buf_set rx = { .buffers = rx_buf, .count = 2 };

    if (spi_transceive_dt(&config->bus, &tx, &rx) == 0) {
        int8_t x = (int8_t)report[1];
        int8_t y = (int8_t)report[2];
        
        // Wir filtern nur Null-Werte, um den Funk nicht zu fluten
        if (x != 0 || y != 0) {
            input_report_rel(data->dev, INPUT_REL_X, x, false, K_FOREVER);
            input_report_rel(data->dev, INPUT_REL_Y, -y, true, K_FOREVER);
        }
    }
}

/* Timer-Callback: Schiebt die Arbeit in die Queue */
static void pinnacle_timer_handler(struct k_timer *dummy) {
    struct pinnacle_data *data = CONTAINER_OF(dummy, struct pinnacle_data, timer);
    k_work_submit(&data->work);
}

static int pinnacle_init(const struct device *dev) {
    const struct pinnacle_config *config = dev->config;
    struct pinnacle_data *data = dev->data;
    data->dev = dev;

    if (!spi_is_ready_dt(&config->bus)) return -ENODEV;

    k_work_init(&data->work, pinnacle_work_handler);
    k_timer_init(&data->timer, pinnacle_timer_handler, NULL);

    /* Trackpad aktivieren */
    uint8_t init_cmd[] = { 0x04, 0x01 }; 
    struct spi_buf_set init_tx = { .buffers = &(struct spi_buf){ .buf = init_cmd, .len = 2 }, .count = 1 };
    spi_write_dt(&config->bus, &init_tx);

    /* Starte die Abfrage alle 10ms */
    k_timer_start(&data->timer, K_MSEC(10), K_MSEC(10));

    return 0;
}

#define PINNACLE_INST(n) \
    static struct pinnacle_data pinnacle_data_##n; \
    static const struct pinnacle_config pinnacle_config_##n = { \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8), 0), \
    }; \
    DEVICE_DT_INST_DEFINE(n, pinnacle_init, NULL, \
                         &pinnacle_data_##n, &pinnacle_config_##n, \
                         APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PINNACLE_INST)
