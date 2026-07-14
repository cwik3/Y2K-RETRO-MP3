#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "Audio.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <TJpg_Decoder.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <SpotifyArduino.h>
#include "secrets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


WiFiMulti wifiMulti;
WiFiClientSecure client;
SpotifyArduino spotify(client, clientId, clientSecret, refreshToken);

// ==========================================
// 2. PIN DEFINITIONS
// ==========================================
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_RST  8
#define TFT_CS   10
#define TFT_DC   9
#define TFT_BL   21

#define SD_CS   5
#define SD_MISO 19
#define SD_MOSI 4
#define SD_SCLK 6

#define I2S_LRCK 17
#define I2S_DOUT 18
#define I2S_BCLK 16

#define ENC_CLK 40
#define ENC_DT  41
#define ENC_SW  39

#define SW1_PIN  48
#define SW2_PIN  13
#define POT1_PIN 1
#define POT2_PIN 2

// ==========================================
// 3. GLOBAL OBJECTS & STATE
// ==========================================
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
SPIClass spiSD(HSPI);
Audio audio;

// The Audio library is NOT thread-safe. audio.loop() runs continuously on
// core 0 (audioTask below). Any other code (running on core 1, i.e. the
// main Arduino loop()) that touches the `audio` object -- connecttoFS,
// pauseResume, setVolume, isRunning, getAudioCurrentTime, etc. -- MUST take
// this mutex first, or you get corrupted internal buffer state and a
// crash/reboot (exactly the "Can't stuff any more in I2S" + Guru Meditation
// you were seeing). It's recursive because audio_eof_mp3() is called BY
// audio.loop() itself (already holding the mutex) and also needs to call
// connecttoFS().
SemaphoreHandle_t audioMutex;
#define AUDIO_LOCK()   xSemaphoreTakeRecursive(audioMutex, portMAX_DELAY)
#define AUDIO_UNLOCK() xSemaphoreGiveRecursive(audioMutex)

volatile int encoderValue = 0;
unsigned long lastUITime = 0;
unsigned long lastSpotifyCheck = 0;

String currentTrack = "Waiting...";
String currentArtist = "Spotify API";
bool spotifyIsPlaying = false;

enum AppState { STATE_MENU, STATE_SPOTIFY, STATE_SD_BROWSE, STATE_SD_PLAYING };
AppState appState = STATE_MENU;

int browseIndex = 0;
bool screenNeedsFullDraw = true; // true only when entering a state: draw background+static chrome
bool listNeedsRedraw = true;     // true whenever just the scrolling list/buttons need repainting
bool sdMounted = false;          // set from SD.begin() result in setup()

// --- Menu ---
const char* menuItems[] = { "SPOTIFY STATUS", "SD CARD PLAYER" };
const int MENU_COUNT = 2;
int menuIndex = 0;
int lastMenuEncoderValue = 0;
const int ENCODER_STEPS_PER_CLICK = 2; // KY-040 modules vary 2-4, tune if too sensitive/sluggish

// --- Encoder switch (short = select, long = back) ---
bool encSwLastState = HIGH;
unsigned long encSwPressStart = 0;
bool encSwLongTriggered = false;
const unsigned long LONG_PRESS_MS = 700;

// --- SW1/SW2 debounce ---
bool sw1LastState = HIGH;
bool sw2LastState = HIGH;
unsigned long lastButtonAction = 0;
const unsigned long BUTTON_DEBOUNCE = 250;

// --- SD player ---
String trackList[40];
int trackCount = 0;
int currentTrackIndex = 0;
bool isPlaying = true;
int smoothVol = 0; // EMA state for volume pot smoothing

// ==========================================
// 4. Y2K / CYBERSIGIL DRAW HELPERS
// ==========================================
uint16_t COL_BG_TOP, COL_BG_BOT, COL_PANEL, COL_PANEL_BORDER;
uint16_t COL_MAGENTA, COL_ACID, COL_DIM, COL_DIMMER, COL_SCANLINE;

void initCyberPalette() {
  COL_BG_TOP      = tft.color565(4, 2, 10);
  COL_BG_BOT      = tft.color565(14, 6, 24);
  COL_PANEL       = tft.color565(6, 4, 12);
  COL_PANEL_BORDER= tft.color565(255, 0, 140);
  COL_MAGENTA     = tft.color565(255, 0, 140);
  COL_ACID        = tft.color565(180, 255, 60);
  COL_DIM         = tft.color565(150, 130, 170);
  COL_DIMMER      = tft.color565(70, 55, 85);
  COL_SCANLINE    = tft.color565(0, 0, 0);
}

uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  uint8_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint8_t r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
  uint8_t r = r1 + (uint8_t)((r2 - r1) * t);
  uint8_t g = g1 + (uint8_t)((g2 - g1) * t);
  uint8_t b = b1 + (uint8_t)((b2 - b1) * t);
  return (r << 11) | (g << 5) | b;
}

void drawCyberBackground() {
  for (int y = 0; y < tft.height(); y++) {
    float t = (float)y / (tft.height() - 1);
    uint16_t rowColor = lerpColor(COL_BG_TOP, COL_BG_BOT, t);
    tft.drawFastHLine(0, y, tft.width(), rowColor);
    if (y % 3 == 0) {
      tft.drawFastHLine(0, y, tft.width(), lerpColor(rowColor, COL_SCANLINE, 0.35f));
    }
  }
  for (int x = 8; x < tft.width(); x += 16) {
    tft.drawFastVLine(x, 0, 2, COL_DIMMER);
  }
}

void drawSigilCorners(int x, int y, int w, int h, uint16_t color) {
  const int t = 4;
  tft.drawFastHLine(x - 2, y, t, color);
  tft.drawFastVLine(x, y - 2, t, color);
  tft.drawFastHLine(x + w - t + 2, y, t, color);
  tft.drawFastVLine(x + w - 1, y - 2, t, color);
  tft.drawFastHLine(x - 2, y + h - 1, t, color);
  tft.drawFastVLine(x, y + h - 2, t, color);
  tft.drawFastHLine(x + w - t + 2, y + h - 1, t, color);
  tft.drawFastVLine(x + w - 1, y + h - 2, t, color);
}

// Trims `text` (stripping a trailing ".mp3" first) so its rendered pixel
// width fits within maxWidth at the current text size, appending "..." if
// it had to cut anything. Fixes long filenames overflowing button bounds.
String truncateToWidth(const String &text, int maxWidth) {
  String label = text;
  if (label.endsWith(".mp3") || label.endsWith(".MP3")) {
    label = label.substring(0, label.length() - 4);
  }

  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  if (bw <= (uint16_t)maxWidth) return label;

  const String ellipsis = "...";
  while (label.length() > 1) {
    label.remove(label.length() - 1);
    String test = label + ellipsis;
    tft.getTextBounds(test, 0, 0, &bx, &by, &bw, &bh);
    if (bw <= (uint16_t)maxWidth) return test;
  }
  return ellipsis;
}

void drawGlassButton(int x, int y, int w, int h, const char* label, bool selected) {
  uint16_t fill   = selected ? tft.color565(16, 10, 26) : tft.color565(8, 5, 14);
  uint16_t border = selected ? COL_ACID : COL_DIMMER;

  tft.fillRect(x, y, w, h, fill);
  tft.drawRect(x, y, w, h, border);
  tft.drawFastHLine(x + 2, y + 2, w - 4, selected ? COL_MAGENTA : COL_DIMMER);
  drawSigilCorners(x, y, w, h, selected ? COL_ACID : COL_DIMMER);

  tft.setTextSize(1);
  tft.setTextColor(selected ? COL_ACID : COL_DIM, fill);

  // Clip the label to the button's interior width before centering it, so
  // long filenames get a "..." instead of spilling past the border.
  int maxTextWidth = w - 10;
  String clipped = truncateToWidth(String(label), maxTextWidth);

  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(clipped, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor(x + (w - bw) / 2, y + (h - bh) / 2);
  tft.print(clipped);
}

void drawGlassPanel(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, COL_PANEL);
  tft.drawRect(x, y, w, h, COL_PANEL_BORDER);
  drawSigilCorners(x, y, w, h, COL_MAGENTA);
}

// ==========================================
// 5. TJpg / ENCODER ISR / AUDIO TASK
// ==========================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return 0;
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

void IRAM_ATTR encoderISR() {
  if (digitalRead(ENC_CLK) != digitalRead(ENC_DT)) {
    encoderValue++;
  } else {
    encoderValue--;
  }
}

void audioTask(void *parameter) {
  while (true) {
    AUDIO_LOCK();
    audio.loop();
    AUDIO_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Legacy weak-symbol callback: library calls this by name on file end.
// NOTE: this runs on core 0, called synchronously from inside audio.loop(),
// which means audioTask already holds audioMutex. That's exactly why the
// mutex must be recursive -- this take/give pair just re-enters it safely.
void audio_eof_mp3(const char *info) {
  if (appState == STATE_SD_PLAYING && trackCount > 0) {
    AUDIO_LOCK();
    currentTrackIndex = (currentTrackIndex + 1) % trackCount;
    String path = trackList[currentTrackIndex];
    if (!path.startsWith("/")) path = "/" + path;
    audio.connecttoFS(SD, path.c_str());
    isPlaying = true;
    AUDIO_UNLOCK();
  }
}

void spotifyCallback(CurrentlyPlaying currentlyPlaying) {
  if (currentlyPlaying.isPlaying) {
    currentTrack = String(currentlyPlaying.trackName);
    currentArtist = String(currentlyPlaying.artists[0].artistName);
    spotifyIsPlaying = true;
  } else {
    currentTrack = "Nothing playing";
    currentArtist = "on Spotify";
    spotifyIsPlaying = false;
  }
}

// ==========================================
// 6. SD CARD TRACK LIST
// ==========================================
void scanSDTracks() {
  trackCount = 0;
  File root = SD.open("/");
  if (!root) {
    Serial.println("SD: failed to open root directory");
    return;
  }
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    String lower = name;
    lower.toLowerCase();
    bool isJunk = name.startsWith(".") || name.startsWith("._");
    if (!entry.isDirectory() && lower.endsWith(".mp3") && !isJunk && trackCount < 40) {
      trackList[trackCount] = name;
      trackCount++;
      Serial.printf("SD: found track [%d] %s\n", trackCount - 1, name.c_str());
    }
    entry.close();
  }
  root.close();
  Serial.printf("SD: scan complete, %d tracks found\n", trackCount);
}

// Called from core 1 (main loop / button handlers). Must take audioMutex
// before touching `audio`, since audioTask on core 0 is calling audio.loop()
// continuously and connecttoFS() is not safe to call concurrently with it.
void playTrack(int idx) {
  if (trackCount == 0) return;
  idx = ((idx % trackCount) + trackCount) % trackCount;
  currentTrackIndex = idx;
  String path = trackList[idx];
  if (!path.startsWith("/")) path = "/" + path;

  AUDIO_LOCK();
  audio.connecttoFS(SD, path.c_str());
  AUDIO_UNLOCK();

  isPlaying = true;
}

int readSmoothedVolume() {
  int raw = analogRead(POT1_PIN);
  smoothVol = (smoothVol * 3 + raw) / 4; // simple EMA to kill ADC jitter
  return map(smoothVol, 0, 4095, 0, 21);
}

// ==========================================
// 7. STATE ENTRY / DRAW FUNCTIONS
// ==========================================
void enterMenu() {
  appState = STATE_MENU;
  screenNeedsFullDraw = true;
  listNeedsRedraw = true;
  lastMenuEncoderValue = encoderValue;
}

void enterSpotify() {
  appState = STATE_SPOTIFY;
  screenNeedsFullDraw = true;
}

void enterSDPlayer() {
  appState = STATE_SD_BROWSE; // start in browse mode, not auto-playing
  screenNeedsFullDraw = true;
  listNeedsRedraw = true;
  browseIndex = 0;
  lastMenuEncoderValue = encoderValue; // resync so browse list doesn't jump
  if (trackCount == 0) scanSDTracks();
}

void drawMenuStatic() {
  drawCyberBackground();
  tft.setTextSize(2);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(22, 6);
  tft.print("PRO-DASH");
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM);
  tft.setCursor(10, 112);
  tft.print("Turn: select   Click: enter");
}

void drawMenuButtons() {
  const int btnW = 130, btnH = 28, gap = 8, startY = 38;
  for (int i = 0; i < MENU_COUNT; i++) {
    drawGlassButton(15, startY + i * (btnH + gap), btnW, btnH, menuItems[i], i == menuIndex);
  }
}

void drawSpotifyStatic() {
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(4, 4);
  tft.print("SPOTIFY STATUS");
  drawGlassPanel(8, 18, 144, 60);
  tft.setCursor(4, 100);
  tft.setTextColor(COL_DIM);
  tft.print("SW1:Skip SW2:Play/Pause");
  tft.setCursor(4, 112);
  tft.print("Hold Enc: Back to menu");
}

void drawSpotifyDynamic() {
  tft.setTextSize(1);
  tft.setTextColor(COL_ACID, COL_PANEL);
  tft.setCursor(14, 26);
  tft.print(currentTrack.substring(0, 22) + "               ");

  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setCursor(14, 42);
  tft.print(currentArtist.substring(0, 22) + "               ");

  tft.setTextColor(spotifyIsPlaying ? COL_ACID : COL_MAGENTA, COL_PANEL);
  tft.setCursor(14, 60);
  tft.print(spotifyIsPlaying ? "> PLAYING   " : "|| PAUSED    ");
}

void drawSDProgressBar(int cur, int dur) {
  int barX = 14, barY = 96, barW = 128, barH = 4;
  tft.drawRect(barX, barY, barW, barH, COL_DIMMER);
  if (dur > 0) {
    int fillW = map(constrain(cur, 0, dur), 0, dur, 0, barW - 2);
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, COL_ACID);
  }
}

void drawSDPlayerStatic() {
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(4, 4);
  tft.print("SD CARD PLAYER");

  if (!sdMounted) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 50);
    tft.print("SD CARD NOT DETECTED");
    tft.setCursor(4, 112);
    tft.setTextColor(COL_DIM);
    tft.print("Hold Enc: Back to menu");
    return;
  }

  drawGlassPanel(8, 18, 144, 40);   // track name
  drawGlassPanel(8, 62, 144, 40);   // time / state / volume
  tft.setCursor(4, 116);
  tft.setTextColor(COL_DIM);
  tft.print("SW1:Next  SW2:Play/Pause  Hold Enc:Back");
}

// Background + header only -- painted ONCE when entering the browse screen.
void drawSDBrowseStatic() {
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(4, 4);
  tft.print("SELECT TRACK");
  tft.setTextColor(COL_DIM);
  tft.setCursor(4, 116);
  tft.print("Hold Enc: Back to menu");
}

// Just the scrolling list of buttons -- this is what gets repainted on every
// encoder tick. No background redraw here, which is what was causing the
// flicker/clunkiness: drawGlassButton() already fills its own rect, so
// repainting just the rows is enough to keep it visually clean.
void drawSDBrowseList() {
  if (!sdMounted) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 50);
    tft.print("SD CARD NOT DETECTED");
    return;
  }
  if (trackCount == 0) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 50);
    tft.print("NO MP3 FILES FOUND");
    return;
  }

  int startY = 20;
  int visible = trackCount < 4 ? trackCount : 4;
  for (int i = 0; i < visible; i++) {
    int idx = (browseIndex + i) % trackCount;
    bool isSelected = (i == 0); // top row = current selection
    drawGlassButton(8, startY + (i * 26), 144, 22, trackList[idx].c_str(), isSelected);
  }
}

void drawSDPlayerDynamic() {
  if (!sdMounted) return;
  tft.setTextSize(1);
  String name = trackCount > 0 ? trackList[currentTrackIndex] : "No MP3 files found";
  tft.setTextColor(COL_ACID, COL_PANEL);
  tft.setCursor(14, 24);
  tft.print(truncateToWidth(name, 132) + "               ");
  tft.setCursor(14, 38);
  tft.printf("Track %d / %d      ", trackCount > 0 ? currentTrackIndex + 1 : 0, trackCount);

  AUDIO_LOCK();
  bool running = audio.isRunning();
  int cur = running ? audio.getAudioCurrentTime() : 0;
  int dur = running ? audio.getAudioFileDuration() : 0;
  AUDIO_UNLOCK();

  char timeBuf[24];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d / %02d:%02d   ", cur / 60, cur % 60, dur / 60, dur % 60);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.setCursor(14, 70);
  tft.print(timeBuf);

  int volPct = readSmoothedVolume();
  tft.setTextColor(isPlaying ? COL_ACID : COL_MAGENTA, COL_PANEL);
  tft.setCursor(14, 86);
  tft.printf("%s  Vol:%2d   ", isPlaying ? "> PLAY" : "|| PAUSE", volPct);

  drawSDProgressBar(cur, dur);
}

// ==========================================
// 8. INPUT HANDLING
// ==========================================
void handleEncoderSwitch() {
  bool state = digitalRead(ENC_SW);

  if (encSwLastState == HIGH && state == LOW) {
    encSwPressStart = millis();
    encSwLongTriggered = false;
  }

  if (state == LOW && !encSwLongTriggered && millis() - encSwPressStart > LONG_PRESS_MS) {
    encSwLongTriggered = true;
    if (appState != STATE_MENU) {
      enterMenu();
    }
  }

  if (encSwLastState == LOW && state == HIGH) {
    unsigned long heldFor = millis() - encSwPressStart;
    if (!encSwLongTriggered && heldFor < LONG_PRESS_MS) {
      // short click = select
      if (appState == STATE_MENU) {
        if (menuIndex == 0) enterSpotify();
        else enterSDPlayer();
      } else if (appState == STATE_SD_BROWSE && trackCount > 0) {
        currentTrackIndex = browseIndex % trackCount;
        playTrack(currentTrackIndex);
        appState = STATE_SD_PLAYING;
        screenNeedsFullDraw = true;
      }
    }
  }

  encSwLastState = state;
}

// Sets listNeedsRedraw (NOT screenNeedsFullDraw) -- moving the selection
// should only repaint the buttons/list, never the background.
void handleSelectionEncoder(int maxCount, int &idx) {
  if (maxCount <= 0) return; // guard against div/mod by zero
  int delta = encoderValue - lastMenuEncoderValue;
  if (abs(delta) >= ENCODER_STEPS_PER_CLICK) {
    if (delta > 0) idx = (idx + 1) % maxCount;
    else idx = (idx - 1 + maxCount) % maxCount;
    lastMenuEncoderValue = encoderValue;
    listNeedsRedraw = true; // redraw list on move
  }
}

void handleSW1SW2() {
  bool sw1State = digitalRead(SW1_PIN);
  bool sw2State = digitalRead(SW2_PIN);

  if (millis() - lastButtonAction > BUTTON_DEBOUNCE) {
    // SW1 = Next
    if (sw1LastState == HIGH && sw1State == LOW) {
      if (appState == STATE_SPOTIFY) {
        if (wifiMulti.run() == WL_CONNECTED) spotify.nextTrack();
      } else if (appState == STATE_SD_PLAYING) {
        playTrack(currentTrackIndex + 1);
      }
      lastButtonAction = millis();
    }
    // SW2 = Play/Pause
    if (sw2LastState == HIGH && sw2State == LOW) {
      if (appState == STATE_SPOTIFY) {
        if (wifiMulti.run() == WL_CONNECTED) {
          if (spotifyIsPlaying) spotify.pause();
          else spotify.play();
        }
      } else if (appState == STATE_SD_PLAYING) {
        isPlaying = !isPlaying;
        AUDIO_LOCK();
        audio.pauseResume();
        AUDIO_UNLOCK();
      }
      lastButtonAction = millis();
    }
  }

  sw1LastState = sw1State;
  sw2LastState = sw2State;
}

// ==========================================
// 9. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  audioMutex = xSemaphoreCreateRecursiveMutex();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  initCyberPalette();

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(10, 10);
  tft.print("Scanning Wi-Fi...");

  wifiMulti.addAP(ssidHome, passHome);
  wifiMulti.addAP(ssidHotspot, passHotspot);
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected: " + WiFi.SSID());

  client.setInsecure();

  spiSD.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, spiSD, 4000000);
  Serial.printf("SD: mount %s\n", sdMounted ? "OK" : "FAILED");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(10);

  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 1, NULL, 0);

  lastMenuEncoderValue = encoderValue;
  enterMenu();
}

// ==========================================
// 10. MAIN LOOP
// ==========================================
void loop() {
  handleEncoderSwitch();
  handleSW1SW2();

  // --- Spotify polling ---
  if (appState == STATE_SPOTIFY && millis() - lastSpotifyCheck > 5000) {
    if (wifiMulti.run() == WL_CONNECTED) {
      spotify.getCurrentlyPlaying(spotifyCallback);
    } else {
      currentTrack = "Wi-Fi Lost!";
      currentArtist = "Searching...";
    }
    lastSpotifyCheck = millis();
  }

  // --- Live volume control while in SD player mode ---
  if (appState == STATE_SD_PLAYING) {
    int vol = readSmoothedVolume();
    AUDIO_LOCK();
    audio.setVolume(vol);
    AUDIO_UNLOCK();
  }

  // --- Per-state input + redraw ---
  if (appState == STATE_MENU) {
    if (screenNeedsFullDraw) {
      drawMenuStatic();
      screenNeedsFullDraw = false;
      listNeedsRedraw = true;
    }
    handleSelectionEncoder(MENU_COUNT, menuIndex);
    if (listNeedsRedraw) {
      drawMenuButtons();
      listNeedsRedraw = false;
    }
  }
  else if (appState == STATE_SPOTIFY) {
    if (screenNeedsFullDraw) {
      drawSpotifyStatic();
      screenNeedsFullDraw = false;
    }
  }
  else if (appState == STATE_SD_BROWSE) {
    if (screenNeedsFullDraw) {
      drawSDBrowseStatic();
      screenNeedsFullDraw = false;
      listNeedsRedraw = true;
    }
    handleSelectionEncoder(trackCount, browseIndex);
    if (listNeedsRedraw) {
      drawSDBrowseList();
      listNeedsRedraw = false;
    }
  }
  else if (appState == STATE_SD_PLAYING) {
    if (screenNeedsFullDraw) {
      drawSDPlayerStatic();
      screenNeedsFullDraw = false;
    }
  }

  // --- Dynamic field refresh (every 200ms) ---
  if (millis() - lastUITime > 200) {
    if (appState == STATE_SPOTIFY) drawSpotifyDynamic();
    else if (appState == STATE_SD_PLAYING) drawSDPlayerDynamic();
    lastUITime = millis();
  }
}
