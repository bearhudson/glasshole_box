#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// =========================================================================
// Heltec WiFi LoRa 32 (V3) Hardware Pin Definitions
// =========================================================================
#define OLED_SDA      17
#define OLED_SCL      18
#define OLED_RST      21
#define VEXT_PIN      36    // Active LOW: powers OLED screen
#define LED_PIN       35    // Onboard White LED
#define BUTTON_PIN    0     // PRG / User button (Active LOW)
#define BATTERY_PIN   20    // ADC Battery Pin

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define RSSI_THRESHOLD -75  // Stronger than -75 dBm triggers alert
const int SCAN_TIME_SECONDS = 3;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);

// =========================================================================
// Bluetooth SIG Company IDs & Service UUIDs
// =========================================================================
#define CID_LUXOTTICA       0x0D53 // Ray-Ban Meta / Stories
#define CID_SNAPCHAT        0x03C2 // Snap Inc. Spectacles
#define CID_META_1          0x01AB // Meta Platforms / Facebook
#define CID_META_2          0x058E // Meta Technologies
#define CID_BOSE            0x009E // Bose Frames
#define CID_VUZIX           0x0731 // Vuzix Corporation

static const BLEUUID UUID_META_SMARTGLASSES((uint16_t)0xFD5F);
static const BLEUUID UUID_NORDIC_UART("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
static const BLEUUID UUID_EVEN_REALITIES("00002760-08c2-11e1-9073-0e8ac72e0001");

// =========================================================================
// Known Smart Glasses Broadcast Name Signatures
// =========================================================================
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

struct DetectionRecord {
  bool active = false;
  String name;
  String matchType;
  int rssi;
  String mac;
};

DetectionRecord lastDetection;
volatile bool alertTriggered = false;
bool glassesDetectedLastCycle = false;

// Interrupt-driven button handling
volatile bool toggleDisplayRequested = false;
volatile unsigned long lastButtonInterruptTime = 0;
bool isDisplayOn = true;

// Battery Reading Variables
float lastVBat = 0.0f;

void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastButtonInterruptTime > 350) {
    toggleDisplayRequested = true;
    lastButtonInterruptTime = now;
  }
}

// Read Raw ADC Values from Pin 20
void sampleRawBattery() {
  uint32_t sumMV = 0;
  for (int i = 0; i < 16; i++) {
    sumMV += analogReadMilliVolts(BATTERY_PIN);
    delayMicroseconds(300);
  }
  uint32_t lastPinMV = sumMV / 16;
  lastVBat = (lastPinMV * 12.85f) / 1000.0f;
}

// Case-insensitive match helper
bool containsIgnoreCase(String haystack, const char* needle) {
  haystack.toLowerCase();
  String needleStr = String(needle);
  needleStr.toLowerCase();
  return haystack.indexOf(needleStr) >= 0;
}

// Full hardware reset and init sequence for OLED wake
void wakeDisplayHardware() {
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW); // Turn on power rail
  delay(50);

  // Hardware reset pulse
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

// Low power display sleep
void sleepDisplayHardware() {
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  digitalWrite(VEXT_PIN, HIGH); // Cut power rail
}

void processDisplayToggle() {
  if (!toggleDisplayRequested) return;
  toggleDisplayRequested = false;

  isDisplayOn = !isDisplayOn;
  if (isDisplayOn) {
    wakeDisplayHardware();
  } else {
    sleepDisplayHardware();
  }
}

// Clean Header: "GLASSHOLE" on left, Right-aligned voltage reading
void drawHeader() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.print(F("Glassholio v1"));

  char vBuf[10];
  snprintf(vBuf, sizeof(vBuf), "%.2fV", lastVBat);
  
  int vWidth = strlen(vBuf) * 6;
  int startX = SCREEN_WIDTH - vWidth;
  
  display.setCursor(startX, 0);
  display.print(vBuf);

  display.drawLine(0, 10, SCREEN_WIDTH, 10, SSD1306_WHITE);
}

// Draw default scanning display
void drawScanningScreen(int scanCount, bool targetPresent) {
  if (!isDisplayOn) return;

  display.clearDisplay();
  drawHeader();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 18);
  display.printf("Filter : >%ddBm", RSSI_THRESHOLD);

  display.setCursor(0, 34);
  display.printf("Scan #%d ", scanCount);
  for (int i = 0; i < (scanCount % 4); i++) {
    display.print(F("."));
  }

  display.setCursor(0, 50);
  if (targetPresent) {
    display.print(F("* GLASSES DETECTED! *"));
  } else {
    display.print(F("No targets nearby"));
  }

  display.display();
}

// Draw alert screen
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
  display.printf("ID  : %s", det.name.substring(0, 14).c_str());

  display.setCursor(0, 26);
  display.printf("Type: %s", det.matchType.substring(0, 14).c_str());

  display.setCursor(0, 37);
  display.printf("RSSI: %d dBm (STRONG)", det.rssi);

  display.setCursor(0, 48);
  display.printf("MAC : %s", det.mac.c_str());

  int barWidth = map(constrain(det.rssi, -100, -30), -100, -30, 0, SCREEN_WIDTH);
  display.drawRect(0, 59, SCREEN_WIDTH, 5, SSD1306_WHITE);
  display.fillRect(0, 59, barWidth, 5, SSD1306_WHITE);

  display.display();
}

// BLE Callback
class AdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    int rssi = advertisedDevice.getRSSI();
    bool isMatch = false;
    String matchReason = "";
    String devName = advertisedDevice.haveName() ? String(advertisedDevice.getName().c_str()) : "Unnamed";

    // 1. Manufacturer Specific Data / Company ID
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

    // Trigger
    if (isMatch && rssi > RSSI_THRESHOLD) {
      lastDetection.active = true;
      lastDetection.name = devName;
      lastDetection.matchType = matchReason;
      lastDetection.rssi = rssi;
      lastDetection.mac = String(advertisedDevice.getAddress().toString().c_str());
      alertTriggered = true;
    }
  }
};

void setup() {
  // 1. Hardware Pins & Button Interrupt
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // 2. Configure Battery ADC Pin (GPIO 20)
  pinMode(BATTERY_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  // 3. Power on and Initialize OLED
  wakeDisplayHardware();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println(F("GLASSHOLE DETECTOR"));
  display.setCursor(0, 36);
  display.println(F("Initializing BLE..."));
  display.display();
  delay(1000);

  // 4. Initialize BLE
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
}

int scanCycle = 0;

void loop() {
  processDisplayToggle();

  alertTriggered = false;
  scanCycle++;

  sampleRawBattery();
  drawScanningScreen(scanCycle, glassesDetectedLastCycle);

  // Run BLE scan (button interrupt remains active during this call)
  pBLEScan->start(SCAN_TIME_SECONDS, false);

  processDisplayToggle();

  if (alertTriggered) {
    glassesDetectedLastCycle = true;
    drawAlertScreen(lastDetection);
    
    // Strobe onboard LED (GPIO 35)
    for (int i = 0; i < 6; i++) {
      digitalWrite(LED_PIN, HIGH);
      delay(80);
      digitalWrite(LED_PIN, LOW);
      delay(80);
      processDisplayToggle();
    }
    
    if (isDisplayOn) {
      delay(1800);
    }
  } else {
    glassesDetectedLastCycle = false;
  }

  pBLEScan->clearResults();
}