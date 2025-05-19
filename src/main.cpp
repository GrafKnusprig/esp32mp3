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

class LazyShuffler
{
public:
    LazyShuffler(int start, int end)
        : start(start), end(end), total(end - start + 1), remaining(total)
    {
        round++;
    }

    int next()
    {
        if (remaining == 0)
        {
            remaining = total;
            used.clear();
            history.clear();
            round++;
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
    static int round;
};

int LazyShuffler::round = 0;

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
// std::vector<String> tracks;
// std::vector<int> playOrder;
// int orderPos = 0;
int totalFiles = -1;
int currentIdx = -1;
bool isPlaying = false;
unsigned long lastBookmarkMs = 0;
volatile bool _lockShuffle = false;
volatile bool _lockButtons = false;
volatile bool _lockPlay = false;
volatile bool _lockBookmark = false;
LazyShuffler shuffler(0, 0);

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

bool writeIndexFile()
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    if (SD.exists("/index") && SD.exists("/folderindex"))
    {
        LOGLN("Index and folderindex found");
        xSemaphoreGive(sdMutex);
        return true;
    }

    // Open the index file for writing
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

    // Step 1: Read all lines into a vector, sort, and write to a temp file
    const int BATCH_SIZE = 128; // Tune for available RAM
    std::vector<String> batch;
    std::vector<String> tempFiles;
    File inFile = SD.open("/index", FILE_READ);
    int batchNum = 0;
    while (inFile.available())
    {
        String line = inFile.readStringUntil('\n');
        if (line.length() > 0)
            batch.push_back(line);
        if ((int)batch.size() >= BATCH_SIZE || !inFile.available())
        {
            std::sort(batch.begin(), batch.end());
            String tempName = "/index_tmp" + String(batchNum++);
            tempFiles.push_back(tempName);
            File tmp = SD.open(tempName.c_str(), FILE_WRITE);
            for (const auto &l : batch)
                tmp.println(l);
            tmp.close();
            batch.clear();
        }
    }
    inFile.close();
    batch.clear(); // free memory

    // Step 2: Merge temp files into the final sorted index
    File outFile = SD.open("/index_sorted", FILE_WRITE);
    std::vector<File> tmps;
    std::vector<String> lines;
    for (const auto &name : tempFiles)
    {
        File f = SD.open(name.c_str(), FILE_READ);
        tmps.push_back(f);
        lines.push_back(f.readStringUntil('\n'));
    }

    while (true)
    {
        int minIdx = -1;
        String minStr;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (lines[i].length() == 0)
                continue;
            if (minIdx == -1 || lines[i] < minStr)
            {
                minIdx = i;
                minStr = lines[i];
            }
        }
        if (minIdx == -1)
            break;
        outFile.println(minStr);
        if (tmps[minIdx].available())
            lines[minIdx] = tmps[minIdx].readStringUntil('\n');
        else
            lines[minIdx] = "";
    }
    for (auto &f : tmps)
        f.close();
    outFile.close();
    tmps.clear();
    lines.clear();

    // Remove temp files and original index
    for (const auto &name : tempFiles)
        SD.remove(name.c_str());
    tempFiles.clear();
    SD.remove("/index");

    // Rename sorted index to /index
    SD.rename("/index_sorted", "/index");

    // Now, count files per folder and write the summary line
    // We'll walk through the sorted index file line by line
    File idx = SD.open("/index", FILE_READ);
    std::vector<int> folderCounts;
    String lastFolder = "";
    int count = 0;
    while (idx.available())
    {
        String line = idx.readStringUntil('\n');
        int slash = line.lastIndexOf('/');
        String folder = (slash > 0) ? line.substring(0, slash) : "/";
        if (folder != lastFolder && lastFolder.length() > 0)
        {
            folderCounts.push_back(count);
            count = 0;
        }
        count++;
        lastFolder = folder;
    }
    if (count > 0)
        folderCounts.push_back(count);
    idx.close();

    // Write the summary line to a separate file "folderindex"
    File folderIdx = SD.open("/folderindex", FILE_WRITE);
    for (size_t i = 0; i < folderCounts.size(); ++i)
    {
        folderIdx.print(folderCounts[i]);
        if (i != folderCounts.size() - 1)
            folderIdx.print(" ");
    }
    folderIdx.println();
    folderIdx.close();
    folderCounts.clear();

    LOGLN("Index file created and sorted, folder counts written to /folderindex");
    xSemaphoreGive(sdMutex);
    return true;
}

// void rewriteShuffleFile()
// {
//     if (shuffleLocked())
//     {
//         LOGLN("Skipped shuffle file update: busy");
//         return;
//     }

//     lockShuffle();
//     lockButtons();
//     lockPlay();
//     xSemaphoreTake(sdMutex, portMAX_DELAY);

//     if (SD.exists("/shuffle.txt"))
//     {
//         SD.remove("/shuffle.txt");
//         LOGLN("Shuffle file deleted");
//     }

//     File shuffleFile = SD.open("/shuffle.txt", FILE_WRITE);
//     if (shuffleFile)
//     {
//         shuffleFile.printf("mode:%d\n", currentMode);
//         shuffleFile.printf("folderIndex:%d\n", currentFolderIndex);
//         shuffleFile.printf("orderPos:%d\n", orderPos);
//         for (int idx : playOrder)
//         {
//             shuffleFile.printf("%d\n", idx);
//         }
//         shuffleFile.close();
//         LOGLN("Shuffle file written");
//     }
//     else
//     {
//         LOGLN("Failed to update shuffle file");
//     }
//     xSemaphoreGive(sdMutex);
//     unlockPlay();
//     unlockButtons();
//     unlockShuffle();
// }

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

// void shuffleOrder()
// {
//     xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder

//     bool hasIndex = current >= 0 && current < (int)tracks.size();

//     if (shuffleLocked())
//     {
//         LOGLN("Skipped shuffling: busy");
//         return;
//     }

//     lockShuffle();
//     lockPlay();
//     lockButtons();

//     playOrder.clear();
//     for (int i = 0; i < (int)tracks.size(); i++)
//     {
//         if (hasIndex && i == current)
//             continue;

//         if (currentMode == MODE_FOLDER && currentFolderIndex >= 0 && currentFolderIndex < (int)allFolders.size())
//         {
//             if (tracks[i].startsWith(allFolders[currentFolderIndex]))
//             {
//                 playOrder.push_back(i);
//             }
//         }
//         else
//         {
//             playOrder.push_back(i);
//         }
//     }
//     std::vector<int> _playorder = playOrder;

//     // Fisher-Yates shuffle
//     for (int i = _playorder.size() - 1; i > 0; i--)
//     {
//         int j = esp_random() % (i + 1);
//         std::swap(_playorder[i], _playorder[j]);
//     }

//     if (hasIndex)
//     {
//         playOrder.clear();
//         playOrder = _playorder;
//         playOrder.push_back(current);
//     }
//     else
//     {
//         playOrder = _playorder;
//     }

//     orderPos = 0;
//     unlockButtons();
//     unlockPlay();
//     unlockShuffle();

//     rewriteShuffleFile();

//     LOG("Shuffle list generated (Mode: %s) with current track at pos 0\n", currentMode == MODE_FOLDER ? "folder" : "all");
// }

// void validateShuffleList()
// {
//     int expectedSize = 0;
//     if (currentMode == MODE_FOLDER && currentFolderIndex >= 0 && currentFolderIndex < (int)allFolders.size())
//     {
//         for (const auto &track : tracks)
//         {
//             if (track.startsWith(allFolders[currentFolderIndex]))
//                 expectedSize++;
//         }
//     }
//     else
//     {
//         expectedSize = tracks.size();
//     }

//     if ((int)playOrder.size() != expectedSize)
//     {
//         LOGLN("Shuffle list size mismatch. Regenerating shuffle list...");
//         shuffleOrder();
//     }
// }

// void loadShuffleFile()
// {
//     if (!SD.exists("/shuffle.txt"))
//     {
//         LOGLN("shuffle.txt not found");
//         return;
//     }

//     File shuffleFile = SD.open("/shuffle.txt", FILE_READ);
//     if (!shuffleFile)
//     {
//         LOGLN("Failed to open shuffle.txt");
//         return;
//     }

//     lockShuffle();
//     lockPlay();
//     lockButtons();

//     playOrder.clear();
//     String modeLine = shuffleFile.readStringUntil('\n');
//     String folderLine = shuffleFile.readStringUntil('\n');
//     String posLine = shuffleFile.readStringUntil('\n');

//     if (modeLine.startsWith("mode:"))
//         currentMode = (PlayMode)modeLine.substring(5).toInt();
//     if (folderLine.startsWith("folderIndex:"))
//         currentFolderIndex = folderLine.substring(12).toInt();
//     if (posLine.startsWith("orderPos:"))
//         orderPos = posLine.substring(9).toInt();

//     while (shuffleFile.available())
//     {
//         int idx = shuffleFile.readStringUntil('\n').toInt();
//         if (idx >= 0 && idx < (int)tracks.size())
//             playOrder.push_back(idx);
//     }
//     shuffleFile.close();

//     unlockButtons();
//     unlockPlay();
//     unlockShuffle();

//     validateShuffleList();
//     LOG("Loaded shuffle.txt with %d tracks, mode %d, folderIndex %d\n", (int)playOrder.size(), currentMode, currentFolderIndex);
// }

// // Helper to extract folder of a track
// String getFolderOfTrack(const String &trackPath)
// {
//     int lastSlash = trackPath.lastIndexOf('/');
//     return (lastSlash > 0) ? trackPath.substring(0, lastSlash) : "/";
// }

void playTrack(int idx, uint32_t off)
{
    if (playLocked())
    {
        LOGLN("Skipped playTrack: busy");
        return;
    }

    // Debug: log entry to playTrack()
    LOG("playTrack() called with idx=%d\n", idx);
    // LOG("Checking idx = %d against tracks.size() = %d\n", idx, (int)tracks.size());

    if (idx < 0 || idx >= totalFiles)
    {
        LOG("Invalid track index: %d\n", idx);
        return;
    }

    xQueueReset(bookmarkQueue);

    if (mp3.isRunning())
        mp3.stop();
    if (wav.isRunning())
        wav.stop();
    if (flac.isRunning())
        flac.stop();
    fileSrc.close();
    
    isPlaying = false;

    currentIdx = idx;

    String currentPath;
    // Read the path from the index file at line number idx
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
    }
    else
    {
        LOGLN("Failed to open /index for reading path");
        xSemaphoreGive(sdMutex);
        return;
    }
    xSemaphoreGive(sdMutex);

    // Debug: log track path
    LOG("Track path: %s\n", currentPath.c_str());
    LOG("Track %u: %s  (seek %u bytes)\n", idx, currentPath.c_str(), off);

    xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (!fileSrc.open(currentPath.c_str()))
    {
        LOGLN("file open failed");
        xSemaphoreGive(sdMutex);
        return;
    }

    if (currentPath.endsWith(".mp3") || currentPath.endsWith(".MP3"))
    {
        currentType = TYPE_MP3;
        if (off)
            fileSrc.seek(off, SEEK_SET);
        mp3.begin(&fileSrc, &audioOut);
        isPlaying = mp3.isRunning();
    }
    else if (currentPath.endsWith(".wav") || currentPath.endsWith(".WAV"))
    {
        currentType = TYPE_WAV;
        wav.begin(&fileSrc, &audioOut);
        isPlaying = wav.isRunning();
    }
    else if (currentPath.endsWith(".flac") || currentPath.endsWith(".FLAC"))
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

void switchToNextFolder()
{
    return;
    // xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder
    // if (allFolders.empty())
    //     return;
    // currentFolderIndex = (currentFolderIndex + 1) % allFolders.size();
    // current = -1; // Reset current track
    // shuffleOrder();
    // LOG("Switched to next folder: %s\n", allFolders[currentFolderIndex].c_str());
    // playTrack(playOrder[orderPos], 0);
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
            bookmarkFile.printf("%d %u\n", currentIdx, pos);
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
    return (read == 2 && idx >= 0 && idx < totalFiles);
}

void nextTrack()
{
    LOGLN("nextTrack() called");
    xQueueReset(bookmarkQueue); // 🧼 flush pending bookmarks before switching folder

    LOGLN("Bookmark file flushed, waiting for lock...");
    lockBookmark();

    int next = shuffler.next();
    if (currentIdx == next)
    {
        next = shuffler.next();
    }

    unlockBookmark();
    playTrack(next, 0);
}

void previousTrack()
{
    LOGLN("previousTrack() called");
    xQueueReset(bookmarkQueue);

    LOGLN("Bookmark file flushed, waiting for lock...");
    lockBookmark();

    int last = shuffler.last();
    if (last < 0)
    {
        // Optionally, replay current track or do nothing
        LOGLN("No previous track available");
        unlockBookmark();
        // Optionally: playTrack(currentIdx, 0);
        return;
    }

    unlockBookmark();
    playTrack(last, 0);
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

    // read the folderindex file
    // read all the int values from the first line in the file which are separated with spaces
    // sum all the values up and write them to the variable totalFiles
    File folderIdx = SD.open("/folderindex", FILE_READ);
    if (folderIdx)
    {
        String line = folderIdx.readStringUntil('\n');
        folderIdx.close();
        totalFiles = 0;
        int start = 0;
        while (start < line.length())
        {
            int end = line.indexOf(' ', start);
            if (end == -1)
                end = line.length();
            String numStr = line.substring(start, end);
            int val = numStr.toInt();
            if (val > 0)
                totalFiles += val;
            start = end + 1;
        }
        LOG("Total files from folderindex: %d\n", totalFiles);
    }
    else
    {
        LOGLN("Failed to open /folderindex");
        totalFiles = -1;
    }

    shuffler = LazyShuffler(0, totalFiles - 1);

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
        playTrack(idx, off);
    }
    else
    {
        playTrack(shuffler.next(), 0);
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
            LOGLN("fileSrc not open during bookmark getPos()");
        }

        if (now - lastBookmarkMs > 5000)
        {
            if (pos > 0)
            {
                xQueueSend(bookmarkQueue, &pos, 0);
                lastBookmarkMs = now;
                LOG("Queued bookmark %u @ %u bytes\n", currentIdx, pos);
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
                                LOGLN("Volume up held for 1.5s, skipping track");
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
                                    previousTrack();
                                    blinkLed(5);
                                }
                                else
                                {
                                    playTrack(currentIdx, 0);
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
                            previousTrack();
                            blinkLed(5);
                        }
                        else
                        {
                            playTrack(currentIdx, 0);
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
                        return;
                    }
                    else
                    {
                        return;
                    }
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
