#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// =========================================================================
// Heltec WiFi LoRa 32 (V3) Hardware Pin Definitions
// =========================================================================
#define OLED_SDA        17
#define OLED_SCL        18
#define OLED_RST        21
#define VEXT_PIN        36    // Active LOW: powers OLED
#define LED_PIN         35    // Onboard White LED
#define BUTTON_PIN      0     // PRG Button (Active LOW)
#define BATTERY_PIN     20    // ADC Battery Pin

#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define RSSI_THRESHOLD  -75
const int SCAN_TIME_SECONDS = 3;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
Preferences prefs;

// =========================================================================
// Bluetooth SIG Company IDs & Service UUIDs
// =========================================================================
#define CID_LUXOTTICA       0x0D53
#define CID_SNAPCHAT        0x03C2
#define CID_META_1          0x01AB
#define CID_META_2          0x058E
#define CID_BOSE            0x009E
#define CID_VUZIX           0x0731

static const BLEUUID UUID_META_SMARTGLASSES((uint16_t)0xFD5F);
static const BLEUUID UUID_NORDIC_UART("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static const BLEUUID UUID_EVEN_REALITIES("00002760-08c2-11e1-9073-0e8ac72e0001");
static const BLEUUID UUID_CTS_SERVICE((uint16_t)0x1805);
static const BLEUUID UUID_CTS_CHAR((uint16_t)0x2A2B);

const char* TARGET_NAMES[] = {
  "frame", "monocle", "brilliant", "openglass", "friend", "omi",
  "basedhardware", "mentra", "mach1", "beewearable",
  "ray-ban", "stories", "meta view", "spectacles", "even realities",
  "g1", "g2", "xreal", "nreal", "air 2", "air ultra", "rokid",
  "vuzix", "blade", "shield", "z100", "ultralite", "rayneo", "tcl",
  "myvu", "meizu", "magic leap",
  "echo frames", "bose frames", "tenor", "soprano", "tempo", "alto", "rondo",
  "razer anzu", "solos", "airgo", "lucyd", "huawei eyewear", "engo"
};
const size_t NUM_TARGET_NAMES = sizeof(TARGET_NAMES) / sizeof(TARGET_NAMES[0]);

BLEScan* pBLEScan;

// =========================================================================
// Flash Log Data Structures (NVS Ring Buffer)
// =========================================================================
#define MAX_LOG_ENTRIES 30

struct DetectionRecord {
  uint32_t timestamp;  // Epoch or relative seconds
  bool isRealTime;     // True if synced via CTS
  char name[16];
  char matchType[16];
  char mac[18];
  int8_t rssi;
};

DetectionRecord logBuffer[MAX_LOG_ENTRIES];
uint16_t logCount = 0;
uint16_t logHead = 0;

DetectionRecord lastDetection;
volatile bool alertTriggered = false;
bool glassesDetectedLastCycle = false;
bool timeIsSynced = false;

// =========================================================================
// UI & State Machine Variables
// =========================================================================
enum AppMode {
  MODE_SCANNING,
  MODE_REVIEW
};
AppMode currentMode = MODE_SCANNING;
int reviewIndex = 0;
bool isDisplayOn = true;
float lastVBat = 0.0f;

// Button click & double-click tracking
volatile bool buttonPressedFlag = false;
volatile unsigned long buttonPressTime = 0;
volatile unsigned long buttonReleaseTime = 0;

void IRAM_ATTR buttonISR() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    buttonPressTime = millis();
  } else {
    buttonReleaseTime = millis();
    buttonPressedFlag = true;
  }
}

// Case-insensitive match helper
bool containsIgnoreCase(String haystack, const char* needle) {
  haystack.toLowerCase();
  String needleStr = String(needle);
  needleStr.toLowerCase();
  return haystack.indexOf(needleStr) >= 0;
}

// Read Raw ADC Battery Pin
void sampleRawBattery() {
  uint32_t sumMV = 0;
  for (int i = 0; i < 16; i++) {
    sumMV += analogReadMilliVolts(BATTERY_PIN);
    delayMicroseconds(300);
  }
  uint32_t lastPinMV = sumMV / 16;
  lastVBat = (lastPinMV * 12.85f) / 1000.0f;
}

// Format timestamp: "MM/DD HH:MM" if synced, or "T+HH:MM:SS" if relative
String formatTimestamp(uint32_t ts, bool isReal) {
  char buf[20];
  if (isReal) {
    time_t raw = ts;
    struct tm* t = localtime(&raw);
    strftime(buf, sizeof(buf), "%m/%d %H:%M", t);
  } else {
    uint32_t s = ts % 60;
    uint32_t m = (ts / 60) % 60;
    uint32_t h = (ts / 3600);
    snprintf(buf, sizeof(buf), "T+%02uh:%02um", h, m);
  }
  return String(buf);
}

// =========================================================================
// Flash Memory Storage Helpers (NVS)
// =========================================================================
void loadLogsFromNVS() {
  prefs.begin("glass_logs", true);
  logCount = prefs.getUShort("count", 0);
  logHead = prefs.getUShort("head", 0);
  prefs.getBytes("buffer", logBuffer, sizeof(logBuffer));
  prefs.end();
}

void saveLogToNVS(const DetectionRecord& rec) {
  logBuffer[logHead] = rec;
  logHead = (logHead + 1) % MAX_LOG_ENTRIES;
  if (logCount < MAX_LOG_ENTRIES) logCount++;

  prefs.begin("glass_logs", false);
  prefs.putUShort("count", logCount);
  prefs.putUShort("head", logHead);
  prefs.putBytes("buffer", logBuffer, sizeof(logBuffer));
  prefs.end();
}

void clearLogsNVS() {
  logCount = 0;
  logHead = 0;
  memset(logBuffer, 0, sizeof(logBuffer));
  prefs.begin("glass_logs", false);
  prefs.clear();
  prefs.end();
}

// =========================================================================
// Strategy 1: Passive BLE Current Time Service (CTS) Sync
// =========================================================================
void syncTimeViaCTS(BLEAddress pAddress) {
  if (timeIsSynced) return;

  BLEClient* pClient = BLEDevice::createClient();
  if (!pClient->connect(pAddress)) {
    delete pClient;
    return;
  }

  BLERemoteService* pRemoteService = pClient->getService(UUID_CTS_SERVICE);
  if (pRemoteService != nullptr) {
    BLERemoteCharacteristic* pChar = pRemoteService->getCharacteristic(UUID_CTS_CHAR);
    if (pChar != nullptr && pChar->canRead()) {
      String value = pChar->readValue();
      if (value.length() >= 7) {
        // Parse standard Bluetooth SIG CTS byte array
        uint16_t year = ((uint8_t)value[1] << 8) | (uint8_t)value[0];
        uint8_t month = (uint8_t)value[2];
        uint8_t day = (uint8_t)value[3];
        uint8_t hours = (uint8_t)value[4];
        uint8_t minutes = (uint8_t)value[5];
        uint8_t seconds = (uint8_t)value[6];

        struct tm t = {0};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hours;
        t.tm_min = minutes;
        t.tm_sec = seconds;

        time_t newEpoch = mktime(&t);
        struct timeval tv = { .tv_sec = newEpoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        timeIsSynced = true;
      }
    }
  }
  pClient->disconnect();
  delete pClient;
}

// =========================================================================
// Display & Power Routines
// =========================================================================
void wakeDisplayHardware() {
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(50);

  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, HIGH);
  delay(10);
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);
  delay(50);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false);
  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.display();
}

void sleepDisplayHardware() {
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  digitalWrite(VEXT_PIN, HIGH);
}

void drawHeader() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(currentMode == MODE_REVIEW ? F("Logs") : F("Glassholio v1"));

  char vBuf[10];
  snprintf(vBuf, sizeof(vBuf), "%.2fV", lastVBat);
  int vWidth = strlen(vBuf) * 6;
  display.setCursor(SCREEN_WIDTH - vWidth, 0);
  display.print(vBuf);

  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
}

void drawScanningScreen(int scanCount, bool targetPresent) {
  if (!isDisplayOn || currentMode != MODE_SCANNING) return;

  display.clearDisplay();
  drawHeader();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 18);
  display.printf("Filter: >%d dBm", RSSI_THRESHOLD);

  display.setCursor(0, 34);
  display.printf("Scan #%d ", scanCount);
  for (int i = 0; i < (scanCount % 4); i++) {
    display.print(F("."));
  }

  display.setCursor(0, 50);
  if (targetPresent) {
    display.print(F("* GLASSES DETECTED! *"));
  } else {
    display.printf("Stored: %d logs", logCount);
  }

  display.display();
}

void drawAlertScreen(const DetectionRecord& det) {
  if (!isDisplayOn) return;

  display.clearDisplay();
  display.fillRect(0, 0, SCREEN_WIDTH, 11, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.printf("! DETECTED | %.2fV !", lastVBat);

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 15);
  display.printf("ID  : %s", String(det.name).substring(0, 14).c_str());

  display.setCursor(0, 26);
  display.printf("Type: %s", String(det.matchType).substring(0, 14).c_str());

  display.setCursor(0, 37);
  display.printf("RSSI: %d dBm (SAVED)", det.rssi);

  display.setCursor(0, 48);
  display.printf("Time: %s", formatTimestamp(det.timestamp, det.isRealTime).c_str());

  int barWidth = map(constrain(det.rssi, -100, -30), -100, -30, 0, SCREEN_WIDTH);
  display.drawRect(0, 59, SCREEN_WIDTH, 5, SSD1306_WHITE);
  display.fillRect(0, 59, barWidth, 5, SSD1306_WHITE);

  display.display();
}

// Draw NVS Review Screen
void drawReviewScreen() {
  if (!isDisplayOn) return;

  display.clearDisplay();
  drawHeader();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (logCount == 0) {
    display.setCursor(18, 30);
    display.print(F("No stored logs"));
    display.display();
    return;
  }

  // Calculate index for newest-first order
  int actualIndex = (logHead - 1 - reviewIndex + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
  DetectionRecord rec = logBuffer[actualIndex];

  display.setCursor(0, 14);
  display.printf("[%d/%d] %s", reviewIndex + 1, logCount, formatTimestamp(rec.timestamp, rec.isRealTime).c_str());

  display.setCursor(0, 26);
  display.printf("ID  : %s", String(rec.name).substring(0, 14).c_str());

  display.setCursor(0, 37);
  display.printf("Type: %s", String(rec.matchType).substring(0, 14).c_str());

  display.setCursor(0, 48);
  display.printf("RSSI: %ddBm | %s", rec.rssi, String(rec.mac).substring(9).c_str());

  display.display();
}

// =========================================================================
// BLE Scanner Callback
// =========================================================================
class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Strategy 1: Check for Current Time Service provider
    if (!timeIsSynced && advertisedDevice.haveServiceUUID()) {
      if (advertisedDevice.isAdvertisingService(UUID_CTS_SERVICE)) {
        syncTimeViaCTS(advertisedDevice.getAddress());
      }
    }

    int rssi = advertisedDevice.getRSSI();
    bool isMatch = false;
    String matchReason = "";
    String devName = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "Unnamed";

    // 1. Manufacturer Data / Company ID
    if (advertisedDevice.haveManufacturerData()) {
      String mData = advertisedDevice.getManufacturerData();
      if (mData.length() >= 2) {
        uint16_t companyId = ((uint8_t)mData[1] << 8) | (uint8_t)mData[0];

        if (companyId == CID_LUXOTTICA) {
          isMatch = true;
          matchReason = "Luxottica/RayBan";
        } else if (companyId == CID_SNAPCHAT) {
          isMatch = true;
          matchReason = "Snap Inc.";
        } else if (companyId == CID_META_1 || companyId == CID_META_2) {
          if (mData.indexOf("META_RB") >= 0 || containsIgnoreCase(devName, "Ray-Ban")) {
            isMatch = true;
            matchReason = "Meta/Ray-Ban";
          }
        } else if (companyId == CID_VUZIX) {
          isMatch = true;
          matchReason = "Vuzix Corp";
        }
      }
    }

    // 2. Service UUIDs
    if (!isMatch && advertisedDevice.haveServiceUUID()) {
      if (advertisedDevice.isAdvertisingService(UUID_META_SMARTGLASSES)) {
        isMatch = true;
        matchReason = "Meta 0xFD5F";
      } else if (advertisedDevice.isAdvertisingService(UUID_EVEN_REALITIES)) {
        isMatch = true;
        matchReason = "Even Realities";
      } else if (advertisedDevice.isAdvertisingService(UUID_NORDIC_UART)) {
        isMatch = true;
        matchReason = "NordicUART Frame";
      }
    }

    // 3. Advertised Names
    if (!isMatch && advertisedDevice.haveName()) {
      for (size_t i = 0; i < NUM_TARGET_NAMES; i++) {
        if (containsIgnoreCase(devName, TARGET_NAMES[i])) {
          isMatch = true;
          matchReason = TARGET_NAMES[i];
          break;
        }
      }
    }

    // Alert & Save to Non-Volatile Memory
    if (isMatch && rssi > RSSI_THRESHOLD) {
      lastDetection.timestamp = timeIsSynced ? (uint32_t)time(NULL) : (millis() / 1000);
      lastDetection.isRealTime = timeIsSynced;
      strncpy(lastDetection.name, devName.c_str(), sizeof(lastDetection.name) - 1);
      strncpy(lastDetection.matchType, matchReason.c_str(), sizeof(lastDetection.matchType) - 1);
      strncpy(lastDetection.mac, advertisedDevice.getAddress().toString().c_str(), sizeof(lastDetection.mac) - 1);
      lastDetection.rssi = (int8_t)rssi;

      saveLogToNVS(lastDetection);
      alertTriggered = true;
    }
  }
};

// =========================================================================
// Advanced Button Handler (Single, Double, Long Press)
// =========================================================================
unsigned long lastClickReleaseTime = 0;
uint8_t clickCount = 0;

void handleButtonEvents() {
  // Check for long press while held down (> 1.5s)
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (millis() - buttonPressTime > 1500) {
      if (currentMode == MODE_REVIEW) {
        clearLogsNVS();
        reviewIndex = 0;
        drawReviewScreen();
        delay(500);
      }
      buttonPressedFlag = false;
      clickCount = 0;
      return;
    }
  }

  // Register released clicks
  if (buttonPressedFlag) {
    buttonPressedFlag = false;
    clickCount++;
    lastClickReleaseTime = millis();
  }

  // Evaluate click pattern after 400ms pause
  if (clickCount > 0 && (millis() - lastClickReleaseTime > 400)) {
    if (clickCount == 1) {
      // Single Click
      if (currentMode == MODE_REVIEW) {
        if (logCount > 0) {
          reviewIndex = (reviewIndex + 1) % logCount; // Page to next entry
          drawReviewScreen();
        }
      } else {
        // Toggle screen sleep/wake
        isDisplayOn = !isDisplayOn;
        if (isDisplayOn) wakeDisplayHardware();
        else sleepDisplayHardware();
      }
    } else if (clickCount >= 2) {
      // Double Click -> Toggle Review Mode
      if (!isDisplayOn) {
        isDisplayOn = true;
        wakeDisplayHardware();
      }
      currentMode = (currentMode == MODE_SCANNING) ? MODE_REVIEW : MODE_SCANNING;
      reviewIndex = 0;
      if (currentMode == MODE_REVIEW) drawReviewScreen();
    }
    clickCount = 0;
  }
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, CHANGE);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  loadLogsFromNVS();
  wakeDisplayHardware();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(F("GLASSHOLE DETECTOR"));
  display.setCursor(0, 36);
  display.printf("Loaded %d flash logs", logCount);
  display.display();
  delay(1200);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

int scanCycle = 0;

void loop() {
  handleButtonEvents();

  if (currentMode == MODE_SCANNING) {
    alertTriggered = false;
    scanCycle++;

    sampleRawBattery();
    drawScanningScreen(scanCycle, glassesDetectedLastCycle);

    pBLEScan->start(SCAN_TIME_SECONDS, false);

    handleButtonEvents();

    if (alertTriggered) {
      glassesDetectedLastCycle = true;
      drawAlertScreen(lastDetection);

      for (int i = 0; i < 6; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(80);
        digitalWrite(LED_PIN, LOW);
        delay(80);
        handleButtonEvents();
      }

      if (isDisplayOn) delay(1800);
    } else {
      glassesDetectedLastCycle = false;
    }

    pBLEScan->clearResults();
  } else {
    // In Review Mode: maintain active responsive UI
    sampleRawBattery();
    delay(50);
  }
}