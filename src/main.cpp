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
#define BTN_VOL_UP 33
#define BTN_VOL_DN 27
#define LED_PIN 2

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
std::vector<String> tracks;
std::vector<int> playOrder;
int orderPos = 0;
int current = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;
volatile bool _lockShuffle = false;
volatile bool _lockButtons = false;
volatile bool _lockPlay = false;
volatile bool _lockBookmark = false;

// Play mode enum and state
enum PlayMode
{
    MODE_ALL,
    MODE_FOLDER
};
PlayMode currentMode = MODE_ALL;
int currentFolderIndex = -1;
std::vector<String> allFolders;

// define your fixed steps
const float volSteps[] = {
    0.02f, 0.03f, 0.04f, 0.05f,
    0.10f, 0.15f, 0.20f, 0.25f,
    0.30f, 0.40f, 0.50f, 0.60f,
    0.70f, 0.80f, 0.90f, 1.00f};
const int VOL_COUNT = sizeof(volSteps) / sizeof(volSteps[0]);

// track which step you’re on
int volIndex = 7; // start at 0.05 (index 3)

bool lastUpState = HIGH, lastDnState = HIGH;
bool upHeld = false, dnHeld = false;

QueueHandle_t bookmarkQueue;
File bookmarkFile;

inline void lockShuffle() { _lockShuffle = true; }
inline void unlockShuffle() { _lockShuffle = false; }
inline bool shuffleLocked() { return _lockShuffle; }

inline void lockButtons() { _lockButtons = true; }
inline void unlockButtons() { _lockButtons = false; }
inline bool buttonsLocked() { return _lockButtons; }

inline void lockPlay() { _lockPlay = true; }
inline void unlockPlay() { _lockPlay = false; }
inline bool playLocked() { return _lockPlay; }

inline void lockBookmark() { _lockBookmark = true; }
inline void unlockBookmark() { _lockBookmark = false; }
inline bool bookmarkLocked() { return _lockBookmark; }

void rewriteShuffleFile()
{
    if (shuffleLocked())
    {
        LOGLN("Skipped shuffle file update: busy");
        return;
    }

    lockShuffle();
    lockButtons();
    lockPlay();
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    if (SD.exists("/shuffle.txt"))
    {
        SD.remove("/shuffle.txt");
        LOGLN("Shuffle file deleted");
    }

    File shuffleFile = SD.open("/shuffle.txt", FILE_WRITE);
    if (shuffleFile)
    {
        shuffleFile.printf("mode:%d\n", currentMode);
        shuffleFile.printf("folderIndex:%d\n", currentFolderIndex);
        shuffleFile.printf("orderPos:%d\n", orderPos);
        for (int idx : playOrder)
        {
            shuffleFile.printf("%d\n", idx);
        }
        shuffleFile.close();
        LOGLN("Shuffle file written");
    }
    else
    {
        LOGLN("Failed to update shuffle file");
    }
    xSemaphoreGive(sdMutex);
    unlockPlay();
    unlockButtons();
    unlockShuffle();
}

// Move blinkTaskHandle to global scope
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

void shuffleOrder()
{
    xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder

    bool hasIndex = current >= 0 && current < (int)tracks.size();

    if (shuffleLocked())
    {
        LOGLN("Skipped shuffling: busy");
        return;
    }

    lockShuffle();
    lockPlay();
    lockButtons();

    playOrder.clear();
    for (int i = 0; i < (int)tracks.size(); i++)
    {
        if (hasIndex && i == current)
            continue;

        if (currentMode == MODE_FOLDER && currentFolderIndex >= 0 && currentFolderIndex < (int)allFolders.size())
        {
            if (tracks[i].startsWith(allFolders[currentFolderIndex]))
            {
                playOrder.push_back(i);
            }
        }
        else
        {
            playOrder.push_back(i);
        }
    }
    std::vector<int> _playorder = playOrder;

    // Fisher-Yates shuffle
    for (int i = _playorder.size() - 1; i > 0; i--)
    {
        int j = esp_random() % (i + 1);
        std::swap(_playorder[i], _playorder[j]);
    }

    if (hasIndex)
    {
        playOrder.clear();
        playOrder = _playorder;
        playOrder.push_back(current);
    }
    else
    {
        playOrder = _playorder;
    }

    orderPos = 0;
    unlockButtons();
    unlockPlay();
    unlockShuffle();

    rewriteShuffleFile();

    LOG("Shuffle list generated (Mode: %s) with current track at pos 0\n", currentMode == MODE_FOLDER ? "folder" : "all");
}

void validateShuffleList()
{
    int expectedSize = 0;
    if (currentMode == MODE_FOLDER && currentFolderIndex >= 0 && currentFolderIndex < (int)allFolders.size())
    {
        for (const auto &track : tracks)
        {
            if (track.startsWith(allFolders[currentFolderIndex]))
                expectedSize++;
        }
    }
    else
    {
        expectedSize = tracks.size();
    }

    if ((int)playOrder.size() != expectedSize)
    {
        LOGLN("Shuffle list size mismatch. Regenerating shuffle list...");
        shuffleOrder();
    }
}

void loadShuffleFile()
{
    if (!SD.exists("/shuffle.txt"))
    {
        LOGLN("shuffle.txt not found");
        return;
    }

    File shuffleFile = SD.open("/shuffle.txt", FILE_READ);
    if (!shuffleFile)
    {
        LOGLN("Failed to open shuffle.txt");
        return;
    }

    lockShuffle();
    lockPlay();
    lockButtons();

    playOrder.clear();
    String modeLine = shuffleFile.readStringUntil('\n');
    String folderLine = shuffleFile.readStringUntil('\n');
    String posLine = shuffleFile.readStringUntil('\n');

    if (modeLine.startsWith("mode:"))
        currentMode = (PlayMode)modeLine.substring(5).toInt();
    if (folderLine.startsWith("folderIndex:"))
        currentFolderIndex = folderLine.substring(12).toInt();
    if (posLine.startsWith("orderPos:"))
        orderPos = posLine.substring(9).toInt();

    while (shuffleFile.available())
    {
        int idx = shuffleFile.readStringUntil('\n').toInt();
        if (idx >= 0 && idx < (int)tracks.size())
            playOrder.push_back(idx);
    }
    shuffleFile.close();

    unlockButtons();
    unlockPlay();
    unlockShuffle();

    validateShuffleList();
    LOG("Loaded shuffle.txt with %d tracks, mode %d, folderIndex %d\n", (int)playOrder.size(), currentMode, currentFolderIndex);
}

// Helper to extract folder of a track
String getFolderOfTrack(const String &trackPath)
{
    int lastSlash = trackPath.lastIndexOf('/');
    return (lastSlash > 0) ? trackPath.substring(0, lastSlash) : "/";
}

// Forward declaration for playTrack to avoid compiler error
void playTrack(int idx, uint32_t off);

void switchToNextFolder()
{
    xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder
    if (allFolders.empty())
        return;
    currentFolderIndex = (currentFolderIndex + 1) % allFolders.size();
    current = -1; // Reset current track
    shuffleOrder();
    LOG("Switched to next folder: %s\n", allFolders[currentFolderIndex].c_str());
    playTrack(playOrder[orderPos], 0);
}

void loadTracksRecursive(File dir, String path = "")
{
    while (File f = dir.openNextFile())
    {
        if (f.isDirectory())
        {
            String folder = path + "/" + f.name();
            // Skip folders that start with '.'
            if (!folder.isEmpty() && !String(f.name()).startsWith("."))
            {
                loadTracksRecursive(f, folder);
            }
        }
        else
        {
            String n = f.name();
            if (n.endsWith(".mp3") || n.endsWith(".MP3") || n.endsWith(".wav") || n.endsWith(".WAV") ||
                n.endsWith(".flac") || n.endsWith(".FLAC"))
            {
                tracks.push_back(path + "/" + n);
                // Add folder only if a music file is found
                if (!path.isEmpty())
                    allFolders.push_back(path);
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
    std::sort(allFolders.begin(), allFolders.end());
    allFolders.erase(std::unique(allFolders.begin(), allFolders.end()), allFolders.end());
    LOG("Found %u tracks\n", (unsigned)tracks.size());
}

void bookmarkTask(void *pv)
{
    if (bookmarkLocked())
    {
        LOGLN("Skipped bookmarkTask: busy");
        vTaskDelete(NULL);
        return;
    }

    uint32_t pos;
    for (;;)
    {
        if (xQueueReceive(bookmarkQueue, &pos, portMAX_DELAY) == pdTRUE)
        {
            if (!isPlaying || !bookmarkFile)
                continue;

            xSemaphoreTake(sdMutex, portMAX_DELAY);
            bookmarkFile.seek(0);
            bookmarkFile.printf("%d %u\n", current, pos);
            bookmarkFile.flush();
            xSemaphoreGive(sdMutex);
        }
    }
}

bool readBookmark(int &idx, uint32_t &off)
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (!SD.exists("/bookmark.txt"))
    {
        xSemaphoreGive(sdMutex);
        return false;
    }

    File f = SD.open("/bookmark.txt", FILE_READ);
    if (!f)
    {
        xSemaphoreGive(sdMutex);
        return false;
    }

    String line = f.readStringUntil('\n');
    f.close();
    xSemaphoreGive(sdMutex);

    int read = sscanf(line.c_str(), "%d %u", &idx, &off);
    return (read == 2 && idx >= 0 && idx < (int)tracks.size());
}

void playTrack(int idx, uint32_t off)
{
    if (playLocked())
    {
        LOGLN("Skipped playTrack: busy");
        return;
    }
    xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder

    if (mp3.isRunning())
        mp3.stop();
    if (wav.isRunning())
        wav.stop();
    if (flac.isRunning())
        flac.stop();
    fileSrc.close();

    // Debug: log entry to playTrack()
    LOG("playTrack() called with idx=%d\n", idx);
    LOG("Checking idx = %d against tracks.size() = %d\n", idx, (int)tracks.size());

    if (idx < 0 || idx >= (int)tracks.size())
    {
        LOG("Invalid track index: %d\n", idx);
        return;
    }

    current = idx;
    String path = tracks[idx];
    // Debug: log track path
    LOG("Track path: %s\n", path.c_str());
    LOG("Track %u: %s  (seek %u bytes)\n", idx, path.c_str(), off);

    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (!fileSrc.open(path.c_str()))
    {
        LOGLN("file open failed");
        xSemaphoreGive(sdMutex);
        return;
    }

    if (path.endsWith(".mp3") || path.endsWith(".MP3"))
    {
        currentType = TYPE_MP3;
        if (off)
            fileSrc.seek(off, SEEK_SET);
        mp3.begin(&fileSrc, &audioOut);
        isPlaying = mp3.isRunning();
    }
    else if (path.endsWith(".wav") || path.endsWith(".WAV"))
    {
        currentType = TYPE_WAV;
        wav.begin(&fileSrc, &audioOut);
        isPlaying = wav.isRunning();
    }
    else if (path.endsWith(".flac") || path.endsWith(".FLAC"))
    {
        currentType = TYPE_FLAC;
        if (off)
            fileSrc.seek(off, SEEK_SET);
        flac.begin(&fileSrc, &audioOut);
        isPlaying = flac.isRunning();
    }
    else
    {
        currentType = TYPE_UNKNOWN;
        LOGLN("unsupported file type");
    }

    xSemaphoreGive(sdMutex);
}

void nextTrack()
{
    xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder
    lockBookmark();

    orderPos++;

    // trap if out of bounds
    if (orderPos >= (int)playOrder.size())
    {
        LOG("orderPos out of bounds after increment! Reshuffling.\n");
        shuffleOrder(); // resets orderPos = 0
    }
    else if (playOrder[orderPos] < 0 || playOrder[orderPos] >= (int)tracks.size())
    {
        LOG("Corrupted playOrder! Index %d = %d (tracks.size = %d)\n", orderPos, playOrder[orderPos], (int)tracks.size());
        current = -1;
        shuffleOrder();
    }

    rewriteShuffleFile();
    unlockBookmark();
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

    loadTracks();
    if (tracks.empty())
    {
        LOGLN("No MP3s");
        while (1)
            delay(1000);
    }

    int idx;
    uint32_t off;
    bool bookmarkFound = readBookmark(idx, off);
    LOGLN(bookmarkFound ? "Bookmark file opened" : "No bookmark found");

    bookmarkFile = SD.open("/bookmark.txt", FILE_WRITE);
    if (!bookmarkFile)
    {
        LOGLN("Failed to open bookmark.txt for writing");
    }

    bookmarkQueue = xQueueCreate(5, sizeof(uint32_t));
    xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 1, NULL, 0);

    if (bookmarkFound)
    {
        LOG("Resume track %d @ byte %u\n", idx, off);
        loadShuffleFile();
        auto it = std::find(playOrder.begin(), playOrder.end(), idx);
        if (it != playOrder.end())
        {
            orderPos = std::distance(playOrder.begin(), it);
            playTrack(idx, off);
        }
        else
        {
            LOG("Bookmark index not in shuffle list, reshuffling\n");
            shuffleOrder();
            playTrack(playOrder[orderPos], off);
        }
    }
    else
    {
        LOG("No bookmark found, starting from the beginning\n");
        loadShuffleFile();
        nextTrack();
    }

    // button setup
    pinMode(BTN_VOL_UP, INPUT_PULLUP);
    pinMode(BTN_VOL_DN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // LED off by default

    extern bool lastUpState, lastDnState;
    extern bool upHeld, dnHeld;
    lastUpState = digitalRead(BTN_VOL_UP);
    lastDnState = digitalRead(BTN_VOL_DN);
    upHeld = lastUpState;
    dnHeld = lastDnState;
}

void loop()
{
    unsigned long now = millis();
    static bool bothHeld = false;
    static unsigned long sharedDownTime = 0;
    static bool bothActionTriggered = false;
    static unsigned long lastPrevPressTime = 0;
    if (isPlaying)
    {
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
            LOGLN("loop() returned false — decoder done");
            isPlaying = false;
            uint32_t flushDummy = fileSrc.getPos();
            xQueueSend(bookmarkQueue, &flushDummy, 0);
            delay(10);
            nextTrack();
            return;
        }

        unsigned long now = millis();
        uint32_t pos = 0;
        if (fileSrc.isOpen())
        {
            pos = fileSrc.getPos();
        }
        else
        {
            LOG("fileSrc not open during bookmark getPos(), attempting reopen\n");
            if (current >= 0 && current < (int)tracks.size())
            {
                String path = tracks[current];
                if (!fileSrc.open(path.c_str()))
                {
                    LOG("Failed to reopen file: %s\n", path.c_str());
                }
                else
                {
                    pos = fileSrc.getPos();
                    LOG("Reopened file for bookmark: %s\n", path.c_str());
                }
            }
            else
            {
                LOG("Invalid current index %d while trying to reopen fileSrc\n", current);
            }
        }

        if (now - lastBookmarkMs > 5000)
        {
            if (pos > 0)
            {
                xQueueSend(bookmarkQueue, &pos, 0);
                lastBookmarkMs = now;
                LOG("Queued bookmark %u @ %u bytes\n", current, pos);
            }
        }
    }

    // Debounce and state for buttons
    const unsigned long debounceDelay = 50;
    static unsigned long lastUpDebounceTime = 0;
    static unsigned long lastDnDebounceTime = 0;
    static unsigned long downTimeUp = 0, downTimeDn = 0;
    static bool upActionTriggered = false, dnActionTriggered = false;

    if (!buttonsLocked())
    {

        // Volume Up / Skip with debounce
        bool upReading = digitalRead(BTN_VOL_UP);
        if (upReading != lastUpState)
        {
            lastUpDebounceTime = now;
        }
        if ((now - lastUpDebounceTime) > debounceDelay)
        {
            if (!(upHeld == LOW && dnHeld == LOW))
            {
                if (upReading != upHeld)
                {
                    upHeld = upReading;
                    if (upHeld == LOW)
                    {
                        downTimeUp = now;
                        upActionTriggered = false;
                    }
                    else
                    {
                        if (!upActionTriggered)
                        {
                            unsigned long heldTime = now - downTimeUp;
                            if (heldTime > 1500)
                            {
                                nextTrack();
                                blinkLed(5);
                            }
                            else if (volIndex < VOL_COUNT - 1)
                            {
                                volIndex++;
                                audioOut.SetGain(volSteps[volIndex]);
                                blinkLed(2);
                                LOG("Vol: %.2f\n", volSteps[volIndex]);
                            }
                        }
                    }
                }

                if (upHeld == LOW && !upActionTriggered)
                {
                    unsigned long heldTime = now - downTimeUp;
                    if (heldTime > 1500)
                    {
                        nextTrack();
                        blinkLed(5);
                        upActionTriggered = true;
                    }
                }
            }
        }
        lastUpState = upReading;

        // Volume Down / Restart or Previous with debounce
        bool dnReading = digitalRead(BTN_VOL_DN);
        if (dnReading != lastDnState)
        {
            lastDnDebounceTime = now;
        }
        if ((now - lastDnDebounceTime) > debounceDelay)
        {
            if (!(upHeld == LOW && dnHeld == LOW))
            {
                if (dnReading != dnHeld)
                {
                    dnHeld = dnReading;
                    if (dnHeld == LOW)
                    {
                        downTimeDn = now;
                        dnActionTriggered = false;
                    }
                    else
                    {
                        if (!dnActionTriggered)
                        {
                            unsigned long heldTime = now - downTimeDn;
                            if (heldTime > 1500)
                            {
                                unsigned long timeSinceLast = now - lastPrevPressTime;
                                lastPrevPressTime = now;

                                if (timeSinceLast < 10000)
                                {
                                    if (orderPos > 0)
                                    {
                                        orderPos -= 2;
                                        nextTrack();
                                        blinkLed(5);
                                    }
                                    else
                                    {
                                        playTrack(playOrder[orderPos], 0);
                                        blinkLed(5);
                                    }
                                }
                                else
                                {
                                    playTrack(playOrder[orderPos], 0);
                                    blinkLed(5);
                                }
                            }
                            else if (volIndex > 0)
                            {
                                volIndex--;
                                audioOut.SetGain(volSteps[volIndex]);
                                blinkLed(2);
                                LOG("Vol: %.2f\n", volSteps[volIndex]);
                            }
                        }
                    }
                }

                if (dnHeld == LOW && !dnActionTriggered)
                {
                    unsigned long heldTime = now - downTimeDn;
                    if (heldTime > 1500)
                    {
                        unsigned long timeSinceLast = now - lastPrevPressTime;
                        lastPrevPressTime = now;

                        if (timeSinceLast < 10000)
                        {
                            if (orderPos > 0)
                            {
                                orderPos -= 2;
                                nextTrack();
                                blinkLed(5);
                            }
                            else
                            {
                                playTrack(playOrder[orderPos], 0);
                                blinkLed(5);
                            }
                        }
                        else
                        {
                            playTrack(playOrder[orderPos], 0);
                            blinkLed(5);
                        }
                        dnActionTriggered = true;
                    }
                }
            }
        }

        // Check for simultaneous button press (after individual handling)
        if (upHeld == LOW && dnHeld == LOW && !bothHeld)
        {
            bothHeld = true;
            sharedDownTime = now;
            bothActionTriggered = false;
        }

        if (bothHeld && upReading == HIGH && dnReading == HIGH)
        {
            bothHeld = false;
            unsigned long heldTime = now - sharedDownTime;

            if (!bothActionTriggered)
            {
                if (heldTime > 1500)
                {
                    if (currentMode == MODE_ALL)
                    {
                        if (current >= 0 && current < (int)tracks.size())
                        {
                            currentFolderIndex = std::distance(allFolders.begin(), std::find(allFolders.begin(), allFolders.end(), getFolderOfTrack(tracks[current])));
                        }
                        else if (!tracks.empty())
                        {
                            currentFolderIndex = std::distance(allFolders.begin(), std::find(allFolders.begin(), allFolders.end(), getFolderOfTrack(tracks[0])));
                            current = 0;
                        }
                        else
                        {
                            LOG("No valid track for folder switching\n");
                            return;
                        }

                        currentMode = MODE_FOLDER;
                        shuffleOrder();
                        LOG("Entered folder shuffle: %s\n", allFolders[currentFolderIndex].c_str());
                    }
                    else
                    {
                        currentMode = MODE_ALL;
                        currentFolderIndex = -1;
                        shuffleOrder();
                        LOG("Back to full shuffle mode\n");
                    }
                    rewriteShuffleFile();
                }
                else if (currentMode == MODE_FOLDER)
                {
                    switchToNextFolder();
                }
            }

            bothActionTriggered = false;
            upHeld = upReading;
            dnHeld = dnReading;
        }
        lastDnState = dnReading;
    }

    // 3-second LED blink in folder mode
    static unsigned long lastFolderBlink = 0;
    if (currentMode == MODE_FOLDER && now - lastFolderBlink > 3000)
    {
        blinkLed(1);
        lastFolderBlink = now;
    }
}
