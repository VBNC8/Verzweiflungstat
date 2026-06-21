// =======================================================================================
// PROJEKT:      Cube-Clock (ESP32-S3 Waveshare Zero)
// VERSION:      v1.1.64 (Strict Specifications Edition)
// BESCHREIBUNG: Energiesparende Würfel-Uhr mit LED-Matrix, Helligkeits- und Lagesensor.
//               STARTUP: Startet direkt in die Uhrzeit. Kein Menü, kein Akku, kein Datum.
//               CABLE TAP: Klopfen am Kabel zeigt exakt 7 Sekunden das Datum.
//               OTA: Aktivierung NUR durch ein 2. Klopfen während dieser 7 Sekunden.
//               BLINK LOGIC: Am Kabel Dreiertakt (Links-Rechts-Aus) solange der Akku lädt.
//                            Bei vollem Akku (>= 3150) reines Wechselblinken (Links-Rechts).
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

// ===================================================================
// 1. HARDWARE PIN-MAPPING
// ===================================================================
const int rowPins[5] = {2, 3, 4, 5, 6};      
const int colPins[5] = {7, 8, 9, 10, 11};    

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
Point einzelMinuten[9] = { {1,0}, {1,1}, {1,2}, {1,3}, {1,4}, {4,4}, {4,3}, {4,2}, {4,1} }; 
Point zehnerMinuten[5] = { {0,0}, {0,1}, {0,2}, {0,3}, {0,4} };                                  
Point einerStunden[5]  = { {2,0}, {2,1}, {2,2}, {3,3}, {3,4} };                               
Point sechserStunden[3]= { {3,0}, {3,1}, {3,2} };                                  

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
bool otaGestartet = false;
unsigned long ntpStartTimer = 0;

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
  pinMode(PIN_BATTERIE_MESSUNG, INPUT);
  delay(2); 
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

void IRAM_ATTR onTimer() {
  static int aktiveReihe = 0;
  static bool ledPhase = false;
  static int bamCounter = 0; 
  
  if (!ledPhase) {
    digitalWrite(rowPins[aktiveReihe], HIGH);
    aktiveReihe++; if (aktiveReihe >= 5) aktiveReihe = 0;
    bamCounter++; if (bamCounter >= 31) bamCounter = 0;
    
    bool reiheHatAktivitaet = false;
    for (int c = 0; c < 5; c++) {
      if (currentFrame[aktiveReihe][c] > bamCounter) { 
        digitalWrite(colPins[c], HIGH); 
        reiheHatAktivitaet = true; 
      } else { 
        digitalWrite(colPins[c], LOW); 
      }
    }
    
    uint32_t anZeit = ledAnZeit_us; 
    if (aktiveReihe == 2 || aktiveReihe == 3) {
      anZeit = (anZeit * 115) / 100; 
    } else if (aktiveReihe == 4) {
      anZeit = (anZeit * 4) / 5;     
    }
    
    if (reiheHatAktivitaet && anZeit > 5) { 
      digitalWrite(rowPins[aktiveReihe], LOW); 
      ledPhase = true;
      timerAlarm(timer, anZeit, true, 0);
    } else {
      timerAlarm(timer, REIHE_GESAMT_ZEIT, true, 0);
    }
  } else {
    digitalWrite(rowPins[aktiveReihe], HIGH);
    for (int c = 0; c < 5; c++) digitalWrite(colPins[c], LOW);
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
  ArduinoOTA.setHostname("Cube-Clock");
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

  if(wm.autoConnect("Cube-Clock-Setup")) {
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
    int versuche = 0;
    time_t now = time(nullptr); struct tm* timeinfo = localtime(&now);
    while (timeinfo->tm_year < 120 && versuche < 10) { 
      vTaskDelay(pdMS_TO_TICKS(500)); 
      now = time(nullptr); timeinfo = localtime(&now);
      versuche++;
    }
    if (timeinfo->tm_year >= 120) {
      letzterSyncTag = timeinfo->tm_mday; 
      WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      return;
    }
  }
  
  struct tm tm_test;
  tm_test.tm_year = 2026 - 1900; tm_test.tm_mon = 5; tm_test.tm_mday = 21;          
  tm_test.tm_hour = 12; tm_test.tm_min = 34; tm_test.tm_sec = 0; tm_test.tm_isdst = 1;          
  time_t t_test = mktime(&tm_test);
  struct timeval tv = { .tv_sec = t_test, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
}

// ===================================================================
// 4. SETUP
// ===================================================================
void setup() {
  Serial.begin(115200);
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
  rtc_gpio_hold_dis((gpio_num_t)PIN_WAKEUP_INPUT);

  for (int i = 0; i < 5; i++) {
    pinMode(rowPins[i], OUTPUT); pinMode(colPins[i], OUTPUT);
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
  } else {
    isBatterieBetrieb = true;
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(50);
  veml.begin();
  veml.setGain(VEML7700_GAIN_1_8); 
  veml.setIntegrationTime(VEML7700_IT_25MS); 
  
  bool sensorGefunden = false;
  if (lis.begin(0x18)) { lis3dh_i2c_addr = 0x18; sensorGefunden = true; } 
  else if (lis.begin(0x19)) { lis3dh_i2c_addr = 0x19; sensorGefunden = true; }

  if (sensorGefunden) {
    lis.setRange(LIS3DH_RANGE_2_G);
    writeI2CDirect(lis3dh_i2c_addr, 0x22, 0x80); 
    writeI2CDirect(lis3dh_i2c_addr, 0x25, 0x00); 
    lis.setClick(2, 13, 15, 20, 150); 
    writeI2CDirect(lis3dh_i2c_addr, 0x3A, 0x0B);
    lis.getClick();
  }

  analogSetAttenuation(ADC_11db);
  anzeigeTimer = millis();

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);
    if (timeinfo->tm_year < 120) {
      if (!isBatterieBetrieb) holeNTPZeit();
      else ntpSyncErforderlich = true;
    }
  } else {
    holeNTPZeit(); 
  }
}

// ===================================================================
// 5. MAIN LOOP
// ===================================================================
unsigned long sensorTimer = 0;
float geglaettetesDelay = 100.0;

void loop() {
  time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);

  if (!isBatterieBetrieb && otaModusAktiviert && otaGestartet) {
    ArduinoOTA.handle();
  }

  // --- STRIKTE KABEL- UND KLOPF-STEUERUNG ---
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    if (isBatterieBetrieb) {
      isBatterieBetrieb = false;
      stoppeOTA();
      datumAnzeigeAktiv = false;
    }
    
    // Klopfen im Kabelmodus abfragen
    if (lis.getClick()) {
      if (!datumAnzeigeAktiv && !otaModusAktiviert) {
        // 1. Klopfen -> Schalte Datum für exakt 7 Sekunden an
        Serial.println("[CLICK] 1. Klopfen: Zeige Datum für 7s");
        datumAnzeigeAktiv = true;
        datumMenueTimer = millis();
      } 
      else if (datumAnzeigeAktiv && !otaModusAktiviert) {
        // 2. Klopfen WÄHREND der 7 Sekunden -> Direkt in den OTA Modus springen
        Serial.println("[CLICK] 2. Klopfen während Datumsphase! Starte OTA...");
        otaModusAktiviert = true;
        datumAnzeigeAktiv = false; 
        WiFi.mode(WIFI_STA); WiFi.begin(); 
        setupOTA();
      }
    }
  } else {
    // Wechsel in den Akkubetrieb
    if (!isBatterieBetrieb) {
      isBatterieBetrieb = true;
      datumAnzeigeAktiv = false;
      stoppeOTA();
      anzeigeTimer = millis(); 
    }
  }

  // --- BACKGROUND SYNC ---
  if (ntpSyncErforderlich && !ntpSyncAktiv) {
    ntpSyncAktiv = true;
    ntpStartTimer = millis();
    WiFi.mode(WIFI_STA); WiFi.begin(); 
  }
  if (ntpSyncAktiv) {
    if (WiFi.status() == WL_CONNECTED) {
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
      if (timeinfo->tm_year >= 120) {
        ntpSyncErforderlich = false; ntpSyncAktiv = false;
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      }
    } else if (millis() - ntpStartTimer > 6000) {
      ntpSyncAktiv = false; WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    }
  }

  // --- A. TIEFSCHLAF (NUR IM AKKUBETRIEB) ---
  if (isBatterieBetrieb && (millis() - anzeigeTimer > maxAnzeigeZeit)) {
    matrixAusschalten();
    lis.getClick(); 
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

  // --- B. SENSOREN ---
  if (millis() - sensorTimer > 100) {
    sensorTimer = millis();
    float lux = veml.readLux();
    float constLux = constrain(lux, 0.3, 300.0); 
    float expFaktor = constLux / 300.0; expFaktor = expFaktor * expFaktor; 
    int zielDelay = 45 + (int)(expFaktor * (260 - 45)); 
    geglaettetesDelay = (geglaettetesDelay * 0.92) + (zielDelay * 0.08);
    
    portDISABLE_INTERRUPTS();
    ledAnZeit_us = (uint32_t)geglaettetesDelay;
    portENABLE_INTERRUPTS();
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
    if (millis() - otaStartTimer > 60000) stoppeOTA();
  } 
  // ZUSTAND 2: Datumsanzeige aktiv für 7 Sekunden (NUR nach Klopfen am Kabel)
  else if (!isBatterieBetrieb && datumAnzeigeAktiv) {
    if (millis() - datumMenueTimer >= 7000) {
      datumAnzeigeAktiv = false; // 7 Sekunden vorbei -> Zurück zur Uhrzeit
    } else {
      int tag = timeinfo->tm_mday; int monat = timeinfo->tm_mon + 1;
      
      // Tag (Einer/Zehner via Minuten-LEDs)
      int eTag = tag % 10; for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10; for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      
      // Monat (Einer/Sechser via Stunden-LEDs)
      int eMon = monat % 6;  for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6;  for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
    }
  } 
  // ZUSTAND 3: Normaler Uhrenbetrieb (Startzustand & Standard-Modus)
  else {
    unsigned long abgelaufeneZeit = millis() - anzeigeTimer;

    if (abgelaufeneZeit < 10000 || !isBatterieBetrieb) {
      // Uhrzeit anzeigen
      int eMin = timeinfo->tm_min % 10; for(int i=0; i<eMin; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zMin = timeinfo->tm_min / 10; for(int i=0; i<zMin; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eStd = timeinfo->tm_hour % 6;  for(int i=0; i<eStd; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sStd = timeinfo->tm_hour / 6;  for(int i=0; i<sStd; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      // --- SEKUNDEN-BLINKLOGIK ---
      if (!isBatterieBetrieb) {
        // Kabelmodus: Unterscheidung nach Ladezustand
        int batVal = leseBatterieSicher();
        if (batVal < 3150) {
          // A. AKKU LÄDT: Dreiertakt (Sekunde % 3 -> Links, Rechts, Aus)
          int takt = timeinfo->tm_sec % 3;
          if (takt == 0) { targetFrame[2][3] = 6; targetFrame[2][4] = 0; } // Links an
          else if (takt == 1) { targetFrame[2][3] = 0; targetFrame[2][4] = 6; } // Rechts an
          else { targetFrame[2][3] = 0; targetFrame[2][4] = 0; } // Beide aus
        } else {
          // B. AKKU VOLL: Reines, rhythmisches Wechselblinken
          if (timeinfo->tm_sec % 2 == 0) { targetFrame[2][3] = 6; targetFrame[2][4] = 0; } 
          else { targetFrame[2][3] = 0; targetFrame[2][4] = 6; }
        }
      } else {
        // Akkubetrieb: Klassisches Wechselblinken
        if (timeinfo->tm_sec % 2 == 0) { targetFrame[2][3] = 6; targetFrame[2][4] = 0; } 
        else { targetFrame[2][3] = 0; targetFrame[2][4] = 6; }
      }
    }
    else if (abgelaufeneZeit >= 10000 && abgelaufeneZeit < 15000) {
      // Datum im Akkubetrieb (Wechselphase)
      int tag = timeinfo->tm_mday; int monat = timeinfo->tm_mon + 1;
      int eTag = tag % 10; for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10; for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eMon = monat % 6;  for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6;  for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
    }
    else {
      // Akku im Akkubetrieb (Wechselphase)
      int rawBat = leseBatterieSicher();
      if (rawBat >= 2650) {
        int grueneLeds = map(rawBat, 2650, 3200, 1, 9); grueneLeds = constrain(grueneLeds, 1, 9);
        for(int i = 0; i < grueneLeds; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      } else {
        int roteLeds = map(rawBat, 2300, 2649, 1, 5); roteLeds = constrain(roteLeds, 1, 5);
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
