#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
// ---- CONFIG ----
#define WIFI_SSID     "Odido-9F33C7"
#define WIFI_PASS     "KXCSM8QLJNE9YD8M"
#define OLLAMA_HOST   "http://192.168.1.98:11434"
#define BRIDGE_HOST   "http://192.168.1.98:8888"
#define OLLAMA_MODEL  "gemma3"
#define AGENT_NAME    "Pixel"
#define USE_BRIDGE    true

#define BTN_LEFT  0
#define BTN_RIGHT 14
#define BUZZER_PIN 2

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);
WiFiServer webServer(80);

// ---- COLORS ----
#define BG          0x0000
// Warm amber/orange eye colors (toned down for camera)
#define EYE_OUTER   0xC2E0  // Muted warm orange
#define EYE_MID     0xD380  // Mid amber
#define EYE_INNER   0xE480  // Warm amber
#define EYE_BRIGHT  0xF5C0  // Soft yellow highlight
#define EYE_WHITE   0xDF9E  // Soft white (not pure white)
#define EYE_DIM     0x6180  // Dim amber
#define MOUTH_GLOW  0xC2E0
#define TEXT_FG     0xC638  // Slightly dimmed white
#define BUBBLE_BG   0x18C3  // Dark gray bubble

// ---- STATE ----
enum Mood { IDLE, HAPPY, THINKING, TALKING, SURPRISED, SLEEPING, SAD };
enum Screen { SCREEN_BOOT, SCREEN_FACE, SCREEN_CHAT, SCREEN_POMO };

Mood mood = IDLE;
Screen currentScreen = SCREEN_BOOT;
int energy = 100;
int happiness = 80;
unsigned long lastInteraction = 0;
unsigned long lastBlink = 0;
bool blinking = false;
int blinkFrame = 0;  // 0=open, 1-4=closing, 5=closed, 6-9=opening
float lookX = 0, lookY = 0;        // Eye look direction
float targetLookX = 0, targetLookY = 0;
unsigned long lastLookChange = 0;
float breathePhase = 0;
String lastResponse = "";
String typingBuffer = "";
int typingIdx = 0;
bool isTyping = false;
unsigned long lastTypeChar = 0;
String chatHistory[6];
int chatCount = 0;
// Pomodoro
bool pomoActive = false;
int pomoRemaining = 0;       // seconds left
int pomoWork = 25 * 60;      // 25 min
int pomoBreak = 5 * 60;      // 5 min
bool pomoIsBreak = false;
int pomoSessions = 0;
unsigned long pomoLastTick = 0;

bool btnLeftWas = false;
bool btnRightWas = false;
// Track screen cycling with long-press
unsigned long btnLeftHoldStart = 0;
bool btnLeftLongHandled = false;

// Prompts
const char* prompts[] = {
  "Tell me something interesting in one short sentence.",
  "How are you feeling right now? Reply in character, one sentence.",
  "Tell me a fun fact in one sentence.",
  "Say something encouraging in one sentence.",
  "What are you thinking about? One sentence.",
  "Tell me a joke in one sentence.",
  "Give me a creative idea in one sentence.",
  "What's your mood right now? One sentence."
};
int promptCount = 8;
int promptIdx = 0;
String serialBuffer = "";

// ---- SOUND ----
void beep(int freq, int dur) {
  ledcAttach(BUZZER_PIN, freq, 8);
  ledcWrite(BUZZER_PIN, 128);
  delay(dur);
  ledcWrite(BUZZER_PIN, 0);
  ledcDetach(BUZZER_PIN);
}
void soundHappy()    { beep(880,50); delay(30); beep(1100,50); delay(30); beep(1320,80); }
void soundPoke()     { beep(600,30); delay(20); beep(900,60); }
void soundThinking() { beep(440,40); delay(20); beep(330,40); }
void soundWake()     { beep(523,60); delay(40); beep(659,60); delay(40); beep(784,100); }
void soundSleep()    { beep(300,80); delay(60); beep(250,120); }
void soundType()     { beep(1800,8); }
void soundAlarm()    {
  for (int i = 0; i < 3; i++) {
    beep(1000, 150); delay(80);
    beep(1200, 150); delay(80);
    beep(1400, 200); delay(150);
  }
}

// ---- EYE DRAWING (LOONA STYLE) ----

// Draw a single rounded-rect eye with glow and highlights
void drawEye(int cx, int cy, int w, int h, float openRatio, float lx, float ly) {
  if (openRatio <= 0.05) {
    // Fully closed - just a horizontal line with glow
    int lineW = w;
    for (int t = -2; t <= 2; t++) {
      uint16_t c = (t == 0) ? EYE_MID : EYE_DIM;
      sprite.drawFastHLine(cx - lineW/2, cy + t, lineW, c);
    }
    return;
  }

  int eyeH = (int)(h * openRatio);
  if (eyeH < 4) eyeH = 4;
  int eyeY = cy - eyeH/2;
  int r = min(w/4, eyeH/3);  // Corner radius

  // Outer glow (larger, dimmer)
  sprite.fillRoundRect(cx - w/2 - 4, eyeY - 4, w + 8, eyeH + 8, r + 4, 0x4100);

  // Eye body - gradient from outer to inner
  sprite.fillRoundRect(cx - w/2, eyeY, w, eyeH, r, EYE_OUTER);
  sprite.fillRoundRect(cx - w/2 + 3, eyeY + 3, w - 6, eyeH - 6, r - 1, EYE_MID);
  sprite.fillRoundRect(cx - w/2 + 7, eyeY + 7, w - 14, eyeH - 14, r - 2, EYE_INNER);

  // Pupil area (darker center region that shifts with look direction)
  int pupilW = w / 3;
  int pupilH = eyeH / 3;
  int pupilX = cx + (int)(lx * w/6) - pupilW/2;
  int pupilY = cy + (int)(ly * eyeH/6) - pupilH/2;
  sprite.fillRoundRect(pupilX, pupilY, pupilW, pupilH, pupilH/3, EYE_BRIGHT);

  // Main highlight (top-left, large)
  int hlX = cx - w/4 + (int)(lx * 3);
  int hlY = eyeY + eyeH/4 + (int)(ly * 3);
  sprite.fillRoundRect(hlX - 6, hlY - 4, 14, 10, 4, EYE_WHITE);

  // Small secondary highlight (bottom-right)
  int hl2X = cx + w/6 + (int)(lx * 2);
  int hl2Y = cy + eyeH/6 + (int)(ly * 2);
  sprite.fillCircle(hl2X, hl2Y, 3, EYE_WHITE);
}

void drawEyes() {
  int screenW = 320;
  int screenH = 170;
  int eyeW = 55;   // Width of each eye
  int eyeH = 50;   // Height when fully open
  int eyeGap = 30; // Gap between eyes
  int centerY = screenH / 2 - 20;  // Move eyes up to make room

  // Breathing makes eyes bob slightly
  float breatheY = sin(breathePhase) * 2.0;
  centerY += (int)breatheY;

  int leftEyeX  = screenW/2 - eyeGap/2 - eyeW/2;
  int rightEyeX = screenW/2 + eyeGap/2 + eyeW/2;

  float openRatio = 1.0;

  // Blink animation
  if (blinking) {
    if (blinkFrame <= 3)       openRatio = 1.0 - blinkFrame * 0.33;
    else if (blinkFrame == 4)  openRatio = 0.0;
    else if (blinkFrame <= 7)  openRatio = (blinkFrame - 4) * 0.33;
    else                       openRatio = 1.0;
  }

  // Mood modifiers
  float leftSquish = 1.0, rightSquish = 1.0;
  int leftOffY = 0, rightOffY = 0;
  int leftExtraH = 0, rightExtraH = 0;

  switch (mood) {
    case HAPPY:
      // Squished happy eyes (like ^_^)
      openRatio *= 0.55;
      leftOffY = 6;
      rightOffY = 6;
      break;
    case SAD:
      // Droopy, slightly smaller
      openRatio *= 0.7;
      leftOffY = 4;
      rightOffY = 4;
      // Tilt eyes (inner corners up)
      leftExtraH = -4;
      rightExtraH = -4;
      break;
    case SURPRISED:
      // Wide open!
      openRatio = 1.0;
      eyeW = 62;
      eyeH = 58;
      break;
    case THINKING:
      // One eye slightly squished
      leftSquish = 0.7;
      rightSquish = 1.0;
      break;
    case SLEEPING:
      openRatio = 0.0;
      break;
    case TALKING:
      // Slight pulse with talking
      openRatio *= 0.85 + 0.15 * sin(millis() * 0.008);
      break;
    default:
      break;
  }

  drawEye(leftEyeX, centerY + leftOffY, eyeW, eyeH + leftExtraH, openRatio * leftSquish, lookX, lookY);
  drawEye(rightEyeX, centerY + rightOffY, eyeW, eyeH + rightExtraH, openRatio * rightSquish, lookX, lookY);
}

// Draw subtle mouth (small glow below eyes when talking/happy)
void drawMouth() {
  int cx = 160;
  int cy = 115;

  switch (mood) {
    case TALKING: {
      // Animated glowing oval mouth
      int mouthW = 12 + (int)(4 * sin(millis() * 0.01));
      int mouthH = 4 + (int)(3 * sin(millis() * 0.008));
      sprite.fillEllipse(cx, cy, mouthW, mouthH, EYE_DIM);
      sprite.fillEllipse(cx, cy, mouthW - 3, max(1, mouthH - 2), EYE_OUTER);
      break;
    }
    case HAPPY:
      // Small upward curve
      for (int i = -12; i <= 12; i++) {
        int y = cy - (i * i) / 20 + 8;
        sprite.drawPixel(cx + i, y, EYE_DIM);
        sprite.drawPixel(cx + i, y + 1, EYE_DIM);
      }
      break;
    case SAD:
      // Small downward curve
      for (int i = -8; i <= 8; i++) {
        int y = cy + (i * i) / 16;
        sprite.drawPixel(cx + i, y, EYE_DIM);
      }
      break;
    case SURPRISED:
      // Small O
      sprite.drawCircle(cx, cy, 6, EYE_OUTER);
      sprite.drawCircle(cx, cy, 5, EYE_DIM);
      break;
    default:
      break;
  }
}

// Thinking dots animation
void drawThinkDots() {
  if (mood != THINKING) return;
  int t = (millis() / 350) % 4;
  int cx = 160;
  int cy = 140;
  for (int i = 0; i < 3; i++) {
    uint16_t c = (i <= t) ? EYE_OUTER : 0x2104;
    int bounce = (i == (t % 3)) ? -3 : 0;
    sprite.fillCircle(cx - 16 + i * 16, cy + bounce, 4, c);
  }
}

// Zzz for sleeping
void drawSleepZzz() {
  if (mood != SLEEPING) return;
  unsigned long t = millis() / 600;
  sprite.setFreeFont(NULL);
  for (int i = 0; i < 3; i++) {
    int offset = (t + i) % 4;
    int x = 200 + i * 12;
    int y = 60 - offset * 10 - i * 8;
    uint16_t c = (i == 0) ? EYE_DIM : (i == 1) ? EYE_OUTER : EYE_MID;
    sprite.setTextColor(c);
    sprite.setTextSize(1 + i);
    sprite.drawString("z", x, y);
  }
  sprite.setTextSize(1);
}

// Speech bubble overlay (bottom of screen)
void drawSpeechBubble(String text) {
  if (text.length() == 0) return;

  int bubbleH = 30;
  int bubbleY = 170 - bubbleH;
  int bubbleX = 10;
  int bubbleW = 300;

  sprite.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 8, BUBBLE_BG);
  sprite.drawRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 8, 0x3186);

  sprite.setTextColor(TEXT_FG);
  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);

  // Two lines max
  int maxChars = 48;
  String line1 = text.substring(0, min(maxChars, (int)text.length()));
  String line2 = "";
  if ((int)text.length() > maxChars) {
    int space = line1.lastIndexOf(' ');
    if (space > 0) {
      line2 = text.substring(space + 1, min(space + 1 + maxChars, (int)text.length()));
      line1 = text.substring(0, space);
    } else {
      line2 = text.substring(maxChars, min(maxChars * 2, (int)text.length()));
    }
    if ((int)text.length() > maxChars * 2) line2 = line2.substring(0, maxChars - 3) + "...";
  }

  sprite.drawString(line1, bubbleX + 8, bubbleY + 5);
  if (line2.length() > 0) sprite.drawString(line2, bubbleX + 8, bubbleY + 18);
}

// ---- FACE SCREEN ----
void drawFaceScreen() {
  sprite.fillSprite(BG);

  drawEyes();
  drawMouth();
  drawThinkDots();
  drawSleepZzz();

  // Show response in bubble
  String displayText = isTyping ? typingBuffer : lastResponse;
  if (displayText.length() > 0 && mood != SLEEPING) {
    drawSpeechBubble(displayText);
  }

  // Tiny status indicator (top-left, minimal)
  sprite.fillCircle(8, 8, 3, WiFi.isConnected() ? 0x07E0 : 0xF800);

  sprite.pushSprite(0, 0);
}

// ---- CHAT SCREEN ----
void drawChatScreen() {
  sprite.fillSprite(BG);

  // Mini eyes at top
  drawEye(140, 20, 25, 20, mood == SLEEPING ? 0 : 0.8, lookX, lookY);
  drawEye(180, 20, 25, 20, mood == SLEEPING ? 0 : 0.8, lookX, lookY);

  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);

  int y = 42;
  int startIdx = max(0, chatCount - 5);
  for (int i = startIdx; i < chatCount && i < startIdx + 5; i++) {
    String line = chatHistory[i % 6];
    int idx = 0;
    int maxChars = 50;
    while (idx < (int)line.length() && y < 160) {
      String seg = line.substring(idx, min(idx + maxChars, (int)line.length()));
      bool isUser = line.startsWith(">");
      sprite.setTextColor(isUser ? 0x07FF : EYE_OUTER);
      sprite.drawString(seg, 4, y);
      y += 11;
      idx += maxChars;
    }
    y += 3;
  }

  // Hint
  sprite.setTextColor(0x4208);
  sprite.drawString("[L] back", 4, 158);

  sprite.pushSprite(0, 0);
}

// ---- POMODORO SCREEN ----
void startPomodoro() {
  pomoActive = true;
  pomoIsBreak = false;
  pomoRemaining = pomoWork;
  pomoLastTick = millis();
  currentScreen = SCREEN_POMO;
  mood = HAPPY;
  soundHappy();
  startTyping("Focus time! Let's go!");
}

void updatePomodoro() {
  if (!pomoActive) return;

  unsigned long now = millis();
  if (now - pomoLastTick >= 1000) {
    pomoLastTick += 1000;
    pomoRemaining--;

    if (pomoRemaining <= 0) {
      if (pomoIsBreak) {
        // Break over, start new work session
        pomoSessions++;
        pomoIsBreak = false;
        pomoRemaining = pomoWork;
        mood = SURPRISED;
        soundAlarm();
        mood = HAPPY;
        startTyping("Break's over! Round " + String(pomoSessions + 1) + "!");
      } else {
        // Work done, start break
        pomoIsBreak = true;
        pomoRemaining = (pomoSessions > 0 && (pomoSessions + 1) % 4 == 0) ? 15 * 60 : pomoBreak;
        mood = SURPRISED;
        soundAlarm();
        mood = HAPPY;
        startTyping(pomoRemaining > 5 * 60 ? "Long break! You earned it!" : "Nice work! Take a break!");
      }
    }

    // Alert at 5 min, 1 min, and 10 sec
    if (pomoRemaining == 300) { beep(600, 100); }
    if (pomoRemaining == 60) { beep(800, 100); delay(50); beep(800, 100); }
    if (pomoRemaining == 10) { beep(1000, 50); }
  }
}

void drawPomoScreen() {
  sprite.fillSprite(BG);

  // Mini eyes at top, mood-aware
  float pomoOpen = pomoIsBreak ? 0.6 : 0.9;
  drawEye(140, 22, 30, 25, pomoOpen, lookX, lookY);
  drawEye(180, 22, 30, 25, pomoOpen, lookX, lookY);

  // Timer
  int mins = pomoRemaining / 60;
  int secs = pomoRemaining % 60;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);

  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(pomoIsBreak ? 0x07E0 : EYE_OUTER);
  sprite.setFreeFont(&FreeSansBold24pt7b);
  sprite.setTextSize(1);
  sprite.drawString(buf, 160, 80);

  // Mode label
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);
  sprite.setTextColor(pomoIsBreak ? 0x07E0 : EYE_MID);
  sprite.setTextDatum(MC_DATUM);
  sprite.drawString(pomoIsBreak ? "BREAK" : "FOCUS", 160, 50);

  // Progress arc (simple bar)
  int totalTime = pomoIsBreak ? (pomoRemaining > 5 * 60 ? 15 * 60 : pomoBreak) : pomoWork;
  float progress = 1.0f - ((float)pomoRemaining / totalTime);
  int barW = 200;
  int barX = (320 - barW) / 2;
  int barY = 110;
  sprite.fillRoundRect(barX, barY, barW, 6, 3, 0x2104);
  int fillW = (int)(barW * progress);
  if (fillW > 0) {
    uint16_t barColor = pomoIsBreak ? 0x07E0 : EYE_OUTER;
    sprite.fillRoundRect(barX, barY, fillW, 6, 3, barColor);
  }

  // Session dots
  int dotY = 125;
  int spacing = 16;
  int startX = 160 - (4 * spacing) / 2;
  for (int i = 0; i < 4; i++) {
    int x = startX + i * spacing;
    if (i < pomoSessions % 4) {
      sprite.fillCircle(x, dotY, 3, EYE_OUTER);
    } else if (i == pomoSessions % 4 && !pomoIsBreak) {
      // Current session - pulsing
      int pulse = (millis() / 300) % 2;
      sprite.fillCircle(x, dotY, 3, pulse ? EYE_MID : EYE_DIM);
    } else {
      sprite.drawCircle(x, dotY, 3, 0x4208);
    }
  }

  // Speech bubble at bottom
  String displayText = isTyping ? typingBuffer : lastResponse;
  if (displayText.length() > 0) {
    drawSpeechBubble(displayText);
  }

  // Hint
  sprite.setTextColor(0x4208);
  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);
  sprite.drawString("[L] pause  [R] stop  Hold[L] back", 4, 158);

  sprite.pushSprite(0, 0);
}

// ---- BOOT ANIMATION ----
void runBootAnimation() {
  tft.fillScreen(BG);
  delay(300);

  // Phase 1: Two dots fade in
  for (int brightness = 0; brightness < 20; brightness++) {
    uint16_t c = tft.color565(brightness * 12, brightness * 6, 0);
    tft.fillCircle(130, 85, 3 + brightness/2, c);
    tft.fillCircle(190, 85, 3 + brightness/2, c);
    delay(40);
  }
  delay(200);

  // Phase 2: Dots grow into eyes
  for (int i = 0; i < 20; i++) {
    float t = i / 20.0;
    int w = 8 + (int)(47 * t);
    int h = 8 + (int)(42 * t);
    int r = max(2, (int)(w/4));

    tft.fillScreen(BG);
    // Left eye
    tft.fillRoundRect(130 - w/2, 85 - h/2, w, h, r, EYE_OUTER);
    tft.fillRoundRect(130 - w/2 + 3, 85 - h/2 + 3, w - 6, h - 6, r - 1, EYE_MID);
    // Right eye
    tft.fillRoundRect(190 - w/2, 85 - h/2, w, h, r, EYE_OUTER);
    tft.fillRoundRect(190 - w/2 + 3, 85 - h/2 + 3, w - 6, h - 6, r - 1, EYE_MID);

    beep(300 + i * 40, 15);
    delay(30);
  }
  delay(300);

  // Phase 3: Eyes blink once
  for (int i = 10; i >= 0; i--) {
    tft.fillScreen(BG);
    int h = i * 5;
    if (h < 4) h = 2;
    int r = max(2, min(55/4, h/3));
    tft.fillRoundRect(130 - 27, 85 - h/2, 55, h, r, EYE_OUTER);
    tft.fillRoundRect(190 - 27, 85 - h/2, 55, h, r, EYE_OUTER);
    delay(20);
  }
  delay(100);
  for (int i = 0; i <= 10; i++) {
    tft.fillScreen(BG);
    int h = i * 5;
    if (h < 4) h = 2;
    int r = max(2, min(55/4, h/3));
    tft.fillRoundRect(130 - 27, 85 - h/2, 55, h, r, EYE_OUTER);
    tft.fillRoundRect(190 - 27, 85 - h/2, 55, h, r, EYE_OUTER);
    delay(20);
  }
  soundWake();
  delay(200);

  // Phase 4: WiFi connect with eyes watching
  tft.fillScreen(BG);
  // Draw static eyes
  tft.fillRoundRect(130 - 27, 85 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(130 - 24, 85 - 22, 49, 44, 11, EYE_MID);
  tft.fillRoundRect(190 - 27, 85 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(190 - 24, 85 - 22, 49, 44, 11, EYE_MID);
  // Highlights
  tft.fillRoundRect(118, 70, 14, 10, 4, EYE_WHITE);
  tft.fillRoundRect(178, 70, 14, 10, 4, EYE_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(0x4208, BG);
  tft.drawString("Connecting...", 160, 145);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
  }

  tft.fillRect(60, 138, 200, 20, BG);
  if (WiFi.isConnected()) {
    tft.setTextColor(0x07E0, BG);
    tft.drawString("Online!", 160, 145);
    soundHappy();
  } else {
    tft.setTextColor(0xF800, BG);
    tft.drawString("Offline", 160, 145);
  }
  delay(800);
}

// ---- TYPING EFFECT ----
void startTyping(String text) {
  typingBuffer = "";
  typingIdx = 0;
  isTyping = true;
  lastTypeChar = millis();
  lastResponse = text;
}

void updateTyping() {
  if (!isTyping) return;
  if (millis() - lastTypeChar > 25) {
    if (typingIdx < (int)lastResponse.length()) {
      typingBuffer += lastResponse[typingIdx];
      typingIdx++;
      lastTypeChar = millis();
      if (typingIdx % 3 == 0) soundType();
    } else {
      isTyping = false;
    }
  }
}

// ---- ASK AI (Bridge or direct Ollama) ----
String askBridge(String prompt) {
  // Try up to 2 times
  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.begin(String(BRIDGE_HOST));
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(60000);  // 60s for Blender commands
    http.setConnectTimeout(10000);

    JsonDocument doc;
    doc["prompt"] = prompt;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    String response = "";

    if (code == 200) {
      String payload = http.getString();
      JsonDocument resDoc;
      if (!deserializeJson(resDoc, payload)) {
        response = resDoc["response"].as<String>();
        response.trim();
        if (response.length() > 120) response = response.substring(0, 117) + "...";
      }
      http.end();
      return response;
    }
    http.end();

    if (attempt == 0) {
      Serial.printf("Bridge attempt 1 failed (%d), retrying...\n", code);
      delay(1000);
    } else {
      return "Bridge err:" + String(code);
    }
  }
  return "Hmm...";
}

String askOllama(String prompt) {
  if (!WiFi.isConnected()) return "No WiFi...";

  if (USE_BRIDGE) return askBridge(prompt);

  HTTPClient http;
  http.begin(String(OLLAMA_HOST) + "/api/generate");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(30000);

  String sysPrompt = "You are " AGENT_NAME ", a tiny cute AI creature living inside a small screen. "
    "You look like a desk pet robot with big glowing amber eyes. "
    "Keep ALL responses to 1-2 short sentences max. Be warm, playful, and expressive.";

  JsonDocument doc;
  doc["model"] = OLLAMA_MODEL;
  doc["prompt"] = prompt;
  doc["system"] = sysPrompt;
  doc["stream"] = false;
  doc["options"]["num_predict"] = 60;
  doc["options"]["temperature"] = 0.8;

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  String response = "Hmm...";

  if (code == 200) {
    String payload = http.getString();
    JsonDocument resDoc;
    if (!deserializeJson(resDoc, payload)) {
      response = resDoc["response"].as<String>();
      response.trim();
      if (response.length() > 120) response = response.substring(0, 117) + "...";
    }
  } else {
    response = "Err:" + String(code);
  }
  http.end();
  return response;
}

// ---- EMAIL CHECK ----
bool isEmailCheck(String msg) {
  msg.toLowerCase();
  return (msg.indexOf("check") >= 0 && msg.indexOf("email") >= 0) ||
         (msg.indexOf("check") >= 0 && msg.indexOf("mail") >= 0) ||
         msg.indexOf("inbox") >= 0 ||
         (msg.indexOf("any") >= 0 && msg.indexOf("mail") >= 0) ||
         (msg.indexOf("read") >= 0 && msg.indexOf("email") >= 0);
}

void checkEmailCommand() {
  mood = THINKING;
  startTyping("Checking mail...");
  drawFaceScreen();

  HTTPClient http;
  http.begin(String(BRIDGE_HOST) + "/check-email");
  http.setTimeout(15000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      int count = doc["count"] | 0;
      if (count == 0) {
        mood = IDLE;
        startTyping("No new emails!");
        soundPoke();
      } else {
        mood = HAPPY;
        JsonArray emails = doc["emails"].as<JsonArray>();

        // Show summary of latest emails
        String msg = String(count) + " emails. Latest: ";
        String from = emails[0]["from"] | "?";
        String subj = emails[0]["subject"] | "?";
        msg += from + " - " + subj;
        if (msg.length() > 115) msg = msg.substring(0, 112) + "...";
        startTyping(msg);
        soundHappy();

        // Print all to serial
        Serial.println("\n===== INBOX =====");
        for (int i = 0; i < count && i < 5; i++) {
          String f = emails[i]["from"] | "?";
          String s = emails[i]["subject"] | "?";
          String p = emails[i]["preview"] | "";
          Serial.println(String(i + 1) + ". " + f + " | " + s);
          if (p.length() > 0) Serial.println("   " + p);
        }
        Serial.println("=================\n");
      }
    }
  } else {
    mood = SAD;
    startTyping("Couldn't check mail: " + String(code));
  }
  http.end();
  lastInteraction = millis();
}

// ---- POMODORO DETECTION ----
bool isPomoRequest(String msg) {
  msg.toLowerCase();
  return (msg.indexOf("pomodoro") >= 0 || msg.indexOf("pomo") >= 0 ||
          msg.indexOf("focus") >= 0 || msg.indexOf("timer") >= 0 ||
          msg.indexOf("work session") >= 0);
}

bool isPomoStop(String msg) {
  msg.toLowerCase();
  return (msg.indexOf("stop") >= 0 || msg.indexOf("cancel") >= 0 ||
          msg.indexOf("end") >= 0 || msg.indexOf("quit") >= 0);
}

// ---- INTERACTION ----
void chatWithAgent(String userMessage) {
  userMessage.trim();

  // Check for email commands
  if (isEmailCheck(userMessage)) {
    checkEmailCommand();
    return;
  }

  // Check for Pomodoro commands
  if (isPomoRequest(userMessage) && !pomoActive) {
    startPomodoro();
    return;
  }
  if (isPomoStop(userMessage) && pomoActive) {
    pomoActive = false;
    currentScreen = SCREEN_FACE;
    mood = HAPPY;
    startTyping("Timer stopped! Good work!");
    soundHappy();
    return;
  }

  if (mood == SLEEPING) { mood = SURPRISED; soundWake(); drawFaceScreen(); delay(300); }
  mood = THINKING;
  soundThinking();
  drawFaceScreen();

  String response = askOllama(userMessage);
  chatHistory[chatCount % 6] = "> " + userMessage;
  chatCount++;
  chatHistory[chatCount % 6] = String(AGENT_NAME) + ": " + response;
  chatCount++;
  Serial.println(String(AGENT_NAME) + ": " + response);

  startTyping(response);

  if (response.indexOf("happy") >= 0 || response.indexOf("love") >= 0 || response.indexOf("!") >= 0) {
    mood = HAPPY; happiness = min(100, happiness + 10); soundHappy();
  } else if (response.indexOf("sad") >= 0 || response.indexOf("lonely") >= 0) {
    mood = SAD;
  } else {
    mood = TALKING;
  }
  energy = max(0, energy - 5);
  lastInteraction = millis();
}

void talkToAgent() {
  if (mood == SLEEPING) { mood = SURPRISED; soundWake(); drawFaceScreen(); delay(300); }
  mood = THINKING;
  soundThinking();
  drawFaceScreen();

  String prompt = prompts[promptIdx];
  promptIdx = (promptIdx + 1) % promptCount;
  String response = askOllama(prompt);

  chatHistory[chatCount % 6] = "> " + String(prompts[(promptIdx - 1 + promptCount) % promptCount]);
  chatCount++;
  chatHistory[chatCount % 6] = AGENT_NAME ": " + response;
  chatCount++;

  startTyping(response);

  if (response.indexOf("happy") >= 0 || response.indexOf("love") >= 0 || response.indexOf("!") >= 0) {
    mood = HAPPY; happiness = min(100, happiness + 10); soundHappy();
  } else if (response.indexOf("sad") >= 0 || response.indexOf("lonely") >= 0) {
    mood = SAD;
  } else {
    mood = TALKING;
  }
  energy = max(0, energy - 5);
  lastInteraction = millis();
}

void pokeAgent() {
  mood = SURPRISED;
  soundPoke();
  startTyping("Whoa!");
  happiness = min(100, happiness + 5);
  energy = max(0, energy - 2);
  lastInteraction = millis();
  drawFaceScreen();
  delay(600);
  mood = HAPPY;
  soundHappy();
}

// ---- EMAIL NOTIFICATIONS ----
void checkNotifications() {
  HTTPClient http;
  http.begin(String(BRIDGE_HOST) + "/notifications");
  http.setTimeout(5000);

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    Serial.println("[Notif] Got: " + String(payload.length()) + " bytes");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.println("[Notif] JSON error: " + String(err.c_str()));
    }
    if (!err) {
      int count = doc["count"] | 0;
      Serial.println("[Notif] Count: " + String(count));
      if (count > 0) {
        JsonArray notifs = doc["notifications"].as<JsonArray>();
        String from = notifs[0]["from"] | "someone";
        String subject = notifs[0]["subject"] | "New message";
        String summary = notifs[0]["summary"] | "";
        String body = notifs[0]["body"] | "";

        // Print full details to serial
        Serial.println("\n===== NEW EMAIL =====");
        Serial.println("From: " + from);
        Serial.println("Subject: " + subject);
        if (summary.length() > 0) {
          Serial.println("Summary: " + summary);
        }
        if (body.length() > 0) {
          Serial.println("--- Body ---");
          Serial.println(body);
        }
        Serial.println("====================\n");

        // React on screen — surprised!
        mood = SURPRISED;
        currentScreen = SCREEN_FACE;
        drawFaceScreen();
        beep(800, 80); delay(60);
        beep(1000, 80); delay(60);
        beep(1200, 120);
        delay(400);

        // Show who it's from
        startTyping("Mail from " + from + "!");
        mood = HAPPY;
        drawFaceScreen();
        delay(1500);

        // Now think about it and reply
        mood = THINKING;
        startTyping("Reading...");
        drawFaceScreen();

        // Ask the bridge to reply to the email
        HTTPClient replyHttp;
        replyHttp.begin(String(BRIDGE_HOST) + "/email-reply");
        replyHttp.addHeader("Content-Type", "application/json");
        replyHttp.setTimeout(60000);

        JsonDocument replyDoc;
        replyDoc["from"] = from;
        replyDoc["subject"] = subject;
        replyDoc["body"] = body;
        String replyBody;
        serializeJson(replyDoc, replyBody);

        String aiReply = "";
        int replyCode = replyHttp.POST(replyBody);
        if (replyCode == 200) {
          String replyPayload = replyHttp.getString();
          JsonDocument replyRes;
          if (!deserializeJson(replyRes, replyPayload)) {
            aiReply = replyRes["reply"].as<String>();
          }
        }
        replyHttp.end();

        if (aiReply.length() == 0) {
          aiReply = "Got a message but couldn't reply!";
        }

        // Show the AI's reply on screen
        mood = TALKING;
        if (aiReply.length() > 115) aiReply = aiReply.substring(0, 112) + "...";
        startTyping(aiReply);
        lastInteraction = millis();

        Serial.println("[Email] Pixel replied: " + aiReply);

        // Chat history
        chatHistory[chatCount % 6] = "[Email] " + from + ": " + body;
        chatCount++;
        chatHistory[chatCount % 6] = AGENT_NAME ": " + aiReply;
        chatCount++;

        // Acknowledge
        HTTPClient ackHttp;
        ackHttp.begin(String(BRIDGE_HOST) + "/notifications?ack=1");
        ackHttp.setTimeout(3000);
        ackHttp.GET();
        ackHttp.end();
      }
    }
  }
  http.end();
}

// ---- ANIMATION ----
void updateAnimations() {
  unsigned long now = millis();

  // Breathing
  breathePhase += 0.03;

  // Blinking
  if (blinking) {
    blinkFrame++;
    if (blinkFrame > 8) { blinking = false; blinkFrame = 0; }
  } else if (now - lastBlink > 3000 + random(3000)) {
    blinking = true;
    blinkFrame = 0;
    lastBlink = now;
  }

  // Random look direction
  if (now - lastLookChange > 2000 + random(3000)) {
    targetLookX = ((float)random(-100, 100)) / 100.0;
    targetLookY = ((float)random(-60, 60)) / 100.0;
    lastLookChange = now;
  }
  // Smooth lerp to target
  lookX += (targetLookX - lookX) * 0.08;
  lookY += (targetLookY - lookY) * 0.08;

  // Pomodoro timer
  updatePomodoro();

  // Check for email notifications every 15s
  static unsigned long lastNotifCheck = 0;
  if (now - lastNotifCheck > 15000 && WiFi.isConnected()) {
    lastNotifCheck = now;
    checkNotifications();
  }

  // Auto-sleep (not during pomodoro)
  if (!pomoActive && mood != SLEEPING && mood != THINKING && (now - lastInteraction > 45000)) {
    mood = SLEEPING;
    startTyping("zzz...");
    soundSleep();
  }

  // Happiness decay
  static unsigned long lastDecay = 0;
  if (now - lastDecay > 30000) {
    happiness = max(0, happiness - 1);
    if (happiness < 30 && mood != SLEEPING && mood != THINKING) mood = SAD;
    lastDecay = now;
  }

  // Energy recovery when sleeping
  static unsigned long lastRest = 0;
  if (mood == SLEEPING && now - lastRest > 5000) {
    energy = min(100, energy + 1);
    lastRest = now;
  }

  updateTyping();
}

// ---- INPUT ----
void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        Serial.println("> " + serialBuffer);
        chatWithAgent(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }
}

void handleButtons() {
  bool left = (digitalRead(BTN_LEFT) == LOW);
  bool right = (digitalRead(BTN_RIGHT) == LOW);

  // Left: long press (1s) = cycle screens
  if (left && !btnLeftWas) {
    btnLeftHoldStart = millis();
    btnLeftLongHandled = false;
  }
  if (left && !btnLeftLongHandled && (millis() - btnLeftHoldStart > 800)) {
    btnLeftLongHandled = true;
    beep(500, 30);
    if (currentScreen == SCREEN_FACE) currentScreen = SCREEN_POMO;
    else if (currentScreen == SCREEN_POMO) currentScreen = SCREEN_CHAT;
    else currentScreen = SCREEN_FACE;
  }

  // Left: short press (on release)
  if (!left && btnLeftWas && !btnLeftLongHandled) {
    if (currentScreen == SCREEN_FACE) {
      talkToAgent();
    } else if (currentScreen == SCREEN_POMO) {
      if (!pomoActive) {
        startPomodoro();
      } else {
        pomoActive = !pomoActive;
        if (pomoActive) {
          pomoLastTick = millis();
          startTyping("Let's go!");
          beep(800, 50);
        } else {
          startTyping("Paused.");
          beep(400, 80);
        }
      }
    } else {
      currentScreen = SCREEN_FACE;
    }
  }

  // Right: short press
  if (right && !btnRightWas) {
    if (currentScreen == SCREEN_FACE) {
      pokeAgent();
    } else if (currentScreen == SCREEN_POMO) {
      if (pomoActive) {
        pomoActive = false;
        currentScreen = SCREEN_FACE;
        mood = HAPPY;
        startTyping("Timer stopped!");
        soundHappy();
      } else {
        currentScreen = SCREEN_FACE;
      }
    } else {
      talkToAgent();
      currentScreen = SCREEN_FACE;
    }
  }

  btnLeftWas = left;
  btnRightWas = right;
}

void checkWiFi() {
  static unsigned long last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  if (!WiFi.isConnected()) { WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
}

// ---- SETUP ----
// ---- WEB SERVER ----
const char WEBPAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pixel</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#111;color:#fff;font-family:-apple-system,sans-serif;min-height:100vh;min-height:100dvh;display:flex;flex-direction:column;position:fixed;top:0;left:0;right:0;bottom:0}
.top{flex-shrink:0}
.eyes{display:flex;justify-content:center;gap:20px;padding:20px 0 8px}
.eye{width:50px;height:40px;background:#fb8c00;border-radius:12px;position:relative;box-shadow:0 0 15px #fb8c0066}
.eye::after{content:'';position:absolute;top:7px;left:8px;width:12px;height:8px;background:#fff;border-radius:5px}
.status{text-align:center;color:#fb8c00;font-size:12px;padding:2px}
.chat{flex:1;overflow-y:auto;padding:8px 12px;-webkit-overflow-scrolling:touch}
.msg{margin:5px 0;padding:7px 10px;border-radius:10px;max-width:85%;font-size:14px;line-height:1.3;word-wrap:break-word}
.msg.user{background:#1a3a4a;margin-left:auto;text-align:right;color:#7df}
.msg.pixel{background:#1a1a1a;color:#fb8c00}
.bottom{flex-shrink:0}
.input-bar{display:flex;gap:8px;padding:8px 12px;background:#181818;border-top:1px solid #333}
input{flex:1;background:#222;border:1px solid #444;color:#fff;padding:10px 14px;border-radius:20px;font-size:16px;outline:none;-webkit-appearance:none}
input:focus{border-color:#fb8c00}
button{background:#fb8c00;color:#000;border:none;padding:10px 18px;border-radius:20px;font-weight:bold;font-size:14px;cursor:pointer}
button:active{background:#e67c00}
.quick{display:flex;gap:6px;padding:5px 12px;overflow-x:auto}
.quick button{background:#222;color:#fb8c00;border:1px solid #444;font-size:11px;padding:5px 10px;white-space:nowrap}
</style></head><body>
<div class="top">
<div class="eyes"><div class="eye"></div><div class="eye"></div></div>
<div class="status" id="st">Connecting...</div>
<div class="quick">
<button onclick="send('check my email')">Email</button>
<button onclick="send('start pomodoro')">Pomodoro</button>
<button onclick="send('how are you?')">Chat</button>
<button onclick="send('create a cube in blender')">Blender</button>
</div>
</div>
<div class="chat" id="chat"></div>
<div class="bottom">
<div class="input-bar">
<input id="msg" placeholder="Talk to Pixel..." autocomplete="off">
<button onclick="go()">Send</button>
</div>
</div>
<script>
const chat=document.getElementById('chat'),inp=document.getElementById('msg'),st=document.getElementById('st');
function addMsg(text,cls){
const d=document.createElement('div');d.className='msg '+cls;d.textContent=text;
chat.appendChild(d);chat.scrollTop=chat.scrollHeight;
}
function send(t){inp.value=t;go()}
async function go(){
const m=inp.value.trim();if(!m)return;inp.value='';
addMsg(m,'user');
st.textContent='Thinking...';
try{
const r=await fetch('/chat',{method:'POST',headers:{'Content-Type':'text/plain'},body:m});
const t=await r.text();
addMsg(t||'...','pixel');
st.textContent='Online';
}catch(e){addMsg('Error: '+e,'system');st.textContent='Error';}
}
inp.addEventListener('keydown',e=>{if(e.key==='Enter')go()});
st.textContent='Online';
addMsg('Hi! I\'m Pixel. Talk to me!','pixel');
</script></body></html>
)rawhtml";

String pendingWebMessage = "";
bool webMessageReady = false;

void setupWebServer() {
  webServer.begin();
  Serial.println("[Web] http://" + WiFi.localIP().toString());
}

void handleWebMessages() {
  WiFiClient client = webServer.accept();
  if (!client) return;

  unsigned long timeout = millis() + 3000;
  while (!client.available() && millis() < timeout) delay(1);
  if (!client.available()) { client.stop(); return; }

  String request = client.readStringUntil('\n');
  String headers = "";
  while (client.available()) {
    String line = client.readStringUntil('\n');
    headers += line;
    if (line == "\r" || line.length() <= 1) break;
  }

  if (request.indexOf("GET / ") >= 0) {
    // Serve web page
    String page = FPSTR(WEBPAGE);
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println("Content-Length: " + String(page.length()));
    client.println();
    client.print(page);
    client.stop();
    return;
  }

  if (request.indexOf("POST /chat") >= 0) {
    // Read body
    delay(10);
    String body = "";
    while (client.available()) {
      body += (char)client.read();
    }
    body.trim();
    Serial.println("[Web] > " + body);

    // Process the message
    chatWithAgent(body);

    // Reply with whatever Pixel said
    String reply = lastResponse;
    reply.replace("\"", "'");

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println("Content-Length: " + String(reply.length()));
    client.println();
    client.print(reply);
    client.stop();
    return;
  }

  // Default 404
  client.println("HTTP/1.1 404 Not Found");
  client.println("Connection: close");
  client.println();
  client.stop();
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(BG);

  sprite.createSprite(320, 170);
  sprite.setSwapBytes(true);

  runBootAnimation();

  // Start web server
  if (WiFi.isConnected()) {
    setupWebServer();
  }

  lastInteraction = millis();
  mood = HAPPY;
  startTyping("Hi there!");
  currentScreen = SCREEN_FACE;
}

// ---- LOOP ----
void loop() {
  checkWiFi();
  handleWebMessages();
  handleSerial();
  handleButtons();
  updateAnimations();

  if (currentScreen == SCREEN_FACE) drawFaceScreen();
  else if (currentScreen == SCREEN_POMO) drawPomoScreen();
  else drawChatScreen();

  delay(25);
}
