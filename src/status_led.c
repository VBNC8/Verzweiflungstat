/*
 * status_led.c — RGB status indicator using the onboard WS2812 NeoPixel.
 *
 * Target board: Waveshare RP2040 Zero
 *   GP16 = WS2812 RGB LED (1 pixel), board DTS defines 'led-strip' alias.
 *
 * Three colours indicate three distinct states:
 *
 *   🔵 Blue,  1 Hz blink   = board is powered, ZMK is running (heartbeat)
 *   🟢 Green, 150 ms flash = key pressed on THIS half (UART TX proxy)
 *   🔴 Red,   150 ms flash = key received from the OTHER half (UART RX proxy)
 *                            Central (left) side only: positions with col >= 6
 *                            are forwarded from the Peripheral via UART.
 *
 * How to read it for debugging:
 *   • Right half shows only Blue + Green.
 *     Green flash = key scanned, will be sent to left via UART.
 *   • Left half shows Blue, Green (own keys), and Red (received from right).
 *     If you press a right-half key and see Red on the left → UART is working.
 *     If you press a right-half key but left stays Blue → UART is broken.
 *
 * All LED strip API calls are dispatched through the system work queue so
 * they are never made from interrupt context and are always serialised.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

/* ── Device tree ─────────────────────────────────────────────────────────── */

#define LED_STRIP_NODE DT_ALIAS(led_strip)
BUILD_ASSERT(DT_NODE_HAS_STATUS(LED_STRIP_NODE, okay),
             "led-strip alias not found; is 'waveshare_rp2040_zero' the target board?");

static const struct device *led_strip_dev = DEVICE_DT_GET(LED_STRIP_NODE);

/* ── Colour definitions (dimmed to ~10 % to avoid eye strain) ───────────── */

static const struct led_rgb COL_OFF   = {.r = 0,  .g = 0,  .b = 0};
static const struct led_rgb COL_BLUE  = {.r = 0,  .g = 0,  .b = 25};
static const struct led_rgb COL_GREEN = {.r = 0,  .g = 25, .b = 0};
static const struct led_rgb COL_RED   = {.r = 25, .g = 0,  .b = 0};

/* ── State ───────────────────────────────────────────────────────────────── */

/* Number of columns in the full split matrix (left 0–5, right 6–11). */
#define MATRIX_COLS 12

static ATOMIC_DEFINE(led_flags, 2);
#define FLAG_PHASE    0  /* current heartbeat phase: 0 = off, 1 = blue */
#define FLAG_ACTIVITY 1  /* activity flash in progress */

/* Atomic flag: 0 = local key (Green), 1 = key from other half (Red).
 * Using a dedicated atomic avoids struct-assignment races between the event
 * listener thread and the activity_on work handler. */
static atomic_t activity_from_other = ATOMIC_INIT(0);

/* ── Work items (all LED strip calls go through the system work queue) ───── */

static struct k_work_delayable heartbeat_work;
static struct k_work_delayable activity_off_work;
static struct k_work           activity_on_work;

static void strip_set(const struct led_rgb *color)
{
    struct led_rgb pixels[1] = {*color};
    led_strip_update_rgb(led_strip_dev, pixels, 1);
}

static void heartbeat_work_cb(struct k_work *w)
{
    if (!atomic_test_bit(led_flags, FLAG_ACTIVITY)) {
        bool phase = !atomic_test_bit(led_flags, FLAG_PHASE);
        if (phase) {
            atomic_set_bit(led_flags, FLAG_PHASE);
            strip_set(&COL_BLUE);
        } else {
            atomic_clear_bit(led_flags, FLAG_PHASE);
            strip_set(&COL_OFF);
        }
    }
    k_work_reschedule(k_work_delayable_from_work(w), K_MSEC(500));
}

static void activity_on_work_cb(struct k_work *w)
{
    atomic_set_bit(led_flags, FLAG_ACTIVITY);
    strip_set(atomic_get(&activity_from_other) ? &COL_RED : &COL_GREEN);
}

static void activity_off_work_cb(struct k_work *w)
{
    atomic_clear_bit(led_flags, FLAG_ACTIVITY);
    strip_set(atomic_test_bit(led_flags, FLAG_PHASE) ? &COL_BLUE : &COL_OFF);
}

/* ── ZMK event listener ──────────────────────────────────────────────────── */

static int key_event_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    /* Only react to key-press (state = true), not release. */
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /*
     * On the Central (left half): keys from the right half have their column
     * offset applied by ZMK, so position % MATRIX_COLS >= MATRIX_COLS/2.
     * On the Peripheral (right half): all positions are local (col 0–5)
     * and will always show Green.
     */
    bool from_other_half = (ev->position % MATRIX_COLS) >= (MATRIX_COLS / 2);
    atomic_set(&activity_from_other, from_other_half ? 1 : 0);

    k_work_submit(&activity_on_work);
    k_work_reschedule(&activity_off_work, K_MSEC(150));

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(status_led, key_event_listener);
ZMK_SUBSCRIPTION(status_led, zmk_position_state_changed);

/* ── Init ────────────────────────────────────────────────────────────────── */

static int status_led_init(void)
{
    if (!device_is_ready(led_strip_dev)) {
        return -ENODEV;
    }

    strip_set(&COL_OFF);

    k_work_init(&activity_on_work, activity_on_work_cb);
    k_work_init_delayable(&activity_off_work, activity_off_work_cb);
    k_work_init_delayable(&heartbeat_work, heartbeat_work_cb);

    /* First heartbeat tick after 500 ms, then self-reschedules every 500 ms. */
    k_work_schedule(&heartbeat_work, K_MSEC(500));

    return 0;
}

SYS_INIT(status_led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
