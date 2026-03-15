#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pinnacle, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/devicetree.h>

#define DT_DRV_COMPAT cirque_pinnacle

struct pinnacle_config {
    struct spi_dt_spec bus;
    struct gpio_dt_spec dr_gpio;
};

struct pinnacle_data {
    const struct device *dev;
    struct gpio_callback gpio_cb;
    struct k_work work;
};

static int pinnacle_read(const struct device *dev) {
    const struct pinnacle_config *config = dev->config;
    uint8_t buffer[6];
    uint8_t cmd = 0xFC; // Read command for Pinnacle

    struct spi_buf tx_buf = {.buf = &cmd, .len = 1};
    // Hier wurde "bufs" zu "buffers" geändert:
    struct spi_buf_set tx_bufs = {.buffers = &tx_buf, .count = 1};
    
    struct spi_buf rx_buf = {.buf = buffer, .len = 6};
    // Und hier ebenfalls:
    struct spi_buf_set rx_bufs = {.buffers = &rx_buf, .count = 1};

    return spi_transceive_dt(&config->bus, &tx_bufs, &rx_bufs);
}

static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    pinnacle_read(data->dev);
    // Hier würde normalerweise die ZMK Mouse Report Logik folgen
}

static void pinnacle_gpio_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    struct pinnacle_data *data = CONTAINER_OF(cb, struct pinnacle_data, gpio_cb);
    k_work_submit(&data->work);
}

static int pinnacle_init(const struct device *dev) {
    const struct pinnacle_config *config = dev->config;
    struct pinnacle_data *data = dev->data;

    data->dev = dev;
    k_work_init(&data->work, pinnacle_work_handler);

    if (!spi_is_ready_dt(&config->bus)) return -ENODEV;

    if (config->dr_gpio.port) {
        gpio_pin_configure_dt(&config->dr_gpio, GPIO_INPUT);
        gpio_init_callback(&data->gpio_cb, pinnacle_gpio_callback, BIT(config->dr_gpio.pin));
        gpio_add_callback(config->dr_gpio.port, &data->gpio_cb);
        gpio_pin_interrupt_configure_dt(&config->dr_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    }

    return 0;
}

#define PINNACLE_INST(n) \
    static struct pinnacle_data pinnacle_data_##n; \
    static const struct pinnacle_config pinnacle_config_##n = { \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8), 0), \
        .dr_gpio = GPIO_DT_SPEC_INST_GET_OR(n, dr_gpios, {0}), \
    }; \
    DEVICE_DT_INST_DEFINE(n, pinnacle_init, NULL, &pinnacle_data_##n, \
                         &pinnacle_config_##n, POST_KERNEL, \
                         90, NULL);

DT_INST_FOREACH_STATUS_OKAY(PINNACLE_INST)
