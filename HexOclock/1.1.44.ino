// =======================================================================================
// PROJEKT:      Cube-Clock (ESP32-S3 Waveshare Zero)
// VERSION:      v1.1.44 (The Timezone Anchor & USB-Tap Menu)
// BESCHREIBUNG: Extrem energiesparende Würfel-Uhr mit LED-Matrix, Helligkeits- 
//               und Lagesensor. Schützt die interne RTC-Zeit im Deep Sleep vor Drift.
//               FIX: Erzwingt Zeitzone (UTC+2) sofort nach dem Wakeup gegen 2h-Drift.
//               NEU: Klopfen im USB-Betrieb zeigt 5s Akku + 5s Datum an.
//
// ---------------------------------------------------------------------------------------
// Zukunftskonzept: ULTRA-DIMMUNG FÜR STEALTH-FOLIE (Bei Bedarf aktivieren!)
// Wenn die LEDs trotz 45µs Mindestleuchtzeit hinter der Folie im Dunkeln noch zu hell sind:
//
// Idee: "Frame-Skipping" statt ultrakurzer Timer-Blitze (Schont CPU & verhindert Crashes)
// - Aktuelle Frame-Rate: ~666 Hz (Super flimmerfrei).
// - Bei extremer Dunkelheit aktivieren wir die Reihen z.B. nur noch in JEDEM ZWEITEN Frame.
// - Die "An-Zeit" kann dann bei sicheren 45µs (oder mehr) bleiben, aber die LED ist effektiv
//   nur noch halb so oft an -> Halbe Helligkeit ohne CPU-Stress!
//
// Umsetzungsskizze für onTimer():
//   static int frameCounter = 0;
//   if (!ledPhase) {
//     if (istExtremDunkel && (frameCounter % 2 != 0)) {
//       // Überspringe diesen Durchlauf einfach (Reihen bleiben HIGH/Aus)
//       timerAlarm(timer, REIHE_GESAMT_ZEIT, true, 0);
//       return;
//     }
//     ... (normaler LED-An Code)
//   }
// =======================================================================================

#include <WiFi.h>
#include <Wire.h>
#include <WiFiManager.h>
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

#define PIN_WAKEUP_INPUT 1                  // Kombi-Eingang via Dioden: G-Sensor OR USB-Kabel

#define I2C_SDA 12                          
#define I2C_SCL 13                          
#define PIN_BATTERIE_MESSUNG 13             // Shared Pin mit I2C_SCL!

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
Point sekundenLEDs[2]  = { {2,3}, {2,4} };                                 

volatile uint8_t currentFrame[5][5] = {0}; 
const uint32_t REIHE_GESAMT_ZEIT = 300;       
volatile uint32_t ledAnZeit_us = 100;         
hw_timer_t * timer = NULL;

RTC_DATA_ATTR int letzterSyncTag = -1;       
unsigned long letztesNTPUpdateKabel = 0;     
const unsigned long NTP_INTERVALL_KABEL = 86400000; 

bool isBatterieBetrieb = true; 
unsigned long anzeigeTimer = 0;
unsigned long maxAnzeigeZeit = 20000; // 20 Sekunden im Akkubetrieb

// NEU: Für den Klopf-Zusatzbildschirm im USB-Betrieb
unsigned long usbTapTimer = 0;
bool usbTapAktiv = false;

float displayHelligkeiten[5][5] = {0.0};
const float FADE_SPEED = 1.2; 

bool ntpSyncErforderlich = false;
bool ntpSyncAktiv = false;
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
    uint32_t restZeit = (anZeit < REIHE_GESAMT_ZEIT) ? (REIHE_GESAMT_ZEIT - anZeit) : 20;
    if (restZeit < 20) restZeit = 20; 
    timerAlarm(timer, restZeit, true, 0);
  }
}

void holeNTPZeit() {
  WiFiManager wm;
  wm.setTimeout(12); 
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 

  Serial.println("[NTP] Verbinde mit WLAN...");
  if(wm.autoConnect("Cube-Clock-Setup")) {
    Serial.println("[NTP] WLAN verbunden! Starte NTP-Abfrage...");
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
      Serial.println("[NTP] ERFOLG! Echtzeit geladen.");
      letzterSyncTag = timeinfo->tm_mday; 
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      return;
    }
  }
  
  Serial.println("[NTP] FALLBACK: Setze sichtbare Testzeit...");
  struct tm tm_test;
  tm_test.tm_year = 2026 - 1900; 
  tm_test.tm_mon = 5;            
  tm_test.tm_mday = 15;          
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
  
  // FIX AGAINST 2H DRIFT: Zeitzonen-Zuweisung sofort im Umgebungsspeicher verankern!
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  rtc_gpio_hold_dis((gpio_num_t)PIN_WAKEUP_INPUT);

  // 1. PIN CONFIG FÜR DIE MATRIX
  for (int i = 0; i < 5; i++) {
    pinMode(rowPins[i], OUTPUT); pinMode(colPins[i], OUTPUT);
  }
  matrixAusschalten();

  // 2. GENTLE GLOW LEBENSZEICHEN SOFORT ANWERFEN (Stufe 6 von 31)
  displayHelligkeiten[2][3] = 6.0; displayHelligkeiten[2][4] = 6.0;
  currentFrame[2][3] = 6; currentFrame[2][4] = 6;

  timer = timerBegin(1000000); 
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 100, true, 0); 
  timerStart(timer);

  Serial.printf("\n--- TIMEZONE ANCHOR START V1.1.44 ---\n");

  pinMode(PIN_WAKEUP_INPUT, INPUT_PULLDOWN);

  // 3. SOFORTIGE ENTSCHEIDUNG: KABEL ODER AKKU?
  delay(50); 
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    isBatterieBetrieb = false;
    Serial.println("[POWER-MODE] Kabelbetrieb erkannt. Dauer-An aktiviert.");
  } else {
    isBatterieBetrieb = true;
    Serial.println("[POWER-MODE] Batteriebetrieb aktiv. Auto-Sleep nach 20s.");
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
    lis.setClick(2, 12, 15, 20, 150); 
    writeI2CDirect(lis3dh_i2c_addr, 0x3A, 0x0B);
    lis.getClick();
  }

  analogSetAttenuation(ADC_11db);
  anzeigeTimer = millis();

  // --- ZEIT-PLAUSIBILITÄTS-CHECK ---
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.printf("[WAKEUP] Durch Hardware-Pin geweckt. RTC-Zeit (korrigiert): %02d:%02d:%02d\n", 
                  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    if (timeinfo->tm_year < 120) {
      Serial.println("[RTC-ERROR] Zeit ungültig. Erzwinge NTP...");
      if (!isBatterieBetrieb) {
        holeNTPZeit();
      } else {
        ntpSyncErforderlich = true;
      }
    }
  } else {
    Serial.println("[WAKEUP] Kaltstart oder Timer-Wakeup. Hole frische NTP-Zeit...");
    holeNTPZeit(); 
  }
}

// ===================================================================
// 5. MAIN LOOP
// ===================================================================
unsigned long sensorTimer = 0;
unsigned long debugTimer = 0;
float geglaettetesDelay = 100.0;

void loop() {
  // --- KABEL-VETO-PRÜFUNG IM LAUFENDEN BETRIEB ---
  if (digitalRead(PIN_WAKEUP_INPUT) == HIGH) {
    if (isBatterieBetrieb) {
      Serial.println("[POWER-MODE] Kabel im Betrieb eingesteckt! Wechsle zu Dauer-An.");
      isBatterieBetrieb = false;
    }
    anzeigeTimer = millis(); // Verhindert Deep Sleep bei Kabelbetrieb
    
    // NEU: LIS3DH Erschütterungsabfrage im Kabelbetrieb aktivieren!
    // Wenn geklopft wird, unterbrechen wir das Uhrzeit-Dauerfeuer für das Info-Menü
    if (!usbTapAktiv && lis.getClick()) {
      Serial.println("[USB-TAP] Erschütterung am Kabel erkannt! Zeige Status-Menü...");
      usbTapAktiv = true;
      usbTapTimer = millis();
    }
  } else {
    if (!isBatterieBetrieb) {
      Serial.println("[POWER-MODE] Kabel abgezogen! Zurück im Akkubetrieb (20s Auto-Sleep).");
      isBatterieBetrieb = true;
      usbTapAktiv = false;
      anzeigeTimer = millis(); 
    }
  }

  // --- LIVE DIAGNOSE ---
  if (millis() - debugTimer > 1000) {
    debugTimer = millis();
    long restZeit = (maxAnzeigeZeit - (millis() - anzeigeTimer)) / 1000;
    time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);
    Serial.printf("[DIAGNOSE] Zeit: %02d:%02d:%02d | Mode: %s | Standby in: %lds\n", 
                  timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec,
                  isBatterieBetrieb ? "AKKU" : "USB-KABEL (DAUER-AN)", isBatterieBetrieb ? restZeit : 0);
    
    // G-Sensor Register leeren, damit er im Hintergrund sensibel bleibt
    if (!isBatterieBetrieb) {
      lis.getClick();
    }
  }

  // --- STEALTH BACKGROUND SYNC ---
  if (ntpSyncErforderlich && !ntpSyncAktiv) {
    ntpSyncAktiv = true;
    ntpStartTimer = millis();
    WiFi.mode(WIFI_STA); WiFi.begin(); 
  }
  if (ntpSyncAktiv) {
    if (WiFi.status() == WL_CONNECTED) {
      configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "192.168.178.1", "pool.ntp.org", "time.nist.gov");
      time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);
      if (timeinfo->tm_year >= 120) {
        ntpSyncErforderlich = false; ntpSyncAktiv = false;
        WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
      }
    } else if (millis() - ntpStartTimer > 6000) {
      ntpSyncAktiv = false; WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    }
  }

  // --- A. TIEFSCHLAF ---
  if (isBatterieBetrieb && (millis() - anzeigeTimer > maxAnzeigeZeit)) {
    Serial.println("[POWER] Gehe in den Deep Sleep...");
    delay(100);
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

  // --- B. SENSOREN AUSLESEN & SCHMEIDIGE DIMMUNG ---
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

  // --- C. MATRIX TARGET-FRAME ERSTELLEN ---
  time_t nun = time(nullptr); struct tm* timeinfo = localtime(&nun);
  uint8_t targetFrame[5][5] = {0}; 
  
  // Bestimmung des Anzeige-Modus
  if (!isBatterieBetrieb && usbTapAktiv) {
    // --- SPECIAL-MODUS: USB ANGELEUCHTET / GEKLOPFT ---
    unsigned long abgelaufeneUsbZeit = millis() - usbTapTimer;
    
    if (abgelaufeneUsbZeit < 5000) {
      // 1. Zuerst 5 Sekunden lang den AKKUSTAND anzeigen!
      int rawBat = leseBatterieSicher();
      if (rawBat >= 2650) {
        int grueneLeds = map(rawBat, 2650, 3200, 1, 9); grueneLeds = constrain(grueneLeds, 1, 9);
        for(int i = 0; i < grueneLeds; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      } else {
        int roteLeds = map(rawBat, 2300, 2649, 1, 5); roteLeds = constrain(roteLeds, 1, 5);
        for(int i = 0; i < roteLeds; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      }
    } 
    else if (abgelaufeneUsbZeit >= 5000 && abgelaufeneUsbZeit < 10000) {
      // 2. Danach 5 Sekunden lang das DATUM hinterher schieben!
      int tag = timeinfo->tm_mday; int monat = timeinfo->tm_mon + 1;
      int eTag = tag % 10; for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10; for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eMon = monat % 6; for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6; for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
    } 
    else {
      // Menü vorbei, zurück zum Uhren-Dauerfeuer
      usbTapAktiv = false;
    }
  } 
  else {
    // --- STANDARD-ANZEIGE-LOGIK (Akku-Wechsel oder USB-Daueruhr) ---
    unsigned long abgelaufeneZeit = millis() - anzeigeTimer;

    if (abgelaufeneZeit < 10000 || !isBatterieBetrieb) {
      // Uhrzeit anzeigen
      int eMin = timeinfo->tm_min % 10; for(int i=0; i<eMin; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zMin = timeinfo->tm_min / 10; for(int i=0; i<zMin; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eStd = timeinfo->tm_hour % 6;  for(int i=0; i<eStd; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sStd = timeinfo->tm_hour / 6;  for(int i=0; i<sStd; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
      
      if (ntpSyncAktiv) {
        targetFrame[2][3] = 6; targetFrame[2][4] = 6; 
      }
    }
    else if (abgelaufeneZeit >= 10000 && abgelaufeneZeit < 15000) {
      // Datum anzeigen (nur im Akkubetrieb sichtbar nach 10 Sekunden)
      int tag = timeinfo->tm_mday; int monat = timeinfo->tm_mon + 1;
      int eTag = tag % 10; for(int i=0; i<eTag; i++) targetFrame[einzelMinuten[i].row][einzelMinuten[i].col] = 31;
      int zTag = tag / 10; for(int i=0; i<zTag; i++) targetFrame[zehnerMinuten[i].row][zehnerMinuten[i].col] = 31;
      int eMon = monat % 6; for(int i=0; i<eMon; i++) targetFrame[einerStunden[i].row][einerStunden[i].col] = 31;
      int sMon = monat / 6; for(int i=0; i<sMon; i++) targetFrame[sechserStunden[i].row][sechserStunden[i].col] = 31;
    }
    else {
      // Akku anzeigen (nur im Akkubetrieb sichtbar nach 15 Sekunden)
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
