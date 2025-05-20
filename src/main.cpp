// FreeRTOS includes in correct order
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
SemaphoreHandle_t sdMutex;
#include <vector>
#include <SD.h>
#include <SPI.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorFLAC.h>
#include <AudioOutputI2S.h>
#include "esp_system.h"
#include <freertos/queue.h>
#include <cstdio>
#include <WiFi.h>
#include <unordered_set>
#include <Button.h>

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
#define BTN_VOL_UP GPIO_NUM_33
#define BTN_VOL_DN GPIO_NUM_27
#define LED_PIN 2

class LazyShuffler
{
public:
    LazyShuffler(int start, int end)
        : start(start), end(end), total(end - start + 1), remaining(total)
    {
    }

    int next()
    {
        if (remaining == 0)
        {
            remaining = total;
            used.clear();
            history.clear();
        }

        int val;
        do
        {
            val = start + rand() % total;
        } while (used.find(val) != used.end());

        used.insert(val);
        history.push_back(val);
        remaining--;
        return val;
    }

    int last()
    {
        if (history.size() < 2)
            return -1;
        // Remove the latest number
        history.pop_back();
        int prev = history.back();
        used.erase(prev); // allow it to be picked again if needed
        return prev;
    }

private:
    int start, end, total, remaining;
    std::unordered_set<int> used;
    std::vector<int> history;
};

AudioFileSourceSD fileSrc;
AudioGeneratorMP3 mp3;
AudioGeneratorWAV wav;
AudioGeneratorFLAC flac;
AudioOutputI2S audioOut;
enum AudioType
{
    TYPE_MP3,
    TYPE_WAV,
    TYPE_FLAC,
    TYPE_UNKNOWN
};
AudioType currentType = TYPE_UNKNOWN;
int totalFiles = -1;
int currentIdx = -1;
unsigned long lastBookmarkMs = 0;
LazyShuffler shuffler(0, 0);

// fixed volume steps
const float volSteps[] = {
    0.02f, 0.03f, 0.04f, 0.05f,
    0.10f, 0.15f, 0.20f, 0.25f,
    0.30f, 0.40f, 0.50f, 0.60f,
    0.70f, 0.80f, 0.90f, 1.00f};
const int VOL_COUNT = sizeof(volSteps) / sizeof(volSteps[0]);

int volIndex = 7; // start at 0.05 (index 3)

QueueHandle_t bookmarkQueue;
File bookmarkFile;

bool writeIndexFile()
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    if (SD.exists("/index"))
    {
        LOGLN("Index found");
        xSemaphoreGive(sdMutex);
        return true;
    }

    File indexFile = SD.open("/index", FILE_WRITE);
    if (!indexFile)
    {
        LOGLN("Failed to create index file");
        xSemaphoreGive(sdMutex);
        return false;
    }

    // Helper lambda to recursively walk and write file paths
    std::function<void(File, String)> writePaths;
    int fileCount = 0;
    writePaths = [&](File dir, String path)
    {
        while (File f = dir.openNextFile())
        {
            if (f.isDirectory())
            {
                String folder = path + "/" + f.name();
                // Skip folders that start with '.'
                if (!String(f.name()).startsWith("."))
                {
                    writePaths(f, folder);
                }
            }
            else
            {
                String n = f.name();
                if (n.endsWith(".mp3") || n.endsWith(".MP3") ||
                    n.endsWith(".wav") || n.endsWith(".WAV") ||
                    n.endsWith(".flac") || n.endsWith(".FLAC"))
                {
                    String fullPath = path + "/" + n;
                    indexFile.println(fullPath);
                    fileCount++;
                }
            }
            f.close();
        }
    };

    // Write all file paths (unsorted) to the index file
    File root = SD.open("/");
    writePaths(root, "");
    root.close();
    indexFile.close();

    if (fileCount == 0)
    {
        SD.remove("/index");
        LOGLN("No audio files found for index.");
        xSemaphoreGive(sdMutex);
        return false;
    }

    LOGLN("Index file created");
    xSemaphoreGive(sdMutex);
    totalFiles = fileCount;
    LOG("Total files: %d\n", totalFiles);
    return true;
}

TaskHandle_t blinkTaskHandle = NULL;
void blinkLed(int times)
{
    if (blinkTaskHandle != NULL)
    {
        if (eTaskGetState(blinkTaskHandle) != eDeleted)
        {
            vTaskDelete(blinkTaskHandle);
        }
        blinkTaskHandle = NULL;
    }

    int *blinkCount = new int(times);

    xTaskCreate(
        [](void *param)
        {
            int times = *(int *)param;
            delete (int *)param;

            for (int i = 0; i < times; i++)
            {
                digitalWrite(LED_PIN, HIGH);
                vTaskDelay(40 / portTICK_PERIOD_MS);
                digitalWrite(LED_PIN, LOW);
                vTaskDelay(40 / portTICK_PERIOD_MS);
            }
            vTaskDelete(NULL);
            vTaskDelay(1);
        },
        "blinkTask", 1024, blinkCount, 1, &blinkTaskHandle);
}

void stopPlayback()
{
    if (mp3.isRunning())
        mp3.stop();
    if (wav.isRunning())
        wav.stop();
    if (flac.isRunning())
        flac.stop();
    fileSrc.close();
}

void playTrack(int idx, uint32_t off)
{
    LOG("playTrack() called with idx=%d\n", idx);

    if (idx < 0 || idx >= totalFiles)
    {
        LOG("Invalid track index: %d\n", idx);
        return;
    }

    String currentPath;
    // Read the path from the index file at line number idx

    LOGLN("Opening index file for reading path");
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    File indexFile = SD.open("/index", FILE_READ);
    if (indexFile)
    {
        for (int i = 0; i <= idx; ++i)
        {
            currentPath = indexFile.readStringUntil('\n');
        }
        indexFile.close();
        currentPath.trim();
        xSemaphoreGive(sdMutex);
    }
    else
    {
        LOGLN("Failed to open /index for reading path");
        xSemaphoreGive(sdMutex);
        return;
    }

    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (!fileSrc.open(currentPath.c_str()))
    {
        LOGLN("file open failed");
        xSemaphoreGive(sdMutex);
        return;
    }

    if (currentPath.endsWith(".mp3") || currentPath.endsWith(".MP3"))
    {
        currentIdx = idx;
        currentType = TYPE_MP3;
        if (off)
            fileSrc.seek(off, SEEK_SET);
        mp3.begin(&fileSrc, &audioOut);
    }
    else if (currentPath.endsWith(".wav") || currentPath.endsWith(".WAV"))
    {
        currentIdx = idx;
        currentType = TYPE_WAV;
        wav.begin(&fileSrc, &audioOut);
    }
    else if (currentPath.endsWith(".flac") || currentPath.endsWith(".FLAC"))
    {
        currentIdx = idx;
        currentType = TYPE_FLAC;
        if (off)
            fileSrc.seek(off, SEEK_SET);
        flac.begin(&fileSrc, &audioOut);
    }
    else
    {
        currentType = TYPE_UNKNOWN;
        LOGLN("unsupported file type");
    }
    LOG("Playing %s\n", currentPath.c_str());
    xSemaphoreGive(sdMutex);
}

void bookmarkTask(void *pv)
{
    uint32_t pos;
    for (;;)
    {
        if (xQueueReceive(bookmarkQueue, &pos, portMAX_DELAY) == pdTRUE)
        {
            if (!bookmarkFile)
                continue;

            xSemaphoreTake(sdMutex, portMAX_DELAY);
            bookmarkFile.seek(0);
            // Save: currentIdx, pos, currentMode, currentFolderIdx
            bookmarkFile.printf("%d %d %u\n", totalFiles, currentIdx, pos);
            bookmarkFile.flush();
            xSemaphoreGive(sdMutex);
        }
    }
}

bool readBookmark(int &idx, uint32_t &off, int &files)
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (!SD.exists("/bookmark"))
    {
        xSemaphoreGive(sdMutex);
        return false;
    }

    File f = SD.open("/bookmark", FILE_READ);
    if (!f)
    {
        xSemaphoreGive(sdMutex);
        return false;
    }

    String line = f.readStringUntil('\n');
    f.close();
    xSemaphoreGive(sdMutex);

    int read = sscanf(line.c_str(), "%d %d %u", &files, &idx, &off);

    return true;
}

void nextTrack()
{
    LOGLN("nextTrack() called");
    xQueueReset(bookmarkQueue);

    int next = shuffler.next();
    if (currentIdx == next)
    {
        next = shuffler.next();
    }

    playTrack(next, 0);
}

void previousTrack()
{
    LOGLN("previousTrack() called");
    xQueueReset(bookmarkQueue);

    int last = shuffler.last();
    if (last < 0)
    {
        LOGLN("No previous track available");
        playTrack(currentIdx, 0);
    }
    else
    {
        playTrack(last, 0);
    }
}

static void volumeDown()
{
    if (volIndex > 0)
    {
        volIndex--;
        audioOut.SetGain(volSteps[volIndex]);
        blinkLed(2);
        LOG("Vol: %.2f\n", volSteps[volIndex]);
    }
}

static void volumeUp()
{
    if (volIndex < VOL_COUNT - 1)
    {
        volIndex++;
        audioOut.SetGain(volSteps[volIndex]);
        blinkLed(2);
        LOG("Vol: %.2f\n", volSteps[volIndex]);
    }
}

static void onVolumeUpButtonSingleClick(void *button_handle, void *user_data)
{
    volumeUp();
    blinkLed(1);
}

static void onVolumeDownButtonSingleClick(void *button_handle, void *user_data)
{
    volumeDown();
    blinkLed(1);
}

bool volume_up_button_hold = false;
bool volume_down_button_hold = false;

static void onVolumeUpButtonLongPressRelease(void *button_handle, void *user_data)
{
    nextTrack();
    blinkLed(3);
    volume_up_button_hold = false;
}

static void onVolumeDownButtonLongPressRelease(void *button_handle, void *user_data)
{
    previousTrack();
    blinkLed(3);
    volume_down_button_hold = false;
}

static void onVolumeUpButtonLongPressHold(void *button_handle, void *user_data)
{
    if (volume_down_button_hold)
    {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
        SD.remove("/bookmark");
        SD.remove("/index");
        xSemaphoreGive(sdMutex);
        blinkLed(50);
        LOGLN("Bookmark and index deleted");
        esp_restart();
    }
    else
    {
        volume_up_button_hold = true;
    }
}

static void onVolumeDownButtonLongPressHold(void *button_handle, void *user_data)
{
    if (volume_up_button_hold)
    {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
        SD.remove("/bookmark");
        SD.remove("/index");
        xSemaphoreGive(sdMutex);
        LOGLN("Bookmark and index deleted");
        esp_restart();
    }
    else
    {
        volume_down_button_hold = true;
    }
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

    sdMutex = xSemaphoreCreateMutex();
    if (sdMutex == NULL)
    {
        LOGLN("Failed to create sdMutex!");
        while (true)
            delay(1000);
    }

    if (!SD.begin(SD_CS))
    {
        LOGLN("SD init failed");
        while (1)
            delay(1000);
    }
    LOGLN("SD OK");

    audioOut.SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audioOut.SetGain(volSteps[volIndex]);
    audioOut.begin();
    LOGLN("I²S OK");

    if (!writeIndexFile())
    {
        LOGLN("No MP3s");
        while (1)
            delay(1000);
    }

    int idx;
    uint32_t off;
    int mode;
    int foIdx;
    int total;
    bool bookmarkFound = readBookmark(idx, off, total);
    LOGLN(bookmarkFound ? "Bookmark file opened" : "No bookmark found");

    if (total > 0)
    {
        totalFiles = total;
    }

    shuffler = LazyShuffler(0, totalFiles - 1);

    if (bookmarkFound)
    {
        LOG("Bookmark: %d %u %u\n", idx, off, total);
        LOG("Resume track %d @ byte %u\n", idx, off);
        playTrack(idx, off);
    }
    else
    {
        playTrack(shuffler.next(), 0);
    }

    bookmarkFile = SD.open("/bookmark", FILE_WRITE);
    if (!bookmarkFile)
    {
        LOGLN("Failed to open bookmark for writing");
    }

    bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
    xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 2, NULL, 0);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED off by default

    Button *volUpBtn = new Button(BTN_VOL_UP, false);
    volUpBtn->attachSingleClickEventCb(onVolumeUpButtonSingleClick, NULL);
    volUpBtn->attachLongPressUpEventCb(onVolumeUpButtonLongPressRelease, NULL);
    volUpBtn->attachLongPressHoldEventCb(onVolumeUpButtonLongPressHold, NULL);

    Button *volDnBtn = new Button(BTN_VOL_DN, false);
    volDnBtn->attachSingleClickEventCb(onVolumeDownButtonSingleClick, NULL);
    volDnBtn->attachLongPressUpEventCb(onVolumeDownButtonLongPressRelease, NULL);
    volDnBtn->attachLongPressHoldEventCb(onVolumeDownButtonLongPressHold, NULL);
}

void loop()
{
    unsigned long now = millis();

    bool active = false;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    switch (currentType)
    {
    case TYPE_MP3:
        active = mp3.loop();
        break;
    case TYPE_WAV:
        active = wav.loop();
        break;
    case TYPE_FLAC:
        active = flac.loop();
        break;
    default:
        active = false;
        break;
    }
    xSemaphoreGive(sdMutex);

    if (!active)
    {
        LOGLN("track finished, playing next");
        delay(10);
        nextTrack();
        return;
    }

    else if (now - lastBookmarkMs > 1000)
    {
        unsigned long now = millis();
        uint32_t pos = 0;
        if (fileSrc.isOpen())
        {
            pos = fileSrc.getPos();
        }
        else
        {
            LOGLN("fileSrc not open during bookmark getPos()");
        }
        if (pos >= 0)
        {
            xQueueSend(bookmarkQueue, &pos, 0);
            lastBookmarkMs = now;
            LOG("Queued bookmark %u @ %u bytes\n", currentIdx, pos);
        }
    }
}
