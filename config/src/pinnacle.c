#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/events/mouse_state_changed.h>
#include <zmk/endpoints.h>

LOG_MODULE_REGISTER(pinnacle, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT cirque_pinnacle

struct pinnacle_config {
    struct spi_dt_spec bus;
    struct gpio_dt_spec dr_gpio;
};

struct pinnacle_data {
    struct gpio_callback dr_cb;
    struct k_work work;
    const struct device *dev;
};

// Diese Funktion liest die Daten vom Trackpad, wenn der DR-Pin feuert
static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    const struct pinnacle_config *config = data->dev->config;

    uint8_t read_cmd = 0xFC; // Pinnacle Read Command
    uint8_t report[4]; // Status, X, Y, Wheel
    
    struct spi_buf tx_buf = { .buf = &read_cmd, .len = 1 };
    struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf rx_buf[] = {
        { .buf = NULL, .len = 1 }, // Dummy Byte während Kommando
        { .buf = report, .len = sizeof(report) }
    };
    struct spi_buf_set rx = { .buffers = rx_buf, .count = 2 };

    if (spi_transceive_dt(&config->bus, &tx, &rx) == 0) {
        // Report[1] ist X, Report[2] ist Y (relativ)
        int8_t x = (int8_t)report[1];
        int8_t y = (int8_t)report[2];
        
        if (x != 0 || y != 0) {
            zmk_mouse_pro_report_relative(x, -y, 0, 0, 10); // Schickt Daten an ZMK
        }
    }
}

static void pinnacle_gpio_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
    struct pinnacle_data *data = CONTAINER_OF(cb, struct pinnacle_data, dr_cb);
    k_work_submit(&data->work);
}

static int pinnacle_init(const struct device *dev) {
    const struct pinnacle_config *config = dev->config;
    struct pinnacle_data *data = dev->data;
    data->dev = dev;

    if (!spi_is_ready_dt(&config->bus)) return -ENODEV;
    if (!gpio_is_ready_dt(&config->dr_gpio)) return -ENODEV;

    gpio_pin_configure_dt(&config->dr_gpio, GPIO_INPUT);
    gpio_init_callback(&data->dr_cb, pinnacle_gpio_callback, BIT(config->dr_gpio.pin));
    gpio_add_callback(config->dr_gpio.port, &data->dr_cb);
    gpio_pin_interrupt_configure_dt(&config->dr_gpio, GPIO_INT_EDGE_TO_ACTIVE);

    k_work_init(&data->work, pinnacle_work_handler);

    // Initialisierung des Trackpads (Feed Enable)
    uint8_t init_cmd[] = { 0x04, 0x01 }; // Era Register Feed Enable
    struct spi_buf init_tx_buf = { .buf = init_cmd, .len = 2 };
    struct spi_buf_set init_tx = { .buffers = &init_tx_buf, .count = 1 };
    spi_write_dt(&config->bus, &init_tx);

    return 0;
}

#define PINNACLE_INST(n) \
    static struct pinnacle_data pinnacle_data_##n; \
    static const struct pinnacle_config pinnacle_config_##n = { \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8), 0), \
        .dr_gpio = GPIO_DT_SPEC_INST_GET(n, dr_gpios), \
    }; \
    DEVICE_DT_INST_DEFINE(n, pinnacle_init, NULL, \
                         &pinnacle_data_##n, &pinnacle_config_##n, \
                         APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(PINNACLE_INST)
