#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#if DT_HAS_COMPAT_STATUS_OKAY(cirque_pinnacle)

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#include <zephyr/input/input.h>

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

static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    const struct device *dev = data->dev;
    const struct pinnacle_config *config = data->dev->config;

    uint8_t cmd = 0xFC;
    uint8_t buf[6];
    struct spi_buf tx = {.buf = &cmd, .len = 1};
    struct spi_buf_set txs = {.buffers = &tx, .count = 1};
    struct spi_buf rx = {.buf = buf, .len = 6};
    struct spi_buf_set rxs = {.buffers = &rx, .count = 1};

    if (spi_transceive_dt(&config->bus, &txs, &rxs) == 0) {
        // Die Koordinaten liegen bei Pinnacle oft in buf[1] und buf[2]
        // Wir casten zu int8_t für die Richtung (positiv/negativ)
        int8_t dx = (int8_t)buf[1];
        int8_t dy = (int8_t)buf[2];

        if (dx != 0 || dy != 0) {
            // Wir melden die relative Bewegung an das System
            // input_report_rel(Gerät, Achse, Wert, Sync, Timeout)
            input_report_rel(dev, INPUT_REL_X, dx, false, K_FOREVER);
            
            // Das 'true' beim letzten Aufruf (Sync) sagt ZMK: 
            // "Jetzt ist das Paket fertig, schick es ab!"
            input_report_rel(dev, INPUT_REL_Y, -dy, true, K_FOREVER); 
            // Hinweis: -dy oft nötig, da Trackpad-Y und Screen-Y meist invertiert sind
        }
    }
}

static void pinnacle_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins) {
    struct pinnacle_data *data = CONTAINER_OF(cb, struct pinnacle_data, gpio_cb);
    k_work_submit(&data->work);
}

static int pinnacle_init(const struct device *dev) {
    struct pinnacle_data *data = dev->data;
    const struct pinnacle_config *config = dev->config;
    data->dev = dev;
    k_work_init(&data->work, pinnacle_work_handler);
    
    if (!spi_is_ready_dt(&config->bus)) return -ENODEV;
    
    gpio_pin_configure_dt(&config->dr_gpio, GPIO_INPUT);
    gpio_init_callback(&data->gpio_cb, pinnacle_callback, BIT(config->dr_gpio.pin));
    gpio_add_callback(config->dr_gpio.port, &data->gpio_cb);
    gpio_pin_interrupt_configure_dt(&config->dr_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    return 0;
}

#define PINNACLE_INST(n) \
    static struct pinnacle_data pinnacle_data_##n; \
    static const struct pinnacle_config pinnacle_config_##n = { \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8), 0), \
        .dr_gpio = GPIO_DT_SPEC_INST_GET(n, dr_gpios), \
    }; \
    DEVICE_DT_INST_DEFINE(n, pinnacle_init, NULL, &pinnacle_data_##n, \
                         &pinnacle_config_##n, POST_KERNEL, 90, NULL);

DT_INST_FOREACH_STATUS_OKAY(PINNACLE_INST)

#endif // DT_HAS_COMPAT_STATUS_OKAY
