#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <FastLED.h>
#include <math.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define WIFI_SSID "YOUR_WIFI_NAME"
  #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

// LILYGO T-Dongle-S3 pins, confirmed against the board documentation.
static constexpr int PIN_BOOT = 0;
static constexpr int PIN_BACKLIGHT = 38; // board backlight is active-low on common revisions
static constexpr int LED_DATA = 40;
static constexpr int LED_CLOCK = 39;
static constexpr uint8_t LED_BRIGHTNESS = 24;
static constexpr uint32_t PRICE_INTERVAL_MS = 60000;
static constexpr uint32_t SAVE_INTERVAL_MS = 30000;

TFT_eSPI tft;
CRGB leds[1];
Preferences prefs;

enum Mood { MOOD_HAPPY, MOOD_BULL, MOOD_BEAR, MOOD_SLEEPY, MOOD_HUNGRY };
enum Page { PAGE_PET, PAGE_MARKET, PAGE_STATS };
Page page = PAGE_PET;
Mood mood = MOOD_HAPPY;

float solUsd = NAN;
float solChange24h = NAN;
uint32_t lastPriceFetch = 0;
uint32_t lastSave = 0;
uint32_t lastFrame = 0;
uint32_t lastButtonChange = 0;
uint32_t pressStarted = 0;
bool buttonWasDown = false;
bool buttonLongHandled = false;
bool wifiAttempted = false;
uint32_t xp = 0;
uint8_t happiness = 85;
uint8_t hunger = 20; // 0 full, 100 hungry
uint16_t feedCount = 0;
uint8_t animationFrame = 0;

const uint16_t C_BG = TFT_BLACK;
const uint16_t C_GREEN = 0x07E0;
const uint16_t C_LIME = 0xAFE5;
const uint16_t C_RED = 0xF800;
const uint16_t C_YELLOW = 0xFFE0;
const uint16_t C_WHITE = 0xFFFF;
const uint16_t C_MUTED = 0x7BEF;
const uint16_t C_PURPLE = 0xA01F;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  leds[0] = CRGB(r, g, b);
  FastLED.show();
}

void updateMood() {
  if (hunger >= 75) mood = MOOD_HUNGRY;
  else if (happiness <= 20) mood = MOOD_SLEEPY;
  else if (!isnan(solChange24h) && solChange24h >= 3.0f) mood = MOOD_BULL;
  else if (!isnan(solChange24h) && solChange24h <= -3.0f) mood = MOOD_BEAR;
  else mood = MOOD_HAPPY;

  switch (mood) {
    case MOOD_BULL: setLed(0, 180, 20); break;
    case MOOD_BEAR: setLed(220, 0, 0); break;
    case MOOD_HUNGRY: setLed(255, 90, 0); break;
    case MOOD_SLEEPY: setLed(30, 30, 150); break;
    default: setLed(40, 180, 80); break;
  }
}

const char* moodName() {
  switch (mood) {
    case MOOD_BULL: return "BULLISH!";
    case MOOD_BEAR: return "BEARISH";
    case MOOD_SLEEPY: return "SLEEPY";
    case MOOD_HUNGRY: return "HUNGRY";
    default: return "CHILL";
  }
}

void loadPet() {
  prefs.begin("meme-pet", false);
  xp = prefs.getUInt("xp", 0);
  happiness = prefs.getUChar("happy", 85);
  hunger = prefs.getUChar("hunger", 20);
  feedCount = prefs.getUShort("feeds", 0);
  if (happiness > 100) happiness = 85;
  if (hunger > 100) hunger = 20;
}

void savePet() {
  prefs.putUInt("xp", xp);
  prefs.putUChar("happy", happiness);
  prefs.putUChar("hunger", hunger);
  prefs.putUShort("feeds", feedCount);
  lastSave = millis();
}

void connectWiFi() {
  if (wifiAttempted) return;
  wifiAttempted = true;
  if (String(WIFI_SSID) == "YOUR_WIFI_NAME" || String(WIFI_SSID).length() == 0) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 12000) {
    delay(150);
  }
}

bool fetchSolPrice() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  // Public market data only. For production, replace setInsecure() with a validated CA certificate.
  client.setInsecure();
  HTTPClient https;
  const char* url = "https://api.coingecko.com/api/v3/simple/price?ids=solana&vs_currencies=usd&include_24hr_change=true";
  if (!https.begin(client, url)) return false;
  https.setTimeout(6500);
  https.addHeader("Accept", "application/json");
  int status = https.GET();
  bool ok = false;
  if (status == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, https.getStream());
    if (!err && !doc["solana"]["usd"].isNull()) {
      solUsd = doc["solana"]["usd"].as<float>();
      if (!doc["solana"]["usd_24h_change"].isNull()) {
        solChange24h = doc["solana"]["usd_24h_change"].as<float>();
      }
      ok = true;
    }
  }
  https.end();
  if (ok) lastPriceFetch = millis();
  return ok;
}

void drawFrog(int cx, int cy, uint8_t frame, Mood m) {
  // Chunky original frog sprite drawn from rectangles to avoid external image assets.
  int bob = (frame % 2 == 0) ? 0 : -2;
  if (m == MOOD_SLEEPY) bob = 1;
  cy += bob;
  uint16_t body = (m == MOOD_BEAR) ? 0x4D40 : (m == MOOD_SLEEPY ? 0x5D7F : 0x27E5);
  // feet / body
  tft.fillRoundRect(cx - 27, cy + 13, 54, 24, 8, body);
  tft.fillRoundRect(cx - 36, cy + 25, 20, 9, 4, body);
  tft.fillRoundRect(cx + 16, cy + 25, 20, 9, 4, body);
  // head
  tft.fillRoundRect(cx - 31, cy - 8, 62, 31, 9, body);
  // eye stalks
  tft.fillCircle(cx - 19, cy - 9, 10, body);
  tft.fillCircle(cx + 19, cy - 9, 10, body);
  if (m == MOOD_SLEEPY) {
    tft.drawFastHLine(cx - 25, cy - 9, 12, C_WHITE);
    tft.drawFastHLine(cx + 13, cy - 9, 12, C_WHITE);
  } else {
    tft.fillCircle(cx - 19, cy - 9, 5, C_WHITE);
    tft.fillCircle(cx + 19, cy - 9, 5, C_WHITE);
    tft.fillCircle(cx - 18, cy - 8, 2, TFT_BLACK);
    tft.fillCircle(cx + 20, cy - 8, 2, TFT_BLACK);
  }
  // mouth expression
  if (m == MOOD_BEAR || m == MOOD_HUNGRY) {
    tft.drawLine(cx - 9, cy + 12, cx, cy + 8, TFT_BLACK);
    tft.drawLine(cx, cy + 8, cx + 9, cy + 12, TFT_BLACK);
  } else {
    tft.drawLine(cx - 10, cy + 8, cx, cy + 13, TFT_BLACK);
    tft.drawLine(cx, cy + 13, cx + 10, cy + 8, TFT_BLACK);
  }
  if (m == MOOD_BULL) {
    tft.fillTriangle(cx + 26, cy - 24, cx + 38, cy - 24, cx + 32, cy - 35, C_YELLOW);
    tft.fillTriangle(cx + 32, cy - 29, cx + 32, cy - 21, cx + 43, cy - 25, C_YELLOW);
  }
  if (m == MOOD_HUNGRY) {
    tft.fillCircle(cx + 38, cy + 1, 3, C_YELLOW);
  }
}

void drawHeader(const char* label) {
  tft.fillRect(0, 0, 160, 13, 0x1082);
  tft.setTextColor(C_GREEN, 0x1082);
  tft.setTextSize(1);
  tft.setCursor(5, 3);
  tft.print("MEME PET");
  tft.setTextColor(C_WHITE, 0x1082);
  int16_t x = 157 - (int16_t)strlen(label) * 6;
  tft.setCursor(x, 3);
  tft.print(label);
}

void drawPetPage() {
  tft.fillScreen(C_BG);
  drawHeader("SOL / PET");
  drawFrog(80, 38, animationFrame, mood);
  tft.setTextSize(1);
  tft.setTextColor(C_LIME, C_BG);
  tft.setCursor(4, 61);
  tft.print(moodName());
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(4, 72);
  tft.printf("XP:%lu  H:%u%%", (unsigned long)xp, happiness);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(105, 72);
  if (WiFi.status() == WL_CONNECTED) tft.print("WIFI OK");
  else tft.print("OFFLINE");
}

void drawMarketPage() {
  tft.fillScreen(C_BG);
  drawHeader("MARKET");
  tft.setTextColor(C_WHITE, C_BG);
  tft.setTextSize(2);
  tft.setCursor(5, 22);
  if (isnan(solUsd)) tft.print("SOL: --");
  else tft.printf("$%.2f", solUsd);
  tft.setTextSize(1);
  tft.setCursor(5, 47);
  tft.setTextColor(C_MUTED, C_BG);
  tft.print("SOL 24H:");
  if (isnan(solChange24h)) {
    tft.setTextColor(C_YELLOW, C_BG); tft.print(" waiting");
  } else {
    tft.setTextColor(solChange24h >= 0 ? C_GREEN : C_RED, C_BG);
    tft.printf(" %+.2f%%", solChange24h);
  }
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(5, 61); tft.printf("Mood: %s", moodName());
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(5, 73);
  if (WiFi.status() == WL_CONNECTED) tft.print("Refresh ~60s"); else tft.print("Set Wi-Fi in secrets.h");
}

void drawBar(int x, int y, int w, int h, uint8_t value, uint16_t color) {
  tft.drawRect(x, y, w, h, C_MUTED);
  int fill = (w - 2) * value / 100;
  if (fill > 0) tft.fillRect(x + 1, y + 1, fill, h - 2, color);
}

void drawStatsPage() {
  tft.fillScreen(C_BG);
  drawHeader("STATS");
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, C_BG);
  tft.setCursor(5, 20); tft.printf("LEVEL %lu", (unsigned long)(xp / 100 + 1));
  tft.setCursor(5, 32); tft.printf("XP %lu  FEEDS %u", (unsigned long)xp, feedCount);
  tft.setCursor(5, 45); tft.print("HAPPY");
  drawBar(48, 44, 105, 9, happiness, C_GREEN);
  tft.setCursor(5, 59); tft.print("HUNGRY");
  drawBar(48, 58, 105, 9, hunger, C_YELLOW);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(5, 73); tft.print("Tap: feed   Hold: page");
}

void renderPage() {
  updateMood();
  if (page == PAGE_PET) drawPetPage();
  else if (page == PAGE_MARKET) drawMarketPage();
  else drawStatsPage();
}

void feedPet() {
  hunger = (hunger > 28) ? hunger - 28 : 0;
  happiness = min(100, (int)happiness + 8);
  xp += 10;
  feedCount++;
  setLed(255, 180, 0);
  savePet();
  renderPage();
}

void handleButton() {
  bool down = digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();
  if (down != buttonWasDown && now - lastButtonChange > 30) {
    lastButtonChange = now;
    buttonWasDown = down;
    if (down) {
      pressStarted = now;
      buttonLongHandled = false;
    } else {
      uint32_t held = now - pressStarted;
      if (!buttonLongHandled && held >= 35) feedPet();
    }
  }
  if (down && !buttonLongHandled && now - pressStarted > 850) {
    buttonLongHandled = true;
    page = (Page)((page + 1) % 3);
    renderPage();
  }
}

void setup() {
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, LOW); // active-low backlight
  Serial.begin(115200);
  delay(100);

  FastLED.addLeds<APA102, LED_DATA, LED_CLOCK, BGR>(leds, 1);
  FastLED.setBrightness(LED_BRIGHTNESS);
  setLed(0, 30, 100);

  tft.init();
  tft.setRotation(1); // 160 x 80 landscape
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(C_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 22); tft.print("MEME PET");
  tft.setTextSize(1);
  tft.setCursor(8, 48); tft.print("Booting your degen...");

  loadPet();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) fetchSolPrice();
  updateMood();
  renderPage();
  lastPriceFetch = millis();
  lastFrame = millis();
  Serial.println("MemeCoin Pet ready. Short BOOT = feed, hold BOOT = switch page.");
}

void loop() {
  uint32_t now = millis();
  handleButton();

  if (now - lastFrame >= 450) {
    lastFrame = now;
    animationFrame++;
    if (page == PAGE_PET) renderPage();
  }

  if (WiFi.status() != WL_CONNECTED && String(WIFI_SSID) != "YOUR_WIFI_NAME") {
    static uint32_t lastReconnect = 0;
    if (now - lastReconnect > 30000) {
      lastReconnect = now;
      WiFi.reconnect();
    }
  }
  if (WiFi.status() == WL_CONNECTED && (lastPriceFetch == 0 || now - lastPriceFetch >= PRICE_INTERVAL_MS)) {
    if (fetchSolPrice()) {
      updateMood();
      renderPage();
    } else {
      lastPriceFetch = now; // retry on the next interval, not on every loop pass
    }
  }

  static uint32_t lastNeedsTick = 0;
  if (now - lastNeedsTick >= 60000) {
    lastNeedsTick = now;
    if (hunger < 100) hunger = min(100, (int)hunger + 2);
    if (happiness > 0) happiness--;
    updateMood();
    if (page == PAGE_STATS || page == PAGE_PET) renderPage();
  }

  if (now - lastSave >= SAVE_INTERVAL_MS) savePet();
  delay(4);
}
