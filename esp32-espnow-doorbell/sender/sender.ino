/*
 * ESP-NOW deep-sleep pushbutton sender
 * Board: ESP32S3 Zero (e.g. Waveshare ESP32-S3-Zero)
 *
 * Behavior:
 *  - Board spends all of its time in deep sleep to save battery.
 *  - Pressing the BOOT button (GPIO0, active LOW) wakes the chip.
 *  - On wake, it reads the battery voltage, sends one ESP-NOW
 *    broadcast packet (button press + battery info) to the receiver.
 *  - It then waits for the button to be released (so it doesn't
 *    immediately re-wake) and goes straight back to deep sleep.
 *
 * Battery monitoring:
 *  - The ESP32-S3-Zero has no built-in fuel gauge/voltage divider, so
 *    this requires an EXTERNAL 2-resistor divider wired from
 *    battery+ to BATTERY_ADC_PIN to GND, sized so the divided voltage
 *    stays within the ADC's ~0-3.3V input range at the battery's
 *    maximum voltage (e.g. two equal resistors, e.g. 100k+100k, for a
 *    2:1 divider on a single-cell LiPo which maxes out around 4.2V).
 *    Update BATTERY_DIVIDER_RATIO to match your resistor values:
 *      ratio = (R1 + R2) / R2   where R1 is battery-side, R2 is GND-side.
 *  - Reading the ADC a few times only takes microseconds and happens
 *    while the radio is already powered on for the ESP-NOW send, so
 *    it adds negligible extra battery drain (see README).
 *
 * Notes:
 *  - ESP-NOW broadcast (FF:FF:FF:FF:FF:FF) is used so the sender does
 *    not need to know the receiver's MAC address in advance.
 *  - Both boards must end up on the same Wi-Fi channel. Since neither
 *    board connects to a router here, both stay on the default channel
 *    (1). If you later join a Wi-Fi network on either board, update
 *    ESPNOW_CHANNEL below to match and set it explicitly on both sides.
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#define BOOT_BUTTON_GPIO   GPIO_NUM_0   // BOOT button, active LOW
#define ESPNOW_CHANNEL     1            // must match receiver

// --- Battery monitoring configuration ---
#define BATTERY_ADC_PIN            1      // GPIO connected to the voltage-divider midpoint
#define BATTERY_DIVIDER_RATIO      2.0f   // (R1+R2)/R2 - adjust to your resistor values
#define BATTERY_ADC_SAMPLES        8       // averaged samples for a more stable reading
#define BATTERY_EMPTY_MILLIVOLTS   3300    // LiPo "empty" voltage -> reported as 0%
#define BATTERY_FULL_MILLIVOLTS    4200    // LiPo "full" voltage -> reported as 100%
#define BATTERY_LOW_PERCENT        20      // at/below this, lowBattery flag is set

static const uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery; // 0 or 1
} button_message_t;

RTC_DATA_ATTR static uint32_t bootCount = 0;
RTC_DATA_ATTR static uint32_t sendSequence = 0;

// esp_now_send_cb_t's signature changed between ESP32 Arduino core
// versions: older cores (<=2.x) pass a MAC address (const uint8_t*),
// while newer cores (esp32 Arduino core >=3.x, based on IDF 5.x) pass
// a (const wifi_tx_info_t*). Handle both so the sketch compiles either
// way.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  Serial.print("Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// Reads the battery voltage via the external divider on BATTERY_ADC_PIN
// and returns the estimated true battery voltage in millivolts (i.e.
// already scaled back up by BATTERY_DIVIDER_RATIO).
uint16_t readBatteryMilliVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(200);
  }
  float avgMilliVolts = (float)sum / BATTERY_ADC_SAMPLES;
  return (uint16_t)(avgMilliVolts * BATTERY_DIVIDER_RATIO);
}

// Rough linear estimate of remaining capacity from voltage. LiPo
// discharge curves aren't linear, but this is good enough for a
// "time to recharge" style warning.
uint8_t batteryMilliVoltsToPercent(uint16_t milliVolts) {
  if (milliVolts <= BATTERY_EMPTY_MILLIVOLTS) return 0;
  if (milliVolts >= BATTERY_FULL_MILLIVOLTS) return 100;
  return (uint8_t)(100UL * (milliVolts - BATTERY_EMPTY_MILLIVOLTS) /
                    (BATTERY_FULL_MILLIVOLTS - BATTERY_EMPTY_MILLIVOLTS));
}

void goToSleep() {
  // Wait until the button is released so we don't wake immediately again.
  while (digitalRead(BOOT_BUTTON_GPIO) == LOW) {
    delay(10);
  }
  delay(20); // simple debounce on release

  Serial.println("Going to deep sleep...");
  Serial.flush();

  esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0); // wake on LOW
  esp_deep_sleep_start();
  // Never reaches here.
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  pinMode(BATTERY_ADC_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db); // covers ~0-3.3V range

  Serial.printf("Boot #%u, wakeup cause: %d\n", ++bootCount, esp_sleep_get_wakeup_cause());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, sleeping anyway.");
    goToSleep();
  }

  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add broadcast peer.");
    }
  }

  button_message_t msg;
  msg.sequence = ++sendSequence;
  msg.batteryMilliVolts = readBatteryMilliVolts();
  msg.batteryPercent = batteryMilliVoltsToPercent(msg.batteryMilliVolts);
  msg.lowBattery = (msg.batteryPercent <= BATTERY_LOW_PERCENT) ? 1 : 0;

  Serial.printf("Battery: %u mV (%u%%)%s\n", msg.batteryMilliVolts,
                msg.batteryPercent, msg.lowBattery ? " - LOW" : "");

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(msg));
  if (result != ESP_OK) {
    Serial.println("esp_now_send failed to queue.");
  }

  // Give the radio a moment to actually transmit and call onDataSent
  // before we power everything down.
  delay(150);

  goToSleep();
}

void loop() {
  // Not used: setup() never returns because the board deep sleeps.
}
