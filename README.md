# ESP32-Retro-Arcade
ESP32 Retro Arcade (prototype) – A handheld console with Car Dodge, Snake, and Pong games on a 128×64 OLED, controlled by an analog joystick. Features persistent high scores, sound effects, triple-click exit, and deep sleep power‑off. Built with Arduino, U8g2, and ESP32 NVS.

<div align="center">
  <h1>🎮 ESP32 Retro Arcade</h1>
  <p><strong>Three classic games – Car Dodge, Snake, Pong – on a custom handheld console with OLED display and analog joystick.</strong></p>
  <p>
    <img src="https://img.shields.io/badge/ESP32-Arduino-blue.svg" alt="ESP32">
    <img src="https://img.shields.io/badge/Library-U8g2-brightgreen" alt="U8g2">
    <img src="https://img.shields.io/badge/License-MIT-green" alt="License">
    <img src="https://img.shields.io/badge/PlatformIO-Ready-orange" alt="PlatformIO">
  </p>
</div>

---

## 📖 Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Hardware Required](#hardware-required)
- [Wiring Diagram](#wiring-diagram)
- [Installation](#installation)
- [Usage](#usage)
- [Customisation](#customisation)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## 🕹 Overview
This is a **fully functional retro gaming console** built around an ESP32, a 128×64 I²C OLED screen, and a KY‑023 analog joystick.  
It runs three games, stores high scores permanently, plays sound effects through a passive buzzer, and even goes to deep sleep to save battery.

> [!IMPORTANT]  
> All code is written in Arduino C++ and uses the U8g2 graphics library.  
> You can easily add your own games – the framework is modular.

---

## ✨ Features

| Feature                     | Description                                                                 |
|-----------------------------|-----------------------------------------------------------------------------|
| **Car Game**                | Dodge traffic (cars, trucks, speeders, drifters). Speed increases with score. |
| **Snake Game**              | Classic Snake – walls wrap, only self‑collision kills.                     |
| **Pong**                    | Ball drops from the top; deflect with paddle. Speed ramps every 7 points.   |
| **Persistent High Scores**  | Best scores saved in ESP32 flash (NVS) – survive reboots.                  |
| **Sound Effects**           | Passive buzzer: eating, crashing, paddle hit, game over jingle, boot chime. |
| **Idle Sleep**              | After 5 s inactivity, a “Sleep...” animation appears – any movement wakes. |
| **Deep Sleep Power‑Off**    | Menu item “Turn Off” puts ESP32 into deep sleep. Press RESET to restart.   |
| **Triple‑Click Exit**       | From any game, press button 3× quickly (≤800 ms) to return to menu.        |
| **Scrollable Menu**         | Shows high scores next to each game name. Scroll bar on the right.         |

---

## 🧰 Hardware Required

| Component                     | Recommendation                         | Qty |
|-------------------------------|----------------------------------------|-----|
| ESP32 development board       | ESP32‑DevKitC, NodeMCU‑32S, etc.       | 1   |
| OLED display                  | 128×64, I²C (SSD1306 or SH1106)       | 1   |
| Analog joystick module        | KY‑023 (X, Y, button)                 | 1   |
| Passive buzzer (2‑pin)        | e.g. 5V passive (can be active)       | 1   |
| Breadboard & jumper wires     | –                                      | as needed |
| 5 V power supply (or battery) | Micro‑USB or USB‑C (if using custom PCB) | 1   |

---

## 🔌 Wiring Diagram

| ESP32 GPIO | Component Pin           |
|------------|-------------------------|
| `34`       | Joystick X‑axis (ADC)   |
| `35`       | Joystick Y‑axis (ADC)   |
| `32`       | Joystick button (INPUT_PULLUP) |
| `21`       | OLED SDA (I2C)          |
| `22`       | OLED SCL (I2C)          |
| `25`       | Passive buzzer signal   |
| `GND`      | Joystick GND, buzzer GND, OLED GND |
| `3.3V`     | OLED VCC                |

> 📸 *See the `docs/` folder for a detailed Fritzing diagram (coming soon).*

---

## 🚀 Installation

### 1. Install Required Libraries
- [U8g2](https://github.com/olikraus/u8g2) – graphics library  
  `Arduino IDE → Tools → Manage Libraries → search “U8g2” → install`

### 2. Setup ESP32 Board Support
Add the following URL to **File → Preferences → Additional Boards Manager URLs**:
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

text
Then go to **Tools → Board → Boards Manager**, search for “ESP32” and install the **ESP32 by Espressif Systems** package.

### 3. Open the Sketch
Download or clone this repository, then open `retro_arcade.ino` in Arduino IDE.

### 4. Select your ESP32 Board
**Tools → Board → ESP32 Arduino → ESP32 Dev Module** (or your specific variant).

### 5. Upload
Click the **Upload** button. After a few seconds, the splash screen should appear on the OLED.

---

## 🎮 Usage

| Action                     | Joystick Input                                  |
|----------------------------|-------------------------------------------------|
| Move menu cursor           | Y‑axis **UP / DOWN**                           |
| Select game / item         | Press button (1×)                              |
| Car game steering          | X‑axis **LEFT / RIGHT**                        |
| Snake direction            | X‑axis or Y‑axis                               |
| Pong paddle                | X‑axis **LEFT / RIGHT**                        |
| Triple‑click exit          | Press button **3× quickly** (≤800 ms between) |
| Turn off (from menu)       | Select “Turn Off” → deep sleep (press RESET to wake) |

> **Note:** When the console is idle for 5 seconds, it shows a sleep animation. Any joystick movement instantly wakes it back to the menu.

---

🛠 Customisation
All game parameters and pin definitions are clearly marked at the top of the sketch.

Change pin assignments
cpp
#define PIN_JOY_X      34
#define PIN_JOY_Y      35
#define PIN_JOY_BTN    32
#define I2C_SDA        21
#define I2C_SCL        22
#define PIN_BUZZ       25
Car difficulty tiers
Look for the functions carObsSpeed(), carSpawnEvery(), carSpawnChance(), and carMaxType().

Snake speed
Edit the values inside snakeInterval():

cpp
int ms = 280 - (snakeScore / 4) * 20;
return (unsigned long)max(ms, 100);
Pong ball speed
Adjust PONG_BASE_SPD, PONG_SPEED_STEP, and PONG_SPEED_EVERY.

Sound effects
Each sound is a small function (soundBoot(), soundEat(), …). Change the frequencies or durations.
If you don’t want sound, comment out the tone() calls.

🧪 Troubleshooting
Issue	Likely solution
OLED shows nothing	Check I2C wiring (SDA/SCL), power, and contrast. Try the U8G2_SH1106 constructor if using an SH1106 display.
Joystick drifts / erratic	Increase ADC_SAMPLES or adjust JOY_THRESHOLD_UP/DN and JOY_HYSTERESIS.
Buzzer silent	For passive buzzers, ensure tone() is used. For active buzzers, the code will only click. Try a different GPIO.
USB‑C cable doesn’t power	Your board may be missing the 5.1kΩ pull‑down resistors on CC1/CC2 – required for C‑to‑C cables.
Triple‑click doesn’t exit	Press the button three times within 800 ms (increase TC_WINDOW_MS if needed).
Compilation error ledcSetup not declared	Board not set to ESP32 – go to Tools → Board and select an ESP32 variant.
<div align="center"> <sub>Built with ❤️ for retro gaming on the ESP32. <br> Contributions and bug reports are welcome!</sub> </div>
