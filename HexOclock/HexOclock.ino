// =======================================================================================
// PROJEKT:      HexOclock (ESP32-S3 Waveshare Zero)
// VERSION:      v2.1.10 (Fix: prevent false tap detection on cable power)
// BESCHREIBUNG: Energiesparende Hexagonal-LED-Uhr mit Helligkeits- und Lagesensor.
//               STARTUP: Zeigt 2 Sekunden beide Sekunden-LEDs als Awake-Indikator, dann Uhrzeit.
//                        Kabel-Klopfen wird in den ersten 3 Sekunden nach dem Boot ignoriert.
//               CABLE TAP: Klopfen am Kabel zeigt exakt 7 Sekunden das Datum.
//               OTA: Aktivierung NUR durch ein 2. Klopfen während dieser 7 Sekunden (60s Timeout).
//               BLINK LOGIC: Am Kabel Dreiertakt (Links-Rechts-Aus) solange der Akku lädt.
//                            Bei vollem Akku (>= 3150) reines Wechselblinken (Links-Rechts).
//               LED COLORS: Green LEDs = minutes/days
//                           Orange LEDs = seconds indicators
//                           Red LEDs = hours/months
// =======================================================================================

#include <WiFi.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Adafruit_VEML7700.h>
#include <Adafruit_LIS3DH.h>
#include <Adafruit_Sensor.h>
#include <time.h>
#include <esp_wifi.h>
#include <driver/rtc_io.h>
#include <hal/gpio_hal.h>

// ===================================================================
// VERSION MANAGEMENT
// ===================================================================
const char* FIRMWARE_VERSION = "2.1.10";
const char* PROJECT_NAME = "HexOclock";
const char* BUILD_DATE = __DATE__;
const char* BUILD_TIME = __TIME__;

void printStartupInfo() {
  Serial.println("\n" "====================================================");
  Serial.printf("  %s v%s\n", PROJECT_NAME, FIRMWARE_VERSION);
  Serial.printf("  Build: %s %s\n", BUILD_DATE, BUILD_TIME);
  Serial.println("  ESP32-S3 Zero with VEML7700 + LIS3DH");
  Serial.println("====================================================\n");
}

// ===================================================================
// 1. HARDWARE PIN-MAPPING
// ===================================================================
const int rowPins[5] = {2, 3, 4, 5, 6};      // R0-R4 (bottom to top)
const int colPins[5] = {7, 8, 9, 10, 11};   // C0-C4 (left to right)

#define PIN_WAKEUP_INPUT 1                  

#define I2C_SDA 12                          
#define I2C_SCL 13                          
#define PIN_BATTERIE_MESSUNG 13

Adafruit_VEML7700 veml = Adafruit_VEML7700();
Adafruit_LIS3DH lis = Adafruit_LIS3DH();

uint8_t lis3dh_i2c_addr = 0x18;

// ===================================================================
// 2. MATRIX-KOORDINATEN & SPEICHER
// ===================================================================
struct Point { int row; int col; };

Point einzelMinuten[9] = { 
  {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, 
  {4,4}, {4,3}, {4,2}, {4,1} 
};
Point zehnerMinuten[5] = { 
  {0,0}, {0,1}, {0,2}, {0,3}, {0,4} 
};

Point einerStunden[5] = { 
  {2,0}, {2,1}, {2,2}, {3,3}, {3,4}
};
Point sechserStunden[3] = { 
  {3,0}, {3,1}, {3,2} 
};
const Point sekundenLedLinks = {2,3};
const Point sekundenLedRechts = {2,4};

const uint8_t wMuster[5][5] = {
  {0, 1, 0, 1, 0}, 
  {0, 0, 1, 0, 1}, 
  {0, 0, 0, 0, 0}, 
  {0, 0, 0, 0, 0}, 
  {0, 0, 0, 1, 0}  
};

volatile uint8_t currentFrame[5][5] = {0}; 
const uint32_t REIHE_GESAMT_ZEIT = 300;       
volatile uint32_t ledAnZeit_us = 100;         
hw_timer_t * timer = NULL;

RTC_DATA_ATTR int letzterSyncTag = -1;       

bool isBatterieBetrieb = true; 
unsigned long anzeigeTimer = 0;
unsigned long maxAnzeigeZeit = 20000; 
unsigned long bootTimeMs = 0;
unsigned long startupPhaseTimer = 0;
bool startupInitialized = false;
bool initializationPhaseStateCleared = false;

// Datums- und OTA-Steuerung via Klopfen
unsigned long datumMenueTimer = 0;
unsigned long ersterKabelKlopfTimer = 0;
unsigned long letzterKabelKlopfTimer = 0;
unsigned long otaStartTimer = 0; 
bool datumAnzeigeAktiv = false;
bool otaModusAktiviert = false; 
bool kabelTapReleaseRequired = false;

float displayHelligkeiten[5][5] = {0.0};
const float FADE_SPEED = 1.2; 

bool ntpSyncErforderlich = false;
bool ntpSyncAktiv = false;
bool ntpConfigured = false;
bool otaGestartet = false;
unsigned long ntpStartTimer = 0;

unsigned long batteryReadTimer = 0;
const unsigned long BATTERY_READ_INTERVAL = 500;
int cachedBatteryValue = 0;

// Timing constants
const unsigned long OTA_SESSION_TIMEOUT = 60000;
const unsigned long STARTUP_PHASE_MS = 2000;
const unsigned long INITIALIZATION_PHASE_MS = 3000;
const unsigned long OTA_ARM_DELAY_MS = 500;
const unsigned long TAP_DEBOUNCE_MS = 500;
const uint8_t G_SENSOR_CLICK_THRESHOLD = 200;
const uint8_t SECONDS_LED_BRIGHTNESS = 40;

// ===================================================================
// 3. HILFSFUNKTIONEN
// ===================================================================
void matrixAusschalten() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(rowPins[i], HIGH); 
    digitalWrite(colPins[i], LOW);  
  }
}

int leseBatterieSicher() {
  Wire.end();
  delay(20);
  long summe = 0;
  for(int i = 0; i < 10; i++) {
    summe += analogRead(PIN_BATTERIE_MESSUNG);
    delayMicroseconds(50);
  }
  int messwert = summe / 10;
  Wire.begin(I2C_SDA, I2C_SCL);
  return messwert;
}

void writeI2CDirect(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void shutdownSensors() {
  Serial.println("[SLEEP] Shutting down sensors...");
  writeI2CDirect(lis3dh_i2c_addr, 0x20, 0x00);
  delay(10);
  Wire.beginTransmission(0x10);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(0x01);
  Wire.endTransmission();
  delay(10);
  Serial.println("[SLEEP] Sensors powered down");
}

void resetDateAndOtaState() {
  datumAnzeigeAktiv = false;
  otaModusAktiviert = false;
  datumMenueTimer = 0;
  ersterKabelKlopfTimer = 0;
  letzterKabelKlopfTimer = 0;
  kabelTapReleaseRequired = false;
}

bool hasTimeElapsed(unsigned long now, unsigned long start, unsigned long duration) {
  return (unsigned long)(now - start) >= duration;
}

bool isWithinDuration(unsigned long now, unsigned long start, unsigned long duration) {
  return (unsigned long)(now - start) < duration;
}

void IRAM_ATTR onTimer() {
  static int aktiveReihe = 0;
  static bool ledPhase = false;
  static int bamCounter = 0; 
  
  if (!ledPhase) {
    gpio_set_level((gpio_num_t)rowPins[aktiveReihe], HIGH);
    aktiveReihe++;
    if (aktiveReihe >= 5) aktiveReihe = 0;
    bamCounter++;
    if (bamCounter >= 31) bamCounter = 0;
    
    bool reiheHatAktivitaet = false;
    for (int c = 0; c < 5; c++) {
      if (currentFrame[aktiveReihe][c] > bamCounter) { 
        gpio_set_level((gpio_num_t)colPins[c], HIGH); 
        reiheHatAktivitaet = true; 
      } else { 
        gpio_set_level((gpio_num_t)colPins[c], LOW); 
      }
    }
    
    uint32_t anZeit = ledAnZeit_us; 
    if (aktiveReihe == 2 || aktiveReihe == 3) {
      anZeit = (anZeit * 150) / 100;
    } else if (aktiveReihe == 4) {
      anZeit = (anZeit * 4) / 5;     
    }
    
    if (reiheHatAktivitaet && anZeit > 5) { 
      gpio_set_level((gpio_num_t)rowPins[aktiveReihe], LOW); 
      ledPhase = true;
      timerAlarm(timer, anZeit, true, 0);
    } else {
      timerAlarm(timer, REIHE_GESAMT_ZEIT, true, 0);
    }
  } else {
    gpio_set_level((gpio_num_t)rowPins[aktiveReihe], HIGH);
    for (int c = 0; c < 5; c++) {
      gpio_set_level((gpio_num_t)colPins[c], LOW);
    }
    ledPhase = false;
    
    uint32_t anZeit = ledAnZeit_us;
    if (aktiveReihe == 2 || aktiveReihe == 3) {
      anZeit = (anZeit * 125) / 100;
    } else if (aktiveReihe == 4) {
      anZeit = (anZeit * 4) / 5;
    }
    
    uint32_t restZeit = (anZeit < REIHE_GESAMT_ZEIT) ? (REIHE_GESAMT_ZEIT - anZeit) : 20;
    if (restZeit < 20) restZeit = 20; 
    timerAlarm(timer, restZeit, true, 0);
  }
}

void setupOTA() {
  if (otaGestartet) return; 
  ArduinoOTA.setHostname("HexOclock");
  ArduinoOTA.begin();
  otaGestartet = true;
  otaStartTimer = millis(); 
  Serial.println("[OTA] Service aktiv.");
}

void stoppeOTA() {
  if (!otaGestartet) return;
  ArduinoOTA.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  otaGestartet = false;
  otaModusAktiviert = false;
  Serial.println("[OTA] Service beendet.");
}

void holeNTPZeit() {
  WiFiManager wm;
  wm.setTimeout(12); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 

  if(wm.autoConnect("HexOclock-Setup")) {
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
    int versuche = 0;
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    while (timeinfo->tm_year < 120 && versuche < 10) { 
      vTaskDelay(pdMS_TO_TICKS(500)); 
      now = time(nullptr);
      timeinfo = localtime(&now);
      versuche++;
    }
    if (timeinfo->tm_year >= 120) {
      letzterSyncTag = timeinfo->tm_mday; 
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return;
    }
  }
  
  struct tm tm_test;
  tm_test.tm_year = 2026 - 1900;
  tm_test.tm_mon = 5;
  tm_test.tm_mday = 21;          
  tm_test.tm_hour = 12;
  tm_test.tm_min = 34;
  tm_test.tm_sec = 0;
  tm_test.tm_isdst = 1;          
  time_t t_test = mktime(&tm_test);
  struct timeval tv = { .tv_sec = t_test, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ===================================================================
// 4. SETUP
// ===================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  bootTimeMs = millis();
  
  printStartupInfo();
  
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  rtc_gpio_hold_dis((gpio_num_t)PIN_WAKEUP_INPUT);

  for (int i = 0; i < 5; i++) {
    pinMode(rowPins[i], OUTPUT);
    pinMode(colPins[i], OUTPUT);
  }
  matrixAusschalten();

  timer = timerBegin(1000000); 
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 100, true, 0); 
  timerStart(timer);

  pinMode(PIN_WAKEUP_INPUT, INPUT_PULLDOWN);
  delay(50); 
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    isBatterieBetrieb = false;
    Serial.println("[POWER] Cable power detected");
  } else {
    isBatterieBetrieb = true;
    Serial.println("[POWER] Battery power detected");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  
  if (veml.begin()) {
    veml.setGain(VEML7700_GAIN_1_8); 
    veml.setIntegrationTime(VEML7700_IT_25MS);
    Serial.println("[I2C] VEML7700 initialized");
  } else {
    Serial.println("[ERROR] VEML7700 not found!");
  }
  
  bool sensorGefunden = false;
  if (lis.begin(0x18)) {
    lis3dh_i2c_addr = 0x18;
    sensorGefunden = true;
    Serial.println("[I2C] LIS3DH found at 0x18");
  } 
  else if (lis.begin(0x19)) {
    lis3dh_i2c_addr = 0x19;
    sensorGefunden = true;
    Serial.println("[I2C] LIS3DH found at 0x19");
  }

  if (sensorGefunden) {
    lis.setRange(LIS3DH_RANGE_2_G);
    writeI2CDirect(lis3dh_i2c_addr, 0x22, 0x80); 
    writeI2CDirect(lis3dh_i2c_addr, 0x25, 0x00);
    lis.setClick(1, G_SENSOR_CLICK_THRESHOLD, 20, 25, 150); 
    writeI2CDirect(lis3dh_i2c_addr, 0x3A, 0x0B);
    lis.getClick();
    Serial.println("[SENSOR] LIS3DH click detection configured");
  } else {
    Serial.println("[ERROR] LIS3DH not found!");
  }

  analogSetAttenuation(ADC_11db);
  anzeigeTimer = millis();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    time_t nun = time(nullptr);
    struct tm* timeinfo = localtime(&nun);
    if (timeinfo->tm_year < 120) {
      Serial.println("[TIME] Woken from sleep, time invalid - syncing NTP");
      if (!isBatterieBetrieb) holeNTPZeit();
      else ntpSyncErforderlich = true;
    }
  } else {
    Serial.println("[TIME] Cold start - syncing NTP");
    holeNTPZeit(); 
  }

  startupPhaseTimer = millis();
  Serial.println("[SETUP] Initialization complete!\n");
}

// ===================================================================
// 5. MAIN LOOP
// ===================================================================
unsigned long sensorTimer = 0;
float geglaettetesDelay = 100.0;

void loop() {
  unsigned long jetzt = millis();
  time_t nun = time(nullptr);
  struct tm* timeinfo = localtime(&nun);

  if (!startupInitialized && hasTimeElapsed(jetzt, startupPhaseTimer, STARTUP_PHASE_MS)) {
    startupInitialized = true;
    resetDateAndOtaState();
    Serial.println("[STARTUP] Awake indicator complete - normal display active");
  }
  bool initializationPhaseActive = !startupInitialized || isWithinDuration(jetzt, bootTimeMs, INITIALIZATION_PHASE_MS);

  if (!isBatterieBetrieb && otaModusAktiviert && otaGestartet) {
    ArduinoOTA.handle();
  }

  // --- CABLE TAP CONTROL ---
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    if (isBatterieBetrieb) {
      isBatterieBetrieb = false;
      resetDateAndOtaState();
      Serial.println("[MODE] Switched to cable power");
    }
    
    bool tapDetected = lis.getClick();
    
    // Ignore taps during initialization
    if (initializationPhaseActive) {
      if (tapDetected) {
        Serial.println("[CLICK] Ignoring tap during initialization phase");
      }
      if (!initializationPhaseStateCleared) {
        resetDateAndOtaState();
        initializationPhaseStateCleared = true;
      }
    } else {
      initializationPhaseStateCleared = false;
      
      // CRITICAL: Only process tap if kabelTapReleaseRequired is false
      // This prevents stuck-high sensor from re-triggering taps
      if (!tapDetected) {
        kabelTapReleaseRequired = false;
      }
      
      if (tapDetected && !kabelTapReleaseRequired) {
        // Check debounce
        if (letzterKabelKlopfTimer != 0 && isWithinDuration(jetzt, letzterKabelKlopfTimer, TAP_DEBOUNCE_MS)) {
          Serial.println("[CLICK] Ignoring tap burst from the same shock event");
          letzterKabelKlopfTimer = jetzt;
        }
        // First tap: enter date display
        else if (!datumAnzeigeAktiv && !otaModusAktiviert) {
          Serial.println("[CLICK] 1st tap: Showing date for 7 seconds");
          datumAnzeigeAktiv = true;
          datumMenueTimer = jetzt;
          ersterKabelKlopfTimer = jetzt;
          letzterKabelKlopfTimer = jetzt;
          kabelTapReleaseRequired = true;
        }
        // Second tap during date display: enter OTA
        else if (datumAnzeigeAktiv && !otaModusAktiviert && ersterKabelKlopfTimer != 0 && hasTimeElapsed(jetzt, ersterKabelKlopfTimer, OTA_ARM_DELAY_MS)) {
          Serial.println("[CLICK] 2nd tap during date display: Starting OTA...");
          otaModusAktiviert = true;
          datumAnzeigeAktiv = false;
          letzterKabelKlopfTimer = jetzt;
          kabelTapReleaseRequired = true;
          WiFi.mode(WIFI_STA);
          WiFi.begin();
          setupOTA();
        } 
        // Tap too early during OTA arm delay: ignore
        else if (datumAnzeigeAktiv && !otaModusAktiviert) {
          Serial.println("[CLICK] Ignoring follow-up tap during OTA arm delay");
          letzterKabelKlopfTimer = jetzt;
        }
      }
    }
  } else {
    // Switched to battery power
    if (!isBatterieBetrieb) {
      isBatterieBetrieb = true;
      resetDateAndOtaState();
      stoppeOTA();
      anzeigeTimer = millis();
      Serial.println("[MODE] Switched to battery power");
    }
  }

  // --- NTP SYNC ---
  if (ntpSyncErforderlich && !ntpSyncAktiv) {
    ntpSyncAktiv = true;
    ntpConfigured = false;
    ntpStartTimer = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(); 
  }
  
  if (ntpSyncAktiv) {
    if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
      ntpConfigured = true;
    }
    
    if (ntpConfigured) {
      time_t now = time(nullptr);
      struct tm* ti = localtime(&now);
      if (ti->tm_year >= 120) {
        ntpSyncErforderlich = false;
        ntpSyncAktiv = false;
        ntpConfigured = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[NTP] Time synced successfully");
      } else if (millis() - ntpStartTimer > 6000) {
        ntpSyncAktiv = false;
        ntpConfigured = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[NTP] Sync timeout");
      }
    } else if (millis() - ntpStartTimer > 6000) {
      ntpSyncAktiv = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[NTP] WiFi connection failed");
    }
  }

  // --- DEEP SLEEP (BATTERY ONLY) ---
  if (isBatterieBetrieb && (millis() - anzeigeTimer > maxAnzeigeZeit)) {
    Serial.println("[SLEEP] Entering deep sleep...");
    matrixAusschalten();
    lis.getClick(); 
    shutdownSensors();
    
    uint64_t pin_mask = (1ULL << PIN_WAKEUP_INPUT);
    esp_sleep_enable_ext1_wakeup(pin_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    
    rtc_gpio_init((gpio_num_t)PIN_WAKEUP_INPUT);
    rtc_gpio_set_direction((gpio_num_t)PIN_WAKEUP_INPUT, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_en((gpio_num_t)PIN_WAKEUP_INPUT);
    rtc_gpio_pullup_dis((gpio_num_t)PIN_WAKEUP_INPUT);
    rtc_gpio_hold_en((gpio_num_t)PIN_WAKEUP_INPUT); 
    
    uint64_t sleepTime_us = 15ULL * 60ULL * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleepTime_us); 
    esp_deep_sleep_start();
  }

  // --- SENSORS ---
  if (millis() - sensorTimer > 100) {
    sensorTimer = millis();
    float lux = veml.readLux();
    float constLux = constrain(lux, 0.3, 300.0); 
    float expFaktor = constLux / 300.0;
    expFaktor = expFaktor * expFaktor; 
    int zielDelay = 45 + (int)(expFaktor * (260 - 45)); 
    geglaettetesDelay = (geglaettetesDelay * 0.92) + (zielDelay * 0.08);
    
    portDISABLE_INTERRUPTS();
    ledAnZeit_us = (uint32_t)geglaettetesDelay;
    portENABLE_INTERRUPTS();
  }

  if (millis() - batteryReadTimer > BATTERY_READ_INTERVAL) {
    batteryReadTimer = millis();
    cachedBatteryValue = leseBatterieSicher();
  }

  // --- MATRIX FRAME ---
  uint8_t targetFrame[5][5] = {0}; 
  
  // STATE 0: Startup - both seconds LEDs
  if (!startupInitialized) {
    targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 31;
    targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 31;
  }
  // STATE 1: OTA active
  else if (!isBatterieBetrieb && otaModusAktiviert) {
    float sinusWelle = sin(millis() / 200.0); 
    uint8_t waberHelligkeit = 24 + (uint8_t)(sinusWelle * 7.0 + 0.5); 
    for (int r = 0; r < 5; r++) {
      for (int c = 0; c < 5; c++) {
        if (wMuster[r][c] == 1) targetFrame[r][c] = waberHelligkeit;
      }
    }
    if (millis() - otaStartTimer > OTA_SESSION_TIMEOUT) {
      Serial.println("[OTA] Session timeout reached");
      stoppeOTA();
    }
  } 
  // STATE 2: Date display active (cable only, 7 sec)
  else if (!isBatterieBetrieb && datumAnzeigeAktiv) {
    if (millis() - datumMenueTimer >= 7000) {
      datumAnzeigeAktiv = false;
    } else {
      int tag = timeinfo->tm_mday;
      int monat = timeinfo->tm_mon + 1;
      
      int eTag = tag % 10;
      for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10;
      for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      
      int eMon = monat % 6;
      for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6;
      for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 31;
    }
  } 
  // STATE 3: Cable mode - always show time
  else if (!isBatterieBetrieb) {
    nun = time(nullptr);
    timeinfo = localtime(&nun);
    
    int eMin = timeinfo->tm_min % 10;
    for(int i=0; i<eMin; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
    int zMin = timeinfo->tm_min / 10;
    for(int i=0; i<zMin; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
    int eStd = timeinfo->tm_hour % 6;
    for(int i=0; i<eStd; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
    int sStd = timeinfo->tm_hour / 6;
    for(int i=0; i<sStd; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
    
    int batVal = cachedBatteryValue;
    if (batVal < 3150) {
      int takt = timeinfo->tm_sec % 3;
      if (takt == 0) { 
        targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = SECONDS_LED_BRIGHTNESS; 
        targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
      }
      else if (takt == 1) { 
        targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
        targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = SECONDS_LED_BRIGHTNESS; 
      }
      else { 
        targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
        targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
      }
    } else {
      if (timeinfo->tm_sec % 2 == 0) { 
        targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = SECONDS_LED_BRIGHTNESS; 
        targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
      }
      else { 
        targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
        targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = SECONDS_LED_BRIGHTNESS; 
      }
    }
  }
  // STATE 4: Battery mode - time/date cycling
  else {
    unsigned long abgelaufeneZeit = millis() - anzeigeTimer;

    if (abgelaufeneZeit < 10000) {
      nun = time(nullptr);
      timeinfo = localtime(&nun);
      
      int eMin = timeinfo->tm_min % 10;
      for(int i=0; i<eMin; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zMin = timeinfo->tm_min / 10;
      for(int i=0; i<zMin; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eStd = timeinfo->tm_hour % 6;
      for(int i=0; i<eStd; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sStd = timeinfo->tm_hour / 6;
      for(int i=0; i<sStd; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      int batVal = cachedBatteryValue;
      if (batVal < 3150) {
        int takt = timeinfo->tm_sec % 3;
        if (takt == 0) { 
          targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = SECONDS_LED_BRIGHTNESS; 
          targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
        }
        else if (takt == 1) { 
          targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
          targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = SECONDS_LED_BRIGHTNESS; 
        }
        else { 
          targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
          targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
        }
      } else {
        if (timeinfo->tm_sec % 2 == 0) { 
          targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = SECONDS_LED_BRIGHTNESS; 
          targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = 0; 
        }
        else { 
          targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 0; 
          targetFrame[sekundenLedRechts.row][sekundenLedRechts.col] = SECONDS_LED_BRIGHTNESS; 
        }
      }
    }
    else if (abgelaufeneZeit >= 10000 && abgelaufeneZeit < 15000) {
      nun = time(nullptr);
      timeinfo = localtime(&nun);
      
      int tag = timeinfo->tm_mday;
      int monat = timeinfo->tm_mon + 1;
      int eTag = tag % 10;
      for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10;
      for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eMon = monat % 6;
      for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6;
      for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      targetFrame[sekundenLedLinks.row][sekundenLedLinks.col] = 31;
    }
    else {
      int rawBat = cachedBatteryValue;
      if (rawBat >= 2650) {
        int grueneLeds = map(rawBat, 2650, 3200, 1, 9);
        grueneLeds = constrain(grueneLeds, 1, 9);
        for(int i = 0; i < grueneLeds; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      } else {
        int roteLeds = map(rawBat, 2300, 2649, 1, 5);
        roteLeds = constrain(roteLeds, 1, 5);
        for(int i = 0; i < roteLeds; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      }
    }
  }

  // --- INTERPOLATION ---
  uint8_t interpoliertesFrame[5][5] = {0};
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 5; c++) {
      float ziel = (float)targetFrame[r][c];
      if (displayHelligkeiten[r][c] < ziel) {
        displayHelligkeiten[r][c] += FADE_SPEED;
        if (displayHelligkeiten[r][c] > ziel) displayHelligkeiten[r][c] = ziel;
      } else if (displayHelligkeiten[r][c] > ziel) {
        displayHelligkeiten[r][c] -= FADE_SPEED;
        if (displayHelligkeiten[r][c] < ziel) displayHelligkeiten[r][c] = ziel;
      }
      float normierterWert = displayHelligkeiten[r][c] / 31.0; 
      float korrigierterWert = normierterWert * normierterWert; 
      interpoliertesFrame[r][c] = (uint8_t)(korrigierterWert * 31.0 + 0.5);
    }
  }

  // --- UPDATE FRAME ---
  portDISABLE_INTERRUPTS(); 
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 5; c++) currentFrame[r][c] = interpoliertesFrame[r][c];
  }
  portENABLE_INTERRUPTS(); 

  delay(20); 
}
