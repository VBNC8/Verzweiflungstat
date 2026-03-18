#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>

LOG_MODULE_REGISTER(pinnacle, LOG_LEVEL_INF);

#define DT_DRV_COMPAT cirque_pinnacle

struct pinnacle_config { struct spi_dt_spec bus; };
struct pinnacle_data { const struct device *dev; struct k_timer timer; struct k_work work; };

static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    const struct pinnacle_config *cfg = data->dev->config;
    uint8_t cmd = 0xFC; uint8_t res[4] = {0};
    struct spi_buf tx_b = {.buf = &cmd, .len = 1};
    struct spi_buf rx_b[] = {{.buf = NULL, .len = 1}, {.buf = res, .len = 4}};
    
    // Wir loggen nur, wenn sich wirklich was bewegt, um den Bus nicht zu fluten
    if (spi_transceive_dt(&cfg->bus, &(struct spi_buf_set){&tx_b, 1}, &(struct spi_buf_set){rx_b, 2}) == 0) {
        if (res[1] != 0 || res[2] != 0) {
            input_report_rel(data->dev, INPUT_REL_X, (int8_t)res[1], false, K_FOREVER);
            input_report_rel(data->dev, INPUT_REL_Y, -(int8_t)res[2], true, K_FOREVER);
        }
    }
}

static void pinnacle_timer_handler(struct k_timer *t) {
    struct pinnacle_data *data = CONTAINER_OF(t, struct pinnacle_data, timer);
    k_work_submit(&data->work);
}

static int pinnacle_init(const struct device *dev) {
    struct pinnacle_data *data = dev->data;
    data->dev = dev;
    k_work_init(&data->work, pinnacle_work_handler);
    k_timer_init(&data->timer, pinnacle_timer_handler, NULL);
    
    // Wir starten das Polling erst nach 5 Sekunden, damit USB stabil ist
    k_timer_start(&data->timer, K_MSEC(5000), K_MSEC(20));
    printk("Pinnacle: Treiber geladen, warte 5s auf Start...\n");
    return 0;
}

#define PINNACLE_INST(n) \
    static struct pinnacle_data pinnacle_data_##n; \
    static const struct pinnacle_config pinnacle_config_##n = { \
        .bus = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_MODE_CPHA, 0), \
    }; \
    DEVICE_DT_INST_DEFINE(n, pinnacle_init, NULL, &pinnacle_data_##n, \
                         &pinnacle_config_##n, APPLICATION, 90, NULL);
DT_INST_FOREACH_STATUS_OKAY(PINNACLE_INST)
