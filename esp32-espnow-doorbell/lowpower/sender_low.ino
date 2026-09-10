
#include <WiFi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

// Board: XIAO ESP32C6 Tiny SuperMini

#define PIR_SENSOR_GPIO           GPIO_NUM_2   // D2 / A2
#define ESPNOW_CHANNEL            1

#define BATTERY_ADC_PIN           0            // A0 / D0
#define BATTERY_DIVIDER_RATIO     2.05f        // calibrated battery-only
#define BATTERY_ADC_SAMPLES       8
#define BATTERY_EMPTY_MILLIVOLTS  3300
#define BATTERY_FULL_MILLIVOLTS   4200
#define BATTERY_LOW_PERCENT       20

// XIAO ESP32C6 onboard LED
#define LED_BUILTIN               15

// Wake/send timing
#define COLD_BOOT_DELAY_MS        300
#define WAKE_FAST_DELAY_MS        10
#define TX_INTERVAL_MS            40     // spacing between repeated packets
#define TX_BURST_COUNT            8      // total packets in one trigger
#define TX_FINAL_SETTLE_MS        40

static const uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)) {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery;
} button_message_t;

RTC_DATA_ATTR static uint32_t bootCount = 0;
RTC_DATA_ATTR static uint32_t sendSequence = 0;

uint16_t readBatteryMilliVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(300);
  }

  float adcMv  = (float)sum / BATTERY_ADC_SAMPLES;
  float battMv = adcMv * BATTERY_DIVIDER_RATIO;

  if (battMv < 3000) battMv = 3000;
  if (battMv > 4350) battMv = 4350;

  return (uint16_t)battMv;
}

uint8_t batteryMilliVoltsToPercent(uint16_t mv) {
  if (mv <= BATTERY_EMPTY_MILLIVOLTS) return 0;
  if (mv >= BATTERY_FULL_MILLIVOLTS) return 100;
  return (uint8_t)(100UL * (mv - BATTERY_EMPTY_MILLIVOLTS) /
                   (BATTERY_FULL_MILLIVOLTS - BATTERY_EMPTY_MILLIVOLTS));
}

bool initEspNow() {
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

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(broadcastAddress)) {
    err = esp_now_add_peer(&peerInfo);
    if (err != ESP_OK) {
      Serial.printf("add_peer err=%d\n", err);
      return false;
    }
  }

  return true;
}

void deinitEspNow() {
  esp_now_deinit();
  esp_wifi_stop();
}

void goToSleep() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // wait until PIR drops LOW to avoid immediate wake loop
  while (digitalRead((int)PIR_SENSOR_GPIO) == HIGH) {
    delay(20);
  }
  delay(20);

  Serial.flush();
  uint64_t mask = 1ULL << PIR_SENSOR_GPIO;
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT1) delay(WAKE_FAST_DELAY_MS);
  else delay(COLD_BOOT_DELAY_MS);

  pinMode((int)PIR_SENSOR_GPIO, INPUT);
  pinMode(BATTERY_ADC_PIN, INPUT);

  bootCount++;
  Serial.printf("\nBoot #%lu cause=%d PIR=%d\n",
                (unsigned long)bootCount,
                (int)cause,
                digitalRead((int)PIR_SENSOR_GPIO));

  if (!initEspNow()) {
    goToSleep();
  }

  button_message_t msg;
  msg.sequence = ++sendSequence;
  msg.batteryMilliVolts = readBatteryMilliVolts();
  msg.batteryPercent = batteryMilliVoltsToPercent(msg.batteryMilliVolts);
  msg.lowBattery = (msg.batteryPercent <= BATTERY_LOW_PERCENT) ? 1 : 0;

  Serial.printf("TX seq=%lu batt=%umV (%u%%)%s\n",
                (unsigned long)msg.sequence,
                msg.batteryMilliVolts,
                msg.batteryPercent,
                msg.lowBattery ? " LOW" : "");

  // burst: repeated short sends for better catch on duty-cycled receiver
  for (int i = 0; i < TX_BURST_COUNT; i++) {
    esp_err_t s = esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
    Serial.printf("  burst %d/%d send=%d\n", i + 1, TX_BURST_COUNT, s);
    if (i < TX_BURST_COUNT - 1) delay(TX_INTERVAL_MS);
  }

  delay(TX_FINAL_SETTLE_MS);
  deinitEspNow();
  goToSleep();
}

void loop() {}
