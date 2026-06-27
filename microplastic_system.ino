// ================================================================
//  AI MICROPLASTIC CONTAMINATION RISK ASSESSMENT SYSTEM
//  ESP32 Dev Module — v2.1
//
//  WIRING:
//    IN1      → GPIO  4   Relay 1 → Pump 1 (Fill)
//    IN2      → GPIO 16   Relay 2 → Pump 2 (Drain)
//    Relay VCC→ 5V | Relay GND → GND
//    Turbidity→ GPIO 13  (ADC2_CH4)
//    Buzzer   → GPIO 19
//    Red  LED → GPIO 25
//    Green LED→ GPIO 26
//    Blue  LED→ GPIO 27
//    UV    LED→ GPIO 14
//    OLED SDA → GPIO 21
//    OLED SCL → GPIO 22
//
//  RELAY LOGIC (Active LOW):
//    digitalWrite(IN1, LOW)  → Relay 1 ON  → Pump 1 runs
//    digitalWrite(IN1, HIGH) → Relay 1 OFF → Pump 1 stops
//    digitalWrite(IN2, LOW)  → Relay 2 ON  → Pump 2 runs
//    digitalWrite(IN2, HIGH) → Relay 2 OFF → Pump 2 stops
//
//  MICROPLASTIC % LOGIC (turbidity-based):
//    CLEAN water (turbidity ≤ 150) → MP% = random 0.00 – 0.99%
//    DIRTY water (turbidity  > 150) → MP% = random 98.0 – 100.0%
//
//  CYCLE (2 cycles then stops):
//    FILL   6s  Pump 1 ON
//    SETTLE 3s  Both OFF
//    SCAN       LED flash → read → classify → display
//    DRAIN  6s  Pump 2 ON
//    IDLE   2s  Both OFF → next cycle
//    After 2nd cycle: Final result → HALT
// ================================================================

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── RELAY PINS — same pattern as RelayTest.ino ───────────────
#define IN1   4    // Relay 1 → Pump 1 (Fill)
#define IN2  16    // Relay 2 → Pump 2 (Drain)

// ── OTHER PINS ───────────────────────────────────────────────
#define PIN_TURB     13    // ADC2_CH4 — turbidity sensor data pin
#define PIN_BUZZER   19
#define PIN_LED_RED  25
#define PIN_LED_GRN  26
#define PIN_LED_BLU  27
#define PIN_LED_UV   14

// ── OLED ─────────────────────────────────────────────────────
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;

// ── TIMING ───────────────────────────────────────────────────
#define T_FILL    6000    // Pump 1 ON  (6s)
#define T_SETTLE  3000    // Both OFF   (3s)
#define T_DRAIN   6000    // Pump 2 ON  (6s)
#define T_IDLE    2000    // Both OFF   (2s)
#define CYCLES       2    // Run 2 cycles then stop

// ── SENSOR CALIBRATION ───────────────────────────────────────
// Turbidity sensor: HIGH ADC = CLEAR water, LOW ADC = DIRTY water
// MEASURED real values (GPIO 13 test):
//   Clean water  → ADC ≈ 1900–1930  → CAL_CLEAN = 1950
//   Dirty water  → ADC ≈  400–500   → CAL_DIRTY = 400
#define CAL_CLEAN  1950   // Raw ADC in clean water (measured)
#define CAL_DIRTY   400   // Raw ADC in dirty water (measured)

// readTurbidity() returns 0 (clean) → 1023 (very dirty)
// Classify thresholds — lower turbidity = cleaner water
// ── DECISION TREE THRESHOLDS ─────────────────────────────────
#define TH1   80    // <= Very Low Risk  → CLEAN
#define TH2   150   // <= Low Risk       → CLEAN   (clean/dirty boundary)
#define TH3   400   // <= Medium Risk    → DIRTY
#define TH4   650   // <= High Risk      → DIRTY
                    //  > Very High Risk → DIRTY

// Clean/Dirty boundary for MP% calculation
#define CLEAN_DIRTY_THRESHOLD  150   // turbidity index: below = CLEAN, above = DIRTY

// ── RISK LABELS ──────────────────────────────────────────────
const char* RISK_NAME[] = {
  "Very Low Risk",
  "Low Risk",
  "Medium Risk",
  "High Risk",
  "Very High Risk"
};

// ── STATE MACHINE ─────────────────────────────────────────────
enum State { ST_FILL, ST_SETTLE, ST_SCAN, ST_DRAIN, ST_IDLE, ST_DONE };
State         gState   = ST_FILL;
unsigned long gStateMs = 0;

// ── COUNTERS ─────────────────────────────────────────────────
int   gCycle   = 0;
float gSumTurb = 0;
float gSumPct  = 0;
int   gDirty   = 0;

// =============================================================
//  RELAY CONTROL — exact pattern from RelayTest.ino
// =============================================================
void pump1On()  {
  digitalWrite(IN1, LOW);
  Serial.println(F("[RELAY1] ON  — Pump 1 Fill"));
}
void pump1Off() {
  digitalWrite(IN1, HIGH);
  Serial.println(F("[RELAY1] OFF — Pump 1 Fill"));
}
void pump2On()  {
  digitalWrite(IN2, LOW);
  Serial.println(F("[RELAY2] ON  — Pump 2 Drain"));
}
void pump2Off() {
  digitalWrite(IN2, HIGH);
  Serial.println(F("[RELAY2] OFF — Pump 2 Drain"));
}
void pumpsOff() {
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, HIGH);
}

// =============================================================
//  LED CONTROL
// =============================================================
void ledsOff() {
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GRN, LOW);
  digitalWrite(PIN_LED_BLU, LOW);
  digitalWrite(PIN_LED_UV,  LOW);
}

// Flash LEDs in sequence during sensor scan: R→G→B→UV→ALL→OFF
void ledScanFlash() {
  ledsOff();
  digitalWrite(PIN_LED_RED, HIGH); delay(400); digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GRN, HIGH); delay(400); digitalWrite(PIN_LED_GRN, LOW);
  digitalWrite(PIN_LED_BLU, HIGH); delay(400); digitalWrite(PIN_LED_BLU, LOW);
  digitalWrite(PIN_LED_UV,  HIGH); delay(500);
  // All ON for final reading
  digitalWrite(PIN_LED_RED, HIGH);
  digitalWrite(PIN_LED_GRN, HIGH);
  digitalWrite(PIN_LED_BLU, HIGH);
  delay(300);
  ledsOff();
}

// Show risk result on LEDs
void ledRisk(int r) {
  ledsOff();
  if (r <= 1) {
    // Very Low / Low → Green steady (safe)
    digitalWrite(PIN_LED_GRN, HIGH);
  } else if (r == 2) {
    // Medium → Blue steady (caution)
    digitalWrite(PIN_LED_BLU, HIGH);
  } else if (r == 3) {
    // High → Red steady (warning)
    digitalWrite(PIN_LED_RED, HIGH);
  } else {
    // Very High → Red + UV blink × 5 then Red stays ON
    for (int i = 0; i < 5; i++) {
      digitalWrite(PIN_LED_RED, HIGH);
      digitalWrite(PIN_LED_UV,  HIGH);
      delay(200);
      digitalWrite(PIN_LED_RED, LOW);
      digitalWrite(PIN_LED_UV,  LOW);
      delay(150);
    }
    digitalWrite(PIN_LED_RED, HIGH);  // leave red on
  }
}

// =============================================================
//  SAFE ADC READ — yield() prevents watchdog freeze on ADC2
// =============================================================
int safeAnalogRead() {
  yield();
  int v = analogRead(PIN_TURB);
  yield();
  return v;
}

// =============================================================
//  SENSOR READ — 16-sample averaged, drop min+max
// =============================================================
float readTurbidity() {
  uint32_t sum = 0;
  int mn = 4095, mx = 0;
  for (int i = 0; i < 16; i++) {
    int v = safeAnalogRead();
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    sum += v;
    delayMicroseconds(200);   // increased for ADC2 stability
  }
  float avg = (float)(sum - mn - mx) / 14.0f;
  // HIGH ADC (clean) → LOW turbidity; LOW ADC (dirty) → HIGH turbidity
  float t = (float)map((long)avg, CAL_CLEAN, CAL_DIRTY, 0, 1023);
  return constrain(t, 0.0f, 1023.0f);
}

// Returns "CLEAN" or "DIRTY" based on risk level
const char* waterStatusStr(int r) {
  return (r <= 1) ? "CLEAN" : "DIRTY";
}

// =============================================================
//  DECISION TREE CLASSIFIER
// =============================================================
int classify(float t) {
  if (t <= TH1) return 0;
  if (t <= TH2) return 1;
  if (t <= TH3) return 2;
  if (t <= TH4) return 3;
  return 4;
}

// ── MICROPLASTIC % BASED ON TURBIDITY ────────────────────────
//  CLEAN water (turb <= CLEAN_DIRTY_THRESHOLD):
//    Returns a random float in [0.00, 0.99] (< 1%)
//  DIRTY water (turb  > CLEAN_DIRTY_THRESHOLD):
//    Returns a random float in [98.0, 100.0]
// Uses esp_random() — ESP32 hardware RNG (true random)
float estimatePct(float t, int r) {
  bool isClean = (t <= CLEAN_DIRTY_THRESHOLD);
  if (isClean) {
    // 0.00 – 0.99 %  (random, two decimal places)
    uint32_t rnd = esp_random() % 100;   // 0 … 99
    return (float)rnd / 100.0f;          // 0.00 … 0.99
  } else {
    // 98.0 – 100.0 %  (random, one decimal place)
    uint32_t rnd = esp_random() % 21;    // 0 … 20
    return 98.0f + (float)rnd / 10.0f;  // 98.0 … 100.0
  }
}

// =============================================================
//  OLED HELPERS
// =============================================================
void oledHeader(const char* title) {
  oled.clearDisplay();
  oled.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(4, 2);
  oled.print(title);
  oled.setTextColor(SSD1306_WHITE);
}

void oledStatus(const char* l1, const char* l2) {
  if (!oledOk) return;
  oledHeader("MICROPLASTIC SYS");
  oled.setCursor(4, 16); oled.print(l1);
  oled.setCursor(4, 28); oled.print(l2);
  oled.setCursor(4, 42);
  oled.print("Cycle: "); oled.print(gCycle); oled.print("/"); oled.print(CYCLES);
  oled.display();
}

void oledResult(float turb, int r, float pct) {
  if (!oledOk) return;
  oledHeader("MICROPLASTIC RISK");
  // Water status — big and prominent
  bool isClean = (turb <= CLEAN_DIRTY_THRESHOLD);
  oled.setCursor(0, 14);
  oled.print(isClean ? "Status: CLEAN WATER" : "Status: DIRTY WATER");
  oled.setCursor(0, 26); oled.print("Turbidity : "); oled.print((int)turb);
  // Show 2 decimal places for clean (<1%), 1 decimal for dirty
  oled.setCursor(0, 38); oled.print("MP Risk   : ");
  oled.print(pct, isClean ? 2 : 1); oled.print("%");
  oled.setCursor(0, 50); oled.print(RISK_NAME[r]);
  oled.display();
}

void oledFinal(float avgT, float avgP, bool dirty) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK); oled.setCursor(22, 2);
  oled.print("FINAL RESULT");
  oled.setTextColor(SSD1306_WHITE);
  // Water status — large text (size 2) centred
  oled.setTextSize(2);
  oled.setCursor(dirty ? 4 : 16, 14);
  oled.print(dirty ? "DIRTY" : "CLEAN");
  oled.setTextSize(1);
  oled.setCursor(0, 34); oled.print("Turbidity : "); oled.print(avgT, 1);
  oled.setCursor(0, 44); oled.print("MP Risk   : ");
  oled.print(avgP, dirty ? 1 : 2); oled.print("%");
  oled.setCursor(0, 54); oled.print(dirty ? "CONTAMINATED" : "CLEAN WATER");
  oled.display();
}

// =============================================================
//  BUZZER
//  CLEAN → 5 short beeps (safe signal)
//  DIRTY → continuous beeping for 10 seconds (danger alert)
// =============================================================
void buzzAlert(bool isClean) {
  if (isClean) {
    // 5 short confirmation beeps — CLEAN WATER
    for (int i = 0; i < 5; i++) {
      tone(PIN_BUZZER, 2000, 120);
      delay(200);
      noTone(PIN_BUZZER);
      delay(80);
    }
  } else {
    // Continuous beeping for 10 seconds — DIRTY WATER ALARM
    unsigned long start = millis();
    while (millis() - start < 10000) {
      tone(PIN_BUZZER, 4500, 150);
      delay(200);
      noTone(PIN_BUZZER);
      delay(80);
    }
  }
}

// =============================================================
//  STATE TRANSITIONS
// =============================================================
void enterState(State s) {
  gState   = s;
  gStateMs = millis();
  pumpsOff();  // always safe-off before changing state

  switch (s) {
    case ST_FILL:
      delay(200);     // caps settle before relay fires
      pump1On();
      Serial.println(F("[STATE] FILLING 6s"));
      oledStatus("PUMP 1 ON", "Filling chamber...");
      break;

    case ST_SETTLE:
      Serial.println(F("[STATE] SETTLING 3s"));
      oledStatus("PUMPS OFF", "Water settling...");
      break;

    case ST_SCAN:
      Serial.println(F("[STATE] SCANNING"));
      oledStatus("SCANNING", "LED flash reading...");
      break;

    case ST_DRAIN:
      delay(200);
      pump2On();
      Serial.println(F("[STATE] DRAINING 6s"));
      oledStatus("PUMP 2 ON", "Draining chamber...");
      break;

    case ST_IDLE:
      Serial.println(F("[STATE] IDLE 2s"));
      oledStatus("PUMPS OFF", "Next cycle soon...");
      break;

    case ST_DONE:
      pumpsOff();
      ledsOff();
      Serial.println(F("[HALT] System stopped. Press RESET to run again."));
      break;
  }
}

// =============================================================
//  SETUP
// =============================================================
void setup() {
  // Disable brownout — LM2596 ripple can trigger reset
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(200);
  Serial.println(F("=============================================="));
  Serial.println(F("  AI MICROPLASTIC RISK ASSESSMENT — v2.0     "));
  Serial.println(F("  Relay: IN1=GPIO4  IN2=GPIO16 (Active LOW)  "));
  Serial.println(F("  LEDs : R=25 G=26 B=27 UV=14               "));
  Serial.println(F("  Turb : GPIO13 | Buzz: GPIO19               "));
  Serial.println(F("=============================================="));

  // ── RELAY INIT — exact RelayTest.ino pattern ─────────────
  // HIGH before pinMode: prevents relay clicking ON during boot
  digitalWrite(IN1, HIGH); pinMode(IN1, OUTPUT);   // OFF
  digitalWrite(IN2, HIGH); pinMode(IN2, OUTPUT);   // OFF
  Serial.println(F("[RELAY] IN1 & IN2 set HIGH (OFF)"));

  // ── LED INIT ─────────────────────────────────────────────
  digitalWrite(PIN_LED_RED, LOW); pinMode(PIN_LED_RED, OUTPUT);
  digitalWrite(PIN_LED_GRN, LOW); pinMode(PIN_LED_GRN, OUTPUT);
  digitalWrite(PIN_LED_BLU, LOW); pinMode(PIN_LED_BLU, OUTPUT);
  digitalWrite(PIN_LED_UV,  LOW); pinMode(PIN_LED_UV,  OUTPUT);

  // LED startup test — all ON 500ms then OFF
  digitalWrite(PIN_LED_RED, HIGH);
  digitalWrite(PIN_LED_GRN, HIGH);
  digitalWrite(PIN_LED_BLU, HIGH);
  digitalWrite(PIN_LED_UV,  HIGH);
  delay(500);
  ledsOff();
  Serial.println(F("[LED] Startup test done"));

  // ── BUZZER INIT ──────────────────────────────────────────
  digitalWrite(PIN_BUZZER, LOW); pinMode(PIN_BUZZER, OUTPUT);

  // ── ADC INIT ───────────────────────────────────────────────
  pinMode(PIN_TURB, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_TURB, ADC_11db);
  delay(100);   // let ADC2 settle before first read

  // ── OLED INIT ────────────────────────────────────────────
  Wire.begin(21, 22);
  Wire.setClock(400000);
  delay(50);
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C) ||
      oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledOk = true;
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oledHeader("MICROPLASTIC SYS");
    oled.setCursor(4, 16); oled.print("AI Risk Assessment");
    oled.setCursor(4, 28); oled.print("Decision Tree Model");
    oled.setCursor(4, 40); oled.print("2 Cycles | 5 Levels");
    oled.setCursor(4, 52); oled.print("Initialising...");
    oled.display();
    delay(2500);
    Serial.println(F("[OLED] OK"));
  } else {
    Serial.println(F("[OLED] Not found"));
  }

  // Startup beep
  tone(PIN_BUZZER, 1500, 100); delay(150); noTone(PIN_BUZZER);
  delay(80);
  tone(PIN_BUZZER, 2000, 100); delay(150); noTone(PIN_BUZZER);

  // Begin
  Serial.println(F("\n=== CYCLE 1 / 2 ==="));
  enterState(ST_FILL);
}

// =============================================================
//  LOOP — Non-blocking state machine
// =============================================================
void loop() {

  if (gState == ST_DONE) {
    delay(1000);
    return;
  }

  unsigned long elapsed = millis() - gStateMs;

  switch (gState) {

    // ── Pump 1 ON 6s ─────────────────────────────────────
    case ST_FILL:
      if (elapsed >= T_FILL) {
        pump1Off();
        enterState(ST_SETTLE);
      }
      break;

    // ── Both OFF 3s ──────────────────────────────────────
    case ST_SETTLE:
      if (elapsed >= T_SETTLE) {
        enterState(ST_SCAN);
      }
      break;

    // ── LED flash → Read → Classify → Display → Alert ────
    case ST_SCAN: {

      // Flash LEDs during sensor reading
      ledScanFlash();

      // Read turbidity — also grab a raw single sample for display
      int   rawAdc = safeAnalogRead();
      float turb   = readTurbidity();

      // Classify
      int   r   = classify(turb);
      float pct = estimatePct(turb, r);

      // Accumulate for final average
      gCycle++;
      gSumTurb += turb;
      gSumPct  += pct;
      if (r >= 3) gDirty++;

      // ── Serial output ─────────────────────────────────
      bool isCleanWater = (turb <= CLEAN_DIRTY_THRESHOLD);
      Serial.println(F("=========================================="));
      Serial.println(F("   AI MICROPLASTIC DETECTION SYSTEM       "));
      Serial.println(F("------------------------------------------"));
      Serial.printf("  Cycle        : %d / %d\n", gCycle, CYCLES);
      Serial.printf("  Water Status : %s\n",       isCleanWater ? "CLEAN" : "DIRTY");
      if (isCleanWater) {
        Serial.printf("  Microplastic : %.2f%%\n",  pct);
      } else {
        Serial.printf("  Microplastic : %.1f%%\n",  pct);
      }
      Serial.println(F("=========================================="));

      // ── DIRTY WATER ALERT
      if (!isCleanWater) {
        Serial.println(F("  !! DIRTY WATER DETECTED !!"));
        Serial.printf ("  !! Microplastic : %.1f%%  !!\n", pct);
        Serial.println(F("  !! WATER IS CONTAMINATED !!"));
      } else {
        Serial.println(F("  CLEAN WATER — Safe"));
      }
      Serial.flush();

      // Show on OLED
      oledResult(turb, r, pct);

      // Show on LEDs
      ledRisk(r);

      // Buzzer — 5 beeps CLEAN, continuous 10s DIRTY
      buzzAlert(isCleanWater);

      // ── After 2nd cycle: final result → HALT ────────────────
      if (gCycle >= CYCLES) {
        float avgT   = gSumTurb / CYCLES;
        float avgP   = gSumPct  / CYCLES;
        bool  bad    = (avgT > CLEAN_DIRTY_THRESHOLD);  // based on avg turbidity

        Serial.println(F("\n╔══════════════════════════════════╗"));
        Serial.println(F(  "║  AI MICROPLASTIC DETECTION SYS  ║"));
        Serial.println(F(  "║         FINAL RESULT             ║"));
        Serial.println(F(  "╠══════════════════════════════════╣"));
        Serial.printf(     "  Water Status  : %s\n",  bad ? "DIRTY / CONTAMINATED" : "CLEAN");
        if (bad) {
          Serial.printf(   "  Microplastic  : %.1f%%\n", avgP);
        } else {
          Serial.printf(   "  Microplastic  : %.2f%%\n", avgP);
        }
        Serial.println(F("╚══════════════════════════════════╝"));
        if (bad) {
          Serial.println(F("  !! *** MICROPLASTIC RISK ***     !!"));
          Serial.printf (  "  !! Microplastic : %.1f%%          !!\n", avgP);
          Serial.println(F("  !! WATER IS CONTAMINATED         !!"));
        } else {
          Serial.println(F("  CLEAN WATER — No significant microplastic detected"));
        }
        Serial.flush();

        delay(3000);           // show cycle result 3s
        oledFinal(avgT, avgP, bad);

        // Final buzzer — same logic: 5 beeps clean, continuous dirty
        buzzAlert(!bad);

        // Leave risk LED on permanently until RESET
        ledRisk(bad ? 4 : 0);

        enterState(ST_DONE);   // HALT
        return;
      }

      // ── Cycle 1 done: drain then idle ─────────────────
      delay(3000);   // show result 3s before draining
      ledsOff();
      enterState(ST_DRAIN);
      break;
    }

    // ── Pump 2 ON 6s ─────────────────────────────────────
    case ST_DRAIN:
      if (elapsed >= T_DRAIN) {
        pump2Off();
        enterState(ST_IDLE);
      }
      break;

    // ── Both OFF 2s → next cycle ─────────────────────────
    case ST_IDLE:
      if (elapsed >= T_IDLE) {
        Serial.printf("\n=== CYCLE %d / %d ===\n", gCycle + 1, CYCLES);
        enterState(ST_FILL);
      }
      break;

    case ST_DONE:
      break;
  }

  delay(10);   // feed watchdog
  yield();
}
