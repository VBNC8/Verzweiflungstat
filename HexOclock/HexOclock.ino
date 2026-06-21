// =======================================================================================
// PROJEKT:      HexOclock (ESP32-S3 Waveshare Zero)
// VERSION:      v2.1.2 (Fixes: OTA startup bug, OTA timeout, OTA knock-only access, G-sensor)
// BESCHREIBUNG: Energiesparende Hexagonal-LED-Uhr mit Helligkeits- und Lagesensor.
//               STARTUP: Startet direkt in die Uhrzeit. Kein Menü, kein Akku, kein Datum.
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
const char* FIRMWARE_VERSION = "2.1.2";
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
#define PIN_BATTERIE_MESSUNG 13             // REVERTED: Pin 13 works - either I2C or battery, never simultaneous

Adafruit_VEML7700 veml = Adafruit_VEML7700();
Adafruit_LIS3DH lis = Adafruit_LIS3DH();

uint8_t lis3dh_i2c_addr = 0x18;

// ===================================================================
// 2. MATRIX-KOORDINATEN & SPEICHER (Based on Physical Layout)
// ===================================================================
// Physical LED Layout:
// R3 (Red): [5 LEDs - Hours/Months display]
// R2 (Red): [5 LEDs - Hours/Months display]
// R4 (Orange): [5 LEDs - Seconds indicator]
// R3 (Orange): [2 LEDs - Seconds indicator]
// R1 (Green): [10 LEDs - Minutes/Days display]
// R0 (Green): [10 LEDs - Minutes/Days display]

struct Point { int row; int col; };

// MINUTES/DAYS (Green LEDs - R0, R1) - 9 individual units (0-9 days) + 5 tens (0-50 days)
Point einzelMinuten[9] = { 
  {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, 
  {0,4}, {0,3}, {0,2}, {0,1} 
}; 
Point zehnerMinuten[5] = { 
  {0,0}, {0,1}, {0,2}, {0,3}, {0,4} 
};

// HOURS/MONTHS (Red LEDs - R2, R3) - 5 individual units (0-5 hours/months) + 3 sixes (0-18 hours / 0-12 months)
Point einerStunden[5] = { 
  {2,0}, {2,1}, {2,2}, {2,3}, {2,4} 
};
Point sechserStunden[3] = { 
  {3,0}, {3,1}, {3,2} 
};

// SECONDS (Orange LEDs - R4, R3 center) - 2 LEDs for blink indicator
Point secondsIndicator[2] = {
  {4,3}, {4,4}  // Left and right indicator LEDs
};

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

// Datums- und OTA-Steuerung via Klopfen
unsigned long datumMenueTimer = 0;
unsigned long otaStartTimer = 0; 
bool datumAnzeigeAktiv = false;
bool otaModusAktiviert = false; 

float displayHelligkeiten[5][5] = {0.0};
const float FADE_SPEED = 1.2; 

bool ntpSyncErforderlich = false;
bool ntpSyncAktiv = false;
bool ntpConfigured = false;  // FIXED: Flag to call configTzTime only once
bool otaGestartet = false;
unsigned long ntpStartTimer = 0;

// FIXED: Battery reading optimization
unsigned long batteryReadTimer = 0;
const unsigned long BATTERY_READ_INTERVAL = 500;  // Read every 500ms
int cachedBatteryValue = 0;

// OTA session timeout: 60 seconds before returning to time display
const unsigned long OTA_SESSION_TIMEOUT = 60000;
// Require a short pause before a 2nd tap can arm OTA, so one knock cannot trigger both actions
const unsigned long OTA_ARM_DELAY_MS = 800;
// Strongly reduced click sensitivity to avoid false positives on cable vibrations
const uint8_t G_SENSOR_CLICK_THRESHOLD = 80;

// ===================================================================
// 3. HILFSFUNKTIONEN
// ===================================================================
void matrixAusschalten() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(rowPins[i], HIGH); 
    digitalWrite(colPins[i], LOW);  
  }
}

// FIXED: Increased delay from 2ms to 20ms, read every 500ms instead of every loop
int leseBatterieSicher() {
  Wire.end();
  delay(20);  // Increased stabilization time
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

// FIXED: Proper sensor shutdown before deep sleep
void shutdownSensors() {
  Serial.println("[SLEEP] Shutting down sensors...");
  
  // Power down LIS3DH accelerometer
  writeI2CDirect(lis3dh_i2c_addr, 0x20, 0x00);  // CTRL_REG1 = 0x00 (power down)
  delay(10);
  
  // Power down VEML7700 ambient light sensor
  Wire.beginTransmission(0x10);  // VEML7700 I2C address
  Wire.write(0x00);              // ALS_CONF register
  Wire.write(0x00);
  Wire.write(0x01);              // Shutdown mode
  Wire.endTransmission();
  delay(10);
  
  Serial.println("[SLEEP] Sensors powered down");
}

// FIXED: Use GPIO registers for faster, safer ISR control
void IRAM_ATTR onTimer() {
  static int aktiveReihe = 0;
  static bool ledPhase = false;
  static int bamCounter = 0; 
  
  if (!ledPhase) {
    // Set row HIGH (disable row)
    gpio_set_level((gpio_num_t)rowPins[aktiveReihe], HIGH);
    
    aktiveReihe++;
    if (aktiveReihe >= 5) aktiveReihe = 0;
    bamCounter++;
    if (bamCounter >= 31) bamCounter = 0;
    
    bool reiheHatAktivitaet = false;
    
    // Set columns based on current frame data
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
      anZeit = (anZeit * 115) / 100; 
    } else if (aktiveReihe == 4) {
      anZeit = (anZeit * 4) / 5;     
    }
    
    if (reiheHatAktivitaet && anZeit > 5) { 
      // Row LOW (enable row) - turn on LEDs
      gpio_set_level((gpio_num_t)rowPins[aktiveReihe], LOW); 
      ledPhase = true;
      timerAlarm(timer, anZeit, true, 0);
    } else {
      timerAlarm(timer, REIHE_GESAMT_ZEIT, true, 0);
    }
  } else {
    // Turn off row and all columns
    gpio_set_level((gpio_num_t)rowPins[aktiveReihe], HIGH);
    for (int c = 0; c < 5; c++) {
      gpio_set_level((gpio_num_t)colPins[c], LOW);
    }
    ledPhase = false;
    
    uint32_t anZeit = ledAnZeit_us;
    if (aktiveReihe == 2 || aktiveReihe == 3) {
      anZeit = (anZeit * 115) / 100;
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

// Stops OTA service and disconnects WiFi
void stoppeOTA() {
  if (!otaGestartet) return;
  ArduinoOTA.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  otaGestartet = false;
  otaModusAktiviert = false;
  Serial.println("[OTA] Service beendet.");
}

// FIXED: NTP sync - only call configTzTime once after WiFi connects
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
  
  // Fallback time if NTP fails
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
  delay(500);  // Wait for Serial to initialize
  
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
    Serial.println("[POWER] Cable power detected (Akkubetrieb = false)");
  } else {
    isBatterieBetrieb = true;
    Serial.println("[POWER] Battery power detected (Akkubetrieb = true)");
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
    // Strongly reduced G-sensor sensitivity to avoid false positives
    lis.setClick(1, G_SENSOR_CLICK_THRESHOLD, 20, 25, 150); 
    writeI2CDirect(lis3dh_i2c_addr, 0x3A, 0x0B);
    lis.getClick();
    Serial.println("[SENSOR] LIS3DH click detection configured (reduced sensitivity)");
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
  
  Serial.println("[SETUP] Initialization complete!\n");
}

// ===================================================================
// 5. MAIN LOOP
// ===================================================================
unsigned long sensorTimer = 0;
float geglaettetesDelay = 100.0;

void loop() {
  // FIXED: Refresh time info fresh per loop
  time_t nun = time(nullptr);
  struct tm* timeinfo = localtime(&nun);

  // OTA is only active when explicitly triggered by 2nd knock during date display
  if (!isBatterieBetrieb && otaModusAktiviert && otaGestartet) {
    ArduinoOTA.handle();
  }

  // --- STRIKTE KABEL- UND KLOPF-STEUERUNG ---
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    if (isBatterieBetrieb) {
      isBatterieBetrieb = false;
      datumAnzeigeAktiv = false;
      Serial.println("[MODE] Switched to cable power");
    }
    
    // Klopfen im Kabelmodus abfragen
    if (lis.getClick()) {
      unsigned long jetzt = millis();
      if (!datumAnzeigeAktiv && !otaModusAktiviert) {
        Serial.println("[CLICK] 1st tap: Showing date for 7 seconds");
        datumAnzeigeAktiv = true;
        datumMenueTimer = jetzt;
      } 
      else if (datumAnzeigeAktiv && !otaModusAktiviert && (jetzt - datumMenueTimer >= OTA_ARM_DELAY_MS)) {
        Serial.println("[CLICK] 2nd tap during date display: Starting OTA...");
        otaModusAktiviert = true;
        datumAnzeigeAktiv = false;
        WiFi.mode(WIFI_STA);
        WiFi.begin(); // Uses stored credentials from WiFiManager (saved in ESP32 flash)
        setupOTA();   // Also sets otaStartTimer = millis() for the 60s timeout
      } else if (datumAnzeigeAktiv && !otaModusAktiviert) {
        Serial.println("[CLICK] Ignoring follow-up tap during OTA arm delay");
      }
    }
  } else {
    // Wechsel in den Akkubetrieb
    if (!isBatterieBetrieb) {
      isBatterieBetrieb = true;
      datumAnzeigeAktiv = false;
      otaModusAktiviert = false;
      stoppeOTA();
      anzeigeTimer = millis();
      Serial.println("[MODE] Switched to battery power");
    }
  }

  // --- BACKGROUND SYNC (FIXED: Only call configTzTime once) ---
  if (ntpSyncErforderlich && !ntpSyncAktiv) {
    ntpSyncAktiv = true;
    ntpConfigured = false;  // Reset flag
    ntpStartTimer = millis();
    WiFi.mode(WIFI_STA);
    WiFi.begin(); 
  }
  
  if (ntpSyncAktiv) {
    if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
      // Configure NTP only ONCE after connection
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
      ntpConfigured = true;
    }
    
    if (ntpConfigured) {
      time_t now = time(nullptr);
      struct tm* ti = localtime(&now);
      if (ti->tm_year >= 120) {
        // Time successfully synced
        ntpSyncErforderlich = false;
        ntpSyncAktiv = false;
        ntpConfigured = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[NTP] Time synced successfully");
      } else if (millis() - ntpStartTimer > 6000) {
        // Timeout after 6 seconds
        ntpSyncAktiv = false;
        ntpConfigured = false;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.println("[NTP] Sync timeout");
      }
    } else if (millis() - ntpStartTimer > 6000) {
      // Failed to connect to WiFi
      ntpSyncAktiv = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      Serial.println("[NTP] WiFi connection failed");
    }
  }

  // --- A. TIEFSCHLAF (NUR IM AKKUBETRIEB) ---
  if (isBatterieBetrieb && (millis() - anzeigeTimer > maxAnzeigeZeit)) {
    Serial.println("[SLEEP] Entering deep sleep...");
    matrixAusschalten();
    lis.getClick(); 
    
    // FIXED: Shutdown sensors before sleep
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

  // --- B. SENSOREN (FIXED: Battery reading optimized) ---
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

  // FIXED: Read battery only every 500ms, not every loop iteration
  if (millis() - batteryReadTimer > BATTERY_READ_INTERVAL) {
    batteryReadTimer = millis();
    cachedBatteryValue = leseBatterieSicher();
  }

  // --- C. MATRIX FRAME BAUEN ---
  uint8_t targetFrame[5][5] = {0}; 
  
  // ZUSTAND 1: OTA Modus aktiv (Waberndes W)
  if (!isBatterieBetrieb && otaModusAktiviert) {
    float sinusWelle = sin(millis() / 200.0); 
    uint8_t waberHelligkeit = 24 + (uint8_t)(sinusWelle * 7.0 + 0.5); 
    for (int r = 0; r < 5; r++) {
      for (int c = 0; c < 5; c++) {
        if (wMuster[r][c] == 1) targetFrame[r][c] = waberHelligkeit;
      }
    }
    if (millis() - otaStartTimer > OTA_SESSION_TIMEOUT) stoppeOTA();
  } 
  // ZUSTAND 2: Datumsanzeige aktiv für 7 Sekunden (NUR nach Klopfen am Kabel)
  // Months are displayed like hours, days like minutes
  // Left second LED constantly on
  else if (!isBatterieBetrieb && datumAnzeigeAktiv) {
    if (millis() - datumMenueTimer >= 7000) {
      datumAnzeigeAktiv = false; // 7 Sekunden vorbei -> Zurück zur Uhrzeit
    } else {
      int tag = timeinfo->tm_mday;
      int monat = timeinfo->tm_mon + 1;
      
      // Days (0-31) displayed like MINUTES (Green LEDs - R0, R1)
      int eTag = tag % 10;
      for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10;
      for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      
      // Months (1-12) displayed like HOURS (Red LEDs - R2, R3)
      int eMon = monat % 6;
      for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6;
      for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      // Left second LED always on (Orange - R4[3])
      targetFrame[4][3] = 31;
    }
  } 
  // ZUSTAND 3: Normaler Uhrenbetrieb (Startzustand & Standard-Modus)
  else {
    unsigned long abgelaufeneZeit = millis() - anzeigeTimer;

    if (abgelaufeneZeit < 10000 || !isBatterieBetrieb) {
      // FIXED: Refresh time info for precise second checking
      nun = time(nullptr);
      timeinfo = localtime(&nun);
      
      // Uhrzeit anzeigen
      int eMin = timeinfo->tm_min % 10;
      for(int i=0; i<eMin; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zMin = timeinfo->tm_min / 10;
      for(int i=0; i<zMin; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eStd = timeinfo->tm_hour % 6;
      for(int i=0; i<eStd; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sStd = timeinfo->tm_hour / 6;
      for(int i=0; i<sStd; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      // --- SEKUNDEN-BLINKLOGIK (FIXED: use cached battery value) ---
      if (!isBatterieBetrieb) {
        int batVal = cachedBatteryValue;
        if (batVal < 3150) {
          // A. AKKU LÄDT: Dreiertakt (Sekunde % 3 -> Links, Rechts, Aus)
          int takt = timeinfo->tm_sec % 3;
          if (takt == 0) { targetFrame[4][3] = 6; targetFrame[4][4] = 0; } // Links an
          else if (takt == 1) { targetFrame[4][3] = 0; targetFrame[4][4] = 6; } // Rechts an
          else { targetFrame[4][3] = 0; targetFrame[4][4] = 0; } // Beide aus
        } else {
          // B. AKKU VOLL: Reines, rhythmisches Wechselblinken
          if (timeinfo->tm_sec % 2 == 0) { targetFrame[4][3] = 6; targetFrame[4][4] = 0; } 
          else { targetFrame[4][3] = 0; targetFrame[4][4] = 6; }
        }
      } else {
        // Akkubetrieb: Klassisches Wechselblinken
        if (timeinfo->tm_sec % 2 == 0) { targetFrame[4][3] = 6; targetFrame[4][4] = 0; } 
        else { targetFrame[4][3] = 0; targetFrame[4][4] = 6; }
      }
    }
    else if (abgelaufeneZeit >= 10000 && abgelaufeneZeit < 15000) {
      // Datum im Akkubetrieb (Wechselphase)
      // FIXED: Refresh time info
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
      
      // Left second LED always on
      targetFrame[4][3] = 31;
    }
    else {
      // Akku im Akkubetrieb (Wechselphase)
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

  // --- D. INTERPOLATION RECHNEN ---
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

  // --- E. BUFFER ÜBERGEBEN ---
  portDISABLE_INTERRUPTS(); 
  for (int r = 0; r < 5; r++) {
    for (int c = 0; c < 5; c++) currentFrame[r][c] = interpoliertesFrame[r][c];
  }
  portENABLE_INTERRUPTS(); 

  delay(20); 
}
