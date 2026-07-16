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
#define SERVER_IP "192.168.1.17"

WiFiMulti wifiMulti;
WiFiClientSecure client;
SpotifyArduino spotify(client, clientId, clientSecret, refreshToken);

// ==========================================
// 0. THEME CONFIG
// ==========================================
// Set this to 0 to turn OFF the cover-art-driven gradient/accent-color
// theme entirely and fall back to the plain default background gradient
// and fixed COL_ACID accent -- i.e. the "casual" look. Set back to 1 to
// re-enable sampling colors from the album art. This is the only thing
// you need to flip.
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
#define SW1_PIN 48
#define SW2_PIN 13

// ALL SAFE ADC1 PINS
#define POT1_PIN 1  // Master Volume
#define POT2_PIN 2  // Low/Bass EQ
#define POT3_PIN 7  // Mid EQ
#define POT4_PIN 8  // High/Treble EQ
#define POT5_PIN 9  // Stereo Balance/Pan 

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
unsigned long lastButtonAction = 0;
const unsigned long BUTTON_DEBOUNCE = 250;
String currentSpotifyUrl = "";
String lastSpotifyUrl = "none";

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

uint16_t themeGradient[160]; 

// ==========================================
// 3b. DSP / POTENTIOMETER STATE
// ==========================================
int smoothBass = 2048;
int smoothMid  = 2048;
int smoothHigh = 2048;
int smoothBal  = 2048;

unsigned long lastDspTime = 0;
int8_t lastBassGain = 100, lastMidGain = 100, lastHighGain = 100, lastBalance = 100;

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

  // Pre-fill the gradient so menus look good before the first song loads.
  // This is also what stays in effect permanently when USE_COVER_THEME is 0.
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
  // When USE_COVER_THEME is 0, this function is a no-op: themeGradient stays
  // whatever initCyberPalette() set it to, and COL_ACID stays at its fixed
  // default -- i.e. the plain, non-cover-driven look.
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

  for (int row = 0; row < TEXT_ROW_H; row++) {
    for (int col = 0; col < boxW; col++) {
      int srcCol = srcStart + col;
      sliceBuf[col] = (srcCol >= 0 && srcCol < canvasW) ? buf[row * canvasW + srcCol] : bg;
    }
    tft.drawRGBBitmap(x, y + row, sliceBuf, boxW, 1);
  }
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

void drawCurrentSDCover() {
  if (coverCacheValid) {
    tft.drawRGBBitmap(COVER_CLIP_MINX, COVER_CLIP_MINY, coverCache, CACHE_W, CACHE_H);
  }
}

// Fills the whole cover cache buffer with the panel background color.
// MUST be called right before decoding any new cover art into it.
//
// Why: coverCache is a persistent buffer that's never cleared between
// tracks. TJpgDec only writes the pixels of the actual decoded image --
// and different tracks' embedded art gets decoded at different JPEG
// scales (depending on its original resolution), so a new cover is often
// SMALLER on screen than the previous one. Without clearing first, the
// new (smaller) image only overwrites the center of the buffer, leaving
// the previous cover's edge pixels sitting in the outer ring -- which is
// exactly the "background remains, only the center changes" bug after
// skipping tracks. It only looked "reset" once a track with no embedded
// art came along and coverCacheValid went false, causing a plain panel
// fill instead of a stale blit.
void clearCoverCache() {
  for (int i = 0; i < CACHE_W * CACHE_H; i++) {
    coverCache[i] = COL_PANEL;
  }
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

      clearCoverCache(); // wipe stale pixels from the previous track's cover first
      TJpgDec.drawFsJpg(dX, dY, "/cover.jpg", SD);
      coverCacheValid = true;
      coverThemeDirty = true;      // was: updateThemeFromCover() called here directly
      screenNeedsFullDraw = true;
    }
  }
  AUDIO_UNLOCK();
}

void spotifyCallback(CurrentlyPlaying currentlyPlaying) {
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
  } else {
    currentTrack = "Nothing playing";
    currentArtist = "on Spotify";
    spotifyIsPlaying = false;
    currentSpotifyUrl = "";
    spotifyProgressMs = 0;
    spotifyDurationMs = 0;
  }
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
  tft.fillRect(COVER_X, COVER_Y, COVER_W, COVER_H, COL_PANEL);
}

void playTrack(int idx) {
  if (trackCount == 0) return;
  idx = ((idx % trackCount) + trackCount) % trackCount;
  currentTrackIndex = idx;
  String path = trackList[idx];
  if (!path.startsWith("/")) path = "/" + path;

  coverCacheValid = false;
  clearCoverCache(); // start the next track with a clean cache, not last track's leftovers
  AUDIO_LOCK();
  if (SD.exists("/cover.jpg")) {
    SD.remove("/cover.jpg"); 
  }
  audio.connecttoFS(SD, path.c_str());
  AUDIO_UNLOCK();

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

void updateDSP() {
  smoothBass = (smoothBass * 3 + analogRead(POT2_PIN)) / 4;
  smoothMid  = (smoothMid * 3  + analogRead(POT3_PIN)) / 4;
  smoothHigh = (smoothHigh * 3 + analogRead(POT4_PIN)) / 4;
  smoothBal  = (smoothBal * 3  + analogRead(POT5_PIN)) / 4;

  if (millis() - lastDspTime < 30) return;
  lastDspTime = millis();

  int8_t bassGain = mapEQ(smoothBass);
  int8_t midGain  = mapEQ(smoothMid);
  int8_t highGain = mapEQ(smoothHigh);

  int8_t balance = 0;
  if (smoothBal < 1900) balance = map(smoothBal, 0, 1900, -16, 0);
  else if (smoothBal > 2200) balance = map(smoothBal, 2200, 4095, 0, 16);

  bool toneChanged = (bassGain != lastBassGain || midGain != lastMidGain || highGain != lastHighGain);
  bool balChanged = (balance != lastBalance);

  if (toneChanged || balChanged) {
    AUDIO_LOCK();
    if (toneChanged) audio.setTone(bassGain, midGain, highGain);
    if (balChanged) audio.setBalance(balance);
    AUDIO_UNLOCK();

    lastBassGain = bassGain;
    lastMidGain = midGain;
    lastHighGain = highGain;
    lastBalance = balance;
  }
}

void performWiFiSync() {
  // 1. Stop audio playback to free up the SD card SPI bus safely
  AUDIO_LOCK();
  audio.stopSong();
  AUDIO_UNLOCK();
  isPlaying = false;

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
        if (yPos > 110) yPos = 30; // Wrap text if too many files
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
  drawCyberBackground();
  tft.setTextSize(2);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(22, 6);
  tft.print("PRO-DASH");
  tft.setTextSize(1);
  tft.setTextColor(COL_DIM);
  tft.setCursor(10, 112);
}

void drawMenuButtons() {
  const int btnW = 130, btnH = 28, gap = 8, startY = 38;
  for (int i = 0; i < MENU_COUNT; i++) {
    drawGlassButton(15, startY + i * (btnH + gap), btnW, btnH, menuItems[i], i == menuIndex);
  }
}

void drawSpotifyStatic() {
  drawCyberBackground();
 
  if (coverCacheValid) {
    drawCurrentSDCover();
  } else {
    drawGlassPanel(COVER_X, COVER_Y, COVER_W, COVER_H);
  }
  drawGlassPanel(INFO_X, INFO_Y, INFO_W, INFO_H);
 
  artistMarquee.text = "";
  trackMarquee.text = "";
  spotifyTransportDrawn = false;
}

void drawSpotifyDynamic() {
  tft.setTextSize(1);

  long elapsedMs = spotifyProgressMs;
  if (spotifyIsPlaying) {
    elapsedMs += (long)(millis() - spotifyProgressCapturedAt);
  }
  elapsedMs = constrain(elapsedMs, 0, spotifyDurationMs);
  int curSec = elapsedMs / 1000;
  int durSec = spotifyDurationMs / 1000;

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d", curSec / 60, curSec % 60, durSec / 60, durSec % 60);
  drawClearedText(INFO_X + 4, INFO_Y + 26, INFO_W - 8, timeBuf, COL_DIM, COL_PANEL);

  drawProgressBar(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, curSec, durSec);

  if (!spotifyTransportDrawn || spotifyIsPlaying != lastDrawnSpotifyPlaying) {
    lastDrawnSpotifyPlaying = spotifyIsPlaying;
    spotifyTransportDrawn = true;
  }
}

void drawSDPlayerStatic() {
  drawCyberBackground();
  trackMarquee.text = "";
  lastDrawnSDTrackIndex = -1;
  sdTransportDrawn = false;

  if (!sdMounted) {
    tft.setTextSize(1);
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 40);
    tft.print("SD CARD NOT DETECTED");
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

    tft.setTextColor(COL_DIM);
    tft.setCursor(120, 4);
    tft.print("[BACK]");

    const char* labels[] = {"VOL", "BAS", "MID", "HIG", "PAN"};
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
}

void drawSDPlayerDynamic() {
  if (!sdMounted) return;

  AUDIO_LOCK();
  bool running = audio.isRunning();
  int cur = running ? audio.getAudioCurrentTime() : 0;
  int dur = running ? audio.getAudioFileDuration() : 0;
  AUDIO_UNLOCK();

  if (!isDjMixerActive) {
    tft.setTextSize(1);
    if (currentTrackIndex != lastDrawnSDTrackIndex) {
      char trkBuf[16];
      snprintf(trkBuf, sizeof(trkBuf), "Track %d/%d", trackCount > 0 ? currentTrackIndex + 1 : 0, trackCount);
      drawClearedText(INFO_X + 4, INFO_Y + 8, INFO_W - 8, trkBuf, COL_ACID, COL_PANEL);
      lastDrawnSDTrackIndex = currentTrackIndex;
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
    drawProgressBar(4, 25, 152, 4, cur, dur);

    int volVal = map(smoothVol, 0, 4095, 0, 60);
    int basVal = map(smoothBass, 0, 4095, 0, 60);
    int midVal = map(smoothMid, 0, 4095, 0, 60);
    int higVal = map(smoothHigh, 0, 4095, 0, 60);
    int panVal = map(smoothBal, 0, 4095, 0, 60);

    int currentVals[5] = {volVal, basVal, midVal, higVal, panVal};

    for (int i = 0; i < 5; i++) {
      if (currentVals[i] != lastDrawnSliders[i]) {
        int cx = 16 + i * 32;
        
        if (lastDrawnSliders[i] != -1) {
          int oldY = 100 - lastDrawnSliders[i];
          tft.fillCircle(cx, oldY, 4, COL_BG_BOT); 
          tft.drawFastVLine(cx, oldY - 4, 9, COL_DIMMER); 
        }

        int newY = 100 - currentVals[i];
        
        // ADAPTIVE COLOR FADER LOGIC: Selects black or white based on the gradient brightness behind it
        uint16_t faderColor = (themeGradient[newY] > 0x7FFF) ? ST77XX_BLACK : ST77XX_WHITE;
        tft.fillCircle(cx, newY, 4, faderColor);
        
        if (i == 0 || i == 4) {
          tft.fillCircle(cx, newY, 1, COL_ACID); 
        }
        
        lastDrawnSliders[i] = currentVals[i];
      }
    }
  }
}

void drawSDBrowseStatic() {
  drawCyberBackground();
  tft.setTextSize(1);
  tft.setTextColor(COL_MAGENTA);
  tft.setCursor(4, 4);
  tft.print("SELECT TRACK");
  tft.setTextColor(COL_DIM);
  tft.setCursor(4, 116);
}

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
    bool isSelected = (i == 0);
    drawGlassButton(8, startY + (i * 26), 144, 22, trackList[idx].c_str(), isSelected);
  }
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

  if (millis() - lastButtonAction > BUTTON_DEBOUNCE) {
    if (sw1LastState == HIGH && sw1State == LOW) {
      if (appState == STATE_SPOTIFY) {
        if (wifiMulti.run() == WL_CONNECTED) spotify.nextTrack();
      } else if (appState == STATE_SD_PLAYING) {
        playTrack(currentTrackIndex + 1);
      }
      lastButtonAction = millis();
    }
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
  
  delay(250); 
  while (!Serial) { delay(10); }

  audioMutex = xSemaphoreCreateRecursiveMutex();

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

  spiSD.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  sdMounted = SD.begin(SD_CS, spiSD, 4000000);

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

  if (millis() - lastHeapLog > 15000) {
    lastHeapLog = millis();
  }

  // --- SPOTIFY THEME AND COVER DOWNLOAD LOGIC ---
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
          } else {
            clearCoverCache(); // wipe stale pixels from the previous cover first
            AUDIO_LOCK();
            TJpgDec.setJpgScale(1);
            TJpgDec.drawFsJpg(SPOTIFY_IMG_X, SPOTIFY_IMG_Y, "/spoty.jpg", SD);
            AUDIO_UNLOCK();
 
            coverCacheValid = true;
            coverThemeDirty = true;
            screenNeedsFullDraw = true;
          }
        }
      }
    } else {
      currentTrack = "Wi-Fi Lost!";
      currentArtist = "Searching...";
    }
    lastSpotifyCheck = millis();
  }

  if (appState == STATE_SD_PLAYING) {
    int vol = readSmoothedVolume();
    AUDIO_LOCK();
    audio.setVolume(vol);
    AUDIO_UNLOCK();
    
    updateDSP(); 
  }

  if (millis() - lastMarqueeTime > MARQUEE_STEP_MS) {
    lastMarqueeTime = millis();
    if (appState == STATE_SPOTIFY) {
      marqueeSetText(artistMarquee, currentArtist, INFO_W - 8, COL_ACID, COL_PANEL);
      marqueeDraw(artistMarquee, INFO_X + 4, INFO_Y + 8, COL_PANEL);

      marqueeSetText(trackMarquee, currentTrack, NAME_W - 4, COL_ACID, COL_BG_BOT);
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

  // --- STATE DISPATCH (one branch runs per loop, based on appState) ---
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
  else if (appState == STATE_SYNC) {
    if (screenNeedsFullDraw) {
      screenNeedsFullDraw = false;
      performWiFiSync(); // <--- Run the download engine!
    }
  }

  // --- SD PLAYER REDRAW TIMER (separate concern from state dispatch above:
  // DJ-mixer sliders need a much faster refresh than the plain now-playing
  // view, so this runs its own cadence check rather than living inside the
  // if/else-if chain above). ---
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
}