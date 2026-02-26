#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include "HX711.h"

// --- CONFIGURATION ---
#define EEPROM_SIZE 8          // Space for float (intake)
#define LOADCELL_DOUT_PIN 19
#define LOADCELL_SCK_PIN 18
#define LIFT_THRESHOLD 30.0    // Weight drop to trigger "Drinking" mode (grams)
#define DRINK_MINIMUM 10.0     // Minimum weight change to count as a drink (grams)

// Initialize LCD (Address 0x27 or 0x3F) and Scale
LiquidCrystal_I2C lcd(0x27, 16, 2);
HX711 scale;

// --- GLOBAL VARIABLES ---
float total_intake = 0.0;
float last_stable_weight = 0.0;
bool is_lifting = false;
float calibration_factor = -420.0; // REPLACE THIS after your calibration test

void setup() {
  Serial.begin(115200);
  
  // 1. DATABASE SETUP: Load saved intake from Flash
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, total_intake);
  if (isnan(total_intake) || total_intake < 0) total_intake = 0.0; 

  // 2. INTERFACE SETUP: LCD and HX711
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Litho-Flow KVS");
  
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale(calibration_factor); 
  scale.tare(); // Resets scale to 0 on startup
  
  delay(2000); 
  last_stable_weight = scale.get_units(10); // Set initial baseline weight
  Serial.println("System Ready.");
}

void loop() {
  // EDGE ANALYTICS: Use filtering to stabilize reading
  float current_reading = scale.get_units(5);

  // --- SERIAL MONITOR DEBUGGING ---
  Serial.print("Weight: "); Serial.print(current_reading, 1);
  Serial.print("g | Baseline: "); Serial.print(last_stable_weight, 1);
  Serial.print("g | Total: "); Serial.print(total_intake, 1);
  Serial.println("ml");

  // 3. LOGIC: CONSTANT BASELINE UPDATE
  // If weight is stable and bottle is on the base, keep updating the baseline
  if (current_reading > (last_stable_weight * 0.8) && !is_lifting) {
    last_stable_weight = current_reading;
  }

  // 4. LOGIC: LIFT DETECTION
  // If weight drops significantly, user has picked up the bottle
  if (current_reading < LIFT_THRESHOLD && !is_lifting) { 
    is_lifting = true;
    lcd.setCursor(0, 1);
    lcd.print("Drinking...     ");
  } 

  // 5. LOGIC: REPLACEMENT & CONSUMPTION CALCULATION
  // If weight returns, wait for stability and calculate intake
  if (current_reading > (LIFT_THRESHOLD + 20.0) && is_lifting) {
    lcd.setCursor(0, 1);
    lcd.print("Processing...   ");
    
    delay(4000); // SIGNAL CONDITIONING: Wait for water to stop sloshing
    float new_weight = scale.get_units(15); // Average 15 samples for accuracy
    
    float consumed = last_stable_weight - new_weight;
    
    if (consumed > DRINK_MINIMUM) { 
      total_intake += consumed;
      
      // DATABASE WRITE: Persist data to EEPROM
      EEPROM.put(0, total_intake);
      EEPROM.commit(); 
      
      last_stable_weight = new_weight; // Update baseline to new lower weight
    }
    is_lifting = false;
  }

  // --- DISPLAY UPDATES (GUI PROTOTYPE) ---
  // Row 0: Daily Progress
  lcd.setCursor(0, 0);
  lcd.print("Intake: ");
  lcd.print(total_intake, 0);
  lcd.print(" ml    ");

  // Row 1: Real-time Weight (only shown when bottle is on the base)
  if (!is_lifting) {
    lcd.setCursor(0, 1);
    lcd.print("Weight: ");
    lcd.print(current_reading, 0);
    lcd.print(" g      ");
  }
  
  delay(500); 
}