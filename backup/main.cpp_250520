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

#define SERIAL_OUTPUT 0

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
int totalFiles = -1;
int currentIdx = -1;
int currentFolderIdx = -1;
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

// fixed volume steps
const float volSteps[] = {
    0.02f, 0.03f, 0.04f, 0.05f,
    0.10f, 0.15f, 0.20f, 0.25f,
    0.30f, 0.40f, 0.50f, 0.60f,
    0.70f, 0.80f, 0.90f, 1.00f};
const int VOL_COUNT = sizeof(volSteps) / sizeof(volSteps[0]);

int volIndex = 7; // start at 0.05 (index 3)

bool lastUpState = HIGH, lastDnState = HIGH;
bool upHeld = false, dnHeld = false;

QueueHandle_t bookmarkQueue;
File bookmarkFile;

inline void lockButtons() { _lockButtons = true; }
inline void unlockButtons() { _lockButtons = false; }
inline bool buttonsLocked() { return _lockButtons; }

bool writeIndexFile()
{
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    if (SD.exists("/index") && SD.exists("/folderindex"))
    {
        LOGLN("Index and folderindex found");
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

// Helper to read folder counts from /folderindex
std::vector<int> getFolderCounts()
{
    std::vector<int> folderCounts;
    File folderIdxFile = SD.open("/folderindex", FILE_READ);
    if (!folderIdxFile)
        return folderCounts;
    String line = folderIdxFile.readStringUntil('\n');
    folderIdxFile.close();
    int start = 0;
    while (start < line.length())
    {
        int end = line.indexOf(' ', start);
        if (end == -1)
            end = line.length();
        String numStr = line.substring(start, end);
        int val = numStr.toInt();
        if (val > 0)
            folderCounts.push_back(val);
        start = end + 1;
    }
    return folderCounts;
}

// Helper to get folder start/end indices
void getFolderStartEnd(const std::vector<int> &folderCounts, int folderIdx, int &startIdx, int &endIdx)
{
    startIdx = 0;
    for (int i = 0; i < folderIdx; ++i)
        startIdx += folderCounts[i];
    endIdx = startIdx + folderCounts[folderIdx] - 1;
}

void switchToFolderMode()
{
    LOGLN("Switching to folder mode");
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    // 1. Find the folder of the current track by reading the index file at currentIdx
    String currentTrackPath;
    File indexFile = SD.open("/index", FILE_READ);
    if (indexFile)
    {
        for (int i = 0; i <= currentIdx; ++i)
        {
            currentTrackPath = indexFile.readStringUntil('\n');
        }
        indexFile.close();
        currentTrackPath.trim();
    }
    else
    {
        LOGLN("Failed to open /index for folder mode");
        xSemaphoreGive(sdMutex);
        return;
    }

    LOG("Current track path: %s\n", currentTrackPath.c_str());

    // Extract folder from currentTrackPath
    int lastSlash = currentTrackPath.lastIndexOf('/');
    String currentFolder = (lastSlash > 0) ? currentTrackPath.substring(0, lastSlash) : "/";

    // 2. Find the folder index by scanning /index for folder boundaries
    int folderIdx = -1;
    String lastFolder = "";
    File idxFile = SD.open("/index", FILE_READ);
    if (idxFile)
    {
        while (idxFile.available())
        {
            String line = idxFile.readStringUntil('\n');
            int slash = line.lastIndexOf('/');
            String folder = (slash > 0) ? line.substring(0, slash) : "/";
            if (line == currentTrackPath)
            {
                break;
            }
            if (folder != lastFolder)
            {
                folderIdx++;
                lastFolder = folder;
            }
        }
        idxFile.close();
    }
    else
    {
        LOGLN("Failed to open /index for folder scan");
        xSemaphoreGive(sdMutex);
        return;
    }

    // Save the folderIdx in currentFolderIdx
    currentFolderIdx = folderIdx;

    LOG("Current folder: %s (index %d)\n", currentFolder.c_str(), folderIdx);

    // 3. Read folder counts from /folderindex
    File folderIdxFile = SD.open("/folderindex", FILE_READ);
    if (!folderIdxFile)
    {
        LOGLN("Failed to open /folderindex");
        xSemaphoreGive(sdMutex);
        return;
    }
    String line = folderIdxFile.readStringUntil('\n');
    folderIdxFile.close();

    // Parse folder counts
    std::vector<int> folderCounts;
    int start = 0;
    while (start < line.length())
    {
        int end = line.indexOf(' ', start);
        if (end == -1)
            end = line.length();
        String numStr = line.substring(start, end);
        int val = numStr.toInt();
        if (val > 0)
            folderCounts.push_back(val);
        start = end + 1;
    }

    // 4. Calculate start and end index for shuffler
    int startIdx = 0;
    for (int i = 0; i < folderIdx; ++i)
        startIdx += folderCounts[i];
    int endIdx = startIdx + folderCounts[folderIdx] - 1;

    // 5. Set mode and shuffler, play track
    currentMode = MODE_FOLDER;
    shuffler.~LazyShuffler();
    new (&shuffler) LazyShuffler(startIdx, endIdx);

    LOG("Folder %d: startIdx=%d, endIdx=%d\n", folderIdx, startIdx, endIdx);

    xSemaphoreGive(sdMutex);
    LOG("Mutex given\n");
}

void switchToAllMode()
{
    LOGLN("Switching to all mode");
    currentMode = MODE_ALL;
    currentFolderIdx = -1;
    shuffler.~LazyShuffler();
    new (&shuffler) LazyShuffler(0, totalFiles - 1);
}

void playTrack(int idx, uint32_t off)
{
    LOG("playTrack() called with idx=%d\n", idx);

    if (idx < 0 || idx >= totalFiles)
    {
        LOG("Invalid track index: %d\n", idx);
        return;
    }

    if (mp3.isRunning())
        mp3.stop();
    if (wav.isRunning())
        wav.stop();
    if (flac.isRunning())
        flac.stop();
    fileSrc.close();

    LOG("Stopping previous track\n");

    isPlaying = false;

    currentIdx = idx;

    String currentPath;
    // Read the path from the index file at line number idx

    LOG("waiting for sdMutex...\n");
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
    xSemaphoreTake(sdMutex, portMAX_DELAY);

    std::vector<int> folderCounts = getFolderCounts();
    if (folderCounts.empty())
    {
        LOGLN("No folder counts found");
        xSemaphoreGive(sdMutex);
        return;
    }

    int totalFolders = folderCounts.size();
    LOG("Total folders: %d\n", totalFolders);
    int nextFolderIdx = currentFolderIdx + 1;
    LOG("Current folder index: %d\n", currentFolderIdx);

    if (nextFolderIdx >= totalFolders)
        nextFolderIdx = 0;

    // Find the start index of the next folder in /index
    int startIdx = 0, endIdx = 0;
    getFolderStartEnd(folderCounts, nextFolderIdx, startIdx, endIdx);

    // Set mode, update shuffler, and play first track in next folder
    currentMode = MODE_FOLDER;
    currentFolderIdx = nextFolderIdx;
    shuffler.~LazyShuffler();
    new (&shuffler) LazyShuffler(startIdx, endIdx);
    LOG("Switching to folder %d (startIdx=%d, endIdx=%d)\n", nextFolderIdx, startIdx, endIdx);

    xSemaphoreGive(sdMutex);

    playTrack(shuffler.next(), 0);
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

            xSemaphoreTake(sdMutex, portMAX_DELAY);
            bookmarkFile.seek(0);
            // Save: currentIdx, pos, currentMode, currentFolderIdx
            bookmarkFile.printf("%d %u %d %d\n", currentIdx, pos, (int)currentMode, currentFolderIdx);
            bookmarkFile.flush();
            xSemaphoreGive(sdMutex);
        }
    }
}

bool readBookmark(int &idx, uint32_t &off, int &mode, int &foIdx)
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

    int read = sscanf(line.c_str(), "%d %u %d %d", &idx, &off, &mode, &foIdx);

    return (read >= 2 && idx >= 0 && idx < totalFiles);
}

void nextTrack()
{
    LOGLN("nextTrack() called");
    xQueueReset(bookmarkQueue);

    LOGLN("Bookmark file flushed, waiting for lock...");

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

    LOGLN("Bookmark file flushed, waiting for lock...");

    int last = shuffler.last();
    if (last < 0)
    {
        // Optionally, replay current track or do nothing
        LOGLN("No previous track available");
        // Optionally: playTrack(currentIdx, 0);
        return;
    }

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
    int mode;
    int foIdx;
    bool bookmarkFound = readBookmark(idx, off, mode, foIdx);
    LOGLN(bookmarkFound ? "Bookmark file opened" : "No bookmark found");
    if (bookmarkFound)
    {
        currentMode = static_cast<PlayMode>(mode);
        currentFolderIdx = foIdx;
        LOG("Resume track %d @ byte %u\n", idx, off);

        if (currentMode == MODE_FOLDER)
        {
            std::vector<int> folderCounts = getFolderCounts();
            if (currentFolderIdx < 0 || currentFolderIdx >= folderCounts.size())
            {
                LOGLN("Invalid folder index in bookmark");
                currentMode = MODE_ALL;
                currentFolderIdx = -1;
            }
            else
            {
                int startIdx = 0, endIdx = 0;
                getFolderStartEnd(folderCounts, currentFolderIdx, startIdx, endIdx);
                shuffler.~LazyShuffler();
                new (&shuffler) LazyShuffler(startIdx, endIdx);
            }

            playTrack(idx, off);
        }
        else
        {
            playTrack(idx, off);
        }
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
    xTaskCreatePinnedToCore(bookmarkTask, "bookmarkTask", 4096, NULL, 1, NULL, 0);

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
            delay(10);
            nextTrack();
            return;
        }

        if (now - lastBookmarkMs > 5000)
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
                        switchToFolderMode();
                    }
                    else
                    {
                        switchToAllMode();
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
