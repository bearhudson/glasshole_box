#include <Arduino.h>

// Candidate Battery ADC Pins on Heltec V3 / ESP32-S3
#define ADC_PIN_1      1   // Standard Heltec V3 schematic (ADC1_CH0)
#define ADC_PIN_20     20  // Alternate revision ADC pin

// Candidate Gating / Enable Pins (Active LOW to enable voltage divider)
#define PIN_VBAT_CTRL  37  // Dedicated battery ADC enable pin on V3
#define PIN_VEXT       36  // Peripheral power rail

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000); // Wait for Serial USB CDC

  Serial.println("\n=======================================================");
  Serial.println("    Heltec V3 Battery Diagnostic & Topology Test       ");
  Serial.println("=======================================================");

  // Configure gating pins
  pinMode(PIN_VBAT_CTRL, OUTPUT);
  pinMode(PIN_VEXT, OUTPUT);

  // Set ADC resolution and full-scale attenuation (0-3.1V input range)
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN_1, ADC_11db);
  analogSetPinAttenuation(ADC_PIN_20, ADC_11db);
}

// Method 1: GPIO 1 with dedicated VBAT_CTRL (GPIO 37 LOW) + 4.9x Divider
void testMethod1() {
  digitalWrite(PIN_VBAT_CTRL, LOW);  // Enable divider gate
  delay(10);

  uint32_t raw = analogRead(ADC_PIN_1);
  uint32_t mv = analogReadMilliVolts(ADC_PIN_1);
  
  digitalWrite(PIN_VBAT_CTRL, HIGH); // Disable to save power

  // Heltec V3: (390k + 100k) / 100k = 4.9 multiplier
  float vBat = (mv * 4.9) / 1000.0;
  int pct = constrain((int)(((vBat - 3.3) / (4.2 - 3.3)) * 100.0), 0, 100);

  Serial.printf("[Method 1] GPIO 1 + GPIO 37 LOW | Raw: %4u | ADC: %4u mV | VBat: %.2f V | %3d%%\n", 
                raw, mv, vBat, pct);
}

// Method 2: GPIO 1 with Vext (GPIO 36 LOW) + 4.9x Divider
void testMethod2() {
  digitalWrite(PIN_VEXT, LOW); // Enable Vext rail
  delay(10);

  uint32_t raw = analogRead(ADC_PIN_1);
  uint32_t mv = analogReadMilliVolts(ADC_PIN_1);

  float vBat = (mv * 4.9) / 1000.0;
  int pct = constrain((int)(((vBat - 3.3) / (4.2 - 3.3)) * 100.0), 0, 100);

  Serial.printf("[Method 2] GPIO 1 + GPIO 36 LOW | Raw: %4u | ADC: %4u mV | VBat: %.2f V | %3d%%\n", 
                raw, mv, vBat, pct);
}

// Method 3: GPIO 1 Direct Read (Ungated / Always Connected) + 4.9x Divider
void testMethod3() {
  uint32_t raw = analogRead(ADC_PIN_1);
  uint32_t mv = analogReadMilliVolts(ADC_PIN_1);

  float vBat = (mv * 4.9) / 1000.0;
  int pct = constrain((int)(((vBat - 3.3) / (4.2 - 3.3)) * 100.0), 0, 100);

  Serial.printf("[Method 3] GPIO 1 Direct (No Gate) | Raw: %4u | ADC: %4u mV | VBat: %.2f V | %3d%%\n", 
                raw, mv, vBat, pct);
}

// Method 4: GPIO 20 (Alternate V3 Revision) with GPIO 37 LOW + 2.0x Divider
void testMethod4() {
  digitalWrite(PIN_VBAT_CTRL, LOW);
  delay(10);

  uint32_t raw = analogRead(ADC_PIN_20);
  uint32_t mv = analogReadMilliVolts(ADC_PIN_20);
  
  digitalWrite(PIN_VBAT_CTRL, HIGH);

  // Some revisions use a 1:1 (2.0x) divider on GPIO 20
  float vBat = (mv * 2.0) / 1000.0;
  int pct = constrain((int)(((vBat - 3.3) / (4.2 - 3.3)) * 100.0), 0, 100);

  Serial.printf("[Method 4] GPIO 20 + GPIO 37 LOW| Raw: %4u | ADC: %4u mV | VBat: %.2f V | %3d%%\n", 
                raw, mv, vBat, pct);
}

// Method 5: Multi-sample Averaged Read on GPIO 1 + GPIO 37 (Smooths LiPo ripple)
void testMethod5() {
  digitalWrite(PIN_VBAT_CTRL, LOW);
  delay(10);

  uint32_t mvSum = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) {
    mvSum += analogReadMilliVolts(ADC_PIN_1);
    delay(2);
  }
  digitalWrite(PIN_VBAT_CTRL, HIGH);

  uint32_t avgMv = mvSum / samples;
  float vBat = (avgMv * 4.9) / 1000.0;
  int pct = constrain((int)(((vBat - 3.3) / (4.2 - 3.3)) * 100.0), 0, 100);

  Serial.printf("[Method 5] GPIO 1 (16x Averaged)| Avg: %4u mV | VBat: %.2f V | %3d%%\n", 
                avgMv, vBat, pct);
}

void loop() {
  Serial.println("--- Reading Cycle ---");
  testMethod1();
  testMethod2();
  testMethod3();
  testMethod4();
  testMethod5();
  Serial.println("---------------------\n");

  delay(2500);
}