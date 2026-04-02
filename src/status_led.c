/*
 * status_led.c — Heartbeat + key-activity indicator for the onboard LED.
 *
 * Target board: Raspberry Pi Pico (rpi_pico)
 *   GP25 = single green LED, aliased as 'led0' in the board DTS.
 *
 * LED patterns:
 *   1 Hz blink   (500 ms on / 500 ms off) = board is powered, ZMK running
 *   100 ms flash                           = key event detected
 *                                           (proxy for UART TX/RX activity)
 *   LED off                                = ZMK idle/sleep state active
 *
 * Note on three-colour request:
 *   The standard Raspberry Pi Pico has only one monochrome LED.  To get
 *   three distinct colours (power / UART-in / UART-out) you would need
 *   additional hardware, e.g. an external WS2812 RGB LED wired to any free
 *   GPIO (GP16 on the Waveshare RP2040-Zero is a popular choice) and the
 *   zmk,underglow driver.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#define LED_NODE DT_ALIAS(led0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_NODE, okay),
             "led0 alias not found in device tree; is 'rpi_pico' the target board?");

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

/* Two atomic flags packed together:
 *   FLAG_PHASE    — current heartbeat phase (0=off, 1=on)
 *   FLAG_ACTIVITY — key-activity flash is in progress
 */
static ATOMIC_DEFINE(led_flags, 2);
#define FLAG_PHASE    0
#define FLAG_ACTIVITY 1

static struct k_timer          heartbeat_timer;
static struct k_work_delayable activity_off_work;

/* Timer callback — runs at interrupt level, so only atomic ops + gpio. */
static void heartbeat_cb(struct k_timer *t)
{
    if (atomic_test_bit(led_flags, FLAG_ACTIVITY)) {
        /* Activity flash in progress — don't touch the LED. */
        return;
    }
    /* Toggle heartbeat phase and update LED. */
    bool phase = !atomic_test_bit(led_flags, FLAG_PHASE);
    if (phase) {
        atomic_set_bit(led_flags, FLAG_PHASE);
    } else {
        atomic_clear_bit(led_flags, FLAG_PHASE);
    }
    gpio_pin_set_dt(&status_led, phase ? 1 : 0);
}

/* Work item — runs in ZMK system work queue (thread context). */
static void activity_off_cb(struct k_work *w)
{
    atomic_clear_bit(led_flags, FLAG_ACTIVITY);
    /* Restore LED to current heartbeat phase. */
    gpio_pin_set_dt(&status_led, atomic_test_bit(led_flags, FLAG_PHASE) ? 1 : 0);
}

/* ZMK event listener — called on every key press/release. */
static int key_event_listener(const zmk_event_t *eh)
{
    atomic_set_bit(led_flags, FLAG_ACTIVITY);
    gpio_pin_set_dt(&status_led, 1);
    k_work_reschedule(&activity_off_work, K_MSEC(100));
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led, key_event_listener);
ZMK_SUBSCRIPTION(status_led, zmk_position_state_changed);

static int status_led_init(void)
{
    if (!gpio_is_ready_dt(&status_led)) {
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    k_work_init_delayable(&activity_off_work, activity_off_cb);
    k_timer_init(&heartbeat_timer, heartbeat_cb, NULL);
    /* First tick after 500 ms, then every 500 ms → 1 Hz blink */
    k_timer_start(&heartbeat_timer, K_MSEC(500), K_MSEC(500));

    return 0;
}

SYS_INIT(status_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
