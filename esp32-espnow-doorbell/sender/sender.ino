/*
 * ESP-NOW deep-sleep pushbutton sender
 * Board: ESP32S3 Zero (e.g. Waveshare ESP32-S3-Zero)
 *
 * Behavior:
 *  - Board spends all of its time in deep sleep to save battery.
 *  - Pressing the BOOT button (GPIO0, active LOW) wakes the chip.
 *  - On wake, it sends one ESP-NOW broadcast packet to the receiver.
 *  - It then waits for the button to be released (so it doesn't
 *    immediately re-wake) and goes straight back to deep sleep.
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

static const uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct {
  uint32_t sequence;
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
