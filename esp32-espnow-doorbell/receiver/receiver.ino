/*
 * ESP-NOW receiver: latches onboard LED on when a signal arrives,
 * clears it when the BOOT button is pressed.
 *
 * Board: ESP32-S3 N8R2 devboard, HW-678 (8MB flash / 2MB PSRAM).
 * This board has an addressable WS2812 "NeoPixel" RGB LED on GPIO48
 * instead of a plain on/off GPIO LED, so it is driven with the
 * Adafruit_NeoPixel library.
 *
 * Requires the "Adafruit NeoPixel" library (Arduino IDE Library
 * Manager: Sketch > Include Library > Manage Libraries...).
 *
 * Behavior:
 *  - Stays powered continuously (via USB during the test period).
 *  - Listens for ESP-NOW broadcast packets from the sender.
 *  - When a packet is received, turns the onboard LED red and keeps
 *    it on, ignoring further incoming packets while already lit.
 *  - Every received packet also carries the sender's battery voltage
 *    and percentage; these are logged to Serial. If the sender flags
 *    the battery as low, the LED briefly pulses amber a few times
 *    (in addition to going/staying red) so the low-battery condition
 *    is visible without needing the Serial monitor open.
 *  - Pressing the BOOT button (GPIO0, active LOW) turns the LED back
 *    off (acknowledged/cleared).
 *
 * Notes:
 *  - Must be on the same ESP-NOW channel as the sender (see
 *    ESPNOW_CHANNEL in sender.ino).
 *  - The button_message_t layout below MUST stay in sync with
 *    sender.ino.
 *  - If you see repeated "E BOD: Brownout detector was triggered"
 *    resets in the Serial Monitor, that means the board's 3.3V rail
 *    is briefly dipping too low - almost always because the USB
 *    cable/port/hub can't supply the current spike the chip draws
 *    while switching to full clock speed / initializing the Wi-Fi
 *    radio at boot. This sketch still writes RTC_CNTL_BROWN_OUT_REG
 *    and lowers TX power as a best-effort measure, but on ESP32-S3
 *    this reset frequently happens during the ROM bootloader/early
 *    clock-init stage, BEFORE setup() ever runs - at which point no
 *    code in this sketch can prevent it (the write happens too late).
 *    If the reset loop persists after this mitigation, it confirms
 *    the crash is happening pre-setup() and the fix must be at the
 *    power-supply/hardware level (see README's Troubleshooting
 *    section) or by lowering "Tools > CPU Frequency" in the Arduino
 *    IDE board menu (reduces the current spike at the clock switch).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include "soc/rtc_cntl_reg.h"

#define LED_PIN            48    // onboard WS2812 RGB LED data pin (HW-678)
#define LED_BRIGHTNESS     40    // 0-255, kept low to avoid a harsh glare
#define BOOT_BUTTON_GPIO   0      // BOOT button, active LOW
#define ESPNOW_CHANNEL     1      // must match sender
#define DEBOUNCE_MS        50

Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

typedef struct {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery; // 0 or 1
} button_message_t;

volatile bool ledLatched = false;
volatile int lowBatteryBlinksPending = 0;
portMUX_TYPE lowBatteryMux = portMUX_INITIALIZER_UNLOCKED;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(button_message_t)) {
    return;
  }
  button_message_t msg;
  memcpy(&msg, data, sizeof(msg));

  Serial.printf("Received signal, sequence=%u, battery=%u mV (%u%%)%s\n",
                msg.sequence, msg.batteryMilliVolts, msg.batteryPercent,
                msg.lowBattery ? " - LOW BATTERY" : "");
  ledLatched = true;
  if (msg.lowBattery) {
    portENTER_CRITICAL(&lowBatteryMux);
    lowBatteryBlinksPending = 3; // pulse amber a few times to flag it
    portEXIT_CRITICAL(&lowBatteryMux);
  }
}

bool bootButtonPressed() {
  static bool lastReading = HIGH;
  static unsigned long lastChangeMs = 0;
  static bool stableState = HIGH;
  static bool eventFired = false;

  bool reading = digitalRead(BOOT_BUTTON_GPIO);
  unsigned long now = millis();

  if (reading != lastReading) {
    lastChangeMs = now;
    lastReading = reading;
  }

  if ((now - lastChangeMs) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      eventFired = true;
    }
  }

  if (eventFired) {
    eventFired = false;
    return true;
  }
  return false;
}

void setup() {
  // Best-effort: disable the brownout detector as early as our own code
  // can run. This only helps if the reset is happening AFTER setup()
  // starts (e.g. during our own WiFi.mode()/esp_now_init() calls below).
  // On ESP32-S3, brownout resets very often happen even earlier, during
  // the ROM bootloader / clock-switch-to-240MHz stage, BEFORE this line
  // ever executes - in that case this write cannot help at all, and a
  // persistent reset loop points to the power supply / hardware itself
  // (see the file header note and the README's Troubleshooting section).
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  pixel.setPixelColor(0, 0); // off
  pixel.show();

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  // Lower the radio's max TX power to reduce its peak current draw.
  // The receiver is stationary right next to the sender in this setup,
  // so full range/power isn't needed. 8 dBm (value 34, in the driver's
  // quarter-dBm units) is a conservative reduction from the default.
  esp_wifi_set_max_tx_power(34);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Waiting for ESP-NOW signal...");
}

void loop() {
  static bool lastLedState = false;
  static int blinksRemaining = 0;
  static bool blinkOn = false;
  static unsigned long blinkNextToggleMs = 0;
  const unsigned long BLINK_HALF_PERIOD_MS = 150;

  if (bootButtonPressed()) {
    ledLatched = false;
    Serial.println("Cleared by BOOT button.");
  }

  if (lowBatteryBlinksPending > 0) {
    portENTER_CRITICAL(&lowBatteryMux);
    int pending = lowBatteryBlinksPending;
    lowBatteryBlinksPending = 0;
    portEXIT_CRITICAL(&lowBatteryMux);

    blinksRemaining = pending * 2; // on+off per blink
    blinkOn = false;
    blinkNextToggleMs = millis();
  }

  unsigned long now = millis();
  if (blinksRemaining > 0) {
    if ((long)(now - blinkNextToggleMs) >= 0) {
      blinkOn = !blinkOn;
      pixel.setPixelColor(0, blinkOn ? pixel.Color(255, 160, 0) : 0); // amber
      pixel.show();
      blinkNextToggleMs = now + BLINK_HALF_PERIOD_MS;
      blinksRemaining--;
      if (blinksRemaining == 0) {
        lastLedState = !ledLatched; // force a refresh below to restore correct state
      }
    }
  } else if (ledLatched != lastLedState) {
    pixel.setPixelColor(0, ledLatched ? pixel.Color(255, 0, 0) : 0);
    pixel.show();
    lastLedState = ledLatched;
  }
}
