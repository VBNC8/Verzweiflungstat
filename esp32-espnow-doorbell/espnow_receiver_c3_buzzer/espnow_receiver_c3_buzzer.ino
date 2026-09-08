/*
 * ESP-NOW receiver (buzzer-only variant): sounds an audible beep
 * pattern when a signal arrives, clears any pending pattern when the
 * BOOT button is pressed.
 *
 * Board: ESP32-C3 devboard (e.g. ESP32-C3-DevKitM-1 / a generic
 * "ESP32-C3 Dev Module").
 *
 * This is a SEPARATE sketch from receiver/receiver.ino (the ESP32-S3
 * HW-678 NeoPixel version) - that sketch is left untouched. This one
 * targets a plain ESP32-C3 board with no addressable/NeoPixel LED, so
 * it drives a simple passive/active buzzer instead of any LED, and
 * has no Adafruit_NeoPixel dependency at all.
 *
 * Behavior:
 *  - Stays powered continuously (via USB during the test period).
 *  - Listens for ESP-NOW broadcast packets from the sender (same
 *    button_message_t packet layout as sender/sender.ino - this MUST
 *    stay in sync with the sender).
 *  - When a packet is received, queues a beep alarm pattern: 5 beeps
 *    at 800 Hz, 200 ms on / 200 ms off each (same pattern as the
 *    prior receiver's alarm), played back non-blockingly in loop().
 *    If a new packet arrives while a pattern is still playing, the
 *    pattern simply restarts from the beginning.
 *  - Every received packet also carries the sender's battery voltage
 *    and percentage; these are logged to Serial. If the sender flags
 *    the battery as low, that is also logged (no separate visual/
 *    audible indication in this buzzer-only variant).
 *  - Pressing the BOOT button (active LOW; BOOT_BUTTON_GPIO defaults
 *    to GPIO9, common on ESP32-C3-DevKitM-1-style boards, but this
 *    varies by board - update it below to match yours, or treat it as
 *    a placeholder if you don't know the exact pin yet) immediately
 *    silences/clears any in-progress or pending beep pattern.
 *
 * Wiring:
 *  - Connect a passive buzzer between BUZZER_PIN (default GPIO4 - an
 *    ordinary GPIO on most ESP32-C3 boards, but confirm against your
 *    specific board's pinout/silkscreen before wiring) and GND. An
 *    active (self-oscillating) buzzer will also work but will just
 *    buzz at its own fixed tone any time it's driven HIGH, ignoring
 *    BUZZER_FREQ_HZ.
 *
 * Notes:
 *  - Must be on the same ESP-NOW channel as the sender (see
 *    ESPNOW_CHANNEL in sender.ino).
 *  - esp_wifi_set_channel() can fail on some ESP32-C3 Arduino core
 *    versions with err=12289 (ESP_ERR_WIFI_NOT_INIT) if it's called
 *    before the Wi-Fi driver has actually finished starting up after
 *    WiFi.mode(WIFI_STA). This sketch calls esp_wifi_start() first to
 *    make sure the driver is up, and treats a set_channel failure as
 *    non-fatal (both boards default to channel 1 anyway when neither
 *    joins a Wi-Fi network, so ESP-NOW still works even if this call
 *    fails).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// --- Pins: change these to match your specific ESP32-C3 board ---
#define BUZZER_PIN         4      // passive buzzer +, buzzer - to GND
#define BOOT_BUTTON_GPIO   9      // BOOT button on most ESP32-C3 devkits (GPIO9), active LOW.
                                   // Treat as a placeholder if your board differs/is unknown.

// --- Buzzer alarm pattern: 5 beeps, 800 Hz, 200 ms on / 200 ms off ---
#define BUZZER_FREQ_HZ         800
#define BUZZER_BEEP_COUNT      5
#define BUZZER_ON_MS           200
#define BUZZER_OFF_MS          200
#define BUZZER_PWM_RESOLUTION  8    // bits; 8-bit duty cycle is plenty for a simple tone
#define BUZZER_PWM_DUTY        128  // ~50% duty cycle while "on"

#define ESPNOW_CHANNEL     1        // must match sender
#define DEBOUNCE_MS        50

// LEDC "channel" number, only used on Arduino-ESP32 core versions
// (<3.0) that require ledcAttachPin()/a separate channel index rather
// than attaching directly to the pin.
#define BUZZER_LEDC_CHANNEL 0

typedef struct {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery; // 0 or 1
} button_message_t;

volatile uint32_t pendingPacketCount = 0;
portMUX_TYPE pendingPacketMux = portMUX_INITIALIZER_UNLOCKED;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(button_message_t)) {
    return;
  }
  button_message_t msg;
  memcpy(&msg, data, sizeof(msg));

  Serial.printf("Received signal, sequence=%u, battery=%u mV (%u%%)%s\n",
                msg.sequence, msg.batteryMilliVolts, msg.batteryPercent,
                msg.lowBattery ? " - LOW BATTERY" : "");
  portENTER_CRITICAL(&pendingPacketMux);
  pendingPacketCount++;
  portEXIT_CRITICAL(&pendingPacketMux);
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

// The LEDC attach/write API changed between Arduino-ESP32 core
// versions: cores >=3.0 (IDF 5.x) attach directly to a pin with
// ledcAttach()/ledcWrite(pin, ...), while older cores (<=2.x) require
// ledcSetup()+ledcAttachPin() and write via the channel number. Handle
// both so this sketch compiles either way.
void buzzerInit() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(BUZZER_PIN, BUZZER_FREQ_HZ, BUZZER_PWM_RESOLUTION);
#else
  ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ, BUZZER_PWM_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
#endif
}

void buzzerOn() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(BUZZER_PIN, BUZZER_PWM_DUTY);
#else
  ledcWrite(BUZZER_LEDC_CHANNEL, BUZZER_PWM_DUTY);
#endif
}

void buzzerOff() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(BUZZER_PIN, 0);
#else
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  buzzerInit();
  buzzerOff();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_start(); // ensure the Wi-Fi driver is fully up before touching the channel

  esp_err_t chanErr = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (chanErr != ESP_OK) {
    // Non-fatal: both boards default to channel 1 anyway when neither
    // joins a Wi-Fi network, so ESP-NOW still works.
    Serial.printf("Warning: esp_wifi_set_channel err=%d, continuing on default channel\n", chanErr);
  }

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
  // Non-blocking beep-pattern state machine: BUZZER_BEEP_COUNT beeps,
  // each BUZZER_ON_MS on followed by BUZZER_OFF_MS off.
  static bool alarmActive = false;
  static bool buzzerIsOn = false;
  static int beepsRemaining = 0;
  static unsigned long nextToggleMs = 0;

  if (bootButtonPressed()) {
    alarmActive = false;
    buzzerIsOn = false;
    beepsRemaining = 0;
    buzzerOff();
    Serial.println("Cleared by BOOT button.");
  }

  // A newly received packet (re)starts the pattern from the beginning,
  // even if one was already playing.
  portENTER_CRITICAL(&pendingPacketMux);
  bool gotPacket = pendingPacketCount > 0;
  pendingPacketCount = 0;
  portEXIT_CRITICAL(&pendingPacketMux);

  if (gotPacket) {
    alarmActive = true;
    beepsRemaining = BUZZER_BEEP_COUNT;
    buzzerIsOn = true;
    buzzerOn();
    nextToggleMs = millis() + BUZZER_ON_MS;
  }

  if (alarmActive) {
    unsigned long now = millis();
    if ((long)(now - nextToggleMs) >= 0) {
      if (buzzerIsOn) {
        buzzerOff();
        buzzerIsOn = false;
        nextToggleMs = now + BUZZER_OFF_MS;
        beepsRemaining--;
        if (beepsRemaining <= 0) {
          alarmActive = false;
        }
      } else {
        buzzerOn();
        buzzerIsOn = true;
        nextToggleMs = now + BUZZER_ON_MS;
      }
    }
  }
}
