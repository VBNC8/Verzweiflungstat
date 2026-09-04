/*
 * ESP-NOW receiver: latches onboard LED on when a signal arrives,
 * clears it when the BOOT button is pressed.
 *
 * Board: ESP32S3 devboard (e.g. ESP32-S3-DevKitC-1)
 *
 * Behavior:
 *  - Stays powered continuously (via USB during the test period).
 *  - Listens for ESP-NOW broadcast packets from the sender.
 *  - When a packet is received, turns the onboard LED on and keeps
 *    it on, ignoring further incoming packets while already lit.
 *  - Pressing the BOOT button (GPIO0, active LOW) turns the LED back
 *    off (acknowledged/cleared).
 *
 * Notes:
 *  - LED_PIN below assumes a plain GPIO LED such as GPIO2 used on many
 *    ESP32-S3 devkits. Some boards (e.g. those with an addressable
 *    WS2812 "NeoPixel" LED, often on GPIO48) will need different code
 *    to drive the LED - update LED_PIN or swap in a NeoPixel driver if
 *    your board's onboard LED is RGB/addressable.
 *  - Must be on the same ESP-NOW channel as the sender (see
 *    ESPNOW_CHANNEL in sender.ino).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define LED_PIN            2     // onboard LED GPIO; adjust for your board
#define BOOT_BUTTON_GPIO   0      // BOOT button, active LOW
#define ESPNOW_CHANNEL     1      // must match sender
#define DEBOUNCE_MS        50

typedef struct {
  uint32_t sequence;
} button_message_t;

volatile bool ledLatched = false;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(button_message_t)) {
    return;
  }
  button_message_t msg;
  memcpy(&msg, data, sizeof(msg));

  Serial.printf("Received signal, sequence=%u\n", msg.sequence);
  ledLatched = true;
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
  Serial.begin(115200);
  delay(100);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

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
  if (bootButtonPressed()) {
    ledLatched = false;
    Serial.println("Cleared by BOOT button.");
  }

  digitalWrite(LED_PIN, ledLatched ? HIGH : LOW);
}
