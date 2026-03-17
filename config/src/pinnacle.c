#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

/* ZMK HID und Endpoint Header für die Maus-Übertragung */
#include <zmk/hid.h>
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

/* Diese Funktion verarbeitet die Daten, wenn das Trackpad eine Berührung meldet */
static void pinnacle_work_handler(struct k_work *work) {
    struct pinnacle_data *data = CONTAINER_OF(work, struct pinnacle_data, work);
    const struct pinnacle_config *config = data->dev->config;

    uint8_t read_cmd = 0xFC; // Pinnacle SPI Read Command
    uint8_t report[4];       // [0]=Status, [1]=X, [2]=Y, [3]=Wheel
    
    struct spi_buf tx_buf = { .buf = &read_cmd, .len = 1 };
    struct spi_buf_set tx = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf rx_buf[] = {
        { .buf = NULL, .len = 1 }, // Dummy Byte während des Kommandos
        { .buf = report, .len = sizeof(report) }
    };
    struct spi_buf_set rx = { .buffers = rx_buf, .count = 2 };

    /* SPI Datentransfer */
    if (spi_transceive_dt(&config->bus, &tx, &rx) == 0) {
        int8_t x = (int8_t)report[1];
        int8_t y = (int8_t)report[2];
        
        /* Nur senden, wenn wirklich eine Bewegung stattgefunden hat */
        if (x != 0 || y != 0) {
            // Wir invertieren Y (-y), damit "hoch" auch auf dem Bildschirm "hoch" ist
            zmk_hid_mouse_movement_update(x, -y, 0, 0);
            zmk_endpoints_send_mouse_report();
        }
    }
}

/* Interrupt-Handler
