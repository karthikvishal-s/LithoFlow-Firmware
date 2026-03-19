#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <WiFiClientSecure.h>

// --- CONFIGURATION ---
const char* ssid = "KarthikVishal"; 
const char* password = "12345678";
const char* serverName = "https://punier-bettie-straightly.ngrok-free.dev/api/telemetry"; 
const char* resetServerName = "https://punier-bettie-straightly.ngrok-free.dev/api/reset"; 

#define LOADCELL_DOUT_PIN 19
#define LOADCELL_SCK_PIN 18
#define RESET_BTN_PIN 4 // NEW: Connect button between Pin 4 and GND
#define EEPROM_SIZE 8

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- GLOBAL VARIABLES ---
float total_intake = 0.0;
float last_stable_weight = 0.0;
bool is_lifting = false;
float calibration_factor = -420.0; 
long zero_offset = 0;

// --- CUSTOM HX711 DRIVER ---
long readHX711Raw() {
  while (digitalRead(LOADCELL_DOUT_PIN) == HIGH); 
  long count = 0;
  portDISABLE_INTERRUPTS(); // Protect timing from Wi-Fi tasks
  for (int i = 0; i < 24; i++) {
    digitalWrite(LOADCELL_SCK_PIN, HIGH);
    count = count << 1; 
    digitalWrite(LOADCELL_SCK_PIN, LOW);
    if (digitalRead(LOADCELL_DOUT_PIN)) count++; 
  }
  digitalWrite(LOADCELL_SCK_PIN, HIGH);
  count = count ^ 0x800000;
  digitalWrite(LOADCELL_SCK_PIN, LOW);
  portENABLE_INTERRUPTS();
  return count;
}

float getWeightUnits(int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += (readHX711Raw() - zero_offset);
    delay(2); 
  }
  return (float)(sum / samples) / calibration_factor;
}

// --- CLOUD SYNC ---
void sendDataToCloud(float intake, float sip, String tag) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // Fixes SSL -1 error
    
    HTTPClient http;
    http.begin(client, serverName); // Use the secure client
    
    http.addHeader("Content-Type", "application/json");
    http.addHeader("ngrok-skip-browser-warning", "true"); // Fixes ngrok -1 error

    StaticJsonDocument<200> doc;
    doc["total_intake"] = (int)intake;
    doc["last_sip"] = (int)sip;
    doc["tag"] = tag;

    String requestBody;
    serializeJson(doc, requestBody);
    
    int httpResponseCode = http.POST(requestBody);
    
    if (httpResponseCode > 0) {
      Serial.printf("[CLOUD] Success! Response: %d\n", httpResponseCode);
    } else {
      Serial.printf("[CLOUD] Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

// --- NEW: FULL STACK RESET ---
void handlePhysicalReset() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("RESETTING SYSTEM ");
  Serial.println("\n[HARDWARE] Manual Reset Triggered!");

  // 1. Wipe Local Memory
  total_intake = 0.0;
  EEPROM.put(0, total_intake);
  EEPROM.commit();
  last_stable_weight = getWeightUnits(20); // Recalibrate baseline

  // 2. Wipe Cloud Data
  if (WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(0,1); lcd.print("Clearing Cloud...");
    
    // FIX: Use secure client for the HTTPS ngrok endpoint
    WiFiClientSecure client;
    client.setInsecure(); 
    
    HTTPClient http;
    http.begin(client, resetServerName);
    
    int httpResponseCode = http.POST(""); // Empty body for POST
    Serial.print("[CLOUD] Reset Response: "); Serial.println(httpResponseCode);
    http.end();
  }

  lcd.setCursor(0,1); lcd.print("Reset Complete!  ");
  delay(1500);
  lcd.clear();
}

void setup() {
  Serial.begin(115200);
  
  // Initialize Reset Button
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  
  // STAGE 1: Priority WiFi Boot
  WiFi.disconnect(true);
  delay(1000);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); 
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  // STAGE 2: Peripheral Init & Smart Calibration
  if (WiFi.status() == WL_CONNECTED) {
    lcd.init(); lcd.backlight();
    pinMode(LOADCELL_DOUT_PIN, INPUT);
    pinMode(LOADCELL_SCK_PIN, OUTPUT);
    EEPROM.begin(EEPROM_SIZE);
    
    // Check for previous data
    EEPROM.get(0, total_intake);
    if (isnan(total_intake) || total_intake > 5000) total_intake = 0.0;

    // A. Tare Empty Scale (Do NOT place bottle yet)
    lcd.setCursor(0,0); lcd.print("Taring Scale...");
    long tare_sum = 0;
    for(int i=0; i<20; i++) tare_sum += readHX711Raw();
    zero_offset = tare_sum / 20;

    // B. Wait for Bottle Placement
    lcd.clear();
    lcd.setCursor(0,0); lcd.print("Place Bottle Now");
    float check_w = 0;
    while(check_w < 50.0) { // Wait for at least 50g
      check_w = getWeightUnits(10);
      delay(500);
      Serial.print("Waiting for bottle... Current: "); Serial.println(check_w);
    }

    // C. Lock Initial Baseline
    delay(2000); // Let it settle
    last_stable_weight = getWeightUnits(30);
    lcd.clear();
    lcd.print("Bottle Synced!");
    delay(1500);
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) return;

  // --- NEW: CHECK RESET BUTTON ---
  if (digitalRead(RESET_BTN_PIN) == LOW) {
    delay(50); // Debounce
    if (digitalRead(RESET_BTN_PIN) == LOW) {
      handlePhysicalReset();
      return; // Skip rest of loop for this cycle
    }
  }

  float current_reading = getWeightUnits(20); // Signal smoothing

  // EDGE ANALYTICS: Lift Logic
  if (current_reading < 25.0 && !is_lifting) { 
    is_lifting = true;
    lcd.setCursor(0,1); lcd.print("Status: Drinking");
    Serial.println(">>> Bottle Lifted");
  } 

  // EDGE ANALYTICS: Replace Logic
  if (current_reading > 45.0 && is_lifting) {
    lcd.setCursor(0,1); lcd.print("Status: Syncing ");
    delay(2000); // User requested 2s gap
    
    float new_weight = getWeightUnits(30); 
    float consumed = last_stable_weight - new_weight;
    
    // Intake detected (positive change)
    if (consumed > 8.0) { 
      total_intake += consumed;
      EEPROM.put(0, total_intake);
      EEPROM.commit(); 
      
      // FIX: Added the "hydration_event" tag here
      sendDataToCloud(total_intake, consumed, "hydration_event");
      
      last_stable_weight = new_weight; // Update baseline to current water level
    } 
    // Refill detected (negative change > 50g)
    else if (consumed < -50.0) {
      Serial.println(">>> Refill Detected. Resetting Baseline.");
      last_stable_weight = new_weight;
    }
    
    is_lifting = false;
    lcd.setCursor(0,1); lcd.print("Status: Ready   ");
  }

  // Dashboard Update
  lcd.setCursor(0, 0);
  lcd.print("Intake: "); lcd.print(total_intake, 0); lcd.print("ml   ");
  
  if (!is_lifting) {
    lcd.setCursor(0, 1);
    lcd.print("Weight: "); lcd.print(current_reading, 0); lcd.print("g      ");
  }
  
  delay(300);
}
