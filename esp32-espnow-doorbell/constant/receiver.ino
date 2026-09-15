#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

// Board: ESP32S3 N16R8 DevKit

#define ESPNOW_CHANNEL             1

// ---------------- Onboard NeoPixel ----------------
#define LED_PIN                    48
#define LED_COUNT                  1
#define LED_BRIGHTNESS             255   // MAX brightness

// Ring LED behavior (NORMAL alarm, unchanged style from your example)
#define RING_HOLD_MS               5000
#define LED_BLINK_HALF_MS          100   // 5 Hz

// Battery Warning Pattern (unchanged style: red double blink + 2s pause)
#define WARN_PAUSE_MS              2000
#define WARN_BLINK_MS              100

// BOOT Button (GPIO0, active LOW)
#define BOOT_BUTTON_GPIO           0
#define DEBOUNCE_MS                50

// ---------------- Buzzer ----------------
#define BUZZER_PIN                 16

// New buzzer pattern (separate from LED pattern):
// 1230Hz, 2 beeps/group, 100ms ON, 180ms OFF, 250ms gap, 2 groups
#define BEEP_FREQ_HZ               1500
#define BEEP_DUTY                  20
#define BEEP_ON_MS                 100
#define BEEP_OFF_MS                180
#define BEEP_GROUP_GAP_MS          250
#define BEEP_COUNT_PER_GROUP       2
#define BEEP_GROUP_COUNT           2

#define BUZZER_PWM_RESOLUTION      8
#define BUZZER_LEDC_CHANNEL        0

Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

typedef struct __attribute__((packed)) {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery;
} button_message_t;

// ---------- Packet counter ----------
volatile uint32_t espnowPacketCount = 0;
portMUX_TYPE packetCountMux = portMUX_INITIALIZER_UNLOCKED;

// ---------- LED ring state (Normal) ----------
volatile uint32_t ringUntil = 0;
bool ringActive = false;
bool ledOnState = false;
uint32_t lastLedToggle = 0;

// ---------- Battery Warning state (Persistent) ----------
bool batteryWarningPending = false; // starts after normal ring alarm ends
bool batteryWarningActive = false;
uint8_t warnPhase = 0;
uint32_t warnStepTs = 0;

// ---------- Buzzer grouped sequence ----------
bool buzzerSequenceActive = false;
bool buzzerToneOn = false;
bool buzzerInGroupGap = false;
uint8_t buzzerBeepsRemainingInGroup = 0;
uint8_t buzzerGroupsRemaining = 0;
uint32_t buzzerStepTs = 0;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void buzzerOn() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(BUZZER_PIN, BEEP_DUTY);
#else
  ledcWrite(BUZZER_LEDC_CHANNEL, BEEP_DUTY);
#endif
}

void buzzerOff() {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(BUZZER_PIN, 0);
#else
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);
#endif
}

// --- LED normal alarm start (unchanged behavior) ---
void startRingLed() {
  ringUntil = millis() + RING_HOLD_MS;
  ringActive = true;
  ledOnState = true;
  lastLedToggle = millis();
  setLed(255, 255, 255); // starts white
}

// --- Buzzer grouped pattern start ---
void startBuzzerSequence() {
  buzzerSequenceActive = true;
  buzzerToneOn = true;
  buzzerInGroupGap = false;
  buzzerBeepsRemainingInGroup = BEEP_COUNT_PER_GROUP;
  buzzerGroupsRemaining = BEEP_GROUP_COUNT;
  buzzerStepTs = millis();
  buzzerOn();
}

bool checkBootButtonPressed() {
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

// ---------- LED service (kept as your example logic style) ----------
void serviceRingLed(uint32_t now) {
  // 1) Normal 5-second alarm has priority
  if (ringActive) {
    if ((int32_t)(now - ringUntil) >= 0) {
      ringActive = false;
      ledOnState = false;
      setLed(0, 0, 0);

      // if low battery pending, start warning now
      if (batteryWarningPending) {
        batteryWarningActive = true;
        batteryWarningPending = false;
        warnPhase = 0;
        warnStepTs = now;
      }
      return;
    }

    if ((uint32_t)(now - lastLedToggle) >= LED_BLINK_HALF_MS) {
      lastLedToggle = now;
      ledOnState = !ledOnState;
      setLed(ledOnState ? 255 : 0, ledOnState ? 255 : 0, ledOnState ? 255 : 0); // white blink
    }
    return;
  }

  // 2) If no normal alarm, run battery warning
  if (batteryWarningActive) {
    uint32_t elapsed = now - warnStepTs;
    switch (warnPhase) {
      case 0: // red ON #1
        setLed(255, 0, 0);
        if (elapsed >= WARN_BLINK_MS) { warnPhase = 1; warnStepTs = now; }
        break;
      case 1: // OFF
        setLed(0, 0, 0);
        if (elapsed >= WARN_BLINK_MS) { warnPhase = 2; warnStepTs = now; }
        break;
      case 2: // red ON #2
        setLed(255, 0, 0);
        if (elapsed >= WARN_BLINK_MS) { warnPhase = 3; warnStepTs = now; }
        break;
      case 3: // OFF
        setLed(0, 0, 0);
        if (elapsed >= WARN_BLINK_MS) { warnPhase = 4; warnStepTs = now; }
        break;
      case 4: // pause
        setLed(0, 0, 0);
        if (elapsed >= WARN_PAUSE_MS) { warnPhase = 0; warnStepTs = now; }
        break;
    }
  }
}

// ---------- Buzzer service (new grouped pattern) ----------
void serviceBuzzer(uint32_t now) {
  if (!buzzerSequenceActive) return;

  if (buzzerInGroupGap) {
    if ((uint32_t)(now - buzzerStepTs) >= BEEP_GROUP_GAP_MS) {
      buzzerInGroupGap = false;
      buzzerToneOn = true;
      buzzerBeepsRemainingInGroup = BEEP_COUNT_PER_GROUP;
      buzzerStepTs = now;
      buzzerOn();
    }
    return;
  }

  if (buzzerToneOn) {
    if ((uint32_t)(now - buzzerStepTs) >= BEEP_ON_MS) {
      buzzerOff();
      buzzerToneOn = false;
      buzzerStepTs = now;
    }
  } else {
    if ((uint32_t)(now - buzzerStepTs) >= BEEP_OFF_MS) {
      if (buzzerBeepsRemainingInGroup > 0) buzzerBeepsRemainingInGroup--;

      if (buzzerBeepsRemainingInGroup > 0) {
        buzzerToneOn = true;
        buzzerStepTs = now;
        buzzerOn();
      } else {
        if (buzzerGroupsRemaining > 0) buzzerGroupsRemaining--;
        if (buzzerGroupsRemaining > 0) {
          buzzerInGroupGap = true;
          buzzerStepTs = now;
        } else {
          buzzerSequenceActive = false;
          buzzerOff();
        }
      }
    }
  }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  uint32_t countSnapshot;
  portENTER_CRITICAL(&packetCountMux);
  espnowPacketCount++;
  countSnapshot = espnowPacketCount;
  portEXIT_CRITICAL(&packetCountMux);

  if (len == (int)sizeof(button_message_t)) {
    button_message_t msg;
    memcpy(&msg, data, sizeof(msg));

    Serial.printf("ESP-NOW packet #%lu: Seq=%lu Battery=%umV (%u%%)%s\n",
                  (unsigned long)countSnapshot,
                  (unsigned long)msg.sequence,
                  msg.batteryMilliVolts,
                  msg.batteryPercent,
                  msg.lowBattery ? " LOW" : "");

    // Battery warning behavior kept:
    // - lowBattery => activate/pending warning
    // - batteryPercent > 30 => clear warning
    if (msg.lowBattery) {
      if (ringActive) {
        batteryWarningPending = true; // starts after normal alarm
      } else {
        batteryWarningActive = true;
        batteryWarningPending = false;
        warnPhase = 0;
        warnStepTs = millis();
      }
    } else if (msg.batteryPercent > 30) {
      batteryWarningActive = false;
      batteryWarningPending = false;
      if (!ringActive) setLed(0, 0, 0);
    }
  } else {
    Serial.printf("ESP-NOW packet #%lu: Length=%d (unexpected)\n",
                  (unsigned long)countSnapshot, len);
  }

  // Trigger/restart normal alarm indications
  startRingLed();
  startBuzzerSequence();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nCombined receiver booting...");

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS); // max brightness
  setLed(0, 0, 0);

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(BUZZER_PIN, BEEP_FREQ_HZ, BUZZER_PWM_RESOLUTION);
#else
  ledcSetup(BUZZER_LEDC_CHANNEL, BEEP_FREQ_HZ, BUZZER_PWM_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
#endif
  buzzerOff();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  delay(50);

  esp_wifi_start(); // ensure wifi driver started before set_channel on some cores

  esp_err_t err;
  err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) Serial.printf("set_channel err=%d\n", err);

  err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("ESP-NOW init failed: %d\n", err);
    while (true) delay(1000);
  }

  err = esp_now_register_recv_cb(onDataRecv);
  if (err != ESP_OK) {
    Serial.printf("register_recv_cb failed: %d\n", err);
    while (true) delay(1000);
  }

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Waiting for packets...");
  Serial.println("BOOT: clear battery warning if active/pending, otherwise test alarm.");
}

void loop() {
  uint32_t now = millis();

  if (checkBootButtonPressed()) {
    if (batteryWarningActive || batteryWarningPending) {
      batteryWarningActive = false;
      batteryWarningPending = false;
      warnPhase = 0;
      if (!ringActive) setLed(0, 0, 0);
      Serial.println("Battery warning cleared by BOOT button.");
    } else {
      Serial.println("BOOT test alarm.");
      startRingLed();
      startBuzzerSequence();
    }
  }

  serviceRingLed(now);
  serviceBuzzer(now);

  delay(5);
}
