#include <vector>
#include <SD.h>
#include <SPI.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdio>
#include <WiFi.h>

// ESP32 Dev Kit                   SD Card Module
// ┌──────────────┐                ┌─────────────┐
// │        3.3V  ────────────────▶│ VCC         │
// │         GND  ────────────────▶│ GND         │
// │        GPIO19◀────────────────│ MISO        │
// │        GPIO23────────────────▶│ MOSI        │
// │        GPIO18────────────────▶│ SCK         │
// │         GPIO5────────────────▶│ CS          │
// └──────────────┘                └─────────────┘

// ESP32 Dev Kit                   PCM5100 DAC Module
// ┌──────────────┐                ┌─────────────┐
// │        3.3V  ────────────────▶│ VCC         │
// │         GND  ────────────────▶│ GND         │
// │        GPIO22────────────────▶│ DIN (DATA)  │
// │        GPIO26────────────────▶│ BCK (BITCLK)│
// │        GPIO25────────────────▶│ LRC (LRCLK) │
// └──────────────┘                └─────────────┘

#define SERIAL_OUTPUT 1

#if SERIAL_OUTPUT
#define LOG(...) Serial.printf(__VA_ARGS__)
#define LOGLN(...) Serial.println(__VA_ARGS__)
#else
#define LOG(...) (void)0
#define LOGLN(...) (void)0
#endif

#define SD_CS 5
#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22
#define BTN_NEXT 32

AudioFileSourceSD fileSrc;
AudioGeneratorMP3 mp3;
AudioOutputI2S audioOut;
std::vector<String> tracks;
std::vector<int> playOrder;
int orderPos = 0;
int current = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;

QueueHandle_t bookmarkQueue;
File bookmarkFile;

void shuffleOrder(int excludeIdx = -1)
{
  playOrder.clear();
  for (int i = 0; i < (int)tracks.size(); i++)
  {
    if (i != excludeIdx)
      playOrder.push_back(i);
  }
  // Fisher–Yates
  for (int i = playOrder.size() - 1; i > 0; i--)
  {
    int j = esp_random() % (i + 1);
    std::swap(playOrder[i], playOrder[j]);
  }
  orderPos = 0;
  LOG("🔀 Playlist shuffled%s\n",
      excludeIdx >= 0 ? " (excluding resumed track)" : "");
}

void loadTracks()
{
  File root = SD.open("/");
  while (File f = root.openNextFile())
  {
    if (!f.isDirectory())
    {
      String n = f.name();
      if (n.endsWith(".mp3") || n.endsWith(".MP3"))
      {
        tracks.push_back("/" + n);
      }
    }
    f.close();
  }
  root.close();
  LOG("Found %u tracks\n", (unsigned)tracks.size());
}

int getRandomIndex()
{
  if (tracks.size() < 2)
    return 0;
  int nxt;
  do
  {
    nxt = esp_random() % tracks.size();
  } while (nxt == current);
  return nxt;
}

void bookmarkTask(void *pv)
{
  uint32_t pos;
  for (;;)
  {
    if (xQueueReceive(bookmarkQueue, &pos, portMAX_DELAY) == pdTRUE)
    {
      if (!isPlaying || !bookmarkFile)
        continue;

      bookmarkFile.seek(0);

      // Format: "<index> <offset>\n" + padding to 32 bytes
      char line[33];
      snprintf(line, sizeof(line), "%d %u\n%-24s", current, pos, ""); // pad to 32 bytes
      bookmarkFile.write((const uint8_t *)line, 32);
      bookmarkFile.flush();
    }
  }
}

bool readBookmark(int &idx, uint32_t &off)
{
  if (!SD.exists("/bookmark.txt"))
    return false;
  File b = SD.open("/bookmark.txt", FILE_READ);
  if (!b)
    return false;
  String line = b.readStringUntil('\n');
  b.close();
  int read = sscanf(line.c_str(), "%d %u", &idx, &off);
  return (read == 2 && idx >= 0 && idx < (int)tracks.size());
}

void playTrack(int idx, uint32_t off = 0)
{
  current = idx;
  String &path = tracks[idx];
  LOG("▶️  Track %u: %s  (seek %u bytes)\n", idx, path.c_str(), off);
  if (!fileSrc.open(path.c_str()))
  {
    LOGLN("❌ file open failed");
    return;
  }
  if (off)
    fileSrc.seek(off, SEEK_SET);
  mp3.begin(&fileSrc, &audioOut);
  isPlaying = mp3.isRunning();
}

void nextTrack()
{
  // stop current decode & close file
  if (mp3.isRunning())
  {
    mp3.stop();
  }
  fileSrc.close();

  orderPos++;
  if (orderPos >= (int)playOrder.size())
  {
    shuffleOrder();
  }

  // pick & start a fresh one
  // int nxt = getRandomIndex();
  playTrack(playOrder[orderPos], 0);
}

void setup()
{
  WiFi.mode(WIFI_OFF);
  btStop();
#if SERIAL_OUTPUT
  Serial.begin(115200);
  while (!Serial)
  {
  }
  LOGLN("\n=== MP3 Shuffle w/ No-Stutter Bookmark ===");
#endif

  // optional: use dedicated SPI pins for SD to avoid I2S contention
  // SPI.begin(18, 19, 23, SD_CS);

  if (!SD.begin(SD_CS))
  {
    LOGLN("❌ SD init failed");
    while (1)
      delay(1000);
  }
  LOGLN("✅ SD OK");

  audioOut.SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audioOut.SetGain(0.25f); // -12 dB attenuation
  audioOut.begin();
  LOGLN("✅ I²S OK");

  loadTracks();
  if (tracks.empty())
  {
    LOGLN("❌ No MP3s");
    while (1)
      delay(1000);
  }

  int idx;
  uint32_t off;
  if (readBookmark(idx, off))
  {
    LOG("🔖 Resume track %d @ byte %u\n", idx, off);
    shuffleOrder(idx);
    playTrack(idx, off);
  }
  else
  {
    shuffleOrder();
    nextTrack();
  }

  // open bookmark file once for reuse
  bookmarkFile = SD.open("/bookmark.txt", FILE_WRITE);
  if (!bookmarkFile)
  {
    LOGLN("❌ Failed to open bookmark.txt for writing");
  }

  // button setup
  pinMode(BTN_NEXT, INPUT_PULLUP);

  bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
  xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 1, NULL, 0);
}

void loop()
{
  if (isPlaying)
  {
    if (mp3.isRunning())
    {
      bool active = mp3.loop();

      if (!active)
      {
        LOGLN("⏹️ loop() returned false — decoder done");
        isPlaying = false;
        uint32_t flushDummy = fileSrc.getPos();
        xQueueSend(bookmarkQueue, &flushDummy, 0);
        delay(10);
        nextTrack();
        return;
      }

      unsigned long now = millis();
      uint32_t pos = fileSrc.getPos();

      if (now - lastBookmarkMs > 5000)
      {
        if (pos > 0)
        {
          xQueueSend(bookmarkQueue, &pos, 0);
          lastBookmarkMs = now;
          LOG("✏️ Queued bookmark %u @ %u bytes\n", current, pos);
        }
      }
    }
    else
    {
      LOGLN("🔁 Track ended, shuffling...");
      isPlaying = false;
      uint32_t flushDummy = fileSrc.getPos();
      xQueueSend(bookmarkQueue, &flushDummy, 0);
      delay(10);
      nextTrack();
    }
  }

  // Single-skip on a button press with debounce
  static bool lastNextState = HIGH;
  static unsigned long lastDebounceTime = 0;
  const unsigned long debounceDelay = 50; // ms

  bool reading = digitalRead(BTN_NEXT);
  if (reading != lastNextState)
  {
    // reset the debounce timer on any state change
    lastDebounceTime = millis();
  }

  // only consider the reading if it's been stable for longer than the debounce delay
  if (millis() - lastDebounceTime > debounceDelay)
  {
    // if the button went from HIGH to LOW, it's a press
    static bool buttonPressed = false;
    if (reading == LOW && !buttonPressed)
    {
      nextTrack();
      buttonPressed = true;
    }
    // when the button is released, allow the next press
    if (reading == HIGH)
    {
      buttonPressed = false;
    }
  }

  lastNextState = reading;
}