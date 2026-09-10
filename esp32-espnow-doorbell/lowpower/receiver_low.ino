#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

// Board: ESP32S3 N16R8 DevKit

#define ESPNOW_CHANNEL             1

// ---------------- Onboard NeoPixel ----------------
#define LED_PIN                    48
#define LED_COUNT                  1
#define LED_BRIGHTNESS             255   // max

// Ring LED behavior (same as constant version)
#define RING_HOLD_MS               5000
#define LED_BLINK_HALF_MS          100

// Battery warning behavior (same)
#define WARN_PAUSE_MS              2000
#define WARN_BLINK_MS              100

// BOOT Button
#define BOOT_BUTTON_GPIO           0
#define DEBOUNCE_MS                50

// ---------------- Buzzer pattern ----------------
#define BUZZER_PIN                 16
#define BEEP_FREQ_HZ               1500
#define BEEP_DUTY                  20
#define BEEP_ON_MS                 100
#define BEEP_OFF_MS                180
#define BEEP_GROUP_GAP_MS          250
#define BEEP_COUNT_PER_GROUP       2
#define BEEP_GROUP_COUNT           2

#define BUZZER_PWM_RESOLUTION      8
#define BUZZER_LEDC_CHANNEL        0

// ---------------- Low-power RX duty cycle ----------------
// Listen 60ms every 250ms total cycle
#define RX_LISTEN_ON_MS            60
#define RX_CYCLE_MS                250
#define RX_LISTEN_OFF_MS           (RX_CYCLE_MS - RX_LISTEN_ON_MS)

// ---------------- Doorbell retrigger cooldown ----------------
#define ALARM_COOLDOWN_MS          60000UL   // 60s pause after a triggered alarm

Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

typedef struct __attribute__((packed)) {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery;
} button_message_t;

// packet counter
volatile uint32_t espnowPacketCount = 0;
portMUX_TYPE packetCountMux = portMUX_INITIALIZER_UNLOCKED;

// callback->loop flags
volatile bool packetEvent = false;
button_message_t lastMsg;
volatile bool lastMsgValid = false;
portMUX_TYPE msgMux = portMUX_INITIALIZER_UNLOCKED;

// LED ring state
volatile uint32_t ringUntil = 0;
bool ringActive = false;
bool ledOnState = false;
uint32_t lastLedToggle = 0;

// battery warning state
bool batteryWarningPending = false;
bool batteryWarningActive = false;
uint8_t warnPhase = 0;
uint32_t warnStepTs = 0;

// buzzer state
bool buzzerSequenceActive = false;
bool buzzerToneOn = false;
bool buzzerInGroupGap = false;
uint8_t buzzerBeepsRemainingInGroup = 0;
uint8_t buzzerGroupsRemaining = 0;
uint32_t buzzerStepTs = 0;

// radio duty-cycle state
bool rxRadioOn = false;
uint32_t rxStateTs = 0;

// cooldown state
bool alarmCooldownActive = false;
uint32_t alarmCooldownUntil = 0;

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

void startRingLed() {
  ringUntil = millis() + RING_HOLD_MS;
  ringActive = true;
  ledOnState = true;
  lastLedToggle = millis();
  setLed(255, 255, 255);
}

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
    if (stableState == LOW) eventFired = true;
  }

  if (eventFired) {
    eventFired = false;
    return true;
  }
  return false;
}

void serviceRingLed(uint32_t now) {
  if (ringActive) {
    if ((int32_t)(now - ringUntil) >= 0) {
      ringActive = false;
      ledOnState = false;
      setLed(0, 0, 0);

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
      setLed(ledOnState ? 255 : 0, ledOnState ? 255 : 0, ledOnState ? 255 : 0);
    }
    return;
  }

  if (batteryWarningActive) {
    uint32_t elapsed = now - warnStepTs;
    switch (warnPhase) {
      case 0: setLed(255, 0, 0); if (elapsed >= WARN_BLINK_MS) { warnPhase = 1; warnStepTs = now; } break;
      case 1: setLed(0, 0, 0);   if (elapsed >= WARN_BLINK_MS) { warnPhase = 2; warnStepTs = now; } break;
      case 2: setLed(255, 0, 0); if (elapsed >= WARN_BLINK_MS) { warnPhase = 3; warnStepTs = now; } break;
      case 3: setLed(0, 0, 0);   if (elapsed >= WARN_BLINK_MS) { warnPhase = 4; warnStepTs = now; } break;
      case 4: setLed(0, 0, 0);   if (elapsed >= WARN_PAUSE_MS) { warnPhase = 0; warnStepTs = now; } break;
    }
  }
}

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
  portENTER_CRITICAL_ISR(&packetCountMux);
  countSnapshot = espnowPacketCount + 1;
  espnowPacketCount = countSnapshot;
  portEXIT_CRITICAL_ISR(&packetCountMux);

  if (len == (int)sizeof(button_message_t)) {
    button_message_t msg;
    memcpy(&msg, data, sizeof(msg));

    portENTER_CRITICAL_ISR(&msgMux);
    memcpy((void*)&lastMsg, &msg, sizeof(msg));
    lastMsgValid = true;
    packetEvent = true;
    portEXIT_CRITICAL_ISR(&msgMux);

    Serial.printf("ESP-NOW packet #%lu: Seq=%lu Battery=%umV (%u%%)%s\n",
                  (unsigned long)countSnapshot,
                  (unsigned long)msg.sequence,
                  msg.batteryMilliVolts,
                  msg.batteryPercent,
                  msg.lowBattery ? " LOW" : "");
  } else {
    Serial.printf("ESP-NOW packet #%lu: Length=%d (unexpected)\n",
                  (unsigned long)countSnapshot, len);
  }
}

bool startEspNowRx() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    Serial.printf("wifi_start err=%d\n", err);
  }

  err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK) {
    Serial.printf("set_channel err=%d\n", err);
    return false;
  }

  err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("esp_now_init err=%d\n", err);
    return false;
  }

  err = esp_now_register_recv_cb(onDataRecv);
  if (err != ESP_OK) {
    Serial.printf("register_recv_cb failed: %d\n", err);
    return false;
  }

  return true;
}

void stopEspNowRx() {
  esp_now_deinit();
  esp_wifi_stop();
}

void handleReceivedMessage(const button_message_t &msg) {
  uint32_t now = millis();

  // Always process battery state
  if (msg.lowBattery) {
    if (ringActive) {
      batteryWarningPending = true;
    } else {
      batteryWarningActive = true;
      batteryWarningPending = false;
      warnPhase = 0;
      warnStepTs = now;
    }
  } else if (msg.batteryPercent > 30) {
    batteryWarningActive = false;
    batteryWarningPending = false;
    if (!ringActive) setLed(0, 0, 0);
  }

  // Doorbell alarm cooldown gate (60s pause)
  if (alarmCooldownActive && (int32_t)(now - alarmCooldownUntil) < 0) {
    return;
  }

  startRingLed();
  startBuzzerSequence();

  alarmCooldownActive = true;
  alarmCooldownUntil = now + ALARM_COOLDOWN_MS;
}

void serviceRadioDutyCycle(uint32_t now) {
  if (rxRadioOn) {
    if ((uint32_t)(now - rxStateTs) >= RX_LISTEN_ON_MS) {
      stopEspNowRx();
      rxRadioOn = false;
      rxStateTs = now;
    }
  } else {
    if ((uint32_t)(now - rxStateTs) >= RX_LISTEN_OFF_MS) {
      if (startEspNowRx()) {
        rxRadioOn = true;
      }
      rxStateTs = now;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nLow-power receiver booting...");

  pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);

  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  setLed(0, 0, 0);

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(BUZZER_PIN, BEEP_FREQ_HZ, BUZZER_PWM_RESOLUTION);
#else
  ledcSetup(BUZZER_LEDC_CHANNEL, BEEP_FREQ_HZ, BUZZER_PWM_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
#endif
  buzzerOff();

  if (startEspNowRx()) {
    rxRadioOn = true;
    rxStateTs = millis();
  } else {
    rxRadioOn = false;
    rxStateTs = millis();
  }

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("Duty cycle: ON=%ums OFF=%ums\n", RX_LISTEN_ON_MS, RX_LISTEN_OFF_MS);
  Serial.printf("Doorbell cooldown: %lu ms\n", (unsigned long)ALARM_COOLDOWN_MS);
  Serial.println("BOOT: clear battery warning if active/pending, otherwise test alarm.");
}

void loop() {
  uint32_t now = millis();

  serviceRadioDutyCycle(now);

  // expire cooldown
  if (alarmCooldownActive && (int32_t)(now - alarmCooldownUntil) >= 0) {
    alarmCooldownActive = false;
  }

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
      alarmCooldownActive = true;
      alarmCooldownUntil = millis() + ALARM_COOLDOWN_MS;
    }
  }

  bool gotEvent = false;
  button_message_t msgCopy;
  portENTER_CRITICAL(&msgMux);
  if (packetEvent && lastMsgValid) {
    gotEvent = true;
    memcpy(&msgCopy, (const void*)&lastMsg, sizeof(msgCopy));
    packetEvent = false;
  }
  portEXIT_CRITICAL(&msgMux);

  if (gotEvent) {
    handleReceivedMessage(msgCopy);
  }

  serviceRingLed(now);
  serviceBuzzer(now);

  delay(2);
}
