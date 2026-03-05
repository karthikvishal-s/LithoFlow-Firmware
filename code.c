#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// --- CONFIGURATION ---
#define EEPROM_SIZE 8          
#define LOADCELL_DOUT_PIN 19
#define LOADCELL_SCK_PIN 18
#define LIFT_THRESHOLD 30.0    
#define DRINK_MINIMUM 10.0     

LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- GLOBAL VARIABLES ---
float total_intake = 0.0;
float last_stable_weight = 0.0;
bool is_lifting = false;
float calibration_factor = -420.0; 
long zero_offset = 0; // Manual tare value

// --- CUSTOM HX711 DRIVER (BIT-BANGING) ---
long readHX711Raw() {
  while (digitalRead(LOADCELL_DOUT_PIN) == HIGH); // Wait for data ready

  long count = 0;
  for (int i = 0; i < 24; i++) {
    digitalWrite(LOADCELL_SCK_PIN, HIGH);
    count = count << 1; 
    digitalWrite(LOADCELL_SCK_PIN, LOW);
    if (digitalRead(LOADCELL_DOUT_PIN)) count++; 
  }

  // 25th pulse: Set Gain to 128 for next reading
  digitalWrite(LOADCELL_SCK_PIN, HIGH);
  count = count ^ 0x800000; // Convert 2's complement
  digitalWrite(LOADCELL_SCK_PIN, LOW);

  return count;
}

float getWeightUnits(int samples) {
  long sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += (readHX711Raw() - zero_offset);
  }
  return (float)(sum / samples) / calibration_factor;
}

void setup() {
  Serial.begin(115200);
  
  // 1. PIN MODES FOR CUSTOM DRIVER
  pinMode(LOADCELL_DOUT_PIN, INPUT);
  pinMode(LOADCELL_SCK_PIN, OUTPUT);

  // 2. LOCAL DATABASE SETUP
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, total_intake);
  if (isnan(total_intake) || total_intake < 0) total_intake = 0.0; 

  // 3. INTERFACE SETUP
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Litho-Flow KVS");
  
  // 4. MANUAL TARE (Zeroing the scale)
  delay(1000);
  long tare_sum = 0;
  for(int i=0; i<10; i++) tare_sum += readHX711Raw();
  zero_offset = tare_sum / 10;
  
  delay(1000); 
  last_stable_weight = getWeightUnits(10); 
  Serial.println("System Ready (Custom Driver).");
}

void loop() {
  // 5. EDGE ANALYTICS: Noise filtering
  float current_reading = getWeightUnits(5);

  Serial.print("Weight: "); Serial.print(current_reading, 1);
  Serial.print("g | Total: "); Serial.print(total_intake, 1);
  Serial.println("ml");

  // 6. BASELINE UPDATE LOGIC
  if (current_reading > (last_stable_weight * 0.8) && !is_lifting) {
    last_stable_weight = current_reading;
  }

  // 7. LIFT DETECTION (FEATURE EXTRACTION)
  if (current_reading < LIFT_THRESHOLD && !is_lifting) { 
    is_lifting = true;
    lcd.setCursor(0, 1);
    lcd.print("Drinking...     ");
  } 

  // 8. PROCESSING DRINK EVENTS
  if (current_reading > (LIFT_THRESHOLD + 20.0) && is_lifting) {
    lcd.setCursor(0, 1);
    lcd.print("Processing...   ");
    
    delay(4000); // Signal Conditioning: sloshing wait
    float new_weight = getWeightUnits(15); 
    float consumed = last_stable_weight - new_weight;
    
    if (consumed > DRINK_MINIMUM) { 
      total_intake += consumed;
      
      // PERSISTENCE DEMO: Save to Flash
      EEPROM.put(0, total_intake);
      EEPROM.commit(); 
      
      last_stable_weight = new_weight; 
    }
    is_lifting = false;
  }

  // 9. GUI PROTOTYPE
  lcd.setCursor(0, 0);
  lcd.print("Intake: ");
  lcd.print(total_intake, 0);
  lcd.print(" ml    ");

  if (!is_lifting) {
    lcd.setCursor(0, 1);
    lcd.print("Weight: ");
    lcd.print(current_reading, 0);
    lcd.print(" g      ");
  }
  
  delay(500); 
}