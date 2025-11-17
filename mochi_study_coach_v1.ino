#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "allframes/allframes.h"

// ================== OLED CONFIG ==================
#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ================== BUTTON CONFIG ==================
const int BUTTON_PIN = 2;

const unsigned long DEBOUNCE_DELAY   = 50;
const unsigned long LONG_PRESS_TIME  = 1000;
const unsigned long MIN_PRESS_TIME   = 50;
const unsigned long DOUBLE_PRESS_GAP = 400;

int buttonState      = HIGH;
int lastReading      = HIGH;
bool waitingSecondPress = false;

unsigned long lastReleaseTime   = 0;
unsigned long lastDebounceTime  = 0;
unsigned long pressedTime       = 0;
unsigned long releasedTime      = 0;

// ================== ANIMATION STATE ==================

// Mode animasi (mapping ke GIF yang ada di allframes.h)
enum MochiMode {
  MODE_AWAKE = 0,
  MODE_ANGRY,
  MODE_BASED,
  MODE_ANGRY2,
  MODE_SLEEP,
  MODE_BLINK1,
  MODE_BLINK2,
  MODE_LONGBLINK
};

MochiMode currentMode = MODE_AWAKE;

// Pointer ke GIF yang sedang diputar
const AnimatedGIF* currentGif = nullptr;
uint8_t currentFrame = 0;
unsigned long lastFrameTime = 0;

// ================== MOCHI "BRAIN" (STATE MACHINE) ==================

enum MochiState {
  STATE_IDLE,
  STATE_FOCUS,
  STATE_WAITING_CONFIRM,
  STATE_NAGGING
};

MochiState mochiState = STATE_IDLE;

struct Task {
  String project;
  String text;
  unsigned long estMs;   // estimasi waktu (ms)
};

// Hardcode satu task dulu, bisa di-update lewat Serial
Task currentTask = {
  "B100",
  "Cari 2 jurnal baru untuk subbab 2.2",
  60000UL  // 60 detik utk testing
};

unsigned long focusStartTime = 0;

// WAITING_CONFIRM timeout -> NAGGING
const unsigned long WAIT_CONFIRM_TIMEOUT = 30000; // 30 detik
unsigned long waitingStartTime = 0;

// Buffer input dari Serial (untuk update teks task)
String serialBuffer = "";

// ================== TEXT WRAP CONFIG ==================

const uint8_t CHAR_PER_LINE     = 21;  // kira-kira 6px per karakter di font default
const uint8_t MAX_LINES_FOCUS   = 3;   // maksimal 3 baris teks task

// ================== HELPER: SET MODE (ANIMASI) ==================

void setMode(MochiMode mode) {
  currentMode = mode;

  switch (mode) {
    case MODE_BASED:
      currentGif = &based_gif;
      break;

    case MODE_AWAKE:
      currentGif = &awake_gif;
      break;

    case MODE_SLEEP:
      currentGif = &sleep_gif;
      break;

    case MODE_ANGRY:
      currentGif = &angry_gif;
      break;

    case MODE_ANGRY2:
      currentGif = &angry2_gif;
      break;

    case MODE_BLINK1:
      currentGif = &blink1_gif;
      break;

    case MODE_BLINK2:
      currentGif = &blink2_gif;
      break;

    case MODE_LONGBLINK:
      currentGif = &longblink_gif;
      break;
  }

  currentFrame   = 0;
  lastFrameTime  = millis();
}

// ================== HELPER: PLAY GIF SEKALI (BLOCKING) ==================
// Dipakai cuma buat boot sequence (boleh pakai delay)

void playGifOnce(const AnimatedGIF& gif) {
  for (uint8_t i = 0; i < gif.frame_count; i++) {
    display.clearDisplay();
    display.drawBitmap(
      0, 0,
      gif.frames[i],
      gif.width,
      gif.height,
      1
    );
    display.display();

    uint16_t delayMs = pgm_read_word(&gif.delays[i]);
    delay(delayMs);   // blocking, tapi gapapa di boot
  }
}

// ================== OVERLAY (PROGRESS BAR, DLL) ==================

void drawOverlay(unsigned long now) {
  if (mochiState == STATE_FOCUS) {
    // Progress focus time
    unsigned long elapsed = now - focusStartTime;
    if (elapsed > currentTask.estMs) elapsed = currentTask.estMs;

    float ratio = (float)elapsed / (float)currentTask.estMs;
    int barWidth = (int)(ratio * SCREEN_WIDTH);

    int barHeight = 4;
    int barY = SCREEN_HEIGHT - barHeight;

    // Isi progress bar
    display.fillRect(0, barY, barWidth, barHeight, SSD1306_WHITE);
  }
  // Kalau mau overlay lain per state, tambahin else-if di sini
}

// ================== HELPER: STEP ANIMATION (NON-BLOCKING) ==================

void stepAnimation() {
  if (currentGif == nullptr) return;

  unsigned long now = millis();

  uint16_t delayMs = pgm_read_word(&currentGif->delays[currentFrame]);
  if (now - lastFrameTime < delayMs) return;

  lastFrameTime = now;

  display.clearDisplay();
  display.drawBitmap(
    0, 0,
    currentGif->frames[currentFrame],
    currentGif->width,
    currentGif->height,
    1
  );

  // Tambah overlay (progress bar di FOCUS, dll)
  drawOverlay(now);

  display.display();

  currentFrame++;
  if (currentFrame >= currentGif->frame_count) {
    currentFrame = 0;   // loop
  }
}

// ================== TEXT HELPER (MULTILINE + TRUNCATE) ==================

void printMultilineTruncated(const String &text, int x, int y, uint8_t maxLines) {
  String t = text;
  uint16_t maxChars = CHAR_PER_LINE * maxLines;

  if (t.length() > maxChars) {
    t = t.substring(0, maxChars - 3) + "..."; // tambahkan "..." di akhir
  }

  for (uint8_t line = 0; line < maxLines; line++) {
    uint16_t start = line * CHAR_PER_LINE;
    if (start >= t.length()) break;
    uint16_t end = start + CHAR_PER_LINE;
    if (end > t.length()) end = t.length();

    display.setCursor(x, y + line * 8);  // tiap baris turun 8px
    display.print(t.substring(start, end));
  }
}

// ================== RENDER UI PER STATE ==================

void renderMochi() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  switch (mochiState) {
    case STATE_IDLE:
      display.setCursor(0, 0);
      display.println("No task.");
      display.println("Long press to");
      display.println("start.");
      display.display();
      delay(1000);               // tampil teks 1 detik
      setMode(MODE_LONGBLINK);   // wajah chill
      break;

    case STATE_FOCUS:
      // Baris pertama: [B100]
      display.setCursor(0, 0);
      display.print("[");
      display.print(currentTask.project);
      display.println("]");

      // Teks task di bawah, rapi & dipotong kalau kepanjangan
      printMultilineTruncated(currentTask.text, 0, 10, MAX_LINES_FOCUS);
      display.display();
      delay(1000);
      setMode(MODE_BASED);       // wajah fokus
      break;

    case STATE_WAITING_CONFIRM:
      display.setCursor(0, 0);
      display.println("Udah kelar, bre?");
      display.display();
      delay(1000);
      setMode(MODE_BLINK1);      // wajah nanya / kedip
      break;

    case STATE_NAGGING:
      display.setCursor(0, 0);
      display.println("Lanjut dikit");
      display.println("lagi, plis.");
      display.display();
      delay(1000);
      setMode(MODE_ANGRY2);      // wajah marah
      break;
  }
}

// ================== STATE MACHINE HELPERS ==================

void enterState(MochiState newState) {
  mochiState = newState;

  switch (newState) {
    case STATE_IDLE:
      Serial.println("[STATE] IDLE");
      break;

    case STATE_FOCUS:
      focusStartTime = millis();
      Serial.println("[STATE] FOCUS");
      Serial.print("  Project: ");
      Serial.println(currentTask.project);
      Serial.print("  Task   : ");
      Serial.println(currentTask.text);
      break;

    case STATE_WAITING_CONFIRM:
      waitingStartTime = millis();
      Serial.println("[STATE] WAITING_CONFIRM (sudah selesai?)");
      break;

    case STATE_NAGGING:
      Serial.println("[STATE] NAGGING (ayo, jangan malas 😈)");
      break;
  }

  // Setelah state di-set & logging ke Serial,
  // baru render UI + set animasi wajah
  renderMochi();
}

void updateMochiState() {
  unsigned long now = millis();

  // Timer dari FOCUS → WAITING_CONFIRM
  if (mochiState == STATE_FOCUS) {
    if (now - focusStartTime >= currentTask.estMs) {
      Serial.println("[TIMER] Focus time over -> WAITING_CONFIRM");
      enterState(STATE_WAITING_CONFIRM);
    }
  }

  // WAITING_CONFIRM terlalu lama -> NAGGING
  if (mochiState == STATE_WAITING_CONFIRM) {
    if (now - waitingStartTime >= WAIT_CONFIRM_TIMEOUT) {
      Serial.println("[TIMER] WAITING_CONFIRM timeout -> NAGGING");
      enterState(STATE_NAGGING);
    }
  }
}

// ================== SERIAL INPUT: UPDATE TEKS TASK ==================

void handleSerialInput() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\r' || c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        currentTask.text = serialBuffer;
        Serial.print("[TASK] Updated text: ");
        Serial.println(currentTask.text);
      }
      serialBuffer = "";
    } else {
      serialBuffer += c;
    }
  }
}

// ================== BUTTON EVENT HANDLERS ==================

void onShortPress() {
  // Behavior tergantung state
  if (mochiState == STATE_FOCUS) {
    Serial.println("[EVENT] Short press in FOCUS -> SKIP task");
    enterState(STATE_IDLE);
  } else if (mochiState == STATE_WAITING_CONFIRM) {
    Serial.println("[EVENT] Short press in WAITING_CONFIRM -> NOT DONE, back to FOCUS");
    enterState(STATE_FOCUS);
  } else if (mochiState == STATE_NAGGING) {
    Serial.println("[EVENT] Short press in NAGGING -> OK, back to FOCUS");
    enterState(STATE_FOCUS);
  } else {
    Serial.println("[EVENT] Short press (no special action in this state)");
  }
}

void onDoubleShortPress() {
  // Easter egg / debug animasi
  Serial.println("[EVENT] DOUBLE SHORT PRESS -> MODE_BASED");
  setMode(MODE_BASED);
}

void onLongPress() {
  if (mochiState == STATE_IDLE) {
    Serial.println("[EVENT] Long press in IDLE -> start FOCUS");
    enterState(STATE_FOCUS);
  } else if (mochiState == STATE_FOCUS) {
    Serial.println("[EVENT] Long press in FOCUS -> mark DONE");
    enterState(STATE_IDLE);
  } else if (mochiState == STATE_WAITING_CONFIRM) {
    Serial.println("[EVENT] Long press in WAITING_CONFIRM -> DONE");
    enterState(STATE_IDLE);
  } else if (mochiState == STATE_NAGGING) {
    Serial.println("[EVENT] Long press in NAGGING -> DONE, back to IDLE");
    enterState(STATE_IDLE);
  } else {
    Serial.println("[EVENT] Long press (no special action in this state)");
  }
}

// ================== HANDLE BUTTON (DEBOUNCE + SHORT/LONG/DOUBLE) ==================

void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  unsigned long now = millis();

  // Timeout untuk single short press
  if (waitingSecondPress && (now - lastReleaseTime > DOUBLE_PRESS_GAP)) {
    waitingSecondPress = false;
    Serial.println("SHORT PRESS");
    onShortPress();
  }

  // Debounce
  if (reading != lastReading) {
    lastDebounceTime = now;
    lastReading = reading;
  }

  if ((now - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;

      // HIGH -> LOW = tombol ditekan
      if (buttonState == LOW) {
        pressedTime = now;
      }
      // LOW -> HIGH = tombol dilepas
      else { // buttonState == HIGH
        releasedTime = now;
        unsigned long pressDuration = releasedTime - pressedTime;

        if (pressDuration < MIN_PRESS_TIME) {
          return; // noise
        }

        // LONG PRESS
        if (pressDuration >= LONG_PRESS_TIME) {
          waitingSecondPress = false;
          Serial.println("LONG PRESS");
          onLongPress();
        }
        // SHORT PRESS (kandidat single/double)
        else {
          if (waitingSecondPress && (releasedTime - lastReleaseTime <= DOUBLE_PRESS_GAP)) {
            waitingSecondPress = false;
            Serial.println("DOUBLE SHORT PRESS");
            onDoubleShortPress();
          } else {
            waitingSecondPress = true;
            lastReleaseTime = releasedTime;
            // Aksi single short press dieksekusi di timeout di atas
          }
        }
      }
    }
  }
}

// ================== SETUP ==================

void setup() {
  Serial.begin(115200);
  delay(500);

  // I2C ESP32-C3 (ganti kalau SDA/SCL beda)
  Wire.begin(8, 9);   // SDA=8, SCL=9

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;);
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  display.clearDisplay();
  display.display();

  // 1) Tampilkan teks "Mochi Booting..."
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 26);
  display.println("Mochi Booting...");
  display.display();

  delay(2000); // tunggu 2 detik

  // 2) Mainkan animasi sleep sekali (blocking)
  playGifOnce(sleep_gif);

  // 3) Setelah boot, masuk state IDLE (animasi sesuai state)
  enterState(STATE_IDLE);

  Serial.println("=== Mochi Ready ===");
  Serial.println("Long press  (IDLE)  -> mulai FOCUS");
  Serial.println("Long press  (FOCUS) -> task DONE (kembali IDLE)");
  Serial.println("Short press (FOCUS) -> SKIP task (kembali IDLE)");
  Serial.println("Short press (WAITING_CONFIRM) -> lanjut FOCUS");
  Serial.println("Diam di WAITING_CONFIRM -> NAGGING");
  Serial.println("Ketik teks di Serial -> update deskripsi task.");
}

// ================== LOOP ==================

void loop() {
  handleSerialInput();  // update teks task dari Serial
  handleButton();       // baca tombol & convert ke event
  updateMochiState();   // urus timer FOCUS -> WAITING_CONFIRM -> NAGGING
  stepAnimation();      // mainkan animasi aktif + overlay
}
