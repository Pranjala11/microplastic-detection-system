# 🔬 AI-Based Microplastic Contamination Risk Assessment System

> An embedded IoT system that detects microplastic contamination risk in water using optical light scattering, turbidity sensing, and a machine learning decision tree classifier — built on the ESP32 platform with real-time OLED display, automated pump control, and multi-level alert output.

---

## 📌 Project Overview

Microplastic pollution is one of the most pressing environmental issues of our time, yet conventional detection methods (FTIR spectroscopy, Raman spectroscopy, microscopy) are expensive, slow, and require laboratory infrastructure. This project proposes a **low-cost, real-time, IoT-based alternative** for contamination risk screening in field conditions.

The system automates a full water sampling cycle — filling, settling, scanning, and draining — across two complete cycles, then outputs an averaged final contamination risk result via OLED display, coloured LEDs, and a buzzer alert.

---

## 🧠 How the AI Classification Works

The system uses a **Decision Tree classifier** implemented directly in embedded C++ firmware. Turbidity is measured via a 16-sample averaged ADC read (with min/max dropped for noise rejection), then mapped to a 0–1023 risk index and passed through threshold-based classification:

| Turbidity Index | Risk Level     | Water Status | Microplastic % Estimate |
|-----------------|----------------|--------------|--------------------------|
| 0 – 80          | Very Low Risk  | ✅ CLEAN     | 0.00 – 0.99%             |
| 81 – 150        | Low Risk       | ✅ CLEAN     | 0.00 – 0.99%             |
| 151 – 400       | Medium Risk    | ⚠️ DIRTY    | 98.0 – 100.0%            |
| 401 – 650       | High Risk      | ⚠️ DIRTY    | 98.0 – 100.0%            |
| > 650           | Very High Risk | 🔴 DIRTY    | 98.0 – 100.0%            |

Microplastic percentage is estimated using the ESP32's **hardware true-random number generator** (`esp_random()`), seeded within the appropriate range for each contamination category.

---

## ⚙️ System Cycle (State Machine)

The firmware runs a **non-blocking state machine** through the following sequence, repeated for 2 complete cycles before halting with a final averaged result:

```
ST_FILL   →  ST_SETTLE  →  ST_SCAN  →  ST_DRAIN  →  ST_IDLE
 (6s)          (3s)         (scan)       (6s)          (2s)
  Pump 1 ON   Both OFF   Read+Classify  Pump 2 ON   Both OFF
                                ↓
                         After Cycle 2:
                         Final Result → ST_DONE (HALT)
```

**SCAN phase detail:**
1. LEDs flash in sequence: Red → Green → Blue → UV → All ON
2. 16-sample turbidity reading with averaged ADC
3. Decision tree classification → risk level assigned
4. Result displayed on OLED + LEDs + buzzer alert
5. Serial output printed with full contamination report

---

## 🔌 Hardware Components

| Component             | Role                                      |
|-----------------------|-------------------------------------------|
| ESP32 Dev Module      | Main microcontroller (Wi-Fi + dual-core)  |
| Turbidity Sensor      | Primary water clarity measurement         |
| SSD1306 OLED 128×64  | Real-time result display                  |
| 2-Channel Relay Module| Controls Fill Pump (Pump 1) and Drain Pump (Pump 2) |
| RGB LEDs (R/G/B)      | Visual risk indicator                     |
| UV LED                | Optical scattering enhancement            |
| Active Buzzer         | Audio alert (clean = 5 beeps, dirty = 10s alarm) |
| DC Water Pumps × 2   | Automated fill and drain                  |
| LM2596 Voltage Reg.  | Stable 5V supply (brownout protection)    |
| Li-Ion Battery Pack   | Portable field operation                  |

---

## 🗺️ Wiring Reference

```
ESP32 GPIO   →  Component
─────────────────────────────────────
GPIO  4      →  Relay IN1  → Pump 1 (Fill)
GPIO 16      →  Relay IN2  → Pump 2 (Drain)
GPIO 13      →  Turbidity Sensor (ADC2_CH4)
GPIO 19      →  Buzzer
GPIO 25      →  Red LED
GPIO 26      →  Green LED
GPIO 27      →  Blue LED
GPIO 14      →  UV LED
GPIO 21      →  OLED SDA
GPIO 22      →  OLED SCL
```

> **Relay Logic:** Active LOW — `digitalWrite(IN1, LOW)` turns Pump 1 ON.
> Relay pins are set HIGH before `pinMode()` during boot to prevent startup clicking.

---

## 💡 LED Alert Behaviour

| Risk Level     | LED Output                              |
|----------------|-----------------------------------------|
| Very Low / Low | 🟢 Green steady — Safe                  |
| Medium         | 🔵 Blue steady — Caution                |
| High           | 🔴 Red steady — Warning                 |
| Very High      | 🔴 Red + UV blink × 5 → Red stays ON   |

---

## 🔊 Buzzer Alert Behaviour

| Water Status | Alert                                     |
|--------------|-------------------------------------------|
| CLEAN        | 5 short confirmation beeps (2000 Hz)      |
| DIRTY        | Continuous alarm beeping for 10 seconds (4500 Hz) |

---

## 📟 OLED Output (Per Cycle)

```
┌────────────────────────┐
│   MICROPLASTIC RISK    │  ← Inverted header
│ Status: CLEAN WATER    │
│ Turbidity : 87         │
│ MP Risk   : 0.42%      │
│ Low Risk               │
└────────────────────────┘
```

Final result screen uses **size-2 text** for the water status (CLEAN / DIRTY) for maximum visibility.

---

## 📋 Serial Monitor Output (Sample)

```
==========================================
   AI MICROPLASTIC DETECTION SYSTEM
------------------------------------------
  Cycle        : 1 / 2
  Water Status : CLEAN
  Microplastic : 0.42%
==========================================
  CLEAN WATER — Safe

╔══════════════════════════════════╗
║  AI MICROPLASTIC DETECTION SYS  ║
║         FINAL RESULT             ║
╠══════════════════════════════════╣
  Water Status  : CLEAN
  Microplastic  : 0.38%
╚══════════════════════════════════╝
  CLEAN WATER — No significant microplastic detected
```

---

## 🛠️ Software & Tools

- **Language:** C++ (Arduino framework for ESP32)
- **IDE:** Arduino IDE
- **Libraries:**
  - `Adafruit_SSD1306` — OLED display driver
  - `Adafruit_GFX` — Graphics rendering
  - `Wire.h` — I2C communication
  - `esp_random()` — ESP32 hardware true-RNG

---

## 🚀 Getting Started

### Prerequisites
- Arduino IDE with ESP32 board support installed
- Board: `ESP32 Dev Module`
- Libraries: Install via Arduino Library Manager:
  - `Adafruit SSD1306`
  - `Adafruit GFX Library`

### Upload Steps
1. Clone this repository
2. Open `src/microplastic_system.ino` in Arduino IDE
3. Select board: **ESP32 Dev Module**
4. Select the correct COM port
5. Click **Upload**
6. Open Serial Monitor at **115200 baud** to view live output

### Calibration
Adjust these constants in the code to match your turbidity sensor's actual ADC readings:
```cpp
#define CAL_CLEAN  1950   // ADC value in clean water (measure yours)
#define CAL_DIRTY   400   // ADC value in dirty water (measure yours)
```

---

## 📊 Optical Detection Principle

The system uses multi-angle optical scattering detection:

```
LED Source → Water Sample → Turbidity Sensor
               ↓
      Particles present?
      ├── YES → Light scatters → High turbidity index → DIRTY
      └── NO  → Light transmits → Low turbidity index → CLEAN
```

RGB and UV LEDs illuminate the water sample from multiple angles. When microplastic or suspended particles are present, light scattering increases and transmitted intensity decreases — this is captured as a higher turbidity reading.

---

## ⚠️ Limitations

- The system estimates **contamination risk**, not exact microplastic concentration
- Optical readings can be influenced by non-plastic suspended particles (silt, algae)
- Environmental lighting conditions may affect sensor baseline readings
- Intended as a **field screening tool**, not a replacement for laboratory analysis

---

## 🔭 Future Work

- Camera-based image analysis for particle shape classification
- Deep learning model replacing the decision tree
- Wi-Fi/MQTT integration for cloud-based monitoring dashboard
- GPS tagging for contamination mapping
- Multi-sensor fusion (pH, dissolved oxygen, conductivity)
- Laboratory cross-validation dataset

---

## 📁 Repository Structure

```
microplastic-detection-esp32/
├── src/
│   └── microplastic_system.ino     ← Main firmware
├── docs/
│   ├── project_report.pdf          ← Full technical report
│   └── wiring_diagram.png          ← Circuit diagram
├── images/
│   ├── hardware_photo.jpg          ← Physical prototype
│   └── oled_output.jpg             ← OLED screen output
└── README.md
```

---

## 👤 Author

1. Shruti Diware
2. Avanti Bhoyar
3. Pranjala Meshram
4. Khushali Nikhade

## Emails

1. shrutidiware@gmail.com
2. avantibhoyar321@gmail.com
3. pranjalameshram18@gmail.com
4. khushalinikhade06@gmail.com

## Institute

Jhulelal Institute of Technology 



---

## 📄 License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.

---

> *Built as part of an IoT-based environmental monitoring research project.*
> *For academic and research use.*
