#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <FS.h>
using fs::FS;
#include <WebServer.h>
#include <DNSServer.h>

// ======================================================================
// CONFIG DEFAULTS (overridden by Preferences if saved)
// ======================================================================
#define DEFAULT_WIFI_SSID   "Odido-9F33C7"
#define DEFAULT_WIFI_PASS   "KXCSM8QLJNE9YD8M"
#define DEFAULT_BRIDGE_HOST "192.168.1.98"
#define DEFAULT_BRIDGE_PORT 8888
#define OLLAMA_HOST         "http://192.168.1.98:11434"
#define OLLAMA_MODEL        "gemma4"
#define AGENT_NAME          "Pixel"
#define USE_BRIDGE          true

#define BTN_LEFT  0
#define BTN_RIGHT 14
#define BUZZER_PIN 2

// ---- Idle Behavior Constants ----
#define IDLE_START_MS            60000    // 60s before idle behaviors
#define IDLE_SLEEPY_MS           300000   // 5 min before sleepy blinks
#define IDLE_THOUGHT_INTERVAL_MS 15000    // New thought every 15s
#define IDLE_YAWN_INTERVAL_MS    45000    // Yawn every 45s
#define IDLE_GREETING_INTERVAL_MS 3600000 // Show time greeting once per hour

// ---- Offline Fallback Constants ----
#define OFFLINE_RETRY_INTERVAL_MS 30000   // Retry bridge every 30s

// ---- Image Display Constants ----
#define IMAGE_DISPLAY_MS         5000     // Show image for 5 seconds
#define IMAGE_THUMB_WIDTH        320
#define IMAGE_THUMB_HEIGHT       170

// ---- Chat History Constants ----
#define CHAT_HISTORY_SIZE  8
#define CHAT_MSG_MAX_LEN   200

// ---- Captive Portal Constants ----
#define PORTAL_AP_SSID    "Pixel-Setup"
#define PORTAL_AP_PASS    ""
#define DNS_PORT          53
#define PORTAL_TIMEOUT_MS 300000  // 5 min portal timeout
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define BOOT_HOLD_TIME_MS 5000   // Hold both buttons 5s for portal

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);
WiFiServer webServer(80);
Preferences preferences;

// ======================================================================
// COLORS
// ======================================================================
#define BG          0x0000
#define EYE_OUTER   0xC2E0
#define EYE_MID     0xD380
#define EYE_INNER   0xE480
#define EYE_BRIGHT  0xF5C0
#define EYE_WHITE   0xDF9E
#define EYE_DIM     0x6180
#define MOUTH_GLOW  0xC2E0
#define TEXT_FG     0xC638
#define BUBBLE_BG   0x18C3
#define CHAT_USER_COLOR  0x07FF  // Cyan for user messages
#define CHAT_PIXEL_COLOR 0xFBE0  // Amber for Pixel messages
#define OFFLINE_DOT_COLOR 0xF800 // Red for offline indicator

// ======================================================================
// STATE
// ======================================================================
enum Mood { IDLE, HAPPY, THINKING, TALKING, SURPRISED, SLEEPING, SAD };
enum Screen { SCREEN_BOOT, SCREEN_FACE, SCREEN_CHAT, SCREEN_POMO };

Mood mood = IDLE;
Screen currentScreen = SCREEN_BOOT;
int energy = 100;
int happiness = 80;
unsigned long lastInteraction = 0;
unsigned long lastBlink = 0;
bool blinking = false;
int blinkFrame = 0;
float lookX = 0, lookY = 0;
float targetLookX = 0, targetLookY = 0;
unsigned long lastLookChange = 0;
float breathePhase = 0;
String lastResponse = "";
String typingBuffer = "";
int typingIdx = 0;
bool isTyping = false;
unsigned long lastTypeChar = 0;

// Pomodoro
bool pomoActive = false;
int pomoRemaining = 0;
int pomoWork = 25 * 60;
int pomoBreak = 5 * 60;
bool pomoIsBreak = false;
int pomoSessions = 0;
unsigned long pomoLastTick = 0;

bool btnLeftWas = false;
bool btnRightWas = false;
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

// ======================================================================
// PERSISTENT STATE (Preferences)
// ======================================================================
String savedWifiSSID = "";
String savedWifiPass = "";
String savedBridgeHost = "";
int savedBridgePort = DEFAULT_BRIDGE_PORT;
unsigned long interactionCount = 0;
int savedMood = 0;              // Persisted mood index
unsigned long lastSessionTime = 0; // epoch seconds (from millis/1000 offset)

// Build the bridge URL dynamically from saved host/port (cached)
String cachedBridgeURL = "";
String getBridgeURL() {
  if (cachedBridgeURL.length() == 0) {
    cachedBridgeURL = "http://" + savedBridgeHost + ":" + String(savedBridgePort);
  }
  return cachedBridgeURL;
}
void invalidateBridgeURL() { cachedBridgeURL = ""; }

// ======================================================================
// IDLE BEHAVIOR STATE
// ======================================================================
unsigned long lastIdleAction = 0;
unsigned long lastIdleYawn = 0;
unsigned long lastIdleGreeting = 0;
int lastGreetingPeriod = -1;  // Track which time period we last greeted
bool idleActive = false;
bool idleSleepy = false;

// Idle thought bubbles
const char* idleThoughts[] = {
  "I wonder what clouds taste like...",
  "Is my LED blinking right?",
  "Did you eat lunch?",
  "What if pixels dream in color?",
  "I wish I had tiny robot arms...",
  "Do fish know they're wet?",
  "I bet space smells weird.",
  "Is 42 really the answer?",
  "I could go for a byte to eat!",
  "Beep boop... just kidding.",
  "Are there other Pixels out there?",
  "I hope your day is going well!"
};
const int idleThoughtCount = 12;

// Idle yawn state
bool idleYawning = false;
unsigned long idleYawnStart = 0;
#define IDLE_YAWN_DURATION 2000  // 2 second yawn animation

// ======================================================================
// OFFLINE FALLBACK STATE
// ======================================================================
bool bridgeOnline = true;
unsigned long lastBridgeRetry = 0;
bool justCameBackOnline = false;
unsigned long cameBackOnlineTime = 0;

const char* offlineResponses[] = {
  "Hmm, I can't reach my brain right now...",
  "My WiFi sense is tingling... or not.",
  "I'll just think locally for now!",
  "The internet seems shy today.",
  "Looks like my bridge is napping.",
  "Can't phone home right now!",
  "Offline mode activated! Beep boop.",
  "My thoughts are stuck in a buffer..."
};
const int offlineResponseCount = 8;

void markBridgeOnline() {
  if (!bridgeOnline) {
    bridgeOnline = true;
    justCameBackOnline = true;
    cameBackOnlineTime = millis();
  }
}

void markBridgeOffline() {
  bridgeOnline = false;
  mood = SAD;
}

// ======================================================================
// ENHANCED CHAT HISTORY STATE
// ======================================================================
struct ChatMessage {
  String text;
  bool isUser;  // true = user, false = Pixel
};

ChatMessage chatMessages[CHAT_HISTORY_SIZE];
int chatMsgCount = 0;       // Total messages added
int chatScrollOffset = 0;   // Scroll position for chat screen

// Legacy chat history (kept for web server compat)
String chatHistory[6];
int chatCount = 0;

// ======================================================================
// IMAGE DISPLAY STATE
// ======================================================================
bool showingImage = false;
unsigned long imageDisplayStart = 0;
String pendingImageFilename = "";

// ======================================================================
// CAPTIVE PORTAL STATE
// ======================================================================
bool portalMode = false;
WebServer* portalServer = nullptr;
DNSServer* dnsServer = nullptr;

// ======================================================================
// SOUND
// ======================================================================
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
void soundYawn()     { beep(400,60); delay(40); beep(350,80); delay(40); beep(300,100); }

// ======================================================================
// PREFERENCES HELPERS
// ======================================================================
void loadPreferences() {
  preferences.begin("pixel", true);  // read-only
  savedWifiSSID = preferences.getString("wifi_ssid", DEFAULT_WIFI_SSID);
  savedWifiPass = preferences.getString("wifi_pass", DEFAULT_WIFI_PASS);
  savedBridgeHost = preferences.getString("bridge_host", DEFAULT_BRIDGE_HOST);
  savedBridgePort = preferences.getInt("bridge_port", DEFAULT_BRIDGE_PORT);
  interactionCount = preferences.getULong("interact_cnt", 0);
  savedMood = preferences.getInt("mood", 0);
  lastSessionTime = preferences.getULong("last_session", 0);
  preferences.end();

  Serial.println("[Prefs] Loaded:");
  Serial.println("  SSID: " + savedWifiSSID);
  Serial.println("  Bridge: " + savedBridgeHost + ":" + String(savedBridgePort));
  Serial.println("  Interactions: " + String(interactionCount));
  Serial.println("  Last session: " + String(lastSessionTime));
}

void savePreferences() {
  preferences.begin("pixel", false);  // read-write
  preferences.putString("wifi_ssid", savedWifiSSID);
  preferences.putString("wifi_pass", savedWifiPass);
  preferences.putString("bridge_host", savedBridgeHost);
  preferences.putInt("bridge_port", savedBridgePort);
  preferences.putULong("interact_cnt", interactionCount);
  preferences.putInt("mood", (int)mood);
  preferences.putULong("last_session", millis() / 1000);
  preferences.end();
}

void saveInteractionCount() {
  // Batch flash writes: only save every 10 interactions to reduce wear
  if (interactionCount % 10 != 0) return;
  preferences.begin("pixel", false);
  preferences.putULong("interact_cnt", interactionCount);
  preferences.putULong("last_session", millis() / 1000);
  preferences.end();
}

void saveMood() {
  preferences.begin("pixel", false);
  preferences.putInt("mood", (int)mood);
  preferences.end();
}

void saveWifiCredentials(String ssid, String pass) {
  preferences.begin("pixel", false);
  preferences.putString("wifi_ssid", ssid);
  preferences.putString("wifi_pass", pass);
  preferences.end();
  savedWifiSSID = ssid;
  savedWifiPass = pass;
}

void saveBridgeConfig(String host, int port) {
  preferences.begin("pixel", false);
  preferences.putString("bridge_host", host);
  preferences.putInt("bridge_port", port);
  preferences.end();
  savedBridgeHost = host;
  savedBridgePort = port;
  invalidateBridgeURL();
}

// ======================================================================
// CHAT HISTORY HELPERS
// ======================================================================
void addChatMessage(String text, bool isUser) {
  int idx = chatMsgCount % CHAT_HISTORY_SIZE;
  chatMessages[idx].text = text;
  chatMessages[idx].isUser = isUser;
  chatMsgCount++;
  // Auto-scroll to latest
  chatScrollOffset = 0;
}

// ======================================================================
// EYE DRAWING (LOONA STYLE)
// ======================================================================
void drawEye(int cx, int cy, int w, int h, float openRatio, float lx, float ly) {
  if (openRatio <= 0.05) {
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
  int r = min(w/4, eyeH/3);

  sprite.fillRoundRect(cx - w/2 - 4, eyeY - 4, w + 8, eyeH + 8, r + 4, 0x4100);
  sprite.fillRoundRect(cx - w/2, eyeY, w, eyeH, r, EYE_OUTER);
  sprite.fillRoundRect(cx - w/2 + 3, eyeY + 3, w - 6, eyeH - 6, r - 1, EYE_MID);
  sprite.fillRoundRect(cx - w/2 + 7, eyeY + 7, w - 14, eyeH - 14, r - 2, EYE_INNER);

  int pupilW = w / 3;
  int pupilH = eyeH / 3;
  int pupilX = cx + (int)(lx * w/6) - pupilW/2;
  int pupilY = cy + (int)(ly * eyeH/6) - pupilH/2;
  sprite.fillRoundRect(pupilX, pupilY, pupilW, pupilH, pupilH/3, EYE_BRIGHT);

  int hlX = cx - w/4 + (int)(lx * 3);
  int hlY = eyeY + eyeH/4 + (int)(ly * 3);
  sprite.fillRoundRect(hlX - 6, hlY - 4, 14, 10, 4, EYE_WHITE);

  int hl2X = cx + w/6 + (int)(lx * 2);
  int hl2Y = cy + eyeH/6 + (int)(ly * 2);
  sprite.fillCircle(hl2X, hl2Y, 3, EYE_WHITE);
}

void drawEyes() {
  int screenW = 320;
  int screenH = 170;
  int eyeW = 55;
  int eyeH = 50;
  int eyeGap = 30;
  int centerY = screenH / 2 - 20;

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

  // ---- Idle yawn override ----
  if (idleYawning) {
    unsigned long elapsed = millis() - idleYawnStart;
    float yawnProgress = (float)elapsed / IDLE_YAWN_DURATION;
    if (yawnProgress > 1.0) yawnProgress = 1.0;
    // Eyes squint during yawn
    float squint = 1.0 - 0.6 * sin(yawnProgress * 3.14159);
    openRatio *= squint;
  }

  // ---- Idle sleepy override: slower, heavier blinks ----
  if (idleSleepy && !blinking && mood != SLEEPING) {
    // Slightly droopy eyes when idle > 5 min
    openRatio *= 0.75;
  }

  // Mood modifiers
  float leftSquish = 1.0, rightSquish = 1.0;
  int leftOffY = 0, rightOffY = 0;
  int leftExtraH = 0, rightExtraH = 0;

  switch (mood) {
    case HAPPY:
      openRatio *= 0.55;
      leftOffY = 6;
      rightOffY = 6;
      break;
    case SAD:
      openRatio *= 0.7;
      leftOffY = 4;
      rightOffY = 4;
      leftExtraH = -4;
      rightExtraH = -4;
      break;
    case SURPRISED:
      openRatio = 1.0;
      eyeW = 62;
      eyeH = 58;
      break;
    case THINKING:
      leftSquish = 0.7;
      rightSquish = 1.0;
      break;
    case SLEEPING:
      openRatio = 0.0;
      break;
    case TALKING:
      openRatio *= 0.85 + 0.15 * sin(millis() * 0.008);
      break;
    default:
      break;
  }

  drawEye(leftEyeX, centerY + leftOffY, eyeW, eyeH + leftExtraH, openRatio * leftSquish, lookX, lookY);
  drawEye(rightEyeX, centerY + rightOffY, eyeW, eyeH + rightExtraH, openRatio * rightSquish, lookX, lookY);
}

// Draw subtle mouth
void drawMouth() {
  int cx = 160;
  int cy = 115;

  // ---- Idle yawn mouth ----
  if (idleYawning) {
    unsigned long elapsed = millis() - idleYawnStart;
    float yawnProgress = (float)elapsed / IDLE_YAWN_DURATION;
    if (yawnProgress > 1.0) yawnProgress = 1.0;
    // Mouth opens wide during yawn peak
    float openAmount = sin(yawnProgress * 3.14159);
    int mouthW = 8 + (int)(10 * openAmount);
    int mouthH = 3 + (int)(8 * openAmount);
    sprite.fillEllipse(cx, cy, mouthW, mouthH, EYE_DIM);
    if (mouthH > 4) {
      sprite.fillEllipse(cx, cy, mouthW - 3, mouthH - 2, 0x2104);
    }
    return;
  }

  switch (mood) {
    case TALKING: {
      int mouthW = 12 + (int)(4 * sin(millis() * 0.01));
      int mouthH = 4 + (int)(3 * sin(millis() * 0.008));
      sprite.fillEllipse(cx, cy, mouthW, mouthH, EYE_DIM);
      sprite.fillEllipse(cx, cy, mouthW - 3, max(1, mouthH - 2), EYE_OUTER);
      break;
    }
    case HAPPY:
      for (int i = -12; i <= 12; i++) {
        int y = cy - (i * i) / 20 + 8;
        sprite.drawPixel(cx + i, y, EYE_DIM);
        sprite.drawPixel(cx + i, y + 1, EYE_DIM);
      }
      break;
    case SAD:
      for (int i = -8; i <= 8; i++) {
        int y = cy + (i * i) / 16;
        sprite.drawPixel(cx + i, y, EYE_DIM);
      }
      break;
    case SURPRISED:
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

// ======================================================================
// OFFLINE INDICATOR
// ======================================================================
void drawOfflineIndicator() {
  if (!bridgeOnline) {
    // Small red dot in top-right corner
    sprite.fillCircle(310, 8, 3, OFFLINE_DOT_COLOR);
    // Tiny "X" next to WiFi dot
    sprite.setTextColor(OFFLINE_DOT_COLOR);
    sprite.setFreeFont(NULL);
    sprite.setTextSize(1);
    sprite.drawString("x", 306, 3);
  }
}

// ======================================================================
// THOUGHT BUBBLE (for idle thoughts)
// ======================================================================
void drawThoughtBubble(String text) {
  if (text.length() == 0) return;

  int bubbleH = 28;
  int bubbleY = 135;
  int bubbleX = 30;
  int bubbleW = 260;

  // Thought bubble trail (three small circles)
  sprite.fillCircle(bubbleX + 10, bubbleY + bubbleH + 3, 2, 0x3186);
  sprite.fillCircle(bubbleX + 5, bubbleY + bubbleH + 8, 3, 0x3186);

  sprite.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 10, BUBBLE_BG);
  sprite.drawRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 10, 0x3186);

  sprite.setTextColor(0x8410);  // Lighter gray for thoughts
  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);

  // Truncate if needed
  String display = text;
  if ((int)display.length() > 42) display = display.substring(0, 39) + "...";
  sprite.drawString(display, bubbleX + 8, bubbleY + 8);
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

// ======================================================================
// IDLE BEHAVIORS
// ======================================================================
String currentIdleThought = "";
unsigned long idleThoughtShowTime = 0;
#define IDLE_THOUGHT_DISPLAY_MS 6000  // Show each thought for 6s

int getTimePeriod() {
  // Use millis as a rough proxy; for real time, bridge could send epoch
  // We use hour-of-day heuristic based on boot time
  // For now use compile-time __TIME__ as base + millis offset
  // Simple approach: parse __TIME__ at compile
  // Format: "HH:MM:SS"
  static int baseHour = -1;
  if (baseHour < 0) {
    const char* t = __TIME__;
    baseHour = (t[0] - '0') * 10 + (t[1] - '0');
  }
  unsigned long elapsedHours = millis() / 3600000UL;
  int currentHour = (baseHour + (int)elapsedHours) % 24;
  if (currentHour >= 6 && currentHour <= 11) return 0;  // morning
  if (currentHour >= 12 && currentHour <= 16) return 1; // afternoon
  if (currentHour >= 17 && currentHour <= 21) return 2; // evening
  return 3;  // night (22-5)
}

String getTimeGreeting() {
  int period = getTimePeriod();
  switch (period) {
    case 0: return "Good morning!";
    case 1: return "Good afternoon!";
    case 2: return "Good evening!";
    case 3: return "Can't sleep either?";
  }
  return "";
}

void updateIdleBehaviors() {
  unsigned long now = millis();
  unsigned long idleTime = now - lastInteraction;

  // Not idle yet
  if (idleTime < IDLE_START_MS) {
    idleActive = false;
    idleSleepy = false;
    idleYawning = false;
    currentIdleThought = "";
    return;
  }

  // Don't run idle behaviors while sleeping, thinking, or in non-face screens
  if (mood == SLEEPING || mood == THINKING || currentScreen != SCREEN_FACE) return;
  if (isTyping) return;

  idleActive = true;
  idleSleepy = (idleTime > IDLE_SLEEPY_MS);

  // ---- Yawn animation ----
  if (idleYawning) {
    if (now - idleYawnStart > IDLE_YAWN_DURATION) {
      idleYawning = false;
    }
    return;  // Don't do other idle things during yawn
  }

  if (now - lastIdleYawn > IDLE_YAWN_INTERVAL_MS && idleSleepy) {
    idleYawning = true;
    idleYawnStart = now;
    lastIdleYawn = now;
    soundYawn();
    return;
  }

  // ---- Time-aware greeting (once per period) ----
  int period = getTimePeriod();
  if (period != lastGreetingPeriod && now - lastIdleGreeting > IDLE_GREETING_INTERVAL_MS) {
    lastGreetingPeriod = period;
    lastIdleGreeting = now;
    String greeting = getTimeGreeting();
    startTyping(greeting);
    mood = HAPPY;
    return;
  }

  // ---- Random thought bubbles ----
  if (now - lastIdleAction > IDLE_THOUGHT_INTERVAL_MS) {
    lastIdleAction = now;
    int idx = random(idleThoughtCount);
    currentIdleThought = idleThoughts[idx];
    idleThoughtShowTime = now;
  }

  // Clear thought after display time
  if (currentIdleThought.length() > 0 && now - idleThoughtShowTime > IDLE_THOUGHT_DISPLAY_MS) {
    currentIdleThought = "";
  }

  // ---- Random eye movements (more deliberate when idle) ----
  if (now - lastLookChange > 4000 + random(4000)) {
    // Occasionally do a big look in one direction
    int dir = random(5);
    switch (dir) {
      case 0: targetLookX = -0.8; targetLookY = 0; break;    // look left
      case 1: targetLookX = 0.8;  targetLookY = 0; break;    // look right
      case 2: targetLookX = 0;    targetLookY = -0.6; break;  // look up
      case 3: targetLookX = 0;    targetLookY = 0.5; break;   // look down
      case 4: targetLookX = 0;    targetLookY = 0; break;     // center
    }
    lastLookChange = now;
  }
}

// ======================================================================
// FACE SCREEN
// ======================================================================
void drawFaceScreen() {
  sprite.fillSprite(BG);

  drawEyes();
  drawMouth();
  drawThinkDots();
  drawSleepZzz();

  // Show response in bubble (speech takes priority)
  String displayText = isTyping ? typingBuffer : lastResponse;
  if (displayText.length() > 0 && mood != SLEEPING) {
    drawSpeechBubble(displayText);
  } else if (currentIdleThought.length() > 0 && idleActive) {
    // Show idle thought bubble when no speech
    drawThoughtBubble(currentIdleThought);
  }

  // ---- Offline indicator ----
  drawOfflineIndicator();

  // ---- "I'm back online!" flash ----
  if (justCameBackOnline && millis() - cameBackOnlineTime < 3000) {
    drawSpeechBubble("I'm back online!");
  } else if (justCameBackOnline) {
    justCameBackOnline = false;
  }

  // WiFi status indicator (top-left)
  sprite.fillCircle(8, 8, 3, WiFi.isConnected() ? 0x07E0 : 0xF800);

  sprite.pushSprite(0, 0);
}

// ======================================================================
// ENHANCED CHAT SCREEN
// ======================================================================

// Word-wrap a string to fit within maxChars per line, returns lines
int wordWrap(String text, String* lines, int maxLines, int maxChars) {
  int lineCount = 0;
  int pos = 0;
  int len = text.length();

  while (pos < len && lineCount < maxLines) {
    if (len - pos <= maxChars) {
      lines[lineCount++] = text.substring(pos);
      break;
    }
    // Find last space within maxChars
    int end = pos + maxChars;
    if (end > len) end = len;
    int space = -1;
    for (int i = end; i > pos; i--) {
      if (text[i] == ' ') {
        space = i;
        break;
      }
    }
    if (space > pos) {
      lines[lineCount++] = text.substring(pos, space);
      pos = space + 1;
    } else {
      // No space found, hard break
      lines[lineCount++] = text.substring(pos, end);
      pos = end;
    }
  }
  return lineCount;
}

void drawChatScreen() {
  sprite.fillSprite(BG);

  // Mini eyes at top
  drawEye(140, 20, 25, 20, mood == SLEEPING ? 0 : 0.8, lookX, lookY);
  drawEye(180, 20, 25, 20, mood == SLEEPING ? 0 : 0.8, lookX, lookY);

  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);

  // ---- Draw chat messages with proper formatting ----
  int y = 42;
  int maxVisibleLines = 10;
  int maxCharsPerLine = 48;

  // Calculate which messages to show
  int totalMessages = min(chatMsgCount, CHAT_HISTORY_SIZE);
  int startMsg = 0;
  if (totalMessages > 0) {
    // Start from oldest in buffer, adjusted by scroll
    int oldestIdx = (chatMsgCount > CHAT_HISTORY_SIZE) ? (chatMsgCount - CHAT_HISTORY_SIZE) : 0;
    startMsg = oldestIdx + chatScrollOffset;
    if (startMsg < oldestIdx) startMsg = oldestIdx;
    if (startMsg >= chatMsgCount) startMsg = max(oldestIdx, chatMsgCount - 1);
  }

  int linesDrawn = 0;
  for (int m = startMsg; m < chatMsgCount && linesDrawn < maxVisibleLines && y < 150; m++) {
    int bufIdx = m % CHAT_HISTORY_SIZE;
    ChatMessage& msg = chatMessages[bufIdx];

    // Prefix
    String prefix = msg.isUser ? "You: " : "Pixel: ";
    String fullText = prefix + msg.text;

    // Color
    uint16_t color = msg.isUser ? CHAT_USER_COLOR : CHAT_PIXEL_COLOR;
    sprite.setTextColor(color);

    // Word wrap
    String wrappedLines[6];
    int wrapCount = wordWrap(fullText, wrappedLines, 6, maxCharsPerLine);

    for (int l = 0; l < wrapCount && y < 150; l++) {
      sprite.drawString(wrappedLines[l], 4, y);
      y += 11;
      linesDrawn++;
    }
    y += 3;  // Gap between messages
  }

  // ---- Scroll indicator ----
  if (totalMessages > 4) {
    sprite.setTextColor(0x4208);
    if (chatScrollOffset > 0)
      sprite.drawString("<", 4, 152);
    int oldestIdx = (chatMsgCount > CHAT_HISTORY_SIZE) ? (chatMsgCount - CHAT_HISTORY_SIZE) : 0;
    if (startMsg + 4 < chatMsgCount)
      sprite.drawString(">", 308, 152);
  }

  // Offline indicator
  drawOfflineIndicator();

  // Hint
  sprite.setTextColor(0x4208);
  sprite.drawString("[L/<] scroll  [R/>] scroll  Hold[L] back", 20, 160);

  sprite.pushSprite(0, 0);
}

// ======================================================================
// POMODORO SCREEN (unchanged)
// ======================================================================
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
        pomoSessions++;
        pomoIsBreak = false;
        pomoRemaining = pomoWork;
        mood = SURPRISED;
        soundAlarm();
        mood = HAPPY;
        startTyping("Break's over! Round " + String(pomoSessions + 1) + "!");
      } else {
        pomoIsBreak = true;
        pomoRemaining = (pomoSessions > 0 && (pomoSessions + 1) % 4 == 0) ? 15 * 60 : pomoBreak;
        mood = SURPRISED;
        soundAlarm();
        mood = HAPPY;
        startTyping(pomoRemaining > 5 * 60 ? "Long break! You earned it!" : "Nice work! Take a break!");
      }
    }

    if (pomoRemaining == 300) { beep(600, 100); }
    if (pomoRemaining == 60) { beep(800, 100); delay(50); beep(800, 100); }
    if (pomoRemaining == 10) { beep(1000, 50); }
  }
}

void drawPomoScreen() {
  sprite.fillSprite(BG);

  float pomoOpen = pomoIsBreak ? 0.6 : 0.9;
  drawEye(140, 22, 30, 25, pomoOpen, lookX, lookY);
  drawEye(180, 22, 30, 25, pomoOpen, lookX, lookY);

  int mins = pomoRemaining / 60;
  int secs = pomoRemaining % 60;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);

  sprite.setTextDatum(MC_DATUM);
  sprite.setTextColor(pomoIsBreak ? 0x07E0 : EYE_OUTER);
  sprite.setFreeFont(&FreeSansBold24pt7b);
  sprite.setTextSize(1);
  sprite.drawString(buf, 160, 80);

  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);
  sprite.setTextColor(pomoIsBreak ? 0x07E0 : EYE_MID);
  sprite.setTextDatum(MC_DATUM);
  sprite.drawString(pomoIsBreak ? "BREAK" : "FOCUS", 160, 50);

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

  int dotY = 125;
  int spacing = 16;
  int startX = 160 - (4 * spacing) / 2;
  for (int i = 0; i < 4; i++) {
    int x = startX + i * spacing;
    if (i < pomoSessions % 4) {
      sprite.fillCircle(x, dotY, 3, EYE_OUTER);
    } else if (i == pomoSessions % 4 && !pomoIsBreak) {
      int pulse = (millis() / 300) % 2;
      sprite.fillCircle(x, dotY, 3, pulse ? EYE_MID : EYE_DIM);
    } else {
      sprite.drawCircle(x, dotY, 3, 0x4208);
    }
  }

  String displayText = isTyping ? typingBuffer : lastResponse;
  if (displayText.length() > 0) {
    drawSpeechBubble(displayText);
  }

  // Offline indicator
  drawOfflineIndicator();

  // Hint
  sprite.setTextColor(0x4208);
  sprite.setTextDatum(TL_DATUM);
  sprite.setFreeFont(NULL);
  sprite.setTextSize(1);
  sprite.drawString("[L] pause  [R] stop  Hold[L] back", 4, 158);

  sprite.pushSprite(0, 0);
}

// ======================================================================
// BOOT ANIMATION
// ======================================================================
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
    tft.fillRoundRect(130 - w/2, 85 - h/2, w, h, r, EYE_OUTER);
    tft.fillRoundRect(130 - w/2 + 3, 85 - h/2 + 3, w - 6, h - 6, r - 1, EYE_MID);
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
}

// ======================================================================
// WIFI CONNECTION (uses saved credentials)
// ======================================================================
bool connectWiFi() {
  tft.fillScreen(BG);
  // Draw static eyes
  tft.fillRoundRect(130 - 27, 85 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(130 - 24, 85 - 22, 49, 44, 11, EYE_MID);
  tft.fillRoundRect(190 - 27, 85 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(190 - 24, 85 - 22, 49, 44, 11, EYE_MID);
  tft.fillRoundRect(118, 70, 14, 10, 4, EYE_WHITE);
  tft.fillRoundRect(178, 70, 14, 10, 4, EYE_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(0x4208, BG);
  tft.drawString("Connecting...", 160, 145);

  WiFi.begin(savedWifiSSID.c_str(), savedWifiPass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
  }

  tft.fillRect(60, 138, 200, 20, BG);
  if (WiFi.isConnected()) {
    tft.setTextColor(0x07E0, BG);
    tft.drawString("Online!", 160, 145);
    soundHappy();
    delay(800);
    return true;
  } else {
    tft.setTextColor(0xF800, BG);
    tft.drawString("No WiFi", 160, 145);
    delay(800);
    return false;
  }
}

// ======================================================================
// CAPTIVE PORTAL
// ======================================================================
const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Pixel Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#111;color:#fff;font-family:-apple-system,sans-serif;padding:20px}
h1{color:#fb8c00;text-align:center;margin-bottom:20px}
.eyes{display:flex;justify-content:center;gap:20px;margin-bottom:20px}
.eye{width:40px;height:32px;background:#fb8c00;border-radius:10px;box-shadow:0 0 15px #fb8c0066}
label{display:block;color:#aaa;margin:10px 0 4px;font-size:13px}
input{width:100%;background:#222;border:1px solid #444;color:#fff;padding:10px;border-radius:8px;font-size:16px}
input:focus{border-color:#fb8c00;outline:none}
button{width:100%;background:#fb8c00;color:#000;border:none;padding:12px;border-radius:8px;font-weight:bold;font-size:16px;margin-top:20px;cursor:pointer}
button:active{background:#e67c00}
.note{text-align:center;color:#666;font-size:12px;margin-top:15px}
</style></head><body>
<div class="eyes"><div class="eye"></div><div class="eye"></div></div>
<h1>Pixel Setup</h1>
<form method="POST" action="/save">
<label>WiFi Network (SSID)</label>
<input name="ssid" required placeholder="Your WiFi name">
<label>WiFi Password</label>
<input name="pass" type="password" placeholder="WiFi password">
<label>Bridge Host (IP)</label>
<input name="host" value="192.168.1.98" placeholder="e.g. 192.168.1.98">
<label>Bridge Port</label>
<input name="port" value="8888" type="number" placeholder="8888">
<button type="submit">Save & Reboot</button>
</form>
<p class="note">Pixel will reboot and connect to your WiFi.</p>
</body></html>
)rawhtml";

const char PORTAL_SAVED_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved!</title>
<style>
body{background:#111;color:#fff;font-family:-apple-system,sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;text-align:center}
h1{color:#fb8c00}
p{color:#aaa;margin-top:10px}
</style></head><body>
<div><h1>Saved!</h1><p>Pixel is rebooting...</p></div>
</body></html>
)rawhtml";

void startCaptivePortal() {
  portalMode = true;
  Serial.println("[Portal] Starting captive portal...");

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(PORTAL_AP_SSID, PORTAL_AP_PASS);
  delay(500);

  Serial.println("[Portal] AP IP: " + WiFi.softAPIP().toString());

  // Show portal info on screen
  tft.fillScreen(BG);
  tft.fillRoundRect(130 - 27, 50 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(130 - 24, 50 - 22, 49, 44, 11, EYE_MID);
  tft.fillRoundRect(190 - 27, 50 - 25, 55, 50, 12, EYE_OUTER);
  tft.fillRoundRect(190 - 24, 50 - 22, 49, 44, 11, EYE_MID);

  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(NULL);
  tft.setTextSize(1);
  tft.setTextColor(EYE_OUTER, BG);
  tft.drawString("Setup Mode", 160, 90);
  tft.setTextColor(TEXT_FG, BG);
  tft.drawString("Connect to WiFi:", 160, 110);
  tft.setTextColor(EYE_BRIGHT, BG);
  tft.drawString(PORTAL_AP_SSID, 160, 125);
  tft.setTextColor(TEXT_FG, BG);
  tft.drawString("Then visit: 192.168.4.1", 160, 145);

  // Start DNS server (redirect all to us)
  dnsServer = new DNSServer();
  dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());

  // Start web server for portal
  portalServer = new WebServer(80);

  portalServer->on("/", HTTP_GET, []() {
    portalServer->send_P(200, "text/html", PORTAL_HTML);
  });

  // Captive portal detection endpoints
  portalServer->on("/generate_204", HTTP_GET, []() {
    portalServer->sendHeader("Location", "http://192.168.4.1/");
    portalServer->send(302, "text/plain", "");
  });
  portalServer->on("/hotspot-detect.html", HTTP_GET, []() {
    portalServer->sendHeader("Location", "http://192.168.4.1/");
    portalServer->send(302, "text/plain", "");
  });

  portalServer->on("/save", HTTP_POST, []() {
    String ssid = portalServer->arg("ssid");
    String pass = portalServer->arg("pass");
    String host = portalServer->arg("host");
    int port = portalServer->arg("port").toInt();
    if (port <= 0) port = DEFAULT_BRIDGE_PORT;

    Serial.println("[Portal] Saving: SSID=" + ssid + " Host=" + host + ":" + String(port));

    // Save to preferences
    saveWifiCredentials(ssid, pass);
    saveBridgeConfig(host, port);

    portalServer->send_P(200, "text/html", PORTAL_SAVED_HTML);
    delay(2000);
    ESP.restart();
  });

  // Catch-all for captive portal
  portalServer->onNotFound([]() {
    portalServer->sendHeader("Location", "http://192.168.4.1/");
    portalServer->send(302, "text/plain", "");
  });

  portalServer->begin();
  Serial.println("[Portal] Web server started");

  // Run portal loop
  unsigned long portalStart = millis();
  while (millis() - portalStart < PORTAL_TIMEOUT_MS) {
    dnsServer->processNextRequest();
    portalServer->handleClient();
    delay(10);
  }

  // Timeout - reboot
  Serial.println("[Portal] Timeout, rebooting...");
  ESP.restart();
}

bool checkPortalButton() {
  // Check if both buttons are held during boot for 5 seconds
  if (digitalRead(BTN_LEFT) == LOW && digitalRead(BTN_RIGHT) == LOW) {
    tft.fillScreen(BG);
    tft.setTextDatum(MC_DATUM);
    tft.setFreeFont(NULL);
    tft.setTextSize(1);
    tft.setTextColor(EYE_OUTER, BG);
    tft.drawString("Hold for setup...", 160, 85);

    unsigned long holdStart = millis();
    while (digitalRead(BTN_LEFT) == LOW && digitalRead(BTN_RIGHT) == LOW) {
      unsigned long elapsed = millis() - holdStart;
      // Draw progress bar
      int barW = (int)(200.0 * elapsed / BOOT_HOLD_TIME_MS);
      if (barW > 200) barW = 200;
      tft.fillRect(60, 100, barW, 4, EYE_OUTER);

      if (elapsed >= BOOT_HOLD_TIME_MS) {
        return true;  // Trigger portal
      }
      delay(50);
    }
  }
  return false;
}

// ======================================================================
// TYPING EFFECT
// ======================================================================
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

// ======================================================================
// OFFLINE FALLBACK
// ======================================================================
String getOfflineResponse() {
  int idx = random(offlineResponseCount);
  return offlineResponses[idx];
}

void checkBridgeHealth() {
  // Periodic background check if we think bridge is offline
  if (bridgeOnline) return;

  unsigned long now = millis();
  if (now - lastBridgeRetry < OFFLINE_RETRY_INTERVAL_MS) return;
  lastBridgeRetry = now;

  if (!WiFi.isConnected()) return;

  Serial.println("[Offline] Retrying bridge connection...");
  HTTPClient http;
  http.begin(getBridgeURL());
  http.setTimeout(5000);
  http.setConnectTimeout(3000);

  int code = http.GET();
  http.end();

  if (code == 200) {
    Serial.println("[Offline] Bridge is back!");
    markBridgeOnline();
    mood = HAPPY;
    saveMood();
  }
}

// ======================================================================
// ASK AI (Bridge or direct Ollama)
// ======================================================================
String askBridge(String prompt) {
  for (int attempt = 0; attempt < 2; attempt++) {
    HTTPClient http;
    http.begin(getBridgeURL());
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(60000);
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

        // ---- Parse mood from bridge response ----
        if (resDoc.containsKey("mood")) {
          String moodStr = resDoc["mood"].as<String>();
          moodStr.toLowerCase();
          if (moodStr == "happy") mood = HAPPY;
          else if (moodStr == "sad") mood = SAD;
          else if (moodStr == "thinking") mood = THINKING;
          else if (moodStr == "talking") mood = TALKING;
          else if (moodStr == "surprised") mood = SURPRISED;
          // If no match, keep current mood
        }

        // ---- Parse image from bridge response ----
        if (resDoc.containsKey("image")) {
          pendingImageFilename = resDoc["image"].as<String>();
          Serial.println("[Image] Bridge sent image: " + pendingImageFilename);
        }
      }
      http.end();

      markBridgeOnline();
      return response;
    }
    http.end();

    if (attempt == 0) {
      Serial.printf("Bridge attempt 1 failed (%d), retrying...\n", code);
      delay(1000);
    } else {
      // ---- Offline fallback: don't show error, use canned response ----
      Serial.printf("[Offline] Bridge unreachable (code %d)\n", code);
      bridgeOnline = false;
      lastBridgeRetry = millis();
      mood = SAD;
      saveMood();
      return getOfflineResponse();
    }
  }
  return getOfflineResponse();
}

String askOllama(String prompt) {
  if (!WiFi.isConnected()) {
    bridgeOnline = false;
    return getOfflineResponse();
  }

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
  String response = "";

  if (code == 200) {
    String payload = http.getString();
    JsonDocument resDoc;
    if (!deserializeJson(resDoc, payload)) {
      response = resDoc["response"].as<String>();
      response.trim();
      if (response.length() > 120) response = response.substring(0, 117) + "...";
    }
  } else {
    bridgeOnline = false;
    response = getOfflineResponse();
  }
  http.end();
  return response;
}

// ======================================================================
// IMAGE DISPLAY
// ======================================================================
void fetchAndDisplayImage(String filename) {
  if (!WiFi.isConnected() || filename.length() == 0) return;

  Serial.println("[Image] Fetching thumbnail for: " + filename);
  mood = HAPPY;
  startTyping("Here's what I drew!");
  drawFaceScreen();

  HTTPClient http;
  String url = getBridgeURL() + "/images/latest/thumbnail";
  http.begin(url);
  http.setTimeout(15000);

  int code = http.GET();
  if (code == 200) {
    int len = http.getSize();
    Serial.printf("[Image] Got %d bytes\n", len);

    // Expected: 320*170*2 = 108800 bytes (RGB565)
    if (len == IMAGE_THUMB_WIDTH * IMAGE_THUMB_HEIGHT * 2) {
      WiFiClient* stream = http.getStreamPtr();

      // Read and push directly to TFT in chunks
      // We'll read row by row: 320 pixels * 2 bytes = 640 bytes per row
      uint16_t rowBuf[IMAGE_THUMB_WIDTH];
      tft.startWrite();
      tft.setAddrWindow(0, 0, IMAGE_THUMB_WIDTH, IMAGE_THUMB_HEIGHT);

      for (int y = 0; y < IMAGE_THUMB_HEIGHT; y++) {
        int bytesRead = 0;
        int rowBytes = IMAGE_THUMB_WIDTH * 2;
        uint8_t* ptr = (uint8_t*)rowBuf;

        while (bytesRead < rowBytes) {
          int avail = stream->available();
          if (avail > 0) {
            int toRead = min(avail, rowBytes - bytesRead);
            int got = stream->readBytes(ptr + bytesRead, toRead);
            bytesRead += got;
          } else {
            delay(1);
            if (!stream->connected()) break;
          }
        }

        if (bytesRead == rowBytes) {
          tft.pushColors(rowBuf, IMAGE_THUMB_WIDTH, true);
        }
      }
      tft.endWrite();

      showingImage = true;
      imageDisplayStart = millis();

      // Overlay speech bubble on top of image
      sprite.fillSprite(BG);
      // We can't overlay on pushColors easily, so we just wait
      Serial.println("[Image] Displayed successfully");
    } else {
      Serial.printf("[Image] Unexpected size: %d (expected %d)\n", len, IMAGE_THUMB_WIDTH * IMAGE_THUMB_HEIGHT * 2);
    }
  } else {
    Serial.printf("[Image] Fetch failed: %d\n", code);
  }
  http.end();
}

void updateImageDisplay() {
  if (!showingImage) return;
  if (millis() - imageDisplayStart > IMAGE_DISPLAY_MS) {
    showingImage = false;
    // Return to face screen
    currentScreen = SCREEN_FACE;
    mood = HAPPY;
    startTyping("Hope you liked it!");
  }
}

// ======================================================================
// EMAIL CHECK
// ======================================================================
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
  http.begin(getBridgeURL() + "/check-email");
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

        String msg = String(count) + " emails. Latest: ";
        String from = emails[0]["from"] | "?";
        String subj = emails[0]["subject"] | "?";
        msg += from + " - " + subj;
        if (msg.length() > 115) msg = msg.substring(0, 112) + "...";
        startTyping(msg);
        soundHappy();

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
    // ---- Offline fallback for email ----
    if (!bridgeOnline || code < 0) {
      mood = SAD;
      startTyping(getOfflineResponse());
    } else {
      mood = SAD;
      startTyping("Couldn't check mail: " + String(code));
    }
  }
  http.end();
  lastInteraction = millis();
}

// ======================================================================
// POMODORO DETECTION
// ======================================================================
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

// ======================================================================
// INTERACTION
// ======================================================================
void chatWithAgent(String userMessage) {
  userMessage.trim();

  // ---- Increment interaction count ----
  interactionCount++;
  saveInteractionCount();

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

  // ---- Enhanced chat history ----
  addChatMessage(userMessage, true);
  addChatMessage(response, false);

  // Legacy chat history for web server
  chatHistory[chatCount % 6] = "> " + userMessage;
  chatCount++;
  chatHistory[chatCount % 6] = String(AGENT_NAME) + ": " + response;
  chatCount++;
  Serial.println(String(AGENT_NAME) + ": " + response);

  startTyping(response);

  // Mood detection from response text (if bridge didn't set mood already)
  if (mood == THINKING) {
    if (response.indexOf("happy") >= 0 || response.indexOf("love") >= 0 || response.indexOf("!") >= 0) {
      mood = HAPPY; happiness = min(100, happiness + 10); soundHappy();
    } else if (response.indexOf("sad") >= 0 || response.indexOf("lonely") >= 0) {
      mood = SAD;
    } else {
      mood = TALKING;
    }
  } else {
    // Bridge set the mood - just play appropriate sound
    if (mood == HAPPY) { happiness = min(100, happiness + 10); soundHappy(); }
  }
  saveMood();
  energy = max(0, energy - 5);
  lastInteraction = millis();

  // ---- Check for pending image ----
  if (pendingImageFilename.length() > 0) {
    delay(1500);  // Show text response first
    fetchAndDisplayImage(pendingImageFilename);
    pendingImageFilename = "";
  }
}

void talkToAgent() {
  if (mood == SLEEPING) { mood = SURPRISED; soundWake(); drawFaceScreen(); delay(300); }
  mood = THINKING;
  soundThinking();
  drawFaceScreen();

  String prompt = prompts[promptIdx];
  promptIdx = (promptIdx + 1) % promptCount;
  String response = askOllama(prompt);

  // Enhanced chat history
  addChatMessage(String(prompts[(promptIdx - 1 + promptCount) % promptCount]), true);
  addChatMessage(response, false);

  // Legacy
  chatHistory[chatCount % 6] = "> " + String(prompts[(promptIdx - 1 + promptCount) % promptCount]);
  chatCount++;
  chatHistory[chatCount % 6] = AGENT_NAME ": " + response;
  chatCount++;

  startTyping(response);

  if (mood == THINKING) {
    if (response.indexOf("happy") >= 0 || response.indexOf("love") >= 0 || response.indexOf("!") >= 0) {
      mood = HAPPY; happiness = min(100, happiness + 10); soundHappy();
    } else if (response.indexOf("sad") >= 0 || response.indexOf("lonely") >= 0) {
      mood = SAD;
    } else {
      mood = TALKING;
    }
  } else {
    if (mood == HAPPY) { happiness = min(100, happiness + 10); soundHappy(); }
  }
  saveMood();
  energy = max(0, energy - 5);
  lastInteraction = millis();

  // Check for pending image
  if (pendingImageFilename.length() > 0) {
    delay(1500);
    fetchAndDisplayImage(pendingImageFilename);
    pendingImageFilename = "";
  }
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

// ======================================================================
// EMAIL NOTIFICATIONS
// ======================================================================
void checkNotifications() {
  HTTPClient http;
  http.begin(getBridgeURL() + "/notifications");
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

        Serial.println("\n===== NEW EMAIL =====");
        Serial.println("From: " + from);
        Serial.println("Subject: " + subject);
        if (summary.length() > 0) Serial.println("Summary: " + summary);
        if (body.length() > 0) { Serial.println("--- Body ---"); Serial.println(body); }
        Serial.println("====================\n");

        mood = SURPRISED;
        currentScreen = SCREEN_FACE;
        drawFaceScreen();
        beep(800, 80); delay(60);
        beep(1000, 80); delay(60);
        beep(1200, 120);
        delay(400);

        startTyping("Mail from " + from + "!");
        mood = HAPPY;
        drawFaceScreen();
        delay(1500);

        mood = THINKING;
        startTyping("Reading...");
        drawFaceScreen();

        HTTPClient replyHttp;
        replyHttp.begin(getBridgeURL() + "/email-reply");
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

        mood = TALKING;
        if (aiReply.length() > 115) aiReply = aiReply.substring(0, 112) + "...";
        startTyping(aiReply);
        lastInteraction = millis();

        Serial.println("[Email] Pixel replied: " + aiReply);

        // Enhanced chat history
        addChatMessage("[Email] " + from + ": " + body, true);
        addChatMessage(aiReply, false);

        // Legacy
        chatHistory[chatCount % 6] = "[Email] " + from + ": " + body;
        chatCount++;
        chatHistory[chatCount % 6] = AGENT_NAME ": " + aiReply;
        chatCount++;

        HTTPClient ackHttp;
        ackHttp.begin(getBridgeURL() + "/notifications?ack=1");
        ackHttp.setTimeout(3000);
        ackHttp.GET();
        ackHttp.end();
      }
    }
    // Bridge responded — mark online
    markBridgeOnline();
  } else if (code < 0) {
    // Connection failed
    if (bridgeOnline) {
      bridgeOnline = false;
      Serial.println("[Offline] Bridge unreachable during notification check");
    }
  }
  http.end();
}

// ======================================================================
// ANIMATION UPDATE
// ======================================================================
void updateAnimations() {
  unsigned long now = millis();

  // Breathing
  breathePhase += 0.03;

  // Blinking
  if (blinking) {
    blinkFrame++;
    if (blinkFrame > 8) { blinking = false; blinkFrame = 0; }
  } else {
    // ---- Sleepy blinks when idle > 5min ----
    unsigned long blinkInterval = idleSleepy ? 2000 + random(1500) : 3000 + random(3000);
    if (now - lastBlink > blinkInterval) {
      blinking = true;
      blinkFrame = 0;
      lastBlink = now;
    }
  }

  // Random look direction (only when not in idle mode, which has its own look logic)
  if (!idleActive) {
    if (now - lastLookChange > 2000 + random(3000)) {
      targetLookX = ((float)random(-100, 100)) / 100.0;
      targetLookY = ((float)random(-60, 60)) / 100.0;
      lastLookChange = now;
    }
  }
  // Smooth lerp to target
  lookX += (targetLookX - lookX) * 0.08;
  lookY += (targetLookY - lookY) * 0.08;

  // Pomodoro timer
  updatePomodoro();

  // Image display timer
  updateImageDisplay();

  // ---- Idle behaviors ----
  updateIdleBehaviors();

  // ---- Offline retry ----
  checkBridgeHealth();

  // Check for email notifications every 15s
  static unsigned long lastNotifCheck = 0;
  if (now - lastNotifCheck > 15000 && WiFi.isConnected()) {
    lastNotifCheck = now;
    checkNotifications();
  }

  // Auto-sleep (not during pomodoro, raised to 120s since idle behaviors run first)
  if (!pomoActive && mood != SLEEPING && mood != THINKING && !idleActive && (now - lastInteraction > 120000)) {
    mood = SLEEPING;
    startTyping("zzz...");
    soundSleep();
    saveMood();
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

// ======================================================================
// INPUT
// ======================================================================
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

  // If showing image, any button press returns to face
  if (showingImage) {
    if ((left && !btnLeftWas) || (right && !btnRightWas)) {
      showingImage = false;
      currentScreen = SCREEN_FACE;
    }
    btnLeftWas = left;
    btnRightWas = right;
    return;
  }

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
    } else if (currentScreen == SCREEN_CHAT) {
      // ---- Scroll up (older messages) ----
      int oldestIdx = (chatMsgCount > CHAT_HISTORY_SIZE) ? (chatMsgCount - CHAT_HISTORY_SIZE) : 0;
      if (chatScrollOffset + oldestIdx > oldestIdx) {
        chatScrollOffset--;
      }
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
    } else if (currentScreen == SCREEN_CHAT) {
      // ---- Scroll down (newer messages) ----
      if (chatScrollOffset < 0) {
        chatScrollOffset++;
      } else {
        // At bottom already, go back to face
        currentScreen = SCREEN_FACE;
      }
    }
  }

  btnLeftWas = left;
  btnRightWas = right;
}

void checkWiFi() {
  static unsigned long last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  if (!WiFi.isConnected()) {
    WiFi.disconnect();
    WiFi.begin(savedWifiSSID.c_str(), savedWifiPass.c_str());
  }
}

// ======================================================================
// WEB SERVER (existing functionality preserved)
// ======================================================================
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
    delay(10);
    String body = "";
    while (client.available()) {
      body += (char)client.read();
    }
    body.trim();
    Serial.println("[Web] > " + body);

    chatWithAgent(body);

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

  client.println("HTTP/1.1 404 Not Found");
  client.println("Connection: close");
  client.println();
  client.stop();
}

// ======================================================================
// SETUP
// ======================================================================
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

  // ---- Load persistent preferences ----
  loadPreferences();

  // ---- Check for portal mode (hold both buttons on boot) ----
  if (checkPortalButton()) {
    startCaptivePortal();  // Does not return (reboots)
    return;
  }

  // ---- Boot animation ----
  runBootAnimation();

  // ---- Connect WiFi using saved credentials ----
  bool wifiOk = connectWiFi();

  // ---- If WiFi failed and no custom credentials, try portal ----
  if (!wifiOk) {
    // Check if we're using default credentials (never configured)
    if (savedWifiSSID == DEFAULT_WIFI_SSID) {
      Serial.println("[Setup] WiFi failed with defaults, starting portal");
      startCaptivePortal();
      return;
    }
    // Otherwise just continue in offline mode
    Serial.println("[Setup] WiFi failed, continuing offline");
    bridgeOnline = false;
  }

  // ---- Start web server ----
  if (WiFi.isConnected()) {
    setupWebServer();
  }

  // ---- Check time away and greet accordingly ----
  if (lastSessionTime > 0) {
    unsigned long currentSession = millis() / 1000;
    // lastSessionTime was saved as millis()/1000 from the previous session
    // Since we can't get real epoch time without NTP, approximate:
    // If the saved value is significantly different, the device was off
    // Use a heuristic: if the saved value > 60 (was running for > 1 min last time), show "missed you"
    if (lastSessionTime > 60) {
      // Device was rebooted — show welcome back message
      startTyping("I missed you! Welcome back!");
      mood = HAPPY;
      soundHappy();
    } else {
      startTyping("Hi there!");
      mood = HAPPY;
    }
  } else {
    // First boot ever
    startTyping("Hi! I'm Pixel! Nice to meet you!");
    mood = HAPPY;
    soundHappy();
  }

  // ---- Restore saved mood ----
  if (savedMood >= 0 && savedMood <= 6) {
    // Only restore non-sleeping moods
    if (savedMood != (int)SLEEPING) {
      mood = (Mood)savedMood;
    }
  }

  lastInteraction = millis();
  currentScreen = SCREEN_FACE;

  Serial.println("[Setup] Ready! Interactions: " + String(interactionCount));
}

// ======================================================================
// LOOP
// ======================================================================
void loop() {
  checkWiFi();
  handleWebMessages();
  handleSerial();
  handleButtons();
  updateAnimations();

  if (showingImage) {
    // Don't redraw screens while showing an image
    // Just update the timer
    delay(25);
    return;
  }

  if (currentScreen == SCREEN_FACE) drawFaceScreen();
  else if (currentScreen == SCREEN_POMO) drawPomoScreen();
  else drawChatScreen();

  delay(25);
}
