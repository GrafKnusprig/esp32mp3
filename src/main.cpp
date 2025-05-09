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
// │        GPIO19────────────────▶│ MISO        │
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

// ESP32 Dev Kit                   Buttons
// ┌──────────────┐                ┌─────────────┐
// │        GPIO32────────────────▶│ BTN_NEXT    │
// │        GPIO33────────────────▶│ BTN_VOL_UP  │
// │        GPIO27────────────────▶│ BTN_VOL_DN  │
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
#define BTN_VOL_UP 33
#define BTN_VOL_DN 27

AudioFileSourceSD fileSrc;
AudioGeneratorMP3 mp3;
AudioOutputI2S audioOut;
std::vector<String> tracks;
std::vector<int> playOrder;
int orderPos = 0;
int current = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;

// define your fixed steps
const float volSteps[] = {
    0.02f, 0.03f, 0.04f, 0.05f,
    0.10f, 0.15f, 0.20f, 0.25f,
    0.30f, 0.40f, 0.50f, 0.60f,
    0.70f, 0.80f, 0.90f, 1.00f};
const int VOL_COUNT = sizeof(volSteps) / sizeof(volSteps[0]);

// track which step you’re on
int volIndex = 7; // start at 0.05 (index 3)

QueueHandle_t bookmarkQueue;
File bookmarkFile;

void updateShuffleFile()
{
  File shuffleFile = SD.open("/shuffle.txt", FILE_WRITE);
  if (shuffleFile)
  {
    shuffleFile.printf("orderPos:%d\n", orderPos);
    for (int idx : playOrder)
    {
      shuffleFile.printf("%d\n", idx);
    }
    shuffleFile.close();
    LOG("✅ Shuffle file updated\n");
  }
  else
  {
    LOG("❌ Failed to update shuffle.txt\n");
  }
}

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

  updateShuffleFile();
  LOG("✅ Shuffle list generated with %d tracks\n", (int)playOrder.size());
}

void validateShuffleList()
{
  if (playOrder.size() != tracks.size())
  {
    LOG("❌ Shuffle list size mismatch. Regenerating shuffle...\n");
    shuffleOrder();
  }
}

void loadShuffleFile(int excludeIdx = -1)
{
  if (!SD.exists("/shuffle.txt"))
  {
    LOG("❌ shuffle.txt not found\n");
    return;
  }

  File shuffleFile = SD.open("/shuffle.txt", FILE_READ);
  if (!shuffleFile)
  {
    LOG("❌ Failed to open shuffle.txt\n");
    return;
  }

  playOrder.clear();
  String firstLine = shuffleFile.readStringUntil('\n');
  if (firstLine.startsWith("orderPos:"))
  {
    orderPos = firstLine.substring(9).toInt();
  }
  else
  {
    LOG("❌ Invalid shuffle.txt format\n");
    shuffleFile.close();
    shuffleOrder(excludeIdx);
    return;
  }

  while (shuffleFile.available())
  {
    int idx = shuffleFile.readStringUntil('\n').toInt();
    if (idx >= 0 && idx < (int)tracks.size())
    {
      playOrder.push_back(idx);
    }
  }
  shuffleFile.close();

  if (playOrder.empty())
  {
    LOG("❌ shuffle.txt is empty or invalid\n");
    shuffleOrder(excludeIdx);
  }
  else
  {
    validateShuffleList();
    LOG("✅ Loaded shuffle.txt with %d tracks, starting at position %d\n", (int)playOrder.size(), orderPos);
  }
}

void loadTracksRecursive(File dir, String path = "")
{
  while (File f = dir.openNextFile())
  {
    if (f.isDirectory())
    {
      loadTracksRecursive(f, path + "/" + f.name());
    }
    else
    {
      String n = f.name();
      if (n.endsWith(".mp3") || n.endsWith(".MP3"))
      {
        tracks.push_back(path + "/" + n);
      }
    }
    f.close();
  }
}

void loadTracks()
{
  File root = SD.open("/");
  loadTracksRecursive(root);
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
  updateShuffleFile();
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
  audioOut.SetGain(volSteps[volIndex]);
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
    loadShuffleFile();
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
  pinMode(BTN_VOL_UP, INPUT_PULLUP);
  pinMode(BTN_VOL_DN, INPUT_PULLUP);

  bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
  xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 1, NULL, 0);
}

// Debounce+edge helper: returns true once when pin goes HIGH→LOW
bool checkButton(uint8_t pin,
                 bool &lastState,
                 unsigned long &lastDebounce,
                 bool &pressed,
                 unsigned long now)
{
  const unsigned long DEBOUNCE = 50;
  bool reading = digitalRead(pin);
  if (reading != lastState)
  {
    lastDebounce = now;
  }
  if (now - lastDebounce > DEBOUNCE)
  {
    if (reading == LOW && !pressed)
    {
      pressed = true;
      lastState = reading;
      return true;
    }
    if (reading == HIGH)
    {
      pressed = false;
    }
  }
  lastState = reading;
  return false;
}

void loop()
{
  unsigned long now = millis();
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

  // Static state per button
  static bool lastNextState = HIGH, lastUpState = HIGH, lastDnState = HIGH;
  static unsigned long lastNextDebounce = 0, lastUpDebounce = 0, lastDnDebounce = 0;
  static bool nextPressed = false, upPressed = false, dnPressed = false;

  // Skip button?
  if (checkButton(BTN_NEXT, lastNextState, lastNextDebounce, nextPressed, now))
  {
    nextTrack();
  }

  // Volume Up?
  if (checkButton(BTN_VOL_UP, lastUpState, lastUpDebounce, upPressed, now))
  {
    if (volIndex < VOL_COUNT - 1)
    {
      volIndex++;
      audioOut.SetGain(volSteps[volIndex]);
      LOG("🔊 Vol: %.2f\n", volSteps[volIndex]);
    }
  }

  // Volume Down?
  if (checkButton(BTN_VOL_DN, lastDnState, lastDnDebounce, dnPressed, now))
  {
    if (volIndex > 0)
    {
      volIndex--;
      audioOut.SetGain(volSteps[volIndex]);
      LOG("🔉 Vol: %.2f\n", volSteps[volIndex]);
    }
  }
}

