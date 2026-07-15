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
#define NAME_Y (COVER_Y + COVER_H + 14)        // 78
#define NAME_W (160 - (COVER_X * 2))          // 152

#define PROGRESS_X COVER_X
#define PROGRESS_Y (NAME_Y + 18)              // a bit more breathing room under the name row
#define PROGRESS_W (160 - (COVER_X * 2))      // 152
#define PROGRESS_H 4


#define COVER_CLIP_MINX (COVER_X + 1)
#define COVER_CLIP_MAXX (COVER_X + COVER_W - 2)
#define COVER_CLIP_MINY (COVER_Y + 1)
#define COVER_CLIP_MAXY (COVER_Y + COVER_H - 2)

#define SPOTIFY_IMG_X (COVER_X + 1 + ((COVER_W - 2 - 64) / 2))
#define SPOTIFY_IMG_Y (COVER_Y + 1 + ((COVER_H - 2 - 64) / 2))

// Fixed row height used to clear a text row before redrawing it (text size 1
// glyphs are 8px tall in this font). Used instead of concatenating trailing
// spaces onto Strings every redraw, which was creating a fresh heap
// allocation ~5x/second and slowly fragmenting the heap over long sessions.
#define TEXT_ROW_H 9

// ==========================================
// 3. GLOBAL OBJECTS & STATE
// ==========================================
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
SPIClass spiSD(HSPI);
Audio audio;

// This mutex now guards BOTH audio.loop() AND any direct SD-card access
// (SD.open/openNextFile/File::read/write, TJpgDec draws from SD) made from
// the main loop/core 1. audioTask runs pinned to core 0 and shares the same
// physical SPI bus/SD card with everything the main loop does on core 1;
// without a shared lock, concurrent SPI transactions from both cores can
// corrupt in-flight reads and crash the chip. It's recursive so nested
// locking (e.g. playTrack() called while already holding the lock) is safe.
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
bool jpegIsProgressive(File &f);

enum AppState { STATE_MENU, STATE_SPOTIFY, STATE_SD_BROWSE, STATE_SD_PLAYING };
AppState appState = STATE_MENU;

int browseIndex = 0;
bool screenNeedsFullDraw = true;
bool listNeedsRedraw = true;
bool sdMounted = false;

const char* menuItems[] = { "SPOTIFY STATUS", "SD CARD PLAYER" };
const int MENU_COUNT = 2;
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

// Tracks what's currently on screen so dynamic redraws can skip fields
// that haven't changed, instead of clearing+reprinting everything every
// tick -- this is what was making the screen feel like it was constantly
// flickering/refreshing. Time/volume use plain char buffers (no String
// allocation) since they're compared on every tick.
char lastDrawnSpotifyTimeBuf[16] = "";
bool lastDrawnSpotifyPlaying = true;
bool spotifyTransportDrawn = false;

int lastDrawnSDTrackIndex = -1;
char lastDrawnSDTimeBuf[16] = "";
char lastDrawnSDVolBuf[16] = "";
bool lastDrawnSDPlaying = true;
bool sdTransportDrawn = false;

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

// Draws text after clearing its row with a solid fillRect first. Replaces
// the old "text + a bunch of trailing spaces" trick, which allocated a new
// String on the heap on every single redraw (5x/sec while a screen is
// active) purely to blank out leftover pixels from a longer previous string.
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

// Cover/info panels: flat fill only now, no border or corner accents.
void drawGlassPanel(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, COL_PANEL);
}

void drawProgressBar(int x, int y, int w, int h, int curSec, int durSec) {
  tft.drawRect(x, y, w, h, COL_DIMMER);
  // Always clear the interior first. Previously only the new fillW was
  // drawn, so skipping to a shorter/newer track never erased the leftover
  // fill from the longer previous track -- that's the "bar doesn't reset"
  // bug after switching songs.
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, COL_PANEL);
  if (durSec > 0) {
    int fillW = map(constrain(curSec, 0, durSec), 0, durSec, 0, w - 2);
    tft.fillRect(x + 1, y + 1, fillW, h - 2, COL_ACID);
  }
}



// ==========================================
// 4b. MARQUEE (SCROLLING TEXT) HELPERS
// ==========================================
// Adafruit_GFX has no built-in clip rect for text, so a naive scroll would
// bleed pixels outside the box into whatever is drawn next to it. To get a
// real clip, the full string is rendered once into an off-screen 16-bit
// canvas, and each frame just blits a boxW-wide *slice* of that canvas to
// the screen at the current scroll offset -- no text re-rendering, and no
// per-frame heap allocation, on every scroll step.
struct Marquee {
  GFXcanvas16 *canvas;
  String text;          // text currently rendered into canvas (cache key)
  int textPixelW = 0;   // pixel width of that text
  int boxW = 0;          // width of the destination box on screen
  int scrollX = 0;
  bool scrolling = false;
  unsigned long lastStep = 0;
  unsigned long pauseUntil = 0;
  Marquee(GFXcanvas16 *c) : canvas(c) {}
};

const unsigned long MARQUEE_STEP_MS = 40;   // how often the scroll advances
const unsigned long MARQUEE_PAUSE_MS = 1000; // pause at the start of each loop
const int MARQUEE_GAP_PX = 16;               // blank gap before the text repeats

// 260px covers roughly 40-45 characters at text size 1 -- plenty for a
// track/artist name. Allocated once at startup and reused for the life of
// the program, so scrolling never touches the heap.
GFXcanvas16 artistCanvasBuf(260, TEXT_ROW_H);
GFXcanvas16 trackCanvasBuf(260, TEXT_ROW_H);
Marquee artistMarquee(&artistCanvasBuf);
Marquee trackMarquee(&trackCanvasBuf);

// Re-renders the canvas ONLY when the text actually changed (new song /
// artist / filename). boxW is the width available on screen; if the text
// is wider than that, it's flagged to scroll and a second copy is drawn
// after a gap so the loop wraps without a visible jump.
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

// Blits the currently visible boxW-wide slice of the canvas to (x,y) and
// advances the scroll offset if it's time to step. Call this frequently
// (much faster than the once-a-second field refresh) for a smooth scroll.
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
  if (y >= tft.height()) return 0;

  int minX = COVER_CLIP_MINX, maxX = COVER_CLIP_MAXX;
  int minY = COVER_CLIP_MINY, maxY = COVER_CLIP_MAXY;

  if (x > maxX || y > maxY || x + w - 1 < minX || y + h - 1 < minY) return 1;

  if (x >= minX && x + w - 1 <= maxX && y >= minY && y + h - 1 <= maxY) {
    tft.drawRGBBitmap(x, y, bitmap, w, h);
    return 1;
  }

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int px = x + i;
      int py = y + j;
      if (px >= minX && px <= maxX && py >= minY && py <= maxY) {
        tft.drawPixel(px, py, bitmap[j * w + i]);
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

// Quick-and-dirty progressive-vs-baseline JPEG check. TJpg_Decoder can ONLY
// decode baseline sequential JPEGs -- it silently fails on progressive
// JPEGs, leaving some MCU blocks undrawn (black). This scans for the SOF
// marker: 0xFFC0/0xFFC1 = baseline, 0xFFC2 (and friends) = progressive.
// NOTE: callers must already hold AUDIO_LOCK, since this does SD reads.
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

// Called synchronously from inside audio.loop() during ID3 parsing, which
// itself already runs under AUDIO_LOCK inside audioTask -- so this already
// has exclusive SD access. The explicit lock/unlock here is added anyway
// for safety since the mutex is recursive, in case this ever gets invoked
// from a different call path in the future.
void audio_id3image(File& file, const size_t pos, const size_t size) {
  Serial.printf("ID3 Image found! Size: %d bytes\n", size);

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

    File check = SD.open("/cover.jpg");
    bool progressive = check && jpegIsProgressive(check);
    if (check) check.close();

    if (progressive) {
      Serial.println("Embedded cover is a progressive JPEG -- decoder can't handle it, skipping draw.");
      clearCoverBox();
      AUDIO_UNLOCK();
      return;
    }

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

    TJpgDec.drawFsJpg(dX, dY, "/cover.jpg", SD);
  } else {
    Serial.println("Failed to create temp cover file.");
  }

  AUDIO_UNLOCK();
}

void spotifyCallback(CurrentlyPlaying currentlyPlaying) {
  if (currentlyPlaying.isPlaying) {
    currentTrack = String(currentlyPlaying.trackName);

    // Guard against zero-artist responses (podcasts, local files, some
    // compilation tracks). Indexing artists[0] unconditionally here was
    // reading an uninitialized pointer inside the library's response buffer
    // whenever numArtists was 0, which is exactly what produced the
    // LoadProhibited crash at EXCVADDR 0x00000001.
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

// NOTE: caller must hold AUDIO_LOCK around the SD-writing portion of this.
// The lock is taken internally here, around just the file open/write span,
// rather than around the whole HTTP GET -- holding it for the entire
// network transfer would stall SD-audio playback for as long as the
// download takes. Locking only the SD write still fully protects against
// SPI bus corruption, at the cost of a much shorter, bounded stall.
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
        Serial.printf("Cover download incomplete: got %d of %d bytes, skipping decode\n",
                      written, expectedLen);
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
// 6. SD CARD TRACK LIST
// ==========================================
void scanSDTracks() {
  AUDIO_LOCK();

  trackCount = 0;
  File root = SD.open("/");
  if (!root) {
    Serial.println("SD: failed to open root directory");
    AUDIO_UNLOCK();
    return;
  }

  Serial.println("\n--- SD CARD SCAN START ---");

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    String name = String(entry.name());
    Serial.printf("Saw file/folder: %s ", name.c_str());

    if (entry.isDirectory()) {
      Serial.println(" -> (Skipped: is a folder)");
      entry.close();
      continue;
    }

    String lower = name;
    lower.toLowerCase();

    if (lower.indexOf("._") != -1 || lower.indexOf("/.") != -1 || lower.indexOf("system") != -1) {
      Serial.println(" -> (Skipped: hidden system file)");
      entry.close();
      continue;
    }

    if (lower.endsWith(".mp3")) {
      if (trackCount < 40) {
        trackList[trackCount] = name;
        trackCount++;
        Serial.println(" -> (ADDED TO PLAYLIST!)");
      } else {
        Serial.println(" -> (Skipped: playlist maxed out at 40)");
      }
    } else {
      Serial.println(" -> (Skipped: not an .mp3 file)");
    }

    entry.close();
  }
  root.close();
  Serial.printf("--- SD: scan complete, %d tracks found ---\n\n", trackCount);

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

  if (appState == STATE_SD_PLAYING) {
    clearCoverBox();
  }

  AUDIO_LOCK();
  audio.connecttoFS(SD, path.c_str());
  AUDIO_UNLOCK();

  isPlaying = true;
}

int readSmoothedVolume() {
  int raw = analogRead(POT1_PIN);
  smoothVol = (smoothVol * 3 + raw) / 4;
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
  drawGlassPanel(COVER_X, COVER_Y, COVER_W, COVER_H);
  drawGlassPanel(INFO_X, INFO_Y, INFO_W, INFO_H);

  // Force every field to redraw once on this fresh screen entry. Clearing
  // the marquee's cached text makes marqueeSetText() treat it as brand new
  // on the very next fast-cadence tick (within MARQUEE_STEP_MS).
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

  // Time text genuinely changes every second, so it always redraws.
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d", curSec / 60, curSec % 60, durSec / 60, durSec % 60);
  drawClearedText(INFO_X + 4, INFO_Y + 26, INFO_W - 8, timeBuf, COL_DIM, COL_PANEL);

  // Artist + track name are drawn by the marquee updater in loop(), which
  // runs on a much faster cadence than this once-a-second pass -- a smooth
  // left-scroll needs to move a pixel or two far more often than 1x/sec.

  // Progress bar also updates every second (that's the point of it).
  drawProgressBar(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, curSec, durSec);

  // Transport icons only redrawn when play/pause state actually flips.
  if (!spotifyTransportDrawn || spotifyIsPlaying != lastDrawnSpotifyPlaying) {
    lastDrawnSpotifyPlaying = spotifyIsPlaying;
    spotifyTransportDrawn = true;
  }
}

void drawSDPlayerStatic() {
  drawCyberBackground();

  // Force every field to redraw once on this fresh screen entry.
  lastDrawnSDTrackIndex = -1;
  sdTransportDrawn = false;
  trackMarquee.text = "";

  if (!sdMounted) {
    tft.setTextSize(1);
    tft.setTextColor(COL_MAGENTA);
    tft.setCursor(4, 40);
    tft.print("SD CARD NOT DETECTED");
    return;
  }

  drawGlassPanel(COVER_X, COVER_Y, COVER_W, COVER_H);
  drawGlassPanel(INFO_X, INFO_Y, INFO_W, INFO_H);
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

void drawSDPlayerDynamic() {
  if (!sdMounted) return;
  tft.setTextSize(1);

  // Track number only changes when a new track starts, not every tick. The
  // track name itself is drawn by the marquee updater in loop().
  if (currentTrackIndex != lastDrawnSDTrackIndex) {
    char trkBuf[16];
    snprintf(trkBuf, sizeof(trkBuf), "Track %d/%d", trackCount > 0 ? currentTrackIndex + 1 : 0, trackCount);
    drawClearedText(INFO_X + 4, INFO_Y + 8, INFO_W - 8, trkBuf, COL_ACID, COL_PANEL);
    lastDrawnSDTrackIndex = currentTrackIndex;
  }

  AUDIO_LOCK();
  bool running = audio.isRunning();
  int cur = running ? audio.getAudioCurrentTime() : 0;
  int dur = running ? audio.getAudioFileDuration() : 0;
  AUDIO_UNLOCK();

  // Time genuinely changes every second, so it always redraws.
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d/%02d:%02d", cur / 60, cur % 60, dur / 60, dur % 60);
  drawClearedText(INFO_X + 4, INFO_Y + 26, INFO_W - 8, timeBuf, COL_DIM, COL_PANEL);

  // Volume can change any time the pot is turned, so it stays live too.
  int volPct = readSmoothedVolume();
  char volBuf[16];
  snprintf(volBuf, sizeof(volBuf), "Vol:%2d", volPct);
  drawClearedText(INFO_X + 4, INFO_Y + 44, INFO_W - 8, volBuf, isPlaying ? COL_ACID : COL_MAGENTA, COL_PANEL);

  drawProgressBar(PROGRESS_X, PROGRESS_Y, PROGRESS_W, PROGRESS_H, cur, dur);

  // Transport icons only redrawn when play/pause state actually flips.
  if (!sdTransportDrawn || isPlaying != lastDrawnSDPlaying) {
    lastDrawnSDPlaying = isPlaying;
    sdTransportDrawn = true;
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
        else enterSDPlayer();
      } else if (appState == STATE_SD_BROWSE && trackCount > 0) {
        currentTrackIndex = browseIndex % trackCount;
        appState = STATE_SD_PLAYING;
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

  // Lightweight heap watch. If minFreeHeap trends downward over a long
  // session, that indicates fragmentation/leak worth investigating further;
  // if it stays flat, string-allocation churn is not the culprit.
  if (millis() - lastHeapLog > 15000) {
    Serial.printf("Free heap: %u  Min free heap ever: %u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    lastHeapLog = millis();
  }

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
            Serial.println("Spotify cover is progressive JPEG -- skipping draw.");
            clearCoverBox();
          } else {
            clearCoverBox();
            AUDIO_LOCK();
            TJpgDec.setJpgScale(1);
            TJpgDec.drawFsJpg(SPOTIFY_IMG_X, SPOTIFY_IMG_Y, "/spoty.jpg", SD);
            AUDIO_UNLOCK();
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
  }

  // Marquee scrolling runs on its own fast cadence, independent of the
  // once-a-second field refresh below -- a smooth left-scroll needs to
  // move a pixel or two far more often than 1x/sec to not look choppy.
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
      marqueeSetText(trackMarquee, name, NAME_W - 4, COL_ACID, COL_BG_BOT);
      marqueeDraw(trackMarquee, NAME_X, NAME_Y, COL_BG_BOT);
    }
  }

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
      playTrack(currentTrackIndex);
    }
  }

  // Was 200ms (5Hz) -- nothing shown on these screens changes faster than
  // once a second, so redrawing 5x more often than needed was the main
  // source of the "constantly refreshing" feeling. Combined with the
  // change-detection above, this now redraws once a second and skips
  // fields that haven't actually changed.
  if (millis() - lastUITime > 1000) {
    if (appState == STATE_SPOTIFY) drawSpotifyDynamic();
    else if (appState == STATE_SD_PLAYING) drawSDPlayerDynamic();
    lastUITime = millis();
  }
}