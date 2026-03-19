#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> 
#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <EloquentTinyML.h>
#include "model.h" // This must contain the 'lithoflow_edge_tflite' array

// --- NETWORK CONFIG ---
const char* ssid = "YOUR_WIFI_NAME"; 
const char* password = "YOUR_WIFI_PASSWORD";
// IMPORTANT: Use https and ensure NO trailing slash if your Next.js route doesn't have one
const char* serverName = "https://YOUR_NGROK_URL.ngrok-free.dev/api/telemetry"; 

// --- PINS & HARDWARE ---
#define LOADCELL_DOUT_PIN 19
#define LOADCELL_SCK_PIN 18
#define EEPROM_SIZE 8 // To store total_intake (float)

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- TINYML CONFIG ---
#define NUMBER_OF_INPUTS 2   // [Weight Drop, Duration]
#define NUMBER_OF_OUTPUTS 1  // Probability of "Real Sip"
#define TENSOR_ARENA_SIZE 2 * 1024
Eloquent::TinyML::TfLite<NUMBER_OF_INPUTS, NUMBER_OF_OUTPUTS, TENSOR_ARENA_SIZE> ml;

// --- STATE VARIABLES ---
float total_intake = 0.0;
float last_stable_weight = 0.0;
bool is_lifting = false;
unsigned long lift_start_time = 0;
float calibration_factor = -420.0; // Calibrate this for your specific load cell
long zero_offset = 0;

// --- HX711 BIT-BANGING DRIVER ---
long readHX711Raw() {
  while (digitalRead(LOADCELL_DOUT_PIN) == HIGH); 
  long count = 0;
  portDISABLE_INTERRUPTS(); 
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

// --- CLOUD TELEMETRY (FIXED FOR -1 AND 308 ERRORS) ---
void sendDataToCloud(float intake, float sip, String tag) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // Required for ngrok HTTPS - fixes Error -1
    
    HTTPClient http;
    http.begin(client, serverName);
    
    http.addHeader("Content-Type", "application/json");
    http.addHeader("ngrok-skip-browser-warning", "true"); // Bypasses ngrok gateway page

    StaticJsonDocument<200> doc;
    doc["total_intake"] = intake;
    doc["last_sip"] = sip;
    doc["tag"] = tag;

    String requestBody;
    serializeJson(doc, requestBody);
    
    Serial.println("[CLOUD] POSTing to " + String(serverName));
    int httpResponseCode = http.POST(requestBody);
    
    if (httpResponseCode > 0) {
      Serial.printf("[CLOUD] Success: %d\n", httpResponseCode);
    } else {
      Serial.printf("[CLOUD] Error: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}

void setup() {
  Serial.begin(115200);
  
  // 1. WiFi Connection
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WIFI] Connected");

  // 2. Hardware Init
  lcd.init(); lcd.backlight();
  pinMode(LOADCELL_DOUT_PIN, INPUT);
  pinMode(LOADCELL_SCK_PIN, OUTPUT);
  EEPROM.begin(EEPROM_SIZE);
  
  // 3. TinyML Model Init
  ml.begin(lithoflow_edge_tflite); 

  // 4. Persistence Restore
  EEPROM.get(0, total_intake);
  if (isnan(total_intake)) total_intake = 0.0;
  
  // 5. Initial Tare (Zeroing)
  long tare_sum = 0;
  for(int i=0; i<20; i++) tare_sum += readHX711Raw();
  zero_offset = tare_sum / 20;
  last_stable_weight = getWeightUnits(30);
  
  lcd.print("RenalSense IoT");
  delay(1500);
  lcd.clear();
}

void loop() {
  float current_weight = getWeightUnits(10); 

  // Detect Lift (Bottle picked up)
  if (current_weight < 20.0 && !is_lifting) { 
    is_lifting = true;
    lift_start_time = millis();
    lcd.setCursor(0,1); lcd.print("Status: Drinking");
  } 

  // Detect Replacement (Bottle put back)
  if (current_weight > 40.0 && is_lifting) {
    delay(2000); // Wait for water oscillation to settle
    float new_weight = getWeightUnits(25); 
    float weight_drop = last_stable_weight - new_weight;
    float duration = (millis() - lift_start_time) / 1000.0;
    
    // --- EDGE AI INFERENCE ---
    float inputs[2] = {weight_drop, duration};
    float confidence = ml.predict(inputs);

    Serial.printf("[ML] Drop: %.2fg | Time: %.2fs | Confidence: %.2f\n", weight_drop, duration, confidence);

    // Classify: If confidence > 75% and weight drop is significant
    if (confidence > 0.75 && weight_drop > 5.0) { 
      total_intake += weight_drop;
      
      // Save to EEPROM (Persistence)
      EEPROM.put(0, total_intake);
      EEPROM.commit(); 
      
      // Sync to Cloud
      sendDataToCloud(total_intake, weight_drop, "sip");
      
      lcd.setCursor(0,1); lcd.print("Logged: +"); lcd.print((int)weight_drop); lcd.print("ml");
    } else {
      lcd.setCursor(0,1); lcd.print("Status: Ignored ");
    }
    
    last_stable_weight = new_weight;
    is_lifting = false;
    delay(3000);
    lcd.clear();
  }

  // Periodic Display
  lcd.setCursor(0, 0);
  lcd.print("Total: "); lcd.print((int)total_intake); lcd.print(" ml");
  delay(200);
}
