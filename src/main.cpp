#include <Arduino.h>
#include "Audio.h"
#include <WiFi.h>

// --- Wi-Fi Credentials ---
// PUT YOUR HOME WIFI INFO HERE
const char* ssid = "T-Mobile_Swiatlowod_2589";
const char* password = "17511876877064774083";

// --- I2S / PCM5102A Pins ---
// Update these 3 pins to match wherever you wired them in your pinout!
#define I2S_LRCK  17  // LCK / WS (Word Select)
#define I2S_BCLK  16  // BCK / SCK (Bit Clock)
#define I2S_DOUT  18  // DIN (Data In)

Audio audio;

void setup() {
  Serial.begin(115200);
  
  // Smart Wait for Serial Monitor
  while (!Serial) { delay(10); }
  delay(500);

  Serial.println("\n======================================");
  Serial.println("   Starting PCM5102A Audio Test...");
  Serial.println("======================================");

  // 1. Connect to Wi-Fi
  Serial.printf("Connecting to WiFi: %s ", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[SUCCESS] WiFi Connected!");

  // 2. Setup I2S audio output
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  
  // Set volume (0-21). Keep it low at 10 to start so we don't blow out your ears!
  audio.setVolume(10); 

  // 3. Play a live internet radio stream
  Serial.println("Connecting to Live Radio Stream (Radio Swiss Jazz)...");
  
  // This URL is highly reliable for testing MP3 streams
  audio.connecttohost("http://stream.srg-ssr.ch/m/rsj/mp3_128"); 
}

void loop() {
  // The audio loop must run constantly to decode the MP3 stream and push it to the DAC
  audio.loop();
}

// --- Audio Library Callbacks for Serial Monitor Debugging ---
// These will automatically print metadata like the song name to your terminal!

void audio_info(const char *info){
    Serial.printf("Info: %s\n", info);
}

void audio_showstation(const char *info){
    Serial.printf("Station: %s\n", info);
}

void audio_showstreamtitle(const char *info){
    Serial.printf("Now Playing: %s\n", info);
}

void audio_error(const char *info){
    Serial.printf("Error: %s\n", info);
}