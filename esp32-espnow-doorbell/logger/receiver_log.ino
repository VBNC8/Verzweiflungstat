#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <Preferences.h>

Preferences preferences;

#define FTP_SERVER    "192.168.178.1"
#define FTP_PORT      21
#define FTP_USER      "ESP"
#define FTP_FILE_PATH "/FRITZ.NAS/staircase_log/event_log.csv"

#define ESPNOW_CHANNEL        1

#define LED_PIN               48
#define LED_COUNT             1
#define LED_BRIGHTNESS        24
#define BOOT_BUTTON_PIN       0     

#define RING_HOLD_MS          3500
#define LED_BLINK_HALF_MS     100   
#define LED_FEEDBACK_ON_MS    500   // Green LED stays on for 500ms

// Buzzer pattern (ausgewertet nach Vorlage)
#define BUZZER_PIN            16
#define BEEP_FREQ_HZ          1500
#define BEEP_ON_MS            100
#define BEEP_OFF_MS           180
#define BEEP_GROUP_GAP_MS     250
#define BEEP_COUNT_PER_GROUP  3
#define BEEP_GROUP_COUNT      1
#define BUZZER_COOLDOWN_MS    3000  // 3 second pause between patterns

Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

typedef struct __attribute__((packed)) {
  uint32_t sequence;
  uint16_t batteryMilliVolts;
  uint8_t  batteryPercent;
  uint8_t  lowBattery;
} button_message_t;

// LED State
uint32_t ringUntil = 0;
bool ringActive = false;
bool ledOn = false;
uint32_t lastLedToggle = 0;

// LED Feedback State
uint32_t ledFeedbackUntil = 0;
bool ledFeedbackActive = false;

// Buzzer State Machine
enum BuzzerState { BUZZER_IDLE, BUZZER_ON, BUZZER_OFF, BUZZER_GROUP_GAP, BUZZER_COOLDOWN };
BuzzerState buzzerState = BUZZER_IDLE;
uint8_t groupCountDone = 0;
uint8_t beepCountDone = 0;
uint32_t buzzerStepTs = 0;

bool lastButtonState = HIGH;
uint32_t lastDebounceTime = 0;

// Sequential Logging State
bool pendingLog = false;
char pendingMac[18];
button_message_t pendingMsg;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void setLedFeedback(uint8_t r, uint8_t g, uint8_t b) {
  setLed(r, g, b);
  ledFeedbackActive = true;
  ledFeedbackUntil = millis() + LED_FEEDBACK_ON_MS;
}

void startRingLed() {
  ringUntil = millis() + RING_HOLD_MS;
  if (!ringActive) {
    ringActive = true;
    ledOn = true;
    lastLedToggle = millis();
    setLed(255, 255, 255);
  }
}

void startBuzzerSequence() {
  groupCountDone = 0;
  beepCountDone = 0;
  buzzerState = BUZZER_ON;
  buzzerStepTs = millis();
  tone(BUZZER_PIN, BEEP_FREQ_HZ);
}

void serviceRingLed(uint32_t now) {
  // Handle feedback LED (green success or red error)
  if (ledFeedbackActive) {
    if ((int32_t)(now - ledFeedbackUntil) >= 0) {
      ledFeedbackActive = false;
      if (!ringActive) {
        setLed(0, 0, 0);  // Turn off if ring isn't active
      }
    }
    return;  // Feedback LED takes priority
  }

  if (!ringActive) return;

  if ((int32_t)(now - ringUntil) >= 0) {
    ringActive = false;
    ledOn = false;
    setLed(0, 0, 0);
    return;
  }

  if ((uint32_t)(now - lastLedToggle) >= LED_BLINK_HALF_MS) {
    lastLedToggle = now;
    ledOn = !ledOn;
    setLed(ledOn ? 255 : 0, ledOn ? 255 : 0, ledOn ? 255 : 0);
  }
}

void serviceBuzzer(uint32_t now) {
  if (buzzerState == BUZZER_IDLE) return;

  if (buzzerState == BUZZER_ON) {
    if (now - buzzerStepTs >= BEEP_ON_MS) {
      noTone(BUZZER_PIN);
      beepCountDone++;
      buzzerStepTs = now;
      if (beepCountDone >= BEEP_COUNT_PER_GROUP) {
        groupCountDone++;
        if (groupCountDone >= BEEP_GROUP_COUNT) {
          buzzerState = BUZZER_COOLDOWN;  // Go to cooldown instead of IDLE
          buzzerStepTs = now;
        } else {
          buzzerState = BUZZER_GROUP_GAP;
        }
      } else {
        buzzerState = BUZZER_OFF;
      }
    }
  } else if (buzzerState == BUZZER_OFF) {
    if (now - buzzerStepTs >= BEEP_OFF_MS) {
      tone(BUZZER_PIN, BEEP_FREQ_HZ);
      buzzerState = BUZZER_ON;
      buzzerStepTs = now;
    }
  } else if (buzzerState == BUZZER_GROUP_GAP) {
    if (now - buzzerStepTs >= BEEP_GROUP_GAP_MS) {
      beepCountDone = 0;
      tone(BUZZER_PIN, BEEP_FREQ_HZ);
      buzzerState = BUZZER_ON;
      buzzerStepTs = now;
    }
  } else if (buzzerState == BUZZER_COOLDOWN) {
    if (now - buzzerStepTs >= BUZZER_COOLDOWN_MS) {
      buzzerState = BUZZER_IDLE;  // Finally back to idle after cooldown
    }
  }
}

bool getFormattedTime(char* buf, size_t len) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return false;
  }
  strftime(buf, len, "%Y-%m-%d %H:%M:%S", &timeinfo);
  return true;
}

void uploadLogToFritzBox(const char* logLine) {
  preferences.begin("credentials", true);
  String ftpPass = preferences.getString("ftp_pass", "");
  preferences.end();

  if (ftpPass.length() == 0) {
    Serial.println("FTP: No password configured");
    setLedFeedback(255, 0, 0);  // Red - error
    return;
  }

  WiFiClient ftpClient;
  if (!ftpClient.connect(FTP_SERVER, FTP_PORT)) {
    Serial.println("FTP: Connection failed");
    setLedFeedback(255, 0, 0);  // Red - error
    return;
  }

  delay(100);
  while (ftpClient.available()) ftpClient.read();

  ftpClient.printf("USER %s\r\n", FTP_USER);
  delay(100);
  while (ftpClient.available()) ftpClient.read();

  ftpClient.printf("PASS %s\r\n", ftpPass.c_str());
  delay(100);
  while (ftpClient.available()) ftpClient.read();

  ftpClient.print("TYPE I\r\n");
  delay(100);
  while (ftpClient.available()) ftpClient.read();

  ftpClient.print("PASV\r\n");
  delay(100);

  String pasvResponse = "";
  while (ftpClient.available()) {
    pasvResponse += (char)ftpClient.read();
  }

  int firstPar = pasvResponse.indexOf('(');
  int lastPar = pasvResponse.indexOf(')');
  if (firstPar == -1 || lastPar == -1) {
    Serial.println("FTP: Invalid PASV response");
    setLedFeedback(255, 0, 0);  // Red - error
    ftpClient.print("QUIT\r\n");
    ftpClient.stop();
    return;
  }

  String pasvData = pasvResponse.substring(firstPar + 1, lastPar);
  int commas[5];
  int idx = 0;
  for (int i = 0; i < pasvData.length(); i++) {
    if (pasvData.charAt(i) == ',') commas[idx++] = i;
  }

  if (idx < 5) {
    Serial.println("FTP: PASV parsing failed");
    setLedFeedback(255, 0, 0);  // Red - error
    ftpClient.print("QUIT\r\n");
    ftpClient.stop();
    return;
  }

  String h1 = pasvData.substring(0, commas[0]);
  String h2 = pasvData.substring(commas[0] + 1, commas[1]);
  String h3 = pasvData.substring(commas[1] + 1, commas[2]);
  String h4 = pasvData.substring(commas[2] + 1, commas[3]);
  int p1 = pasvData.substring(commas[3] + 1, commas[4]).toInt();
  int p2 = pasvData.substring(commas[4] + 1).toInt();
  int dataPort = (p1 << 8) + p2;
  String dataIp = h1 + "." + h2 + "." + h3 + "." + h4;

  WiFiClient dataClient;
  if (!dataClient.connect(dataIp.c_str(), dataPort)) {
    Serial.println("FTP: Data connection failed");
    setLedFeedback(255, 0, 0);  // Red - error
    ftpClient.print("QUIT\r\n");
    ftpClient.stop();
    return;
  }

  delay(50);

  ftpClient.printf("APPE %s\r\n", FTP_FILE_PATH);
  delay(100);
  
  String appeResponse = "";
  while (ftpClient.available()) {
    appeResponse += (char)ftpClient.read();
  }
  
  if (!appeResponse.startsWith("1")) {
    Serial.println("FTP: APPE rejected: " + appeResponse);
    setLedFeedback(255, 0, 0);  // Red - error
    dataClient.stop();
    ftpClient.print("QUIT\r\n");
    ftpClient.stop();
    return;
  }

  delay(50);
  dataClient.print(logLine);
  dataClient.print("\r\n");
  dataClient.stop();

  delay(100);
  
  String finalResponse = "";
  while (ftpClient.available()) {
    finalResponse += (char)ftpClient.read();
  }

  Serial.println("FTP: Success - " + String(logLine));
  setLedFeedback(0, 255, 0);  // Green - success (brief flash)

  ftpClient.print("QUIT\r\n");
  delay(50);
  ftpClient.stop();
}

void processLog(const char* sourceMac, const button_message_t &msg) {
  char timeStr[32];
  if (!getFormattedTime(timeStr, sizeof(timeStr))) {
    snprintf(timeStr, sizeof(timeStr), "UNKNOWN_TIME");
  }

  char logLine[128];
  snprintf(logLine, sizeof(logLine), "%s,%s,%lu,%u,%u,%u",
           timeStr, sourceMac, (unsigned long)msg.sequence,
           msg.batteryMilliVolts, msg.batteryPercent, msg.lowBattery);

  if (WiFi.status() == WL_CONNECTED) {
    uploadLogToFritzBox(logLine);
  }
}

void triggerAction(const char* sourceMac, const button_message_t &msg) {
  startRingLed();
  startBuzzerSequence();

  snprintf(pendingMac, sizeof(pendingMac), "%s", sourceMac);
  pendingMsg = msg;
  pendingLog = true;
}

void checkBootButton() {
  int reading = digitalRead(BOOT_BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH) {
    if (millis() - lastDebounceTime > 200) {
      button_message_t testMsg = {999, 3300, 100, 0};
      triggerAction("BOOT:TEST:MAC", testMsg);
      lastDebounceTime = millis();
    }
  }
  lastButtonState = reading;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  char macStr[18] = "00:00:00:00:00:00";
  if (info && info->src_addr) {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  }

  if (len == (int)sizeof(button_message_t)) {
    button_message_t msg;
    memcpy(&msg, data, sizeof(msg));
    triggerAction(macStr, msg);
  }
}

void checkSerialConfig() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.startsWith("SET_WIFI ")) {
      int firstSpace = input.indexOf(' ');
      int secondSpace = input.indexOf(' ', firstSpace + 1);
      if (secondSpace != -1) {
        String ssid = input.substring(firstSpace + 1, secondSpace);
        String pass = input.substring(secondSpace + 1);
        preferences.begin("credentials", false);
        preferences.putString("ssid", ssid);
        preferences.putString("wifi_pass", pass);
        preferences.end();
        ESP.restart();
      }
    } else if (input.startsWith("SET_FTP ")) {
      String pass = input.substring(8);
      preferences.begin("credentials", false);
      preferences.putString("ftp_pass", pass);
      preferences.end();
      ESP.restart();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  pixel.begin();
  pixel.setBrightness(LED_BRIGHTNESS);
  setLed(0, 0, 0);

  preferences.begin("credentials", true);
  String ssid = preferences.getString("ssid", "");
  String wifiPass = preferences.getString("wifi_pass", "");
  preferences.end();

  WiFi.mode(WIFI_STA);
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), wifiPass.c_str());
    uint32_t startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(200);
      checkSerialConfig();
    }
    if (WiFi.status() == WL_CONNECTED) {
      configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");
    }
  }

  WiFi.setSleep(false);

  esp_now_init();
  esp_now_register_recv_cb(onDataRecv);
}

void loop() {
  uint32_t now = millis();
  serviceRingLed(now);
  serviceBuzzer(now);
  checkBootButton();
  checkSerialConfig();

  // Log wird erst ausgeführt, wenn sowohl LED-Ring als auch die Doppel-Beep-Sequenz mit Cooldown komplett fertig sind
  if (pendingLog && !ringActive && buzzerState == BUZZER_IDLE) {
    pendingLog = false;
    processLog(pendingMac, pendingMsg);
  }
}
