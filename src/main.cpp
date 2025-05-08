#include <vector>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdio>

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

AudioFileSourceSD fileSrc;
AudioGeneratorMP3 mp3;
AudioOutputI2S audioOut;
std::vector<String> tracks;
int current = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;

QueueHandle_t bookmarkQueue;

void loadTracks() {
  File root = SD.open("/");
  while (File f = root.openNextFile()) {
    if (!f.isDirectory()) {
      String n = f.name();
      if (n.endsWith(".mp3") || n.endsWith(".MP3")) {
        tracks.push_back("/" + n);
      }
    }
    f.close();
  }
  root.close();
  LOG("Found %u tracks\n", (unsigned)tracks.size());
}

int getRandomIndex() {
  if (tracks.size() < 2)
    return 0;
  int nxt;
  do {
    nxt = esp_random() % tracks.size();
  } while (nxt == current);
  return nxt;
}

void bookmarkTask(void *pv) {
  uint32_t pos;
  for (;;) {
    if (xQueueReceive(bookmarkQueue, &pos, portMAX_DELAY) == pdTRUE) {
      if (!isPlaying) continue;
      File b = SD.open("/bookmark.txt", FILE_WRITE);
      if (b) {
        b.printf("%d %u\n", current, pos);
        b.close();
      }
    }
  }
}

bool readBookmark(int &idx, uint32_t &off) {
  if (!SD.exists("/bookmark.txt"))
    return false;
  File b = SD.open("/bookmark.txt");
  if (!b)
    return false;
  String line = b.readStringUntil('\n');
  b.close();
  int read = sscanf(line.c_str(), "%d %u", &idx, &off);
  return (read == 2 && idx >= 0 && idx < (int)tracks.size());
}

void playTrack(int idx, uint32_t off = 0) {
  current = idx;
  String &path = tracks[idx];
  LOG("▶️  Track %u: %s  (seek %u bytes)\n", idx, path.c_str(), off);
  if (!fileSrc.open(path.c_str())) {
    LOGLN("❌ file open failed");
    return;
  }
  if (off)
    fileSrc.seek(off, SEEK_SET);
  mp3.begin(&fileSrc, &audioOut);
  isPlaying = mp3.isRunning();
}

void nextTrack() {
  playTrack(getRandomIndex(), 0);
}

void setup() {
#if SERIAL_OUTPUT
  Serial.begin(115200);
  while (!Serial) {}
  LOGLN("\n=== MP3 Shuffle w/ No-Stutter Bookmark ===");
#endif

  if (!SD.begin(SD_CS)) {
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
  if (tracks.empty()) {
    LOGLN("❌ No MP3s");
    while (1)
      delay(1000);
  }

  int idx;
  uint32_t off;
  if (readBookmark(idx, off)) {
    LOG("🔖 Resume track %d @ byte %u\n", idx, off);
    playTrack(idx, off);
  } else {
    nextTrack();
  }

  bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
  xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 1, NULL, 0);
}

void loop() {
  if (isPlaying) {
    if (mp3.isRunning()) {
      mp3.loop();
      unsigned long now = millis();
      if (now - lastBookmarkMs > 5000) {
        uint32_t pos = fileSrc.getPos();
        if (pos > 0) {
          xQueueSend(bookmarkQueue, &pos, 0);
          lastBookmarkMs = now;
          LOG("✏️ Queued bookmark %u @ %u bytes\n", current, pos);
        }
      }
    } else {
      LOGLN("🔁 Track ended, shuffling...");
      isPlaying = false;
      SD.remove("/bookmark.txt");
      nextTrack();
    }
  }
}