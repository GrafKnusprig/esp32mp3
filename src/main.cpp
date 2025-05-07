#include <vector>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include "esp_system.h" // esp_random()
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstdio> // sscanf()

// ──────────────────────────────────────────────────────────────────────────────
// Toggle this to 0 to remove all Serial output in production:
#define SERIAL_OUTPUT 0

#if SERIAL_OUTPUT
#define LOG(...) Serial.printf(__VA_ARGS__)
#define LOGLN(...) Serial.println(__VA_ARGS__)
#else
#define LOG(...) (void)0
#define LOGLN(...) (void)0
#endif
// ──────────────────────────────────────────────────────────────────────────────

// pins
#define SD_CS 5
#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22

// globals
AudioFileSourceSD fileSrc;
AudioGeneratorMP3 mp3;
AudioOutputI2S audioOut;
std::vector<String> tracks;
int current = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;

// queue for bookmark positions
QueueHandle_t bookmarkQueue;

// scan SD root for MP3s
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

// pick a new random index ≠ current
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

// background task: write bookmarks to SD
void bookmarkTask(void *pv)
{
  uint32_t pos;
  for (;;)
  {
    if (xQueueReceive(bookmarkQueue, &pos, portMAX_DELAY) == pdTRUE)
    {
      File b = SD.open("/bookmark.txt", FILE_WRITE);
      if (b)
      {
        b.printf("%d %u\n", current, pos);
        b.close();
      }
    }
  }
}

// read "<idx> <byteOffset>" from bookmark.txt
bool readBookmark(int &idx, uint32_t &off)
{
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

// open and optionally seek, then begin decode
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

// shuffle to a new track
void nextTrack()
{
  playTrack(getRandomIndex(), 0);
}

void setup()
{
#if SERIAL_OUTPUT
  Serial.begin(115200);
  while (!Serial)
  {
  }
  LOGLN("\n=== MP3 Shuffle w/ No-Stutter Bookmark ===");
#endif

  if (!SD.begin(SD_CS))
  {
    LOGLN("❌ SD init failed");
    while (1)
      delay(1000);
  }
  LOGLN("✅ SD OK");

  audioOut.begin();
  LOGLN("✅ I²S OK");

  loadTracks();
  if (tracks.empty())
  {
    LOGLN("❌ No MP3s");
    while (1)
      delay(1000);
  }

  // resume if we have a bookmark
  int idx;
  uint32_t off;
  if (readBookmark(idx, off))
  {
    LOG("🔖 Resume track %d @ byte %u\n", idx, off);
    playTrack(idx, off);
  }
  else
  {
    nextTrack();
  }

  // create queue + background task
  bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
  xTaskCreatePinnedToCore(
      bookmarkTask, "bookmarkTask", 4096,
      NULL, 1, NULL, 0);
}

void loop()
{
  if (isPlaying)
  {
    if (mp3.isRunning())
    {
      mp3.loop();
      // enqueue bookmark every 5s without blocking
      unsigned long now = millis();
      if (now - lastBookmarkMs > 5000)
      {
        uint32_t pos = fileSrc.getPos();
        xQueueSend(bookmarkQueue, &pos, 0);
        lastBookmarkMs = now;
        LOG("✏️ Queued bookmark %u @ %u bytes\n", current, pos);
      }
    }
    else
    {
      LOGLN("🔁 Track ended, shuffling...");
      nextTrack();
    }
  }
}