/*
 * ============================================================
 *  ESP32 Retro Arcade
 *  Games  : Car Game · Snake · Pong
 *  Display: SSD1306 128×64 OLED (I2C, U8g2 library)
 *  Input  : KY-023 analog joystick module
 *
 *  Wiring
 *    JOY X   → GPIO 34      OLED SDA → GPIO 21
 *    JOY Y   → GPIO 35      OLED SCL → GPIO 22
 *    JOY BTN → GPIO 32      OLED VCC → 3.3 V
 *
 *  Universal controls
 *    Joystick Y-axis  — menu scroll up/down
 *    Joystick X-axis  — car steer · snake steer · pong paddle
 *    Button (click×1) — confirm / select in menu
 *    Button (click×3) — exit any game/screen → back to main menu
 *
 *  Menu items (scrollable, 3 visible at a time)
 *    1. Car Game   — dodge incoming traffic, speed increases with score
 *    2. Snake      — eat food, walls wrap (no wall death)
 *    3. Pong       — deflect the ball with the paddle
 *    4. Turn Off   — ESP32 enters deep sleep; reset button to wake
 *
 *  Idle timeout: after 5 s of no input the "Sleep...Zzz" animation
 *  plays; any joystick movement wakes the device back to the menu.
 * ============================================================
 */

#include <U8g2lib.h>
#include <vector>
#include <esp_sleep.h>      // needed for esp_deep_sleep_start()
#include <Preferences.h>    // NVS key-value store — persists across power cycles

// ╔══════════════════════════════════════════════════════════╗
// ║  HARDWARE PIN CONFIGURATION  —  edit to match your wiring║
// ╚══════════════════════════════════════════════════════════╝
#define PIN_JOY_X      34   // joystick X-axis (ADC1_CH6)
#define PIN_JOY_Y      35   // joystick Y-axis (ADC1_CH7)
#define PIN_JOY_BTN    32   // joystick push-button (active LOW)
#define I2C_SDA        21   // OLED data
#define I2C_SCL        22   // OLED clock
#define PIN_BUZZ       25   // passive 2-pin buzzer (signal pin; other pin → GND)

// ╔══════════════════════════════════════════════════════════╗
// ║  JOYSTICK TUNING  —  adjust if your stick drifts/misreads║
// ╚══════════════════════════════════════════════════════════╝
#define ADC_SAMPLES        8      // reads averaged per poll (reduces ESP32 ADC noise)
#define JOY_CENTER         2048   // 12-bit midpoint
#define JOY_THRESHOLD_UP   1000   // Y below this  = UP
#define JOY_THRESHOLD_DN   3096   // Y above this  = DOWN
#define JOY_HYSTERESIS     200    // dead-band before a direction resets
#define BTN_DEBOUNCE_MS    200    // ms lockout after any button press

// ╔══════════════════════════════════════════════════════════╗
// ║  GLOBAL TIMING  —  safe to leave as-is                  ║
// ╚══════════════════════════════════════════════════════════╝
#define BOOT_MS          2000   // splash screen duration (ms)
#define IDLE_MS          5000   // inactivity before sleep animation
#define SLEEP_STEP_MS     500   // delay between each sleep anim frame
#define TC_WINDOW_MS      800   // max ms between consecutive triple-clicks

// ╔══════════════════════════════════════════════════════════╗
// ║  CAR GAME SETTINGS  —  editable difficulty knobs        ║
// ╚══════════════════════════════════════════════════════════╝
// Obstacle speed, spawn density, and spawn chance are all
// controlled by score-tier functions at the bottom of the
// car-game section — search for carObsSpeed() to edit them.
#define CAR_FRAME_MS       50   // ms per game frame (~20 fps)
#define CAR_W              12   // player car width  (px)
#define CAR_H               8   // player car height (px)
#define CAR_Y              54   // player car Y position (fixed row)
// Road width at top/bottom — top must be wide enough that spawn X
// covers the full playable lane, not just the narrow vanishing point.
// At top: l=(128-84)/2=22, r=106  → obstacles can spawn x=22..98
// At bottom: l=5, r=123           → player can reach x=5..111
#define ROAD_TOP_W         84   // road width at top of screen  (was 20/48 — too narrow)
#define ROAD_BOT_W        118   // road width at bottom of screen
#define MAX_OBS            10   // max simultaneous obstacles on screen

// ╔══════════════════════════════════════════════════════════╗
// ║  SNAKE GAME SETTINGS                                    ║
// ╚══════════════════════════════════════════════════════════╝
#define CELL               6    // size of one grid cell (px)
#define GRID_W             (128 / CELL)          // 21 cells wide
#define GRID_H             ((64 - 10) / CELL)    //  9 cells tall
#define GRID_TOP           10   // px offset from top (reserved for score HUD)

// ╔══════════════════════════════════════════════════════════╗
// ║  PONG GAME SETTINGS  —  editable                        ║
// ╚══════════════════════════════════════════════════════════╝
#define PONG_FRAME_MS      20   // ms per game frame (~50 fps)
#define PONG_HUD_H         10   // pixels at top reserved for score bar
#define PONG_PADDLE_W      20   // paddle width (px)  — make wider = easier
#define PONG_PADDLE_H       3   // paddle thickness (px)
#define PONG_PADDLE_Y      57   // paddle Y position (fixed row near bottom)
#define PONG_BALL_SZ        3   // ball is a square: SZ × SZ pixels
#define PONG_BASE_SPD    1.8f   // ball speed at score=0 (px/frame)
#define PONG_SPEED_EVERY    7   // points per speed tier increase
#define PONG_SPEED_STEP  0.5f   // px/frame added per tier

// ╔══════════════════════════════════════════════════════════╗
// ║  ALL ENUMS & STRUCTS MUST BE BEFORE ANY FUNCTION        ║
// ║  The Arduino IDE hoists prototypes above source, so any ║
// ║  type used in a function signature must be defined here. ║
// ╚══════════════════════════════════════════════════════════╝

// Application finite-state machine
enum AppState {
  ST_BOOT,    // startup splash screen
  ST_MENU,    // main scrollable menu
  ST_RESULT,  // generic info/result screen (kept for future use)
  ST_SLEEP,   // idle sleep animation
  ST_CAR,     // car game
  ST_SNAKE,   // snake game
  ST_PONG     // pong game  ← NEW
};

// ── Car game: one obstacle slot ───────────────────────────
// Obstacle types:
//   0 = standard car  (8×8,  drives straight)
//   1 = truck         (14×7, very wide)
//   2 = speeder       (5×5,  +2 px/frame bonus speed)
//   3 = drifter       (8×8,  zigzags horizontally)
struct CarObs {
  int  x, y;          // top-left corner position
  int  w, h;          // bounding box
  int  extraVY;       // bonus downward speed (speeder only)
  int  dx;            // drift direction: -1 / 0 / +1
  int  dxTimer;       // frames until next horizontal drift step
  int  dxDelay;       // frames between drift steps
  int  type;          // 0-3 (see above)
  bool active;        // false = this slot is free to reuse
};

// ── Snake game: a grid point ───────────────────────────────
enum SDir { SD_NONE, SD_UP, SD_DOWN, SD_LEFT, SD_RIGHT };
struct Pt  { int x, y; Pt(int x=0,int y=0):x(x),y(y){} };

// ── Pong game: the ball ───────────────────────────────────
struct PongBall {
  float x, y;    // position (sub-pixel precision)
  float vx, vy;  // velocity (px per frame)
};

// ╔══════════════════════════════════════════════════════════╗
// ║  HARDWARE OBJECT                                        ║
// ╚══════════════════════════════════════════════════════════╝
// Change the constructor if you use a different OLED chip,
// e.g. U8G2_SH1106_128X64_NONAME_F_HW_I2C for SH1106.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

// ╔══════════════════════════════════════════════════════════╗
// ║  APPLICATION GLOBALS                                    ║
// ╚══════════════════════════════════════════════════════════╝

// ── Menu ──────────────────────────────────────────────────
// ===== EDITABLE: Menu item labels =====
const char* menuItems[] = { "Car Game", "Snake", "Pong", "Turn Off" };
const int   NUM_ITEMS    = 4;    // keep in sync with the array above
const int   MENU_VISIBLE = 3;    // items displayed at once (scroll window)
// ===== END EDITABLE =====

int         menuCursor   = 0;    // currently highlighted item index
int         menuScroll   = 0;    // index of the top-most visible item
String      resultMsg    = "";   // text shown on the generic result screen
AppState    appState     = ST_BOOT;
unsigned long stateMs    = 0;    // millis() when the current state started
unsigned long lastInputMs = 0;   // millis() of most recent joystick activity

// ── Button (single shared read per frame) ─────────────────
// Only updateJoystick() calls digitalRead. Everything else
// reads btnEdge so there is exactly one edge per press.
bool          btnEdge    = false;
unsigned long lastBtnMs  = 0;

// ── Axis state ────────────────────────────────────────────
bool axisUp   = false;    // true while Y-axis is pushed UP
bool axisDown = false;    // true while Y-axis is pushed DOWN

// ── Idle / sleep animation ────────────────────────────────
unsigned long sleepAnimMs = 0;
int           sleepStep   = 0;   // 0-3 → "Sleep...", "...Z", "...Zz", "...Zzz"

// ── Triple-click exit mechanism ───────────────────────────
// Three button presses each within TC_WINDOW_MS of the previous
// one trigger a return to the main menu from any game screen.
int           tcCount    = 0;
unsigned long tcLastMs   = 0;

// ── Persistent high scores (NVS via Preferences) ──────────
// Loaded once in setup(); updated and saved whenever a new
// best is set. Namespace "retrocon", keys "carScore" etc.
Preferences prefs;
int carHighScore   = 0;   // best car game score ever recorded
int snakeHighScore = 0;   // best snake score ever recorded
int pongHighScore  = 0;   // best pong score ever recorded


// ╔══════════════════════════════════════════════════════════╗
// ║  HIGH SCORE PERSISTENCE                                 ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * loadHighScores()
 * Reads all three best scores from NVS flash into the global vars.
 * Call once in setup(). getInt() returns the default (0) if a key
 * has never been written, so first-boot behaviour is correct.
 */
void loadHighScores() {
  prefs.begin("retrocon", true);           // true = read-only
  carHighScore   = prefs.getInt("carScore",   0);
  snakeHighScore = prefs.getInt("snakeScore", 0);
  pongHighScore  = prefs.getInt("pongScore",  0);
  prefs.end();
  Serial.printf("Loaded scores — Car:%d  Snake:%d  Pong:%d\n",
                carHighScore, snakeHighScore, pongHighScore);
}

/*
 * checkAndSave(current, &best, key)
 * Compares current score against the stored best. If current wins,
 * updates the best variable and writes it to NVS immediately.
 * Returns true when a new record has been set (caller can show
 * a "NEW BEST!" indicator on the game-over screen).
 */
bool checkAndSave(int current, int& best, const char* key) {
  if (current <= best) return false;       // no new record
  best = current;
  prefs.begin("retrocon", false);          // false = read-write
  prefs.putInt(key, best);
  prefs.end();
  Serial.printf("NEW BEST — %s = %d\n", key, best);
  return true;
}

// ╔══════════════════════════════════════════════════════════╗
// ║  JOYSTICK HELPERS                                       ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * readAxis(pin) — averages ADC_SAMPLES reads to smooth ESP32 ADC noise.
 * Returns the averaged 12-bit value (0-4095).
 */
int readAxis(int pin) {
  long s = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) s += analogRead(pin);
  return s / ADC_SAMPLES;
}

/*
 * updateJoystick() — call ONCE at the top of every loop() iteration.
 *   • Updates axisUp / axisDown with hysteresis (prevents chatter).
 *   • Performs a single debounced digitalRead of the button.
 *   • Sets the global btnEdge flag TRUE for exactly one frame
 *     per validated press; all other code reads btnEdge only.
 * Returns btnEdge for convenience.
 */
bool updateJoystick() {
  int y = readAxis(PIN_JOY_Y);

  // Y-axis: active HIGH when pushed past threshold, resets at hysteresis band
  if (!axisUp   && y < JOY_THRESHOLD_UP) axisUp   = true;
  else if (axisUp   && y > JOY_CENTER - JOY_HYSTERESIS) axisUp   = false;

  if (!axisDown && y > JOY_THRESHOLD_DN) axisDown = true;
  else if (axisDown && y < JOY_CENTER + JOY_HYSTERESIS) axisDown = false;

  // Button: active LOW, debounced
  bool raw = (digitalRead(PIN_JOY_BTN) == LOW);
  btnEdge  = raw && (millis() - lastBtnMs > BTN_DEBOUNCE_MS);
  if (btnEdge) lastBtnMs = millis();
  return btnEdge;
}

/*
 * navEdge() — rising-edge detector for Y-axis menu navigation.
 * Returns -1 on a new UP press, +1 on a new DOWN press, 0 otherwise.
 * Uses static locals so it tracks its own previous state.
 */
int navEdge() {
  static bool pU = false, pD = false;
  int r = 0;
  if (axisUp   && !pU) r = -1;   // newly tilted up
  if (axisDown && !pD) r =  1;   // newly tilted down
  pU = axisUp; pD = axisDown;
  return r;
}

/*
 * horizInput() — continuous horizontal reading for car & pong paddle.
 * Returns -1 (left), 0 (center), or +1 (right) with hysteresis.
 */
int horizInput() {
  static bool lA = false, rA = false;
  int x = readAxis(PIN_JOY_X);
  if (!lA && x < JOY_CENTER - 600) lA = true;
  else if (lA && x > JOY_CENTER - 200) lA = false;
  if (!rA && x > JOY_CENTER + 600) rA = true;
  else if (rA && x < JOY_CENTER + 200) rA = false;
  return lA ? -1 : (rA ? 1 : 0);
}

/*
 * tripleClick() — reads the shared btnEdge flag (set this frame).
 * Each click must arrive within TC_WINDOW_MS of the previous one;
 * a longer gap resets the counter to 1.
 * Returns TRUE exactly once when the 3rd click is confirmed, then
 * resets the counter so the sequence can begin again.
 * Only call this in states where triple-click exit should be active.
 */
bool tripleClick() {
  if (!btnEdge) return false;
  unsigned long now = millis();
  if (now - tcLastMs > TC_WINDOW_MS) tcCount = 1;  // gap too long → restart
  else tcCount++;
  tcLastMs = now;
  if (tcCount >= 3) { tcCount = 0; return true; }
  return false;
}


// ╔══════════════════════════════════════════════════════════╗
// ║  SHARED UI HELPERS                                      ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * centered(text, font, y) — draws text horizontally centred at pixel-row y.
 * Sets the font internally; caller doesn't need to set it first.
 */
void centered(const char* t, const uint8_t* f, int y) {
  u8g2.setFont(f);
  u8g2.setCursor((128 - u8g2.getStrWidth(t)) / 2, y);
  u8g2.print(t);
}


// ╔══════════════════════════════════════════════════════════╗
// ║  SOUND EFFECTS  (passive 2-pin buzzer on PIN_BUZZ)      ║
// ╠══════════════════════════════════════════════════════════╣
// ║  All frequencies stay in 500–1300 Hz — the sweet spot   ║
// ║  where a small passive buzzer sounds cleanest.           ║
// ║  Below ~400 Hz it growls; above ~3 kHz it screams.      ║
// ║                                                          ║
// ║  tone(pin, hz, ms) on ESP32 is NON-BLOCKING — the LEDC  ║
// ║  peripheral drives the pin in hardware and stops after   ║
// ║  the given duration. Game physics continue immediately.  ║
// ║  Only the boot jingle and game-over use delay() because  ║
// ║  they play at moments the game is paused anyway.         ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * soundBoot()
 * Four-note ascending chime: C5 → E5 → G5 → C6.
 * Blocking (~600 ms). Called once while the splash screen
 * is displayed so the user hears it without any visual delay.
 */
void soundBoot() {
  tone(PIN_BUZZ, 523,  90); delay(110);   // C5
  tone(PIN_BUZZ, 659,  90); delay(110);   // E5
  tone(PIN_BUZZ, 784,  90); delay(110);   // G5
  tone(PIN_BUZZ, 1047, 160);              // C6 — let it ring out
}

/*
 * soundMenuNav()
 * Short neutral tick on each cursor move. Non-blocking.
 */
void soundMenuNav() {
  tone(PIN_BUZZ, 750, 22);
}

/*
 * soundMenuSelect()
 * Two-note confirmation on menu item press.
 * Slightly blocking (90 ms total) — fine at a menu event.
 */
void soundMenuSelect() {
  tone(PIN_BUZZ, 700, 40); delay(50);
  tone(PIN_BUZZ, 1050, 65);
}

/*
 * soundEat()
 * Quick rising chirp when snake eats food. Non-blocking.
 */
void soundEat() {
  tone(PIN_BUZZ, 1100, 45);
}

/*
 * soundPaddleHit()
 * Short crisp click on pong paddle contact. Non-blocking.
 */
void soundPaddleHit() {
  tone(PIN_BUZZ, 900, 18);
}

/*
 * soundWallHit()
 * Softer click on pong wall/ceiling bounce. Non-blocking.
 * Lower than paddle hit so the two events feel distinct.
 */
void soundWallHit() {
  tone(PIN_BUZZ, 620, 12);
}

/*
 * soundCrash()
 * Descending two-step crunch for car collision.
 * Slightly blocking (~160 ms). Only called at moment of death
 * so it never interrupts active gameplay.
 */
void soundCrash() {
  tone(PIN_BUZZ, 680, 55); delay(65);
  tone(PIN_BUZZ, 510, 90);
}

/*
 * soundGameOver()
 * Descending three-note sad phrase played when any game ends.
 * Blocking (~420 ms). Called once; the game-over screen then
 * stays frozen until the player triple-clicks to exit.
 */
void soundGameOver() {
  tone(PIN_BUZZ, 660, 110); delay(130);
  tone(PIN_BUZZ, 550, 110); delay(130);
  tone(PIN_BUZZ, 460, 180);
}

// ╔══════════════════════════════════════════════════════════╗
// ║  BOOT SPLASH SCREEN                                     ║
// ╚══════════════════════════════════════════════════════════╝

void drawBoot() {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_helvB12_tf);
    u8g2.setCursor((128 - u8g2.getStrWidth("ESP32"))   / 2, 20);
    u8g2.print("ESP32");
    u8g2.setCursor((128 - u8g2.getStrWidth("Gaming"))  / 2, 40);
    u8g2.print("Gaming");
    u8g2.setCursor((128 - u8g2.getStrWidth("Console")) / 2, 60);
    u8g2.print("Console");
  } while (u8g2.nextPage());
}


// ╔══════════════════════════════════════════════════════════╗
// ║  MAIN MENU                                              ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * drawMenu()
 * Shows MENU_VISIBLE items at a time from the menuItems[] array.
 * menuScroll is auto-adjusted so the highlighted item is always visible.
 * A proportional scroll-bar on the right edge indicates list position.
 */
void drawMenu() {
  // Keep the cursor inside the scroll window
  if (menuCursor < menuScroll) menuScroll = menuCursor;
  if (menuCursor >= menuScroll + MENU_VISIBLE)
    menuScroll = menuCursor - MENU_VISIBLE + 1;

  u8g2.firstPage();
  do {
    // ── Title + underline ────────────────────────────────
    u8g2.setFont(u8g2_font_helvB10_tf);
    u8g2.drawStr(2, 11, "What you want?");
    u8g2.drawHLine(2, 13, 118);

    // ── List items ───────────────────────────────────────
    for (int i = 0; i < MENU_VISIBLE; i++) {
      int idx = menuScroll + i;
      if (idx >= NUM_ITEMS) break;
      int y = 26 + i * 14;

      // Best score for this item (-1 = item has no score, e.g. Turn Off)
      int best = -1;
      if      (idx == 0) best = carHighScore;
      else if (idx == 1) best = snakeHighScore;
      else if (idx == 2) best = pongHighScore;

      // Build the "HI:X" string once so we can measure its width
      char hiStr[12] = "";
      if (best >= 0) sprintf(hiStr, "HI:%d", best);

      if (idx == menuCursor) {
        // ── Selected: filled highlight box, inverted colours ──
        u8g2.setDrawColor(1);
        u8g2.drawBox(14, y - 10, 108, 12);
        u8g2.setDrawColor(0);
        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.drawStr(18, y, menuItems[idx]);
        if (best >= 0) {
          u8g2.setFont(u8g2_font_5x7_tf);
          u8g2.drawStr(120 - u8g2.getStrWidth(hiStr), y - 1, hiStr);
        }
        u8g2.setDrawColor(1);
      } else {
        // ── Normal: white text on black background ────────
        u8g2.setFont(u8g2_font_helvB10_tf);
        u8g2.drawStr(18, y, menuItems[idx]);
        if (best >= 0) {
          u8g2.setFont(u8g2_font_5x7_tf);
          // Dim right-aligned score; y-1 aligns 5x7 baseline with helvB10
          u8g2.drawStr(120 - u8g2.getStrWidth(hiStr), y - 1, hiStr);
        }
      }
    }

    // ── Scroll bar ───────────────────────────────────────
    const int SBX  = 125, SBY0 = 15, SBH = 48;
    u8g2.setDrawColor(1);
    u8g2.drawVLine(SBX, SBY0, SBH);
    int thumbH = max(4, SBH * MENU_VISIBLE / NUM_ITEMS);
    int thumbY = SBY0 + (SBH - thumbH)
                 * menuScroll / max(1, NUM_ITEMS - MENU_VISIBLE);
    u8g2.drawBox(SBX - 1, thumbY, 3, thumbH);

  } while (u8g2.nextPage());
}


// ╔══════════════════════════════════════════════════════════╗
// ║  IDLE / SLEEP ANIMATION                                 ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * drawSleep()
 * Cycles through 4 text frames: "Sleep..." → "...Z" → "...Zz" → "...Zzz"
 * The step is advanced by the main loop timer; this function just draws
 * whichever step is current.
 */
void drawSleep() {
  const char* frames[] = { "Sleep...", "Sleep...Z", "Sleep...Zz", "Sleep...Zzz" };
  u8g2.firstPage();
  do { centered(frames[sleepStep], u8g2_font_helvB10_tf, 35); }
  while (u8g2.nextPage());
}


// ╔══════════════════════════════════════════════════════════╗
// ║  GENERIC RESULT / INFO SCREEN                           ║
// ╚══════════════════════════════════════════════════════════╝

/*
 * drawResult()
 * Draws a double-bordered frame with the text stored in resultMsg.
 * Supports a single '\n' in resultMsg to split over two lines.
 * "3x click to exit" hint shown at the bottom.
 */
void drawResult() {
  u8g2.firstPage();
  do {
    u8g2.drawFrame(1, 1, 126, 62);
    u8g2.drawFrame(4, 4, 120, 56);
    int nl = resultMsg.indexOf('\n');
    if (nl != -1) {
      String l1 = resultMsg.substring(0, nl);
      String l2 = resultMsg.substring(nl + 1);
      u8g2.setFont(u8g2_font_helvB10_tf);
      u8g2.setCursor((128 - u8g2.getStrWidth(l1.c_str())) / 2, 26);
      u8g2.print(l1);
      u8g2.setCursor((128 - u8g2.getStrWidth(l2.c_str())) / 2, 40);
      u8g2.print(l2);
    } else {
      centered(resultMsg.c_str(), u8g2_font_helvB10_tf, 33);
    }
    centered("3x click to exit", u8g2_font_5x7_tf, 57);
  } while (u8g2.nextPage());
}


// ╔══════════════════════════════════════════════════════════╗
// ║  CAR GAME                                               ║
// ╠══════════════════════════════════════════════════════════╣
// ║  Road: 84 px wide at top → 118 px at bottom.           ║
// ║  Obstacle spawn X covers the full top width (x=25..101)║
// ║  so obstacles appear on both sides of the road, not    ║
// ║  just the centre. Player can no longer hug one edge.   ║
// ║                                                         ║
// ║  Spawn rate increases every 6 points (formula-based):  ║
// ║   0  pts → 30 fr / 28%   cars only                     ║
// ║   6  pts → 27 fr / 34%                                 ║
// ║  12  pts → 24 fr / 40%   + trucks                      ║
// ║  18  pts → 21 fr / 46%                                 ║
// ║  24  pts → 18 fr / 52%   + speeders                    ║
// ║  30  pts → 15 fr / 58%                                 ║
// ║  36  pts → 12 fr / 64%   + drifters                    ║
// ║  42  pts →  9 fr / 70%                                 ║
// ║  48+ pts →  8 fr / 80%   (hard floor / cap)            ║
// ╚══════════════════════════════════════════════════════════╝

// ── Car game state ────────────────────────────────────────
CarObs        obs[MAX_OBS];       // obstacle pool
int           carX;               // player car left-edge X
int           carScore;           // obstacles successfully avoided
int           carFrame;           // frame counter for spawn timing
bool          carRunning;         // false = game over, show death screen
unsigned long carFrameTs;         // millis() of last frame update
int           laneScroll = 0;     // phase for animated dashed centre line

// ── Difficulty tier functions ─────────────────────────────
// ===== EDITABLE: Car difficulty tiers =====

// Ball speed in pixels per frame at each score bracket
int carObsSpeed() {
  if (carScore <   8) return 2;
  if (carScore <  15) return 3;
  if (carScore <  25) return 4;
  if (carScore <  40) return 5;
  if (carScore <  55) return 6;
  if (carScore <  75) return 7;
  if (carScore < 100) return 8;
  return 9;
}

/*
 * carSpawnEvery()
 * Returns the frame interval between spawn-attempt checks.
 * Decreases by 3 frames every 6 score points (one tier = 6 pts).
 * Hard floor at 8 frames so the screen never gets unplayable.
 * ===== EDITABLE: change 30 (start), 3 (step), 8 (floor) =====
 */
int carSpawnEvery() {
  int tier = carScore / 6;             // one tier per 6 points
  return max(30 - tier * 3, 8);        // 30 → 27 → 24 → … → 8
}

/*
 * carSpawnChance()
 * Returns 0-100 probability that a spawn-attempt actually creates
 * an obstacle. Increases by 6% every 6 score points.
 * Capped at 80% so there is always a small chance of breathing room.
 * ===== EDITABLE: change 28 (start), 6 (step), 80 (cap) =====
 */
int carSpawnChance() {
  int tier = carScore / 6;
  return min(28 + tier * 6, 80);       // 28% → 34% → 40% → … → 80%
}

// Highest obstacle type index available (unlocked by score)
int carMaxType() {
  if (carScore <  8) return 0;   // cars only
  if (carScore < 15) return 1;   // + trucks
  if (carScore < 25) return 2;   // + speeders
  return 3;                      // + drifters
}
// ===== END EDITABLE =====

// ── Road geometry ─────────────────────────────────────────

/*
 * roadBounds(y, l, r)
 * Calculates the left (l) and right (r) pixel X of the road at row y.
 * Road widens linearly from ROAD_TOP_W at y=0 to ROAD_BOT_W at y=63,
 * creating a simple forward-perspective illusion.
 */
void roadBounds(int y, int& l, int& r) {
  float t = (float)y / 63.0f;
  int w = ROAD_TOP_W + (int)(t * (ROAD_BOT_W - ROAD_TOP_W));
  l = (128 - w) / 2;
  r = l + w;
}

/*
 * drawRoad()
 * Draws the two kerb edges (double-pixel for thickness) and an
 * animated dashed centre-lane divider whose phase advances with
 * laneScroll so it appears to scroll toward the player.
 */
void drawRoad() {
  for (int y = 0; y < 64; y++) {
    int l, r;
    roadBounds(y, l, r);
    // Thick kerbs (two pixels wide on each side)
    u8g2.drawPixel(l,     y);
    u8g2.drawPixel(l - 1, y);
    u8g2.drawPixel(r,     y);
    u8g2.drawPixel(r + 1, y);
    // Dashed centre line: 7px on / 7px off, scrolling downward
    if (((y + laneScroll) % 14) < 7) u8g2.drawPixel(64, y);
  }
}

// ── Player car sprite (12×8) ──────────────────────────────
void drawPlayerCar() {
  u8g2.setDrawColor(1);
  u8g2.drawBox(carX,     CAR_Y + 2, CAR_W,     CAR_H - 2); // lower body
  u8g2.drawBox(carX + 2, CAR_Y,     CAR_W - 4, 3);         // roof bump
  u8g2.setDrawColor(0);
  u8g2.drawBox(carX + 3, CAR_Y,     CAR_W - 6, 2);         // windshield cutout
  u8g2.drawBox(carX + 2, CAR_Y + 3, CAR_W - 4, 1);         // rear-window line
  u8g2.drawPixel(carX,             CAR_Y + CAR_H - 1);      // left wheel arch
  u8g2.drawPixel(carX + CAR_W - 1, CAR_Y + CAR_H - 1);     // right wheel arch
  u8g2.setDrawColor(1);
}

// ── Obstacle sprites ──────────────────────────────────────

// Type 0: standard enemy car (8×8)
void drawObsCar(int x, int y) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(x + 1, y,     6, 8);   // narrow body
  u8g2.drawBox(x,     y + 2, 8, 4);   // wide mid-section
  u8g2.setDrawColor(0);
  u8g2.drawBox(x + 2, y + 1, 4, 2);   // windshield
  u8g2.drawBox(x + 2, y + 5, 4, 1);   // rear-window line
  u8g2.setDrawColor(1);
}

// Type 1: truck / bus (14×7) — very wide, hard to avoid
void drawObsTruck(int x, int y) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(x,     y + 1, 14, 5);  // body
  u8g2.drawBox(x + 1, y,     12, 1);  // roof
  u8g2.drawBox(x + 1, y + 6, 12, 1);  // underside
  u8g2.setDrawColor(0);
  u8g2.drawBox(x + 2, y + 2, 4, 2);   // left window
  u8g2.drawBox(x + 8, y + 2, 4, 2);   // right window
  u8g2.setDrawColor(1);
}

// Type 2: speeder (5×5) — small but moves extra fast
void drawObsSpeeder(int x, int y) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(x + 1, y,     3, 5);   // body
  u8g2.drawBox(x,     y + 1, 5, 3);   // wings
  u8g2.setDrawColor(0);
  u8g2.drawPixel(x + 2, y + 2);       // cockpit dot
  u8g2.setDrawColor(1);
}

// Type 3: drifter — same car shape but with a side-arrow showing drift direction
void drawObsDrifter(int x, int y, int dx) {
  drawObsCar(x, y);
  u8g2.setDrawColor(1);
  if (dx > 0) {                        // arrow pointing right
    u8g2.drawPixel(x + 9,  y + 3);
    u8g2.drawPixel(x + 10, y + 4);
    u8g2.drawPixel(x + 9,  y + 5);
  } else if (dx < 0) {                 // arrow pointing left
    u8g2.drawPixel(x - 2, y + 3);
    u8g2.drawPixel(x - 3, y + 4);
    u8g2.drawPixel(x - 2, y + 5);
  }
}

// Dispatcher: calls the right draw function for obs[i].type
void drawObstacle(int i) {
  int x = obs[i].x, y = obs[i].y;
  switch (obs[i].type) {
    case 1:  drawObsTruck(x, y);               break;
    case 2:  drawObsSpeeder(x, y);             break;
    case 3:  drawObsDrifter(x, y, obs[i].dx);  break;
    default: drawObsCar(x, y);                 break;
  }
}

/*
 * spawnObs()
 * Picks a random type (up to carMaxType()) and places it at a random
 * X position that spans the FULL road width at y=0.
 * With ROAD_TOP_W=84 the spawn window is ~76 px wide, so obstacles
 * appear all the way from the left kerb to the right kerb — the player
 * can't simply hug one side to avoid everything.
 *
 * A 3-px inset on each kerb prevents obstacles from spawning on top
 * of the kerb pixels themselves.
 *
 * A minimum horizontal gap check (3 px) prevents two obstacles from
 * spawning so close that they form an impassable wall.
 */
void spawnObs() {
  int type = random(carMaxType() + 1);
  int w, h;
  switch (type) {
    case 1: w = 14; h = 7; break;
    case 2: w =  5; h = 5; break;
    default: w = 8; h = 8; break;
  }

  int l, r;
  roadBounds(0, l, r);

  // 3-px inset from each kerb so obstacles don't sit on the edge line
  l += 3;
  r -= 3;

  if (r - l - w < 1) return;   // road too narrow for this obstacle (shouldn't happen)

  // Try up to 8 random X positions; skip any that would overlap an
  // obstacle still near the top of the screen (y < 24).
  int rx = -1;
  for (int attempt = 0; attempt < 8; attempt++) {
    int cx = l + random(r - l - w + 1);   // full-width random X
    bool blocked = false;
    for (int i = 0; i < MAX_OBS; i++) {
      if (!obs[i].active) continue;
      if (obs[i].y > 24) continue;         // only check near-top obstacles
      // Compute horizontal gap between the two bounding boxes
      int gap = cx - (obs[i].x + obs[i].w);
      if (gap < 0) gap = obs[i].x - (cx + w);
      if (gap < 3) { blocked = true; break; }   // 3-px minimum gap
    }
    if (!blocked) { rx = cx; break; }
  }
  if (rx < 0) return;   // no clear spot this frame — try again next spawn interval

  for (int i = 0; i < MAX_OBS; i++) {
    if (obs[i].active) continue;
    obs[i] = { rx, -h, w, h,
               (type == 2) ? 2 : 0,
               (type == 3) ? (random(2) ? 1 : -1) : 0,
               5, 5, type, true };
    break;
  }
}

/*
 * tickObstacles()
 * Advances all active obstacles downward, applies drifter lateral motion,
 * awards a score point when one exits the bottom, and tests AABB collision
 * with the player car.
 * Returns TRUE if a collision was detected (game over).
 */
bool tickObstacles() {
  int  spd = carObsSpeed();
  bool hit  = false;

  for (int i = 0; i < MAX_OBS; i++) {
    if (!obs[i].active) continue;

    obs[i].y += spd + obs[i].extraVY;   // move downward

    // Horizontal drift for drifter-type obstacles
    if (obs[i].dx != 0) {
      obs[i].dxTimer--;
      if (obs[i].dxTimer <= 0) {
        obs[i].dxTimer = obs[i].dxDelay;
        int nx = obs[i].x + obs[i].dx * 2;
        int l, r;
        roadBounds(obs[i].y + obs[i].h / 2, l, r);
        if (nx < l || nx + obs[i].w > r) obs[i].dx = -obs[i].dx; // bounce off kerb
        else obs[i].x = nx;
      }
    }

    // Past bottom edge → score and recycle slot
    if (obs[i].y > 64) { obs[i].active = false; carScore++; continue; }

    // AABB collision test with player car
    if (!hit &&
        carX + CAR_W  > obs[i].x && carX           < obs[i].x + obs[i].w &&
        CAR_Y + CAR_H > obs[i].y && CAR_Y           < obs[i].y + obs[i].h)
      hit = true;
  }
  return hit;
}

// ── Car game draw calls ───────────────────────────────────

void drawCarGame() {
  u8g2.firstPage();
  do {
    drawRoad();
    for (int i = 0; i < MAX_OBS; i++)
      if (obs[i].active) drawObstacle(i);
    drawPlayerCar();
    // HUD — score + speed tier in top-left
    u8g2.setFont(u8g2_font_5x7_tf);
    char hud[22];
    sprintf(hud, "SC:%d SP:%d", carScore, carObsSpeed());
    u8g2.drawStr(2, 8, hud);
  } while (u8g2.nextPage());
}

void drawCarGameOver() {
  u8g2.firstPage();
  do {
    u8g2.drawFrame(0, 0, 128, 64);
    u8g2.drawFrame(3, 3, 122, 58);
    centered("GAME OVER", u8g2_font_helvB12_tf, 20);
    u8g2.drawHLine(10, 23, 108);           // divider under title
    char sc[20]; sprintf(sc, "Score:  %d", carScore);
    centered(sc, u8g2_font_helvB10_tf, 36);
    char bs[20]; sprintf(bs, "Best:   %d", carHighScore);
    centered(bs, u8g2_font_helvB10_tf, 49);
    centered("3x click to exit", u8g2_font_5x7_tf, 60);
  } while (u8g2.nextPage());
}

// ── Car game logic ────────────────────────────────────────

void resetCarGame() {
  for (int i = 0; i < MAX_OBS; i++) obs[i].active = false;
  int l, r; roadBounds(CAR_Y, l, r);
  carX = l + (r - l - CAR_W) / 2;     // start centred in road
  carScore = 0; carFrame = 0; laneScroll = 0;
  carRunning = true; carFrameTs = millis();
}

/*
 * tickCarGame()
 * Called every loop. Skips work unless CAR_FRAME_MS has elapsed.
 * Order: scroll lane → move player → maybe spawn → move obstacles → draw.
 */
void tickCarGame() {
  unsigned long now = millis();
  if (now - carFrameTs < CAR_FRAME_MS) return;
  carFrameTs = now;

  laneScroll = (laneScroll + carObsSpeed()) % 14;  // scroll centre line

  int mv = horizInput();
  if (mv) {
    int nx = carX + mv * 5;
    int l, r; roadBounds(CAR_Y, l, r);
    carX = constrain(nx, l, r - CAR_W);
  }

  if (++carFrame >= carSpawnEvery()) {
    carFrame = 0;
    if (random(100) < carSpawnChance()) spawnObs();
  }

  if (tickObstacles()) {
    // Collision detected — save high score, then play crash + game-over jingle
    checkAndSave(carScore, carHighScore, "carScore");
    soundCrash();       // short descending crunch
    soundGameOver();    // sad three-note phrase
    carRunning = false; // switches to game-over screen
  }
  drawCarGame();
}


// ╔══════════════════════════════════════════════════════════╗
// ║  SNAKE GAME                                             ║
// ║  Rules: eat diamonds to grow; walls wrap; self = death  ║
// ╚══════════════════════════════════════════════════════════╝

// ── Snake state ───────────────────────────────────────────
std::vector<Pt> snake;      // head = [0], tail = [size-1]
Pt              snakeFood;
SDir            snakeDir;
int             snakeScore;
bool            snakeDead, snakeWin;
unsigned long   snakeMoveTs;

/*
 * getSnakeInput()
 * Returns the direction implied by the joystick this frame.
 * X-axis controls left/right; Y-axis controls up/down.
 */
SDir getSnakeInput() {
  int x = readAxis(PIN_JOY_X), y = readAxis(PIN_JOY_Y);
  if (y > JOY_THRESHOLD_DN) return SD_UP;
  if (y < JOY_THRESHOLD_UP) return SD_DOWN;
  if (x < JOY_THRESHOLD_UP) return SD_LEFT;
  if (x > JOY_THRESHOLD_DN) return SD_RIGHT;
  return SD_NONE;
}

// Helper: returns the direction directly opposite to d
SDir oppositeDir(SDir d) {
  if (d == SD_UP)    return SD_DOWN;
  if (d == SD_DOWN)  return SD_UP;
  if (d == SD_LEFT)  return SD_RIGHT;
  if (d == SD_RIGHT) return SD_LEFT;
  return SD_NONE;
}

/*
 * placeFood()
 * Picks a random grid cell not occupied by the snake.
 * Tries up to 400 times before giving up (avoids infinite loop on huge snakes).
 */
void placeFood() {
  bool ok = false; int att = 0;
  while (!ok && att++ < 400) {
    snakeFood = Pt(random(GRID_W), random(GRID_H));
    ok = true;
    for (auto& s : snake)
      if (s.x == snakeFood.x && s.y == snakeFood.y) { ok = false; break; }
  }
}

/*
 * snakeInterval()
 * Returns the ms between each move tick. Decreases as score grows.
 * Starts at 280ms, reduces 20ms per 4 points, minimum 100ms.
 * ===== EDITABLE: change 280 (start speed) and 20 (step) =====
 */
unsigned long snakeInterval() {
  int ms = 280 - (snakeScore / 4) * 20;
  return (unsigned long)max(ms, 100);
}

void resetSnake() {
  snake.clear();
  int mx = GRID_W / 2, my = GRID_H / 2;
  // Start with 3-segment horizontal snake pointing right
  snake.push_back(Pt(mx,     my));
  snake.push_back(Pt(mx - 1, my));
  snake.push_back(Pt(mx - 2, my));
  snakeDir   = SD_RIGHT;
  snakeScore = 0;
  snakeDead  = false;
  snakeWin   = false;
  placeFood();
  snakeMoveTs = millis();
}

// ── Snake draw helpers ────────────────────────────────────

// Diamond food shape: 5×5 rotated square centred in a CELL
void drawDiamond(int px, int py) {
  int cx = px + 2, cy = py + 2;
  u8g2.drawPixel(cx,     cy - 2);
  u8g2.drawHLine(cx - 1, cy - 1, 3);
  u8g2.drawHLine(cx - 2, cy,     5);
  u8g2.drawHLine(cx - 1, cy + 1, 3);
  u8g2.drawPixel(cx,     cy + 2);
}

// Snake head: solid box with two white eye-pixels facing direction of travel
void drawHead(int px, int py, SDir dir) {
  u8g2.setDrawColor(1);
  u8g2.drawBox(px, py, CELL - 1, CELL - 1);
  u8g2.setDrawColor(0);   // eyes are white (colour 0 on a white face)
  switch (dir) {
    case SD_RIGHT: u8g2.drawPixel(px+3,py+1); u8g2.drawPixel(px+3,py+3); break;
    case SD_LEFT:  u8g2.drawPixel(px+1,py+1); u8g2.drawPixel(px+1,py+3); break;
    case SD_UP:    u8g2.drawPixel(px+1,py+1); u8g2.drawPixel(px+3,py+1); break;
    case SD_DOWN:  u8g2.drawPixel(px+1,py+3); u8g2.drawPixel(px+3,py+3); break;
    default: break;
  }
  u8g2.setDrawColor(1);
}

// Body segment: alternates solid fill / outline+dot → chain-link texture
void drawSeg(int px, int py, bool solid) {
  u8g2.setDrawColor(1);
  if (solid) {
    u8g2.drawBox(px, py, CELL - 1, CELL - 1);
  } else {
    u8g2.drawFrame(px, py, CELL - 1, CELL - 1);
    u8g2.drawPixel(px + 2, py + 2);  // centre dot
  }
}

void drawSnakeGame() {
  u8g2.firstPage();
  do {
    // Score bar
    u8g2.setFont(u8g2_font_5x7_tf);
    char sb[28]; sprintf(sb, "SC:%d  LEN:%d", snakeScore, (int)snake.size());
    u8g2.drawStr(2, 7, sb);

    // Double-frame border around the play area
    u8g2.drawFrame(0, GRID_TOP - 1, 128, 65 - GRID_TOP);
    u8g2.drawFrame(1, GRID_TOP,     126, 63 - GRID_TOP);

    // Food
    drawDiamond(snakeFood.x * CELL, GRID_TOP + snakeFood.y * CELL);

    // Body: draw tail→neck first so head renders on top
    for (int i = (int)snake.size() - 1; i >= 1; i--)
      drawSeg(snake[i].x * CELL, GRID_TOP + snake[i].y * CELL, (i % 2) == 0);

    // Head
    if (!snake.empty())
      drawHead(snake[0].x * CELL, GRID_TOP + snake[0].y * CELL, snakeDir);

    // Game-over overlay: full inverted white panel
    if (snakeDead) {
      u8g2.setDrawColor(1);
      u8g2.drawBox(8, 15, 112, 36);
      u8g2.setDrawColor(0);
      const char* msg = snakeWin ? "YOU WIN!" : "GAME OVER";
      u8g2.setFont(u8g2_font_helvB12_tf);
      u8g2.drawStr((128 - u8g2.getStrWidth(msg)) / 2, 27, msg);
      char sc[20]; sprintf(sc, "Score:%d  Best:%d", snakeScore, snakeHighScore);
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr((128 - u8g2.getStrWidth(sc)) / 2, 38, sc);
      u8g2.drawStr((128 - u8g2.getStrWidth("3x click to exit")) / 2, 48,
                   "3x click to exit");
      u8g2.setDrawColor(1);
    }
  } while (u8g2.nextPage());
}

/*
 * tickSnake()
 * Advances the snake one step when snakeInterval() ms have elapsed.
 * Movement order: read input → compute new head → wrap walls →
 * check food → grow or advance → check self-collision.
 * Wall wrap: off one edge teleports to the opposite edge (no death).
 * Only self-collision (head enters a body segment) kills the snake.
 */
void tickSnake() {
  if (!snakeDead) {
    unsigned long now = millis();
    if (now - snakeMoveTs >= snakeInterval()) {
      snakeMoveTs = now;

      // Accept direction input (never allow 180° reversal)
      SDir nd = getSnakeInput();
      if (nd != SD_NONE && nd != oppositeDir(snakeDir)) snakeDir = nd;

      // Compute candidate new head
      Pt nh = snake.front();
      switch (snakeDir) {
        case SD_UP:    nh.y--; break;
        case SD_DOWN:  nh.y++; break;
        case SD_LEFT:  nh.x--; break;
        case SD_RIGHT: nh.x++; break;
        default: break;
      }

      // ── Wall wrap: teleport, never die ──────────────────
      if (nh.x < 0)        nh.x = GRID_W - 1;
      if (nh.x >= GRID_W)  nh.x = 0;
      if (nh.y < 0)        nh.y = GRID_H - 1;
      if (nh.y >= GRID_H)  nh.y = 0;

      // Grow if food eaten, otherwise move tail
      bool ate = (nh.x == snakeFood.x && nh.y == snakeFood.y);
      snake.insert(snake.begin(), nh);
      if (!ate) snake.pop_back();
      else {
        snakeScore++;
        soundEat();       // quick chirp on food pickup
        placeFood();
        // Win condition: snake fills the entire grid
        if ((int)snake.size() == GRID_W * GRID_H) {
          checkAndSave(snakeScore, snakeHighScore, "snakeScore");
          soundGameOver();  // play jingle on win too
          snakeDead = true; snakeWin = true;
        }
      }

      // ── Self-collision (only lethal condition) ──────────
      for (int i = 1; i < (int)snake.size(); i++) {
        if (snake[i].x == nh.x && snake[i].y == nh.y) {
          checkAndSave(snakeScore, snakeHighScore, "snakeScore");
          soundGameOver();  // sad three-note phrase
          snakeDead = true; snakeWin = false; break;
        }
      }
    }
  }
  drawSnakeGame();
}


// ╔══════════════════════════════════════════════════════════╗
// ║  PONG GAME                                              ║
// ║  Ball falls from top; player deflects it with a paddle  ║
// ║  at the bottom. Ball bounces off top and side walls.    ║
// ║  Hitting angle depends on where on the paddle it lands. ║
// ║  Ball speed increases every PONG_SPEED_EVERY points.    ║
// ║  Miss = game over; triple-click exits to menu.          ║
// ╚══════════════════════════════════════════════════════════╝

// ── Pong state ────────────────────────────────────────────
PongBall      pBall;
int           pPaddleX;       // paddle left-edge X
int           pScore;
bool          pDead;          // true = game over, show death screen
unsigned long pFrameTs;       // millis() of last frame update

/*
 * pongBallSpeed()
 * Returns the magnitude (px/frame) the ball should travel at the
 * current score. Increases by PONG_SPEED_STEP every PONG_SPEED_EVERY points.
 * ===== EDITABLE: adjust PONG_BASE_SPD / PONG_SPEED_STEP / PONG_SPEED_EVERY =====
 */
float pongBallSpeed() {
  int tiers = pScore / PONG_SPEED_EVERY;
  return PONG_BASE_SPD + tiers * PONG_SPEED_STEP;
}

/*
 * resetPong()
 * Clears state, centres the paddle, and launches the ball from near
 * the top of the play area with a random horizontal direction.
 */
void resetPong() {
  pPaddleX = (128 - PONG_PADDLE_W) / 2;   // centre paddle
  pScore   = 0;
  pDead    = false;

  // Ball: random X, just below top wall, moving downward
  pBall.x  = random(PONG_BALL_SZ, 128 - PONG_BALL_SZ * 2);
  pBall.y  = (float)(PONG_HUD_H + 2);
  pBall.vx = (random(2) == 0 ? -PONG_BASE_SPD : PONG_BASE_SPD);
  pBall.vy = PONG_BASE_SPD;

  pFrameTs = millis();
}

/*
 * rescaleBallSpeed()
 * After a paddle hit (which adjusts vx/vy for angle) this function
 * preserves the direction but normalises the speed to the current tier.
 * This prevents the ball from accidentally slowing after a shallow hit.
 */
void rescaleBallSpeed() {
  float mag = sqrt(pBall.vx * pBall.vx + pBall.vy * pBall.vy);
  if (mag < 0.01f) return;
  float target = pongBallSpeed();
  pBall.vx = (pBall.vx / mag) * target;
  pBall.vy = (pBall.vy / mag) * target;
}

/*
 * updatePong()
 * Runs one physics step:
 *   1. Move paddle left/right
 *   2. Move ball
 *   3. Bounce off left / right walls
 *   4. Bounce off top wall (below the HUD bar)
 *   5. Paddle collision: reflect upward, add angle from hit position
 *   6. Bottom edge: game over
 */
void updatePong() {
  if (pDead) return;

  // 1 — paddle movement
  int mv = horizInput();
  if (mv) {
    pPaddleX += mv * 5;
    pPaddleX = constrain(pPaddleX, 0, 128 - PONG_PADDLE_W);
  }

  // 2 — move ball
  pBall.x += pBall.vx;
  pBall.y += pBall.vy;

  // 3 — left / right wall bounce
  if (pBall.x <= 1) {
    pBall.x  = 1;
    pBall.vx = fabs(pBall.vx);
    soundWallHit();
  }
  if (pBall.x + PONG_BALL_SZ >= 127) {
    pBall.x  = 127 - PONG_BALL_SZ;
    pBall.vx = -fabs(pBall.vx);
    soundWallHit();
  }

  // 4 — top wall bounce (below the HUD separator line)
  if (pBall.y <= PONG_HUD_H + 1) {
    pBall.y  = PONG_HUD_H + 1;
    pBall.vy = fabs(pBall.vy);
    soundWallHit();
  }

  // 5 — paddle collision (only when ball is moving downward)
  if (pBall.vy > 0 &&
      pBall.y + PONG_BALL_SZ >= PONG_PADDLE_Y &&
      pBall.y  < PONG_PADDLE_Y + PONG_PADDLE_H &&
      pBall.x + PONG_BALL_SZ  > pPaddleX &&
      pBall.x  < pPaddleX + PONG_PADDLE_W)
  {
    float rel = ((pBall.x + PONG_BALL_SZ / 2.0f) - (pPaddleX + PONG_PADDLE_W / 2.0f))
                / (PONG_PADDLE_W / 2.0f);
    pBall.vx = rel * 2.5f;
    pBall.vy = -fabs(pBall.vy);
    pBall.vx = constrain(pBall.vx, -3.5f, 3.5f);
    pBall.y  = (float)(PONG_PADDLE_Y - PONG_BALL_SZ);
    pScore++;
    soundPaddleHit();   // crisp click on paddle contact
    rescaleBallSpeed();
  }

  // 6 — missed the paddle → game over
  if (pBall.y + PONG_BALL_SZ >= 64) {
    checkAndSave(pScore, pongHighScore, "pongScore");
    soundGameOver();    // sad jingle plays once before screen freezes
    pDead = true;
  }
}

// ── Pong draw calls ───────────────────────────────────────

/*
 * drawPong()
 * Layout:
 *   Row  0–9  : HUD bar (score text + separator line)
 *   Row 10–63 : play area (border, paddle, ball)
 * Game-over overlay: white panel, black inverted text — consistent
 * with car and snake game-over styles.
 */
void drawPong() {
  u8g2.firstPage();
  do {

    // ════════════════════════════════════════════════════
    //  GAME OVER SCREEN — full 128×64 white, black text
    // ════════════════════════════════════════════════════
    if (pDead) {
      // Fill the entire display white
      u8g2.setDrawColor(1);
      u8g2.drawBox(0, 0, 128, 64);

      // All following text is drawn in black (colour 0)
      u8g2.setDrawColor(0);

      // "GAME OVER" — large, near the top
      u8g2.setFont(u8g2_font_helvB12_tf);
      u8g2.drawStr((128 - u8g2.getStrWidth("GAME OVER")) / 2, 14, "GAME OVER");

      // Thin divider line below the title
      u8g2.drawHLine(10, 17, 108);

      // Score this round — medium font, centred
      char sc[20]; sprintf(sc, "Score:  %d", pScore);
      u8g2.setFont(u8g2_font_helvB10_tf);
      u8g2.drawStr((128 - u8g2.getStrWidth(sc)) / 2, 31, sc);

      // All-time best — same font, one line below
      char bs[20]; sprintf(bs, "Best:   %d", pongHighScore);
      u8g2.drawStr((128 - u8g2.getStrWidth(bs)) / 2, 45, bs);

      // "New best!" banner when the player just set a record
      if (pScore > 0 && pScore == pongHighScore) {
        u8g2.setFont(u8g2_font_5x7_tf);
        u8g2.drawStr((128 - u8g2.getStrWidth("** NEW BEST! **")) / 2, 54,
                     "** NEW BEST! **");
      }

      // Exit hint at the very bottom
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.drawStr((128 - u8g2.getStrWidth("3x click to exit")) / 2, 62,
                   "3x click to exit");

      // Restore draw colour for next frame
      u8g2.setDrawColor(1);

    // ════════════════════════════════════════════════════
    //  LIVE GAME SCREEN
    // ════════════════════════════════════════════════════
    } else {
      // ── Outer border ─────────────────────────────────
      u8g2.drawFrame(0, 0, 128, 64);

      // ── HUD bar (score left, speed right) ────────────
      u8g2.setFont(u8g2_font_5x7_tf);
      char sc[18]; sprintf(sc, "Score: %d", pScore);
      u8g2.drawStr(3, 8, sc);
      char sp[12]; sprintf(sp, "Sp:%.1f", pongBallSpeed());
      u8g2.drawStr(128 - u8g2.getStrWidth(sp) - 3, 8, sp);

      // ── Separator between HUD and play area ──────────
      u8g2.drawHLine(1, PONG_HUD_H, 126);

      // ── Paddle ───────────────────────────────────────
      u8g2.drawBox(pPaddleX, PONG_PADDLE_Y, PONG_PADDLE_W, PONG_PADDLE_H);

      // ── Ball ─────────────────────────────────────────
      u8g2.drawBox((int)pBall.x, (int)pBall.y, PONG_BALL_SZ, PONG_BALL_SZ);
    }

  } while (u8g2.nextPage());
}

/*
 * tickPong()
 * Rate-limited to PONG_FRAME_MS per tick.
 * When not dead: run physics then draw.
 * When dead: just keep drawing the frozen game-over screen.
 */
void tickPong() {
  unsigned long now = millis();
  if (now - pFrameTs < PONG_FRAME_MS) return;
  pFrameTs = now;
  updatePong();
  drawPong();
}


// ╔══════════════════════════════════════════════════════════╗
// ║  SETUP                                                  ║
// ╚══════════════════════════════════════════════════════════╝

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Retro Arcade booting...");

  pinMode(PIN_JOY_BTN, INPUT_PULLUP);
  pinMode(PIN_BUZZ, OUTPUT);           // buzzer signal pin
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  if (!u8g2.begin()) {
    Serial.println("OLED INIT FAILED — check wiring/I2C address");
    while (true) {}
  }

  randomSeed(analogRead(34));
  loadHighScores();
  drawBoot();
  soundBoot();                         // play startup jingle over the splash screen
  stateMs = lastInputMs = millis();
  appState = ST_BOOT;
}


// ╔══════════════════════════════════════════════════════════╗
// ║  MAIN LOOP                                              ║
// ╚══════════════════════════════════════════════════════════╝

void loop() {
  unsigned long now = millis();

  // ── 1. Single joystick read for the whole frame ────────
  // updateJoystick() sets btnEdge, axisUp, axisDown globally.
  // Nothing else calls digitalRead(PIN_JOY_BTN) anywhere.
  updateJoystick();
  int nav = navEdge();   // -1=up, +1=down, 0=none (rising-edge only)

  // Any activity resets the idle timer
  if (axisUp || axisDown || btnEdge) lastInputMs = now;

  // ── 2. Triple-click exit (only relevant in game/result states) ──
  bool tc = false;
  if (appState == ST_CAR   ||
      appState == ST_SNAKE  ||
      appState == ST_PONG   ||
      appState == ST_RESULT)
    tc = tripleClick();

  // ── 3. State machine ───────────────────────────────────
  switch (appState) {

    // ── Boot splash ──────────────────────────────────────
    case ST_BOOT:
      if (now - stateMs >= BOOT_MS) {
        appState = ST_MENU;
        drawMenu();
      }
      break;

    // ── Main menu ─────────────────────────────────────────
    case ST_MENU:
      // Enter idle sleep after IDLE_MS of no activity
      if (now - lastInputMs >= IDLE_MS) {
        sleepAnimMs = now; sleepStep = 0;
        appState = ST_SLEEP; drawSleep(); break;
      }
      // Navigate: nav=+1 means joystick DOWN → cursor UP in list (matching physical feel)
      if (nav ==  1) { menuCursor = (menuCursor - 1 + NUM_ITEMS) % NUM_ITEMS; soundMenuNav(); drawMenu(); }
      if (nav == -1) { menuCursor = (menuCursor + 1) % NUM_ITEMS;             soundMenuNav(); drawMenu(); }

      if (btnEdge) {
        soundMenuSelect();               // confirmation chime on any menu press
        // Reset triple-click state so entering a game starts clean
        tcCount = 0; tcLastMs = 0;

        switch (menuCursor) {
          case 0:   // Car Game
            resetCarGame(); appState = ST_CAR; drawCarGame(); break;

          case 1:   // Snake
            resetSnake(); appState = ST_SNAKE; drawSnakeGame(); break;

          case 2:   // Pong
            resetPong(); appState = ST_PONG; drawPong(); break;

          case 3:   // Turn Off → deep sleep
            u8g2.firstPage();
            do {
              u8g2.drawFrame(1, 1, 126, 62);
              centered("Turning off...", u8g2_font_helvB10_tf, 33);
              centered("Bye! :)", u8g2_font_5x7_tf, 48);
            } while (u8g2.nextPage());
            delay(1200);
            u8g2.setPowerSave(1);        // blank the OLED display
            esp_deep_sleep_start();      // deep sleep; wake = reset button
            break;
        }
      }
      break;

    // ── Generic result / info screen ──────────────────────
    case ST_RESULT:
      if (tc) {
        lastBtnMs = millis();    // extra debounce so exit click doesn't land on menu
        appState = ST_MENU; drawMenu(); lastInputMs = now;
      }
      break;

    // ── Idle sleep animation ──────────────────────────────
    case ST_SLEEP:
      // Advance animation frame every SLEEP_STEP_MS
      if (now - sleepAnimMs >= SLEEP_STEP_MS) {
        sleepAnimMs = now;
        sleepStep = (sleepStep + 1) % 4;
        drawSleep();
      }
      // Any joystick movement or button press wakes to menu
      if (axisUp || axisDown || btnEdge) {
        appState = ST_MENU; lastInputMs = now; drawMenu();
      }
      break;

    // ── Car game ──────────────────────────────────────────
    case ST_CAR:
      if (tc) {
        lastBtnMs = millis();    // debounce guard
        appState = ST_MENU; drawMenu(); lastInputMs = now; break;
      }
      // While running: tick physics + draw. When dead: freeze on game-over screen.
      carRunning ? tickCarGame() : drawCarGameOver();
      break;

    // ── Snake game ────────────────────────────────────────
    case ST_SNAKE:
      if (tc) {
        lastBtnMs = millis();
        appState = ST_MENU; drawMenu(); lastInputMs = now; break;
      }
      tickSnake();
      break;

    // ── Pong game ─────────────────────────────────────────
    case ST_PONG:
      if (tc) {
        lastBtnMs = millis();
        appState = ST_MENU; drawMenu(); lastInputMs = now; break;
      }
      // tickPong() handles both live and dead states internally
      tickPong();
      break;

  }   // end switch(appState)

  delay(20);   // small yield so FreeRTOS / Wi-Fi tasks can run
}

/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║  QUICK CUSTOMISATION REFERENCE                         ║
 * ╠══════════════════════════════════════════════════════════╣
 * ║                                                         ║
 * ║  TRIPLE-CLICK WINDOW                                    ║
 * ║    TC_WINDOW_MS — max ms between clicks (default 800)   ║
 * ║                                                         ║
 * ║  CAR DIFFICULTY                                         ║
 * ║    carObsSpeed()   — px/frame per score bracket         ║
 * ║    carSpawnEvery() — frames between spawn checks        ║
 * ║    carSpawnChance()— 0-100 spawn probability            ║
 * ║    carMaxType()    — score that unlocks each type       ║
 * ║                                                         ║
 * ║  SNAKE SPEED                                            ║
 * ║    snakeInterval() — start ms + step per tier           ║
 * ║                                                         ║
 * ║  SNAKE WALLS                                            ║
 * ║    Remove the 4 wrap lines in tickSnake() and add wall  ║
 * ║    death check to revert to hard walls.                 ║
 * ║                                                         ║
 * ║  PONG DIFFICULTY                                        ║
 * ║    PONG_BASE_SPD   — starting ball speed                ║
 * ║    PONG_SPEED_EVERY— points per speed tier increase     ║
 * ║    PONG_SPEED_STEP — px/frame added per tier            ║
 * ║    PONG_PADDLE_W   — paddle width (wider = easier)      ║
 * ║                                                         ║
 * ║  MENU                                                   ║
 * ║    menuItems[] + NUM_ITEMS — add/rename options         ║
 * ║    Add a new case in the ST_MENU btnEdge block          ║
 * ║    Add the matching state to AppState enum              ║
 * ║                                                         ║
 * ║  OLED CHIP                                              ║
 * ║    Change the U8G2_... constructor line at the top.     ║
 * ║    SH1106: U8G2_SH1106_128X64_NONAME_F_HW_I2C          ║
 * ╚══════════════════════════════════════════════════════════╝
 */
