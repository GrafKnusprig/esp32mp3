// FreeRTOS includes in correct order
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
SemaphoreHandle_t sdMutex;
#include <vector>
#include <SD.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorFLAC.h>
#include <AudioOutputI2S.h>
#include "esp_system.h"
#include <freertos/queue.h>
#include <WiFi.h>
#include <unordered_set>
#include <Button.h>
#include <Adafruit_NeoPixel.h>

// Pin definitions
#define SD_CS 5
#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22
#define BTN_VOL_UP GPIO_NUM_33
#define BTN_VOL_DN GPIO_NUM_27
#define BTN_MODE   GPIO_NUM_32
#define LED_PIN 2
#define NEOPIXEL_PIN 21
#define NEOPIXEL_COUNT 1

// NeoPixel setup
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
uint8_t ledMode = 0; // 0=OFF, 1=RED, 2=GREEN, 3=BLUE
const uint32_t ledColors[] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF};
void updateNeoPixel() {
    strip.setPixelColor(0, ledColors[ledMode]);
    strip.show();
}

// Serial logging
#define SERIAL_OUTPUT 1
#if SERIAL_OUTPUT
#define LOG(...) Serial.printf(__VA_ARGS__)
#define LOGLN(...) Serial.println(__VA_ARGS__)
#else
#define LOG(...) (void)0
#define LOGLN(...) (void)0
#endif

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
            val = start + esp_random() % total;
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
        used.erase(history.back());
        history.pop_back();
        int prev = history.back();
        return prev;
    }
private:
    int start, end, total, remaining;
    std::unordered_set<int> used;
    std::vector<int> history;
};

AudioFileSourceSD *fileSrc = nullptr;
AudioGeneratorMP3 *mp3 = nullptr;
AudioGeneratorWAV *wav = nullptr;
AudioGeneratorFLAC *flac = nullptr;
AudioOutputI2S *audioOut = nullptr;
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
bool lockLoop = false;
unsigned long lastSkip = 0;

// fixed gain
const float volSteps[] = { 0.05f };
const int VOL_COUNT = 1;
int volIndex = 0; // Always 0

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
    std::function<void(File, String)> writePaths;
    int fileCount = 0;
    writePaths = [&](File dir, String path)
    {
        while (File f = dir.openNextFile())
        {
            if (f.isDirectory())
            {
                String folder = path + "/" + f.name();
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
    File root = SD.open("/");
    writePaths(root, "");
    root.close();
    indexFile.flush();
    indexFile.close();
    delay(1000);
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

void blinkWelcomeMessage()
{
    const char *morse = ".... . .-.. .-.. ---   - .... . .-. .";
    const int dotLen = 10;
    const int dashLen = 3 * dotLen;
    const int intraCharGap = dotLen;
    const int interCharGap = 3 * dotLen;
    const int interWordGap = 7 * dotLen;
    if (blinkTaskHandle != NULL)
    {
        if (eTaskGetState(blinkTaskHandle) != eDeleted)
        {
            vTaskDelete(blinkTaskHandle);
        }
        blinkTaskHandle = NULL;
    }
    xTaskCreate(
        [](void *param)
        {
            const char *morse = (const char *)param;
            for (const char *p = morse; *p; ++p)
            {
                if (*p == '.')
                {
                    digitalWrite(LED_PIN, HIGH);
                    vTaskDelay(dotLen / portTICK_PERIOD_MS);
                    digitalWrite(LED_PIN, LOW);
                    vTaskDelay(intraCharGap / portTICK_PERIOD_MS);
                }
                else if (*p == '-')
                {
                    digitalWrite(LED_PIN, HIGH);
                    vTaskDelay(dashLen / portTICK_PERIOD_MS);
                    digitalWrite(LED_PIN, LOW);
                    vTaskDelay(intraCharGap / portTICK_PERIOD_MS);
                }
                else if (*p == ' ')
                {
                    if (*(p + 1) == ' ' && *(p + 2) == ' ')
                    {
                        vTaskDelay((interWordGap - intraCharGap) / portTICK_PERIOD_MS);
                        p += 2;
                    }
                    else
                    {
                        vTaskDelay((interCharGap - intraCharGap) / portTICK_PERIOD_MS);
                    }
                }
            }
            vTaskDelete(NULL);
            vTaskDelay(1);
        },
        "morseBlink", 1024, (void *)morse, 1, &blinkTaskHandle);
}

void stopPlayback()
{
    if (mp3 && mp3->isRunning())
    {
        mp3->stop();
        delete mp3;
        mp3 = nullptr;
    }
    if (wav && wav->isRunning())
    {
        wav->stop();
        delete wav;
        wav = nullptr;
    }
    if (flac && flac->isRunning())
    {
        flac->stop();
        delete flac;
        flac = nullptr;
    }
    if (fileSrc)
    {
        fileSrc->close();
        delete fileSrc;
        fileSrc = nullptr;
    }
}

uint32_t skipID3v2Tag(AudioFileSource *src)
{
    char header[10];
    if (src->read((uint8_t *)header, 10) != 10)
        return 0;
    if (memcmp(header, "ID3", 3) != 0)
    {
        src->seek(0, SEEK_SET); // not an ID3v2 tag
        return 0;
    }
    uint32_t tagSize =
        ((header[6] & 0x7F) << 21) |
        ((header[7] & 0x7F) << 14) |
        ((header[8] & 0x7F) << 7) |
        (header[9] & 0x7F);
    uint32_t skipBytes = tagSize + 10; // +10 header
    src->seek(skipBytes, SEEK_SET);
    return skipBytes;
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
    LOGLN("Opening index file for reading path");
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    lockLoop = true;
    xQueueReset(bookmarkQueue);
    stopPlayback();
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
        lockLoop = false;
        return;
    }
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    fileSrc = new AudioFileSourceSD();
    if (currentPath.isEmpty() || !fileSrc->open(currentPath.c_str()))
    {
        LOGLN("file open failed");
        delete fileSrc;
        fileSrc = nullptr;
        xSemaphoreGive(sdMutex);
        lockLoop = false;
        return;
    }
    // Ensure audioOut is allocated
    if (!audioOut)
    {
        audioOut = new AudioOutputI2S();
        audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
        audioOut->begin();
        audioOut->SetGain(0.05f); // fixed gain
    }
    else
    {
        audioOut->SetGain(0.05f); // fixed gain
    }
    if (currentPath.endsWith(".mp3") || currentPath.endsWith(".MP3"))
    {
        currentIdx = idx;
        currentType = TYPE_MP3;
        if (off == 0)
        {
            uint32_t skipped = skipID3v2Tag(fileSrc);
            LOG("Skipped %u bytes of ID3v2 tag\n", skipped);
        }
        else
        {
            fileSrc->seek(off, SEEK_SET);
        }
        mp3 = new AudioGeneratorMP3();
        mp3->begin(fileSrc, audioOut);
    }
    else if (currentPath.endsWith(".wav") || currentPath.endsWith(".WAV"))
    {
        currentIdx = idx;
        currentType = TYPE_WAV;
        wav = new AudioGeneratorWAV();
        wav->begin(fileSrc, audioOut);
    }
    else if (currentPath.endsWith(".flac") || currentPath.endsWith(".FLAC"))
    {
        currentIdx = idx;
        currentType = TYPE_FLAC;
        fileSrc->seek(off, SEEK_SET);
        flac = new AudioGeneratorFLAC();
        flac->begin(fileSrc, audioOut);
    }
    else
    {
        currentType = TYPE_UNKNOWN;
        LOGLN("unsupported file type");
    }
    lockLoop = false;
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
            // Save: currentIdx, pos, ledMode
            bookmarkFile.printf("%d %d %u %d\n", totalFiles, currentIdx, pos, ledMode);
            bookmarkFile.flush();
            xSemaphoreGive(sdMutex);
        }
    }
}

bool readBookmark(int &idx, uint32_t &off, int &files, uint8_t &ledMode)
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
    int read = sscanf(line.c_str(), "%d %d %u %hhu", &files, &idx, &off, &ledMode);
    return true;
}

void nextTrack()
{
    LOGLN("nextTrack() called");
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
    unsigned long now = millis();
    if (now - lastSkip > 5000)
    {
        playTrack(currentIdx, 0);
    }
    else
    {
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
    lastSkip = now;
}

static void onVolumeUpButtonSingleClick(void *button_handle, void *user_data)
{
    nextTrack();
}

static void onVolumeDownButtonSingleClick(void *button_handle, void *user_data)
{
    previousTrack();
}

static void onModeButtonSingleClick(void *button_handle, void *user_data)
{
    ledMode = (ledMode + 1) % 4;
    updateNeoPixel();
}

void setup()
{
    blinkWelcomeMessage();
#if SERIAL_OUTPUT
    Serial.begin(115200);
    while (!Serial) {}
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
    strip.begin();
    strip.show();
    if (!audioOut)
    {
        audioOut = new AudioOutputI2S();
        audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
        audioOut->begin();
        audioOut->SetGain(0.05f);
    }
    if (!writeIndexFile())
    {
        LOGLN("No MP3s");
        while (1)
            delay(1000);
    }
    int idx = 0;
    uint32_t off = 0;
    int total = totalFiles;
    uint8_t ledModeTmp = 0;
    bool bookmarkFound = readBookmark(idx, off, total, ledModeTmp);
    ledMode = ledModeTmp;
    updateNeoPixel();
    LOGLN(bookmarkFound ? "Bookmark file opened" : "No bookmark found");
    if (total <= 0)
        LOGLN("No files found.");
    while (total <= 0)
        delay(100);
    totalFiles = total;
    shuffler = LazyShuffler(0, totalFiles - 1);
    bookmarkFile = SD.open("/bookmark", FILE_WRITE);
    if (!bookmarkFile)
    {
        LOGLN("Failed to open bookmark for writing");
    }
    bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
    xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 2, NULL, 1);
    if (bookmarkFound)
    {
        LOG("Bookmark: %d %u %u %u\n", idx, off, total, ledMode);
        LOG("Resume track %d @ byte %u\n", idx, off);
        playTrack(idx, off);
    }
    else
    {
        playTrack(shuffler.next(), 0);
    }
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED off by default
    Button *volUpBtn = new Button(BTN_VOL_UP, false);
    volUpBtn->attachSingleClickEventCb(onVolumeUpButtonSingleClick, NULL);
    Button *volDnBtn = new Button(BTN_VOL_DN, false);
    volDnBtn->attachSingleClickEventCb(onVolumeDownButtonSingleClick, NULL);
    Button *modeBtn = new Button(BTN_MODE, false);
    modeBtn->attachSingleClickEventCb(onModeButtonSingleClick, NULL);
}

void loop()
{
    unsigned long now = millis();
    if (lockLoop)
    {
        LOGLN("Loop locked.");
        return;
    }
    bool active = false;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    switch (currentType)
    {
    case TYPE_MP3:
        if (mp3)
            active = mp3->isRunning() && mp3->loop();
        break;
    case TYPE_WAV:
        if (wav)
            active = wav->isRunning() && wav->loop();
        break;
    case TYPE_FLAC:
        if (flac)
            active = flac->isRunning() && flac->loop();
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
        if (fileSrc && fileSrc->isOpen())
        {
            pos = fileSrc->getPos();
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
