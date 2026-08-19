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
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <math.h>
#define SERVER_IP "192.168.1.182"

// Flip any of these to 0 if it doesn't compile 
#define ENABLE_FFT_SPECTRUM     0   // real-time spectrum bars only for SD 
#define ENABLE_BATTERY_MON      0   // battery % icon on the menu screen
#define ENABLE_SPOTIFY_LIKE     0   // long-press SW1 in Spotify view = Like
#define ENABLE_GAPLESS_PREFETCH 1   // SD-cache warm-up before track end

#if ENABLE_FFT_SPECTRUM
#include <arduinoFFT.h>
#endif

#if ENABLE_SPOTIFY_LIKE
#include "mbedtls/base64.h"
#endif

WiFiMulti wifiMulti;
WiFiClientSecure client;
SpotifyArduino spotify(client, clientId, clientSecret, refreshToken);

// ==========================================
// 0. THEME CONFIG
// ==========================================
#define USE_COVER_THEME 0

// ==========================================
// 2. PIN DEFINITIONS
// ==========================================
#define TFT_SCLK 12
#define TFT_MOSI 11
#define TFT_RST  14  
#define TFT_CS   10
#define TFT_DC   13  

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
#define SW1_PIN 47
#define SW2_PIN 48

// ALL SAFE ADC1 PINS
#define POT1_PIN 1  // Master Volume
#define POT2_PIN 2  // Low/Bass EQ
#define POT3_PIN 7  // Mid EQ
#define POT4_PIN 8  // High/Treble EQ
// Dual-purpose: Pan/Balance by default, Speed/Pitch when toggled. Long-
// press SW2 while a track is playing to switch modes (see handleSW1SW2()).
#define POT5_PIN 9

#if ENABLE_BATTERY_MON
//need analog fre pin
#define BATT_PIN 3
#endif

// ==========================================
// 2b. DASHBOARD LAYOUT CONSTANTS
// ==========================================
#define COVER_X 4
#define COVER_Y 4
#define COVER_W 70
#define COVER_H 70

#define INFO_X (COVER_X + COVER_W + 4)
#define INFO_Y COVER_Y
#define INFO_W (160 - INFO_X - 4)
#define INFO_H COVER_H

#define NAME_X COVER_X
#define NAME_Y (COVER_Y + COVER_H + 14)        
#define NAME_W (160 - (COVER_X * 2))          

#define PROGRESS_X COVER_X
#define PROGRESS_Y (NAME_Y + 18)              
#define PROGRESS_W (160 - (COVER_X * 2))      
#define PROGRESS_H 4

#define COVER_CLIP_MINX (COVER_X + 1)
#define COVER_CLIP_MAXX (COVER_X + COVER_W - 2)
#define COVER_CLIP_MINY (COVER_Y + 1)
#define COVER_CLIP_MAXY (COVER_Y + COVER_H - 2)

#define SPOTIFY_IMG_X (COVER_X + 1 + ((COVER_W - 2 - 64) / 2))
#define SPOTIFY_IMG_Y (COVER_Y + 1 + ((COVER_H - 2 - 64) / 2))

#define TEXT_ROW_H 9

// ==========================================
// 3. GLOBAL OBJECTS & STATE
// ==========================================
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
SPIClass spiSD(HSPI);
Audio audio;
SemaphoreHandle_t audioMutex;
#define AUDIO_LOCK()   xSemaphoreTakeRecursive(audioMutex, portMAX_DELAY)
#define AUDIO_UNLOCK() xSemaphoreGiveRecursive(audioMutex)
SemaphoreHandle_t displayMutex;
#define DISPLAY_LOCK()   xSemaphoreTakeRecursive(displayMutex, portMAX_DELAY)
#define DISPLAY_UNLOCK() xSemaphoreGiveRecursive(displayMutex)

SemaphoreHandle_t uiStateMutex;
#define UI_LOCK()   xSemaphoreTake(uiStateMutex, portMAX_DELAY)
#define UI_UNLOCK() xSemaphoreGive(uiStateMutex)

#if ENABLE_FFT_SPECTRUM
SemaphoreHandle_t spectrumMutex;
#endif

volatile int encoderValue = 0;
unsigned long lastUITime = 0;
unsigned long lastMarqueeTime = 0;
unsigned long lastSpotifyCheck = 0;
unsigned long lastHeapLog = 0;

String currentTrack = "Waiting...";
String currentArtist = "Spotify API";
bool spotifyIsPlaying = false;

long spotifyProgressMs = 0;
long spotifyDurationMs = 0;
unsigned long spotifyProgressCapturedAt = 0;

void playTrack(int idx);
void clearCoverBox();
void clearCoverCache();
bool jpegIsProgressive(File &f);
void drawCurrentSDCover();
void enterMenu();
void applyFallbackCover();
bool coverLooksBlack();
void networkTask(void *parameter);

enum AppState { STATE_MENU, STATE_SPOTIFY, STATE_SD_BROWSE, STATE_SD_PLAYING, STATE_SYNC };
AppState appState = STATE_MENU;

const char* menuItems[] = { "SPOTIFY STATUS", "SD CARD PLAYER", "SYNC WIFI SERVER" };
const int MENU_COUNT = 3;

int browseIndex = 0;
bool screenNeedsFullDraw = true;
bool listNeedsRedraw = true;
bool sdMounted = false;

int menuIndex = 0;
int lastMenuEncoderValue = 0;
const int ENCODER_STEPS_PER_CLICK = 2;

bool encSwLastState = HIGH;
unsigned long encSwPressStart = 0;
bool encSwLongTriggered = false;
const unsigned long LONG_PRESS_MS = 700;

bool sw1LastState = HIGH;
bool sw2LastState = HIGH;
bool sw2LongTriggered = false;
unsigned long sw2PressStart = 0;
unsigned long lastButtonAction = 0;
const unsigned long BUTTON_DEBOUNCE = 250;
String currentSpotifyUrl = "";
String lastSpotifyUrl = "none";

#if ENABLE_SPOTIFY_LIKE
String currentSpotifyTrackId = "";
bool sw1LongTriggered = false;
unsigned long sw1PressStart = 0;
unsigned long likedFlashUntil = 0;
#endif

String trackList[40];
int trackCount = 0;
int currentTrackIndex = 0;
bool isPlaying = true;
int smoothVol = 0;

char lastDrawnSpotifyTimeBuf[16] = "";
bool lastDrawnSpotifyPlaying = true;
bool spotifyTransportDrawn = false;

// UI Toggles
bool isDjMixerActive = false;
int lastDrawnSDTrackIndex = -1;
bool lastDrawnSDPlaying = true;
bool sdTransportDrawn = false;
int lastDrawnSliders[5] = {-1, -1, -1, -1, -1};

// RAM CACHE FOR INSTANT ALBUM COVER TOGGLING
#define CACHE_W (COVER_CLIP_MAXX - COVER_CLIP_MINX + 1)
#define CACHE_H (COVER_CLIP_MAXY - COVER_CLIP_MINY + 1)
uint16_t coverCache[CACHE_W * CACHE_H];
bool coverCacheValid = false;
bool coverThemeDirty = false;

bool coverPending = false;
unsigned long coverPendingSince = 0;
const unsigned long COVER_FETCH_TIMEOUT_MS = 2500;

uint16_t themeGradient[160]; 

// ==========================================
// 3b. DSP / POTENTIOMETER STATE
// ==========================================
int smoothBass = 2048;
int smoothMid  = 2048;
int smoothHigh = 2048;

// POT5 is dual-purpose now: Pan/Balance by default, Speed when toggled
int smoothPot5 = 1024;
const int POT5_CENTER = 1024;
const int POT5_DEADZONE = 150; // +/- around center treated as "neutral"

bool pot5IsSpeedMode = false;   // false = Pan (default), true = Speed
bool pot5NeedsReapply = false;  // force a re-apply next updateDSP() pass
                                 // (set on track change AND on mode toggle)

float currentSpeedMod = 1.0f;   // for the on-screen "1.00x" readout
int8_t currentPanValue = 0;     // for the on-screen pan readout

unsigned long lastDspTime = 0;
int8_t lastBassGain = 100, lastMidGain = 100, lastHighGain = 100;
int8_t lastAppliedBalGain = 100;   // sentinel (mapBalance never returns 100)
float lastAppliedSpeedModDsp = 1.0f;

// ==========================================
// 3c. BATTERY MONITORING STATE
// ==========================================
#if ENABLE_BATTERY_MON
float battSmoothedRaw = -1.0f;
int battPercent = 100;
unsigned long lastBattRead = 0;
const unsigned long BATT_READ_INTERVAL_MS = 2000;
const float BATT_DIVIDER_RATIO = 2.0f;
const float BATT_EMPTY_V = 3.3f;
const float BATT_FULL_V  = 4.2f;
#endif

// ==========================================
// 3d. SPECTRUM ANALYZER STATE
// ==========================================
#if ENABLE_FFT_SPECTRUM
#define FFT_SAMPLES 256
#define NUM_BARS 16
#define RAW_SAMPLE_SHIFT 16
#define FFT_NORM_DIVISOR 4000.0f
float fftReal[FFT_SAMPLES];
float fftImag[FFT_SAMPLES];
ArduinoFFT<float> FFT(fftReal, fftImag, FFT_SAMPLES, 44100.0f);

int16_t pcmRing[FFT_SAMPLES];
volatile int pcmRingWritePos = 0;
volatile bool pcmRingFull = false;

float barMagnitude[NUM_BARS] = {0};
float barPeak[NUM_BARS] = {0};
#endif

// ==========================================
// 3e. GAPLESS PREFETCH STATE
// ==========================================
#if ENABLE_GAPLESS_PREFETCH
File nextTrackPreopened;
int nextTrackPreopenedIdx = -1;
#endif

// ==========================================
// 4. Y2K / CYBERSIGIL DRAW HELPERS
// ==========================================
uint16_t COL_BG_TOP, COL_BG_BOT, COL_PANEL, COL_PANEL_BORDER;
uint16_t COL_MAGENTA, COL_ACID, COL_DIM, COL_DIMMER, COL_SCANLINE;

uint16_t lerpColor(uint16_t c1, uint16_t c2, float t) {
  uint8_t r1 = (c1 >> 11) & 0x1F, g1 = (c1 >> 5) & 0x3F, b1 = c1 & 0x1F;
  uint8_t r2 = (c2 >> 11) & 0x1F, g2 = (c2 >> 5) & 0x3F, b2 = c2 & 0x1F;
  uint8_t r = r1 + (uint8_t)((r2 - r1) * t);
  uint8_t g = g1 + (uint8_t)((g2 - g1) * t);
  uint8_t b = b1 + (uint8_t)((b2 - b1) * t);
  return (r << 11) | (g << 5) | b;
}

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
  for (int y = 0; y < 160; y++) {
    float t = (float)y / 127.0;
    themeGradient[y] = lerpColor(COL_BG_TOP, COL_BG_BOT, t);
  }
}

void updateThemeFromCover() {
#if USE_COVER_THEME
  if (!coverCacheValid) return;
 
  uint16_t sampleColors[5];
  const int band = CACHE_H / 5;
 
  for (int i = 0; i < 5; i++) {
    uint32_t rSum = 0, gSum = 0, bSum = 0;
    int count = 0;
    int yStart = i * band;
    int yEnd = (i == 4) ? CACHE_H : yStart + band;
 
    for (int y = yStart; y < yEnd; y += 2) {
      for (int x = 0; x < CACHE_W; x += 2) {
        uint16_t c = coverCache[y * CACHE_W + x];
        rSum += (c >> 11) & 0x1F;
        gSum += (c >> 5) & 0x3F;
        bSum += c & 0x1F;
        count++;
      }
    }
    if (count == 0) count = 1;
    sampleColors[i] = ((rSum / count) << 11) | ((gSum / count) << 5) | (bSum / count);
  }
 
  for (int y = 0; y < tft.height(); y++) {
    float pos = (float)y / tft.height();
    int idx = (int)(pos * 4);
    float localT = (pos * 4) - idx;
    themeGradient[y] = lerpColor(sampleColors[idx], sampleColors[min(idx + 1, 4)], localT);
  }
 
  COL_ACID = sampleColors[0];
#endif
}

void drawCyberBackground() {
  if (coverThemeDirty) {
    updateThemeFromCover();
    coverThemeDirty = false;
  }
 
  for (int y = 0; y < tft.height(); y++) {
    uint16_t rowColor = themeGradient[y];
    tft.drawFastHLine(0, y, tft.width(), rowColor);
    if (y % 3 == 0) {
      tft.drawFastHLine(0, y, tft.width(), lerpColor(rowColor, COL_SCANLINE, 0.35f));
    }
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

void drawClearedText(int x, int y, int w, const String &text, uint16_t fg, uint16_t bg) {
  tft.fillRect(x, y, w, TEXT_ROW_H, bg);
  tft.setTextColor(fg, bg);
  tft.setCursor(x, y);
  tft.print(text);
}

void drawGlassButton(int x, int y, int w, int h, const char* label, bool selected, int hPad = 8) {
  uint16_t fill   = selected ? tft.color565(16, 10, 26) : tft.color565(8, 5, 14);
  uint16_t border = selected ? COL_ACID : COL_DIMMER;

  tft.fillRect(x, y, w, h, fill);
  tft.drawRect(x, y, w, h, border);
  drawSigilCorners(x, y, w, h, border);

  tft.setTextSize(1);
  tft.setTextColor(selected ? COL_ACID : COL_DIM, fill);

  int maxTextWidth = w - hPad;
  String clipped = truncateToWidth(String(label), maxTextWidth);

  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(clipped, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor(x + (w - bw) / 2, y + (h - bh) / 2);
  tft.print(clipped);
}

void drawGlassPanel(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, COL_PANEL);
}

void drawProgressBar(int x, int y, int w, int h, int curSec, int durSec) {
  tft.drawRect(x, y, w, h, COL_DIMMER);
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, COL_PANEL);
  if (durSec > 0) {
    int fillW = map(constrain(curSec, 0, durSec), 0, durSec, 0, w - 2);
    tft.fillRect(x + 1, y + 1, fillW, h - 2, COL_ACID);
  }
}

void drawSpeedIndicator(int x, int y, int w, float speedMod) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%.2fx", speedMod);
  uint16_t bg = themeGradient[constrain(y, 0, 127)];
  DISPLAY_LOCK();
  tft.fillRect(x, y, w, TEXT_ROW_H, bg);
  tft.setTextColor(COL_ACID, bg);
  tft.setCursor(x, y);
  tft.print(buf);
  DISPLAY_UNLOCK();
}

// Pan readout: "C" when centered, "L 8"/"R 8" otherwise. Shares the same
// slot on screen as drawSpeedIndicator() -- only one of the two is ever
// called at a time, depending on pot5IsSpeedMode.
void drawPanIndicator(int x, int y, int w, int8_t panValue) {
  char buf[8];
  if (panValue == 0) {
    snprintf(buf, sizeof(buf), "PAN:C");
  } else if (panValue < 0) {
    snprintf(buf, sizeof(buf), "L%d", -panValue);
  } else {
    snprintf(buf, sizeof(buf), "R%d", panValue);
  }
  uint16_t bg = themeGradient[constrain(y, 0, 127)];
  DISPLAY_LOCK();
  tft.fillRect(x, y, w, TEXT_ROW_H, bg);
  tft.setTextColor(COL_ACID, bg);
  tft.setCursor(x, y);
  tft.print(buf);
  DISPLAY_UNLOCK();
}

#if ENABLE_BATTERY_MON
void drawBatteryIcon(int x, int y) {
  uint16_t col = battPercent > 50 ? COL_ACID
               : (battPercent > 20 ? tft.color565(255, 200, 0) : COL_MAGENTA);
  DISPLAY_LOCK();
  tft.drawRect(x, y, 18, 9, COL_DIMMER);          // body outline
  tft.fillRect(x + 18, y + 2, 2, 5, COL_DIMMER);  // nub
  tft.fillRect(x + 2, y + 2, 14, 5, COL_PANEL);   // clear interior
  int fillW = map(battPercent, 0, 100, 0, 14);
  if (fillW > 0) tft.fillRect(x + 2, y + 2, fillW, 5, col);
  DISPLAY_UNLOCK();
}
#endif

#if ENABLE_FFT_SPECTRUM
// Drawn in the free strip below the progress bar on the normal (non-DJ-
// mixer) SD player view.
void drawSpectrumBars(int x, int y, int w, int h) {
  const int gap = 1;
  int barW = (w - (NUM_BARS - 1) * gap) / NUM_BARS;
  DISPLAY_LOCK();
  for (int i = 0; i < NUM_BARS; i++) {
    int bx = x + i * (barW + gap);
    int barH = (int)(barMagnitude[i] * h);
    tft.fillRect(bx, y, barW, h - barH, COL_PANEL);         // empty headroom
    tft.fillRect(bx, y + h - barH, barW, barH, COL_ACID);   // filled portion

    int peakY = y + h - (int)(barPeak[i] * h);
    tft.drawFastHLine(bx, constrain(peakY, y, y + h - 1), barW, COL_MAGENTA);
  }
  DISPLAY_UNLOCK();
}
#endif

// ==========================================
// 4b. MARQUEE (SCROLLING TEXT) HELPERS
// ==========================================
struct Marquee {
  GFXcanvas16 *canvas;
  String text;          
  int textPixelW = 0;   
  int boxW = 0;          
  int scrollX = 0;
  bool scrolling = false;
  unsigned long lastStep = 0;
  unsigned long pauseUntil = 0;
  Marquee(GFXcanvas16 *c) : canvas(c) {}
};

const unsigned long MARQUEE_STEP_MS = 40;   
const unsigned long MARQUEE_PAUSE_MS = 1000; 
const int MARQUEE_GAP_PX = 16;              

GFXcanvas16 artistCanvasBuf(260, TEXT_ROW_H);
GFXcanvas16 trackCanvasBuf(260, TEXT_ROW_H);
Marquee artistMarquee(&artistCanvasBuf);
Marquee trackMarquee(&trackCanvasBuf);

// Only ever called from the UI task, and only touches the off-screen
// GFXcanvas16 (not the physical tft), so it doesn't need displayMutex.
void marqueeSetText(Marquee &m, const String &text, int boxW, uint16_t fg, uint16_t bg) {
  m.boxW = boxW;
  if (text == m.text) return;
  m.text = text;
  m.scrollX = 0;
  m.lastStep = millis();
  m.pauseUntil = millis() + MARQUEE_PAUSE_MS;

  tft.setTextSize(1);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  m.textPixelW = bw;
  m.scrolling = (int)bw > boxW;

  int canvasW = m.canvas->width();
  m.canvas->fillScreen(bg);
  m.canvas->setTextColor(fg);
  m.canvas->setTextSize(1);
  m.canvas->setCursor(0, 1);
  m.canvas->print(text);

  if (m.scrolling) {
    int secondX = (int)bw + MARQUEE_GAP_PX;
    if (secondX + (int)bw <= canvasW) {
      m.canvas->setCursor(secondX, 1);
      m.canvas->print(text);
    }
  }
}

// This one DOES touch the physical tft (drawRGBBitmap), so it locks.
void marqueeDraw(Marquee &m, int x, int y, uint16_t bg) {
  static uint16_t sliceBuf[300];
  int boxW = m.boxW;
  if (boxW <= 0) return;
  if (boxW > (int)(sizeof(sliceBuf) / sizeof(sliceBuf[0]))) {
    boxW = sizeof(sliceBuf) / sizeof(sliceBuf[0]);
  }

  if (m.scrolling && millis() > m.pauseUntil && millis() - m.lastStep > MARQUEE_STEP_MS) {
    m.lastStep = millis();
    m.scrollX++;
    int wrapPoint = m.textPixelW + MARQUEE_GAP_PX;
    if (m.scrollX >= wrapPoint) {
      m.scrollX = 0;
      m.pauseUntil = millis() + MARQUEE_PAUSE_MS;
    }
  }

  uint16_t *buf = m.canvas->getBuffer();
  int canvasW = m.canvas->width();
  int srcStart = m.scrolling ? m.scrollX : 0;

  DISPLAY_LOCK();
  for (int row = 0; row < TEXT_ROW_H; row++) {
    for (int col = 0; col < boxW; col++) {
      int srcCol = srcStart + col;
      sliceBuf[col] = (srcCol >= 0 && srcCol < canvasW) ? buf[row * canvasW + srcCol] : bg;
    }
    tft.drawRGBBitmap(x, y + row, sliceBuf, boxW, 1);
  }
  DISPLAY_UNLOCK();
}

// ==========================================
// 5. TJpg / ENCODER ISR / AUDIO TASK
// ==========================================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  int minX = COVER_CLIP_MINX, maxX = COVER_CLIP_MAXX;
  int minY = COVER_CLIP_MINY, maxY = COVER_CLIP_MAXY;
 
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int px = x + i;
      int py = y + j;
      if (px >= minX && px <= maxX && py >= minY && py <= maxY) {
        coverCache[(py - minY) * CACHE_W + (px - minX)] = bitmap[j * w + i];
      }
    }
  }
  return 1;
}

void IRAM_ATTR encoderISR() {
  if (digitalRead(ENC_CLK) != digitalRead(ENC_DT)) {
    encoderValue++;
  } else {
    encoderValue--;
  }
}

// Highest-priority task: nothing but feeding the decoder/I2S-DMA ring, and
// yields every 1ms so it can never starve the other two tasks.
void audioTask(void *parameter) {
  while (true) {
    AUDIO_LOCK();
    audio.loop();
    AUDIO_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void audio_eof_mp3(const char *info) {
  if (appState == STATE_SD_PLAYING && trackCount > 0) {
    playTrack(currentTrackIndex + 1);
  }
}

bool jpegIsProgressive(File &f) {
  f.seek(0);
  uint8_t buf[512];
  size_t totalRead = 0;
  const size_t maxScan = 8192;
  uint8_t prev = 0;

  while (totalRead < maxScan) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      uint8_t b = buf[i];
      if (prev == 0xFF) {
        if (b == 0xC2 || b == 0xC6 || b == 0xCA || b == 0xCE) return true;
        if (b == 0xC0 || b == 0xC1) return false;
      }
      prev = b;
    }
    totalRead += n;
  }
  return false;
}

// UI-task-only: this is the sole place the RAM cover cache is ever blitted
// to the physical screen.
void drawCurrentSDCover() {
  if (coverCacheValid) {
    tft.drawRGBBitmap(COVER_CLIP_MINX, COVER_CLIP_MINY, coverCache, CACHE_W, CACHE_H);
  }
}

void clearCoverCache() {
  for (int i = 0; i < CACHE_W * CACHE_H; i++) {
    coverCache[i] = COL_PANEL;
  }
}

bool coverLooksBlack() {
  const int totalPixels = CACHE_W * CACHE_H;
  int blackCount = 0;
  int sampled = 0;
  for (int i = 0; i < totalPixels; i += 4) {
    if (coverCache[i] == 0x0000) blackCount++;
    sampled++;
  }
  return sampled > 0 && (blackCount * 100 / sampled) > 90;
}

void audio_id3image(File& file, const size_t pos, const size_t size) {
  AUDIO_LOCK();
  File cover = SD.open("/cover.jpg", FILE_WRITE);
  if (cover) {
    uint32_t currentPos = file.position(); 
    file.seek(pos);

    uint8_t buffer[2048];
    size_t bytesLeft = size;
    while (bytesLeft > 0) {
      size_t bytesToRead = (bytesLeft < sizeof(buffer)) ? bytesLeft : sizeof(buffer);
      file.read(buffer, bytesToRead);
      cover.write(buffer, bytesToRead);
      bytesLeft -= bytesToRead;
    }
    cover.close();
    file.seek(currentPos);
  }
  AUDIO_UNLOCK();

  AUDIO_LOCK();
  bool decodedOk = false;
  if (SD.exists("/cover.jpg")) {
    File check = SD.open("/cover.jpg");
    bool progressive = check && jpegIsProgressive(check);
    if (check) check.close();

    if (!progressive) {
      uint16_t imgW = 0, imgH = 0;
      TJpgDec.getFsJpgSize(&imgW, &imgH, "/cover.jpg", SD);

      const int maxDim = COVER_W - 2;
      int scale = 1;
      while ((imgW / scale > maxDim) || (imgH / scale > maxDim)) {
        scale *= 2;
        if (scale >= 8) break; 
      }
      TJpgDec.setJpgScale(scale);

      int scaledW = imgW / scale;
      int scaledH = imgH / scale;
      int dX = COVER_CLIP_MINX + (maxDim - scaledW) / 2;
      int dY = COVER_CLIP_MINY + (maxDim - scaledH) / 2;

      clearCoverCache(); 
      TJpgDec.drawFsJpg(dX, dY, "/cover.jpg", SD);

      if (!coverLooksBlack()) {
        decodedOk = true;
        coverCacheValid = true;
        coverThemeDirty = true;      
        screenNeedsFullDraw = true;
      }
    }
  }
  AUDIO_UNLOCK();
  if (!decodedOk) {
    applyFallbackCover();
  }
  coverPending = false;
}

#if ENABLE_FFT_SPECTRUM
// ==========================================
// 5b. SPECTRUM ANALYZER
// ==========================================
void audio_process_raw_samples(int32_t* outBuff, int16_t validSamples) {
  if (xSemaphoreTake(spectrumMutex, 0) != pdTRUE) return;
  // Assumed interleaved stereo (L,R,L,R,...), taking the left channel only
  // -- see the comment block near the top of the file if this needs
  // adjusting for your library version.
  for (int16_t i = 0; i + 1 < validSamples; i += 2) {
    int32_t sample = outBuff[i];
    int16_t s16 = (int16_t)(sample >> RAW_SAMPLE_SHIFT);
    pcmRing[pcmRingWritePos] = s16;
    pcmRingWritePos++;
    if (pcmRingWritePos >= FFT_SAMPLES) {
      pcmRingWritePos = 0;
      pcmRingFull = true;
    }
  }
  xSemaphoreGive(spectrumMutex);
}

void computeSpectrum() {
  if (!pcmRingFull) return;
  if (xSemaphoreTake(spectrumMutex, 0) != pdTRUE) return;

  for (int i = 0; i < FFT_SAMPLES; i++) {
    fftReal[i] = (float)pcmRing[i];
    fftImag[i] = 0.0f;
  }
  xSemaphoreGive(spectrumMutex);

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  const int usableBins = FFT_SAMPLES / 2;
  for (int bar = 0; bar < NUM_BARS; bar++) {
    int startBin = (int)(pow((float)bar / NUM_BARS, 2.0f) * usableBins) + 1;
    int endBin   = (int)(pow((float)(bar + 1) / NUM_BARS, 2.0f) * usableBins) + 1;
    if (endBin <= startBin) endBin = startBin + 1;
    if (endBin > usableBins) endBin = usableBins;

    float sum = 0;
    int count = 0;
    for (int b = startBin; b < endBin; b++) {
      sum += fftReal[b];
      count++;
    }
    float avg = count > 0 ? (sum / count) : 0.0f;
    float norm = constrain(avg / FFT_NORM_DIVISOR, 0.0f, 1.0f);
    barMagnitude[bar] = (norm > barMagnitude[bar]) ? norm : barMagnitude[bar] * 0.75f;
    barPeak[bar] = max(barPeak[bar] * 0.95f, barMagnitude[bar]);
  }
}
#endif

// Runs on networkTask. Writes only the shared Spotify text fields --
// guarded by uiStateMutex since the UI task's marquee reads them.
void spotifyCallback(CurrentlyPlaying currentlyPlaying) {
  UI_LOCK();
  if (currentlyPlaying.isPlaying) {
    currentTrack = String(currentlyPlaying.trackName);

    if (currentlyPlaying.numArtists > 0) {
      currentArtist = String(currentlyPlaying.artists[0].artistName);
    } else {
      currentArtist = "Unknown Artist";
    }

    spotifyIsPlaying = true;
    spotifyProgressMs = currentlyPlaying.progressMs;
    spotifyDurationMs = currentlyPlaying.durationMs;
    spotifyProgressCapturedAt = millis();

    if (currentlyPlaying.numImages > 0) {
      currentSpotifyUrl = String(currentlyPlaying.albumImages[currentlyPlaying.numImages - 1].url);
    } else {
      currentSpotifyUrl = "";
    }

#if ENABLE_SPOTIFY_LIKE
    currentSpotifyTrackId = String(currentlyPlaying.trackId);
#endif
  } else {
    currentTrack = "Nothing playing";
    currentArtist = "on Spotify";
    spotifyIsPlaying = false;
    currentSpotifyUrl = "";
    spotifyProgressMs = 0;
    spotifyDurationMs = 0;
  }
  UI_UNLOCK();
}

bool downloadSpotifyCover(String url) {
  if (url == "" || !sdMounted) return false;

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int expectedLen = http.getSize();

    AUDIO_LOCK();
    File file = SD.open("/spoty.jpg", FILE_WRITE);
    if (file) {
      int written = http.writeToStream(&file);
      file.close();
      AUDIO_UNLOCK();
      http.end();

      if (expectedLen > 0 && written != expectedLen) {
        return false;
      }
      return true;
    }
    AUDIO_UNLOCK();
  }
  http.end();
  return false;
}

// ==========================================
// 6. SD CARD TRACK LIST & DSP HELPERS
// ==========================================
void scanSDTracks() {
  AUDIO_LOCK();
  trackCount = 0;
  File root = SD.open("/");
  if (!root) {
    AUDIO_UNLOCK();
    return;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    String name = String(entry.name());
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    String lower = name;
    lower.toLowerCase();

    if (lower.indexOf("._") != -1 || lower.indexOf("/.") != -1 || lower.indexOf("system") != -1) {
      entry.close();
      continue;
    }

    if (lower.endsWith(".mp3")) {
      if (trackCount < 40) {
        trackList[trackCount] = name;
        trackCount++;
      }
    }
    entry.close();
  }
  root.close();
  AUDIO_UNLOCK();
}

void clearCoverBox() {
  DISPLAY_LOCK();
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, COL_PANEL);
  DISPLAY_UNLOCK();
}

void playTrack(int idx) {
  if (trackCount == 0) return;
  idx = ((idx % trackCount) + trackCount) % trackCount;
  currentTrackIndex = idx;
  String path = trackList[idx];
  if (!path.startsWith("/")) path = "/" + path;

  coverCacheValid = false;
  clearCoverCache(); 
  coverPending = true;
  coverPendingSince = millis();
  AUDIO_LOCK();
  if (SD.exists("/cover.jpg")) {
    SD.remove("/cover.jpg"); 
  }
  audio.connecttoFS(SD, path.c_str());
  AUDIO_UNLOCK();
  pot5NeedsReapply = true; 

#if ENABLE_GAPLESS_PREFETCH
  // If we already warmed this exact track (i.e. it was prefetched as
  // "next" and we're now actually switching to it), release the handle --
  // the real playback path opens its own via connecttoFS() above.
  AUDIO_LOCK();
  if (nextTrackPreopenedIdx == idx && nextTrackPreopened) {
    nextTrackPreopened.close();
  }
  nextTrackPreopenedIdx = -1;
  AUDIO_UNLOCK();
#endif

  if (appState == STATE_SD_PLAYING && !isDjMixerActive) {
    clearCoverBox();
  }
  isPlaying = true;
}

int readSmoothedVolume() {
  int raw = analogRead(POT1_PIN);
  smoothVol = (smoothVol * 3 + raw) / 4;
  return map(smoothVol, 0, 4095, 0, 21);
}

int8_t mapEQ(int rawValue) {
  if (rawValue < 1900) {
    return map(rawValue, 0, 1900, -40, 0);   
  } else if (rawValue > 2200) {
    return map(rawValue, 2200, 4095, 0, 6);  
  }
  return 0; 
}

int8_t mapBalance(int rawValue) {
  int lo = POT5_CENTER - POT5_DEADZONE;
  int hi = POT5_CENTER + POT5_DEADZONE;
  if (rawValue < lo) {
    return map(rawValue, 0, lo, -16, 0);
  } else if (rawValue > hi) {
    return map(rawValue, hi, 4095, 0, 16);
  }
  return 0;
}

// POT5 mapped to playback speed, centered on POT5_CENTER, 50%-150% range.
float mapSpeed(int rawValue) {
  int lo = POT5_CENTER - POT5_DEADZONE;
  int hi = POT5_CENTER + POT5_DEADZONE;
  if (rawValue < lo) {
    return map(rawValue, 0, lo, 50, 100) / 100.0f;
  } else if (rawValue > hi) {
    return map(rawValue, hi, 4095, 100, 150) / 100.0f;
  }
  return 1.0f;
}

void updateDSP() {
  smoothBass = (smoothBass * 3 + analogRead(POT2_PIN)) / 4;
  smoothMid  = (smoothMid * 3  + analogRead(POT3_PIN)) / 4;
  smoothHigh = (smoothHigh * 3 + analogRead(POT4_PIN)) / 4;
  smoothPot5 = (smoothPot5 * 3 + analogRead(POT5_PIN)) / 4;

  if (millis() - lastDspTime < 30) return;
  lastDspTime = millis();

  int8_t bassGain = mapEQ(smoothBass);
  int8_t midGain  = mapEQ(smoothMid);
  int8_t highGain = mapEQ(smoothHigh);

  bool toneChanged = (bassGain != lastBassGain || midGain != lastMidGain || highGain != lastHighGain);
  if (toneChanged) {
    AUDIO_LOCK();
    audio.setTone(bassGain, midGain, highGain);
    AUDIO_UNLOCK();
    lastBassGain = bassGain;
    lastMidGain  = midGain;
    lastHighGain = highGain;
  }

  if (pot5IsSpeedMode) {
    float speedMod = mapSpeed(smoothPot5);
    bool speedChanged = pot5NeedsReapply || (fabs(speedMod - lastAppliedSpeedModDsp) > 0.02f);
    if (speedChanged) {
      AUDIO_LOCK();
      if (audio.isRunning()) {
        uint32_t nativeRate = audio.getSampleRate();
        if (nativeRate > 0) {
          i2s_set_sample_rates(I2S_NUM_0, (uint32_t)(nativeRate * speedMod));
        }
      }
      AUDIO_UNLOCK();
      lastAppliedSpeedModDsp = speedMod;
      currentSpeedMod = speedMod;
      pot5NeedsReapply = false;
    }
  } else {
    int8_t balGain = mapBalance(smoothPot5);
    bool balChanged = pot5NeedsReapply || (balGain != lastAppliedBalGain);
    if (balChanged) {
      AUDIO_LOCK();
      audio.setBalance(balGain);
      AUDIO_UNLOCK();
      lastAppliedBalGain = balGain;
      currentPanValue = balGain;
      pot5NeedsReapply = false;
    }
  }
}

void resetInactivePot5Control() {
  if (pot5IsSpeedMode) {
    // Entering Speed mode -- reset pan to center.
    AUDIO_LOCK();
    audio.setBalance(0);
    AUDIO_UNLOCK();
    lastAppliedBalGain = 0;
    currentPanValue = 0;
  } else {
    // Entering Pan mode -- reset speed to native.
    AUDIO_LOCK();
    if (audio.isRunning()) {
      uint32_t nativeRate = audio.getSampleRate();
      if (nativeRate > 0) i2s_set_sample_rates(I2S_NUM_0, nativeRate);
    }
    AUDIO_UNLOCK();
    lastAppliedSpeedModDsp = 1.0f;
    currentSpeedMod = 1.0f;
  }
}

#if ENABLE_BATTERY_MON
// ==========================================
// 6b. BATTERY MONITORING
// ==========================================
void updateBattery() {
  if (millis() - lastBattRead < BATT_READ_INTERVAL_MS) return;
  lastBattRead = millis();

  int raw = analogRead(BATT_PIN);
  battSmoothedRaw = (battSmoothedRaw < 0) ? raw : (battSmoothedRaw * 0.9f + raw * 0.1f);

  float vAdc = (battSmoothedRaw / 4095.0f) * 3.3f;
  float vBatt = vAdc * BATT_DIVIDER_RATIO;
  float pct = (vBatt - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f;
  battPercent = (int)constrain(pct, 0.0f, 100.0f);
}
#endif

#if ENABLE_SPOTIFY_LIKE
String cachedSpotifyAccessToken = "";
unsigned long spotifyTokenExpiresAt = 0;

String base64Encode(const String &in) {
  size_t outLen = 0;
  mbedtls_base64_encode(NULL, 0, &outLen, (const unsigned char*)in.c_str(), in.length());
  unsigned char *buf = (unsigned char*)malloc(outLen + 1);
  if (!buf) return "";
  size_t written = 0;
  mbedtls_base64_encode(buf, outLen, &written, (const unsigned char*)in.c_str(), in.length());
  buf[written] = 0;
  String result = String((char*)buf);
  free(buf);
  return result;
}

bool refreshSpotifyAccessToken() {
  if (millis() < spotifyTokenExpiresAt && cachedSpotifyAccessToken != "") return true;

  WiFiClientSecure tokenClient;
  tokenClient.setInsecure();
  HTTPClient http;
  http.begin(tokenClient, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization", "Basic " + base64Encode(String(clientId) + ":" + String(clientSecret)));

  String body = "grant_type=refresh_token&refresh_token=" + String(refreshToken);
  int code = http.POST(body);

  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String resp = http.getString();
    int tIdx = resp.indexOf("\"access_token\":\"");
    if (tIdx != -1) {
      tIdx += strlen("\"access_token\":\"");
      int endQuote = resp.indexOf("\"", tIdx);
      cachedSpotifyAccessToken = resp.substring(tIdx, endQuote);

      long expiresIn = 3600;
      int eIdx = resp.indexOf("\"expires_in\":");
      if (eIdx != -1) {
        int numStart = eIdx + strlen("\"expires_in\":");
        expiresIn = resp.substring(numStart, resp.indexOf(",", numStart)).toInt();
      }
      spotifyTokenExpiresAt = millis() + (expiresIn - 60) * 1000UL; // refresh a minute early
      ok = true;
    }
  }
  http.end();
  return ok;
}

// PUT https://api.spotify.com/v1/me/tracks?ids=<id> -- Spotify's "Save
// Track" endpoint (the heart/Like button).
bool likeCurrentSpotifyTrack() {
  UI_LOCK();
  String trackId = currentSpotifyTrackId;
  UI_UNLOCK();
  if (trackId == "") return false;
  if (!refreshSpotifyAccessToken()) return false;

  WiFiClientSecure likeClient;
  likeClient.setInsecure();
  HTTPClient http;
  http.begin(likeClient, "https://api.spotify.com/v1/me/tracks?ids=" + trackId);
  http.addHeader("Authorization", "Bearer " + cachedSpotifyAccessToken);
  http.addHeader("Content-Length", "0");
  int code = http.PUT((uint8_t*)nullptr, 0);
  http.end();

  return code == 200 || code == 204;
}
#endif

#if ENABLE_GAPLESS_PREFETCH
// ==========================================
// 6d. GAPLESS PREFETCH (SD warm-up -- see the honesty note near the state
// declarations above for what this does and doesn't guarantee)
// ==========================================
void maybePrefetchNextTrack() {
  if (appState != STATE_SD_PLAYING || trackCount == 0) return;

  AUDIO_LOCK();
  bool running = audio.isRunning();
  int cur = running ? audio.getAudioCurrentTime() : 0;
  int dur = running ? audio.getAudioFileDuration() : 0;
  AUDIO_UNLOCK();

  if (!running || dur <= 0) return;

  int remaining = dur - cur;
  int nextIdx = (currentTrackIndex + 1) % trackCount;

  if (remaining <= 3 && remaining >= 0 && nextTrackPreopenedIdx != nextIdx) {
    AUDIO_LOCK();
    if (nextTrackPreopened) nextTrackPreopened.close();

    String path = trackList[nextIdx];
    if (!path.startsWith("/")) path = "/" + path;
    nextTrackPreopened = SD.open(path);

    if (nextTrackPreopened) {

      uint8_t warm[4096];
      nextTrackPreopened.read(warm, sizeof(warm));
      nextTrackPreopened.seek(0);
      nextTrackPreopenedIdx = nextIdx;
    }
    AUDIO_UNLOCK();
  }
}
#endif

void performWiFiSync() {
  AUDIO_LOCK();
  audio.stopSong();
  AUDIO_UNLOCK();
  isPlaying = false;

  DISPLAY_LOCK();

  tft.fillScreen(ST77XX_BLACK);
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_ACID);
  tft.setCursor(10, 10);
  tft.print("CONNECTING TO HUB...");

  HTTPClient http;
  String listUrl = String("http://") + SERVER_IP + ":5000/list";
  http.begin(listUrl);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(10, 30);
    tft.print("SERVER NOT FOUND!");
    http.end();
    delay(2000);
    DISPLAY_UNLOCK();
    enterMenu();
    return;
  }

  String fileList = http.getString();
  http.end();

  tft.fillRect(10, 10, 140, 10, ST77XX_BLACK); // Clear header
  tft.setCursor(10, 10);
  tft.print("SYNCING FILES...");

  int start = 0;
  int end = fileList.indexOf('\n');
  int yPos = 30;
  while (end != -1 || start < fileList.length()) {
    String filename = (end == -1) ? fileList.substring(start) : fileList.substring(start, end);
    filename.trim();

    if (filename.length() > 0) {
      String filepath = "/" + filename;
      if (!SD.exists(filepath)) {
        tft.fillRect(0, yPos, 160, 10, ST77XX_BLACK);
        tft.setTextColor(COL_DIM);
        tft.setCursor(10, yPos);
        tft.print("DL: " + filename.substring(0, 15) + "...");

        String dlUrl = String("http://") + SERVER_IP + ":5000/music/" + filename;
        dlUrl.replace(" ", "%20"); // Fix spaces in URLs

        http.begin(dlUrl);
        int dlCode = http.GET();
        if (dlCode == HTTP_CODE_OK) {
          AUDIO_LOCK();
          File f = SD.open(filepath, FILE_WRITE);
          if (f) {
            http.writeToStream(&f);
            f.close();
            tft.setTextColor(COL_ACID);
            tft.setCursor(140, yPos);
            tft.print("OK");
          }
          AUDIO_UNLOCK();
        }
        http.end();
        yPos += 12; 
        if (yPos > 110) yPos = 30; 
      }
    }
    if (end == -1) break;
    start = end + 1;
    end = fileList.indexOf('\n', start);
  }

  tft.fillRect(0, 10, 160, 10, ST77XX_BLACK);
  tft.setTextColor(COL_ACID);
  tft.setCursor(10, 10);
  tft.print("SYNC COMPLETE!");
  delay(2000);
  DISPLAY_UNLOCK();
  scanSDTracks();
  enterMenu();
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
  coverCacheValid = false;
  clearCoverCache();
  appState = STATE_SPOTIFY;
  screenNeedsFullDraw = true;
}

void enterSDPlayer() {
  appState = STATE_SD_BROWSE;
  screenNeedsFullDraw = true;
  listNeedsRedraw = true;
  browseIndex = 0;
  lastMenuEncoderValue = encoderValue;
  if (trackCount == 0) scanSDTracks();
}

void drawMenuStatic() {
  DISPLAY_LOCK();
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_DIMMER);
  tft.setCursor(4, 119);
  tft.print("made by cwik3");
  DISPLAY_UNLOCK();
#if ENABLE_BATTERY_MON
  drawBatteryIcon(136, 4);
#endif
}

void drawMenuButtons() {
  const int btnW = 130, btnH = 28, gap = 8;
  const int totalH = MENU_COUNT * btnH + (MENU_COUNT - 1) * gap;
  const int startY = (128 - totalH) / 2;
  DISPLAY_LOCK();
  for (int i = 0; i < MENU_COUNT; i++) {
    drawGlassButton(15, startY + i * (btnH + gap), btnW, btnH, menuItems[i], i == menuIndex);
  }
  DISPLAY_UNLOCK();
}

void drawSpotifyStatic() {
  DISPLAY_LOCK();
  drawCyberBackground();
 
  if (coverCacheValid) {
    drawCurrentSDCover();
  } else {
    drawGlassPanel(COVER_X, COVER_Y, COVER_W, COVER_H);
  }
  drawGlassPanel(INFO_X, INFO_Y, INFO_W, INFO_H);
  DISPLAY_UNLOCK();
 
  artistMarquee.text = "";
  trackMarquee.text = "";
  spotifyTransportDrawn = false;
}

void drawSpotifyDynamic() {
  // Snapshot the shared Spotify fields under uiStateMutex before using
  // them, since networkTask's spotifyCallback() can be mid-write to these
  // at any moment.
  UI_LOCK();
  long baseProgressMs = spotifyProgressMs;
  long durationMs = spotifyDurationMs;
  bool playingNow = spotifyIsPlaying;
  unsigned long capturedAt = spotifyProgressCapturedAt;
  UI_UNLOCK();

  DISPLAY_LOCK();
  tft.setTextSize(1);

  long elapsedMs = baseProgressMs;
  if (playingNow) {
    elapsedMs += (long)(millis() - capturedAt);
  }
  elapsedMs = constrain(elapsedMs, 0, durationMs);
  int curSec = elapsedMs / 1000;
  int durSec = durationMs / 1000;

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d", curSec / 60, curSec % 60, durSec / 60, durSec % 60);
  drawClearedText(INFO_X + 4, INFO_Y + 26, INFO_W - 8, timeBuf, COL_DIM, COL_PANEL);

  drawProgressBar(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, curSec, durSec);

  if (!spotifyTransportDrawn || playingNow != lastDrawnSpotifyPlaying) {
    lastDrawnSpotifyPlaying = playingNow;
    spotifyTransportDrawn = true;
  }
  DISPLAY_UNLOCK();

#if ENABLE_SPOTIFY_LIKE

  static bool likedRowActive = false;
  bool likeNow = millis() < likedFlashUntil;
  if (likeNow != likedRowActive) {
    DISPLAY_LOCK();
    if (likeNow) {
      drawClearedText(INFO_X + 4, INFO_Y + 44, INFO_W - 8, "<3 LIKED!", COL_MAGENTA, COL_PANEL);
    } else {
      drawClearedText(INFO_X + 4, INFO_Y + 44, INFO_W - 8, "", COL_DIM, COL_PANEL);
    }
    DISPLAY_UNLOCK();
    likedRowActive = likeNow;
  }
#endif
}

void drawSDPlayerStatic() {
  DISPLAY_LOCK();
  drawCyberBackground();
  trackMarquee.text = "";
  lastDrawnSDTrackIndex = -1;
  sdTransportDrawn = false;

  if (!sdMounted) {
    tft.setTextSize(1);
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 40);
    tft.print("SD CARD NOT DETECTED");
    DISPLAY_UNLOCK();
    return;
  }

  if (!isDjMixerActive) {
    drawGlassPanel(COVER_X, COVER_Y, COVER_W, COVER_H);
    drawGlassPanel(INFO_X, INFO_Y, INFO_W, INFO_H);
    drawCurrentSDCover(); 
    
    tft.setTextSize(1);
    tft.setTextColor(COL_ACID);
    tft.setCursor(120, 114);
    tft.print("[MIDI]");
    
  } else {
    for(int i=0; i<5; i++) lastDrawnSliders[i] = -1; 

    tft.setTextSize(1);
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 4);
    tft.print("DJ MIXER");

    // 5th label reflects whichever mode POT5 is currently in.
    const char* labels[5] = {"VOL", "BAS", "MID", "HIG", pot5IsSpeedMode ? "SPD" : "PAN"};
    for (int i = 0; i < 5; i++) {
      int cx = 16 + i * 32; 
      
      tft.drawFastVLine(cx, 40, 60, COL_DIMMER);
      tft.drawFastHLine(cx - 3, 40, 7, COL_DIMMER);
      tft.drawFastHLine(cx - 3, 100, 7, COL_DIMMER);
      
      tft.setTextColor(COL_DIM);
      int16_t bx, by; uint16_t bw, bh;
      tft.getTextBounds(labels[i], 0, 0, &bx, &by, &bw, &bh);
      tft.setCursor(cx - (bw / 2), 110);
      tft.print(labels[i]);
    }
  }
  DISPLAY_UNLOCK();
}

void drawSDPlayerDynamic() {
  if (!sdMounted) return;

  AUDIO_LOCK();
  bool running = audio.isRunning();
  int cur = running ? audio.getAudioCurrentTime() : 0;
  int dur = running ? audio.getAudioFileDuration() : 0;
  AUDIO_UNLOCK();

  DISPLAY_LOCK();
  if (!isDjMixerActive) {
    tft.setTextSize(1);
    if (currentTrackIndex != lastDrawnSDTrackIndex) {
      char trkBuf[16];
      snprintf(trkBuf, sizeof(trkBuf), "Track %d/%d", trackCount > 0 ? currentTrackIndex + 1 : 0, trackCount);
      drawClearedText(INFO_X + 4, INFO_Y + 8, INFO_W - 8, trkBuf, COL_ACID, COL_PANEL);
      lastDrawnSDTrackIndex = currentTrackIndex;
    }

    static float lastDrawnSpeed = -1.0f;
    static int8_t lastDrawnPan = -100;
    if (pot5IsSpeedMode) {
      if (fabs(currentSpeedMod - lastDrawnSpeed) > 0.005f) {
        drawSpeedIndicator(76, 114, 40, currentSpeedMod);
        lastDrawnSpeed = currentSpeedMod;
        lastDrawnPan = -100; // force a redraw if we switch back to pan mode
      }
    } else {
      if (currentPanValue != lastDrawnPan) {
        drawPanIndicator(76, 114, 40, currentPanValue);
        lastDrawnPan = currentPanValue;
        lastDrawnSpeed = -1.0f; // force a redraw if we switch back to speed mode
      }
    }

    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d", cur / 60, cur % 60, dur / 60, dur % 60);
    drawClearedText(INFO_X + 4, INFO_Y + 26, INFO_W - 8, timeBuf, COL_DIM, COL_PANEL);

    int volPct = readSmoothedVolume();
    char volBuf[16];
    snprintf(volBuf, sizeof(volBuf), "Vol:%2d", volPct);
    drawClearedText(INFO_X + 4, INFO_Y + 44, INFO_W - 8, volBuf, isPlaying ? COL_ACID : COL_MAGENTA, COL_PANEL);

    drawProgressBar(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, cur, dur);

    if (!sdTransportDrawn || isPlaying != lastDrawnSDPlaying) {
      lastDrawnSDPlaying = isPlaying;
      sdTransportDrawn = true;
    }

  } else {
    // Live pan/speed readout, right side near the "DJ MIXER" title -- runs
    // every dynamic pass (30ms cadence in DJ mode), same as the faders.
    static float lastDrawnSpeedDj = -1.0f;
    static int8_t lastDrawnPanDj = -100;
    if (pot5IsSpeedMode) {
      if (fabs(currentSpeedMod - lastDrawnSpeedDj) > 0.005f) {
        drawSpeedIndicator(100, 4, 56, currentSpeedMod);
        lastDrawnSpeedDj = currentSpeedMod;
        lastDrawnPanDj = -100;
      }
    } else {
      if (currentPanValue != lastDrawnPanDj) {
        drawPanIndicator(100, 4, 56, currentPanValue);
        lastDrawnPanDj = currentPanValue;
        lastDrawnSpeedDj = -1.0f;
      }
    }

    drawProgressBar(4, 25, 152, 4, cur, dur);

    int volVal = map(smoothVol, 0, 4095, 0, 60);
    int basVal = map(smoothBass, 0, 4095, 0, 60);
    int midVal = map(smoothMid, 0, 4095, 0, 60);
    int higVal = map(smoothHigh, 0, 4095, 0, 60);
    int pot5Val = map(smoothPot5, 0, 4095, 0, 60);

    int currentVals[5] = {volVal, basVal, midVal, higVal, pot5Val};

    for (int i = 0; i < 5; i++) {
      if (currentVals[i] != lastDrawnSliders[i]) {
        int cx = 16 + i * 32;
        
        if (lastDrawnSliders[i] != -1) {
          int oldY = 100 - lastDrawnSliders[i];
          tft.fillCircle(cx, oldY, 4, COL_BG_BOT); 
          tft.drawFastVLine(cx, oldY - 4, 9, COL_DIMMER); 
        }

        int newY = 100 - currentVals[i];
        
        uint16_t faderColor = (themeGradient[newY] > 0x7FFF) ? ST77XX_BLACK : ST77XX_WHITE;
        tft.fillCircle(cx, newY, 4, faderColor);
        
        if (i == 0 || i == 4) {
          tft.fillCircle(cx, newY, 1, COL_ACID); 
        }
        
        lastDrawnSliders[i] = currentVals[i];
      }
    }
  }
  DISPLAY_UNLOCK();
}

void drawSDBrowseStatic() {
  DISPLAY_LOCK();
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(4, 4);
  tft.print("SELECT TRACK");
  tft.setTextColor(COL_DIM);
  tft.setCursor(4, 116);
  DISPLAY_UNLOCK();
}

void drawSDBrowseList() {
  DISPLAY_LOCK();
  if (!sdMounted) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 50);
    tft.print("SD CARD NOT DETECTED");
    DISPLAY_UNLOCK();
    return;
  }
  if (trackCount == 0) {
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 50);
    tft.print("NO MP3 FILES FOUND");
    DISPLAY_UNLOCK();
    return;
  }

  int startY = 20;
  int visible = trackCount < 4 ? trackCount : 4;
  for (int i = 0; i < visible; i++) {
    int idx = (browseIndex + i) % trackCount;
    bool isSelected = (i == 0);
    drawGlassButton(8, startY + (i * 26), 144, 22, trackList[idx].c_str(), isSelected);
  }
  DISPLAY_UNLOCK();
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
      if (appState == STATE_MENU) {
        if (menuIndex == 0) enterSpotify();
        else if (menuIndex == 1) enterSDPlayer();
        else if (menuIndex == 2) {
          appState = STATE_SYNC;
          screenNeedsFullDraw = true;
        }
      } else if (appState == STATE_SD_BROWSE && trackCount > 0) {
        currentTrackIndex = browseIndex % trackCount;
        appState = STATE_SD_PLAYING;
        isDjMixerActive = false;
        screenNeedsFullDraw = true;
        playTrack(currentTrackIndex); 
      } else if (appState == STATE_SD_PLAYING) {
        isDjMixerActive = !isDjMixerActive;
        screenNeedsFullDraw = true;
      }
    }
  }

  encSwLastState = state;
}

void handleSelectionEncoder(int maxCount, int &idx) {
  if (maxCount <= 0) return;
  int delta = encoderValue - lastMenuEncoderValue;
  if (abs(delta) >= ENCODER_STEPS_PER_CLICK) {
    if (delta > 0) idx = (idx + 1) % maxCount;
    else idx = (idx - 1 + maxCount) % maxCount;
    lastMenuEncoderValue = encoderValue;
    listNeedsRedraw = true;
  }
}

void handleSW1SW2() {
  bool sw1State = digitalRead(SW1_PIN);
  bool sw2State = digitalRead(SW2_PIN);

#if ENABLE_SPOTIFY_LIKE
  // SW1 press/hold tracking. Short press keeps its original meaning
  // (skip); a long press ONLY does something different in STATE_SPOTIFY,
  // where it fires "Like" instead. SD player behavior is unchanged.
  if (sw1LastState == HIGH && sw1State == LOW) {
    sw1PressStart = millis();
    sw1LongTriggered = false;
  }
  if (sw1State == LOW && !sw1LongTriggered && millis() - sw1PressStart > LONG_PRESS_MS) {
    sw1LongTriggered = true;
    if (appState == STATE_SPOTIFY && wifiMulti.run() == WL_CONNECTED) {
      if (likeCurrentSpotifyTrack()) {
        likedFlashUntil = millis() + 1500;
      }
    }
  }
#endif

  // SW2 press/hold tracking. Short press keeps its original meaning
  // (Play/Pause, fires on release below); a long press during SD playback
  // toggles POT5 between Pan and Speed mode instead. Outside SD_PLAYING,
  // holding SW2 does nothing extra -- it just falls through to nothing on
  // release, same as before this feature existed.
  if (sw2LastState == HIGH && sw2State == LOW) {
    sw2PressStart = millis();
    sw2LongTriggered = false;
  }
  if (sw2State == LOW && !sw2LongTriggered && millis() - sw2PressStart > LONG_PRESS_MS) {
    sw2LongTriggered = true;
    if (appState == STATE_SD_PLAYING) {
      pot5IsSpeedMode = !pot5IsSpeedMode;
      resetInactivePot5Control();
      pot5NeedsReapply = true;
      screenNeedsFullDraw = true; // redraw DJ mixer label / readout immediately
    }
  }

  if (millis() - lastButtonAction > BUTTON_DEBOUNCE) {
    if (sw1LastState == LOW && sw1State == HIGH) {
      // Skip fires on release (not press) so a long-press-triggered Like
      // in STATE_SPOTIFY can preempt it via the wasLong check below.
      bool wasLong =
#if ENABLE_SPOTIFY_LIKE
        sw1LongTriggered;
#else
        false;
#endif
      if (!wasLong) {
        if (appState == STATE_SPOTIFY) {
          if (wifiMulti.run() == WL_CONNECTED) spotify.nextTrack();
        } else if (appState == STATE_SD_PLAYING) {
          playTrack(currentTrackIndex + 1);
        }
      }
      lastButtonAction = millis();
    }
    if (sw2LastState == LOW && sw2State == HIGH) {
      // Same pattern as SW1 above: a long-press-triggered mode toggle
      // preempts the short-press Play/Pause action on release.
      if (!sw2LongTriggered) {
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
      }
      lastButtonAction = millis();
    }
  }

  sw1LastState = sw1State;
  sw2LastState = sw2State;
}

void applyFallbackCover() {
  int randCover = random(1, 5); 
  String fallbackPath = "/backup" + String(randCover) + ".jpg";

  AUDIO_LOCK();
  if (SD.exists(fallbackPath)) {
    uint16_t imgW = 0, imgH = 0;
    TJpgDec.getFsJpgSize(&imgW, &imgH, fallbackPath.c_str(), SD);

    const int maxDim = COVER_W - 2;
    int scale = 1;
    while ((imgW / scale > maxDim) || (imgH / scale > maxDim)) {
      scale *= 2;
      if (scale >= 8) break; 
    }
    TJpgDec.setJpgScale(scale);

    int scaledW = imgW / scale;
    int scaledH = imgH / scale;
    int dX = COVER_CLIP_MINX + (maxDim - scaledW) / 2;
    int dY = COVER_CLIP_MINY + (maxDim - scaledH) / 2;

    clearCoverCache();
    TJpgDec.drawFsJpg(dX, dY, fallbackPath.c_str(), SD);
    coverCacheValid = true;
    coverThemeDirty = true;
    screenNeedsFullDraw = true;
  }
  AUDIO_UNLOCK();
}

// ==========================================
// 8b. NETWORK TASK (medium priority)
// ==========================================
// Owns everything that isn't real-time-critical audio and isn't direct user
// input: Wi-Fi sync, Spotify polling + cover art, and the "no embedded
// cover art at all" timeout. Keeping this off the UI task means a Spotify
// poll or a JPEG decode never makes the encoder/buttons feel laggy.
void networkTask(void *parameter) {
  while (true) {

    if (appState == STATE_SYNC) {
      performWiFiSync();
    }

    // --- Spotify status polling + cover art ---
    if (appState == STATE_SPOTIFY && millis() - lastSpotifyCheck > 5000) {
      if (wifiMulti.run() == WL_CONNECTED) {
        spotify.getCurrentlyPlaying(spotifyCallback);

        if (currentSpotifyUrl != lastSpotifyUrl && currentSpotifyUrl != "") {
          lastSpotifyUrl = currentSpotifyUrl;

          if (downloadSpotifyCover(currentSpotifyUrl)) {
            AUDIO_LOCK();
            File check = SD.open("/spoty.jpg");
            bool progressive = check && jpegIsProgressive(check);
            if (check) check.close();
            AUDIO_UNLOCK();
 
            if (progressive) {
              coverCacheValid = false;
              clearCoverCache();
              clearCoverBox();
              applyFallbackCover();
            } else {
              clearCoverCache(); 
              AUDIO_LOCK();
              TJpgDec.setJpgScale(1);
              TJpgDec.drawFsJpg(SPOTIFY_IMG_X, SPOTIFY_IMG_Y, "/spoty.jpg", SD);
              AUDIO_UNLOCK();

              if (coverLooksBlack()) {
                applyFallbackCover();
              } else {
                coverCacheValid = true;
                coverThemeDirty = true;
                screenNeedsFullDraw = true;
              }
            }
          } else {
            applyFallbackCover();
          }
        }
      } else {
        UI_LOCK();
        currentTrack = "Wi-Fi Lost!";
        currentArtist = "Searching...";
        UI_UNLOCK();
      }
      lastSpotifyCheck = millis();
    }

    // --- SD tracks with no embedded cover tag at all never trigger
    // audio_id3image(), so this timeout is what decides "not coming" and
    // shows a backup cover instead of leaving the panel blank forever.
    if (appState == STATE_SD_PLAYING && coverPending &&
        millis() - coverPendingSince > COVER_FETCH_TIMEOUT_MS) {
      coverPending = false;
      if (!coverCacheValid) {
        applyFallbackCover();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// 9. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  unsigned long waitStart = millis();
  while(!Serial && (millis() - waitStart < 2000)) {
    delay(10);
  }

  audioMutex = xSemaphoreCreateRecursiveMutex();
  displayMutex = xSemaphoreCreateRecursiveMutex();
  uiStateMutex = xSemaphoreCreateMutex();
#if ENABLE_FFT_SPECTRUM
  spectrumMutex = xSemaphoreCreateMutex();
#endif
  randomSeed(esp_random()); 

  Serial.println("\n--- Initializing Wi-Fi ---");
  WiFi.mode(WIFI_STA); 
  WiFi.disconnect(true);
  delay(100);

  wifiMulti.addAP(ssidHome, passHome);
  wifiMulti.addAP(ssidHotspot, passHotspot);
  
  Serial.print("Scanning Wi-Fi");
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected: " + WiFi.SSID());
  client.setInsecure();

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  pinMode(SW1_PIN, INPUT_PULLUP);
  pinMode(SW2_PIN, INPUT_PULLUP);
  
  analogReadResolution(12);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  initCyberPalette();

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(SD_MISO, INPUT_PULLUP);
  
  spiSD.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, spiSD, 4000000);

  if (!sdMounted) {
    Serial.println("Cold boot mount failed. Flushing SPI bus and warm-booting SD...");
    spiSD.end();
    delay(500);
    
    digitalWrite(SD_CS, HIGH);
    spiSD.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
    sdMounted = SD.begin(SD_CS, spiSD, 4000000);
  }

  if (sdMounted) {
    Serial.println("SD Card successfully mounted!");
  } else {
    Serial.println("CRITICAL FAULT: SD Card unresponsive.");
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(10);

  // Priority scheme: audio (3, high) > network (2, medium) > loop()/UI (1,
  // low, default Arduino priority -- unchanged). Audio stays pinned to
  // core 0 so it's never fighting the UI/network tasks for CPU time on
  // core 1; network is deliberately NOT pinned to core 0 so it can't add
  // jitter to decode timing either.
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 2, NULL, 1);

  lastMenuEncoderValue = encoderValue;
  enterMenu();
}

// ==========================================
// 10. MAIN LOOP -- low-priority UI task: encoder, buttons, TFT redraw.
// Nothing here blocks on the network or on SD decode work anymore; the
// heaviest thing it does is a screen redraw or a JPEG blit from RAM.
// ==========================================
void loop() {
  handleEncoderSwitch();
  handleSW1SW2();
  updateDSP();

  if (millis() - lastHeapLog > 15000) {
    lastHeapLog = millis();
  }

  if (millis() - lastMarqueeTime > MARQUEE_STEP_MS) {
    lastMarqueeTime = millis();
    if (appState == STATE_SPOTIFY) {
      UI_LOCK();
      String artistSnapshot = currentArtist;
      String trackSnapshot = currentTrack;
      UI_UNLOCK();

      marqueeSetText(artistMarquee, artistSnapshot, INFO_W - 8, COL_ACID, COL_PANEL);
      marqueeDraw(artistMarquee, INFO_X + 4, INFO_Y + 8, COL_PANEL);

      marqueeSetText(trackMarquee, trackSnapshot, NAME_W - 4, COL_ACID, COL_BG_BOT);
      marqueeDraw(trackMarquee, NAME_X, NAME_Y, COL_BG_BOT);
    } else if (appState == STATE_SD_PLAYING && sdMounted) {
      String name = trackCount > 0 ? trackList[currentTrackIndex] : "No files";
      if (name.endsWith(".mp3") || name.endsWith(".MP3")) {
        name = name.substring(0, name.length() - 4);
      }
      
      if (!isDjMixerActive) {
        marqueeSetText(trackMarquee, name, NAME_W - 4, COL_ACID, COL_BG_BOT);
        marqueeDraw(trackMarquee, NAME_X, NAME_Y, COL_BG_BOT);
      } else {
        marqueeSetText(trackMarquee, name, 152, COL_ACID, COL_BG_BOT);
        marqueeDraw(trackMarquee, 4, 15, COL_BG_BOT);
      }
    }
  }

  // --- STATE DISPATCH. Note there's no STATE_SYNC branch here anymore --
  // networkTask owns that screen entirely (see networkTask() above). ---
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

  if (appState == STATE_SD_PLAYING) {
    if (isDjMixerActive && millis() - lastUITime > 30) {
      drawSDPlayerDynamic();
      lastUITime = millis();
    } else if (!isDjMixerActive && millis() - lastUITime > 1000) {
      drawSDPlayerDynamic();
      lastUITime = millis();
    }
  }

  static unsigned long lastSpotifyUITime = 0;
  if (millis() - lastSpotifyUITime > 1000) {
    if (appState == STATE_SPOTIFY) {
      drawSpotifyDynamic();
    }
    lastSpotifyUITime = millis();
  }

#if ENABLE_BATTERY_MON
  updateBattery();
  static unsigned long lastBattDrawTime = 0;
  if (appState == STATE_MENU && millis() - lastBattDrawTime > 2000) {
    drawBatteryIcon(136, 4);
    lastBattDrawTime = millis();
  }
#endif

#if ENABLE_FFT_SPECTRUM
  static unsigned long lastSpectrumTime = 0;
  if (appState == STATE_SD_PLAYING && !isDjMixerActive && isPlaying &&
      millis() - lastSpectrumTime > 40) {
    computeSpectrum();
    drawSpectrumBars(4, 112, 112, 14);
    lastSpectrumTime = millis();
  }
#endif

#if ENABLE_GAPLESS_PREFETCH
  static unsigned long lastPrefetchCheck = 0;
  if (appState == STATE_SD_PLAYING && millis() - lastPrefetchCheck > 500) {
    maybePrefetchNextTrack();
    lastPrefetchCheck = millis();
  }
#endif
}