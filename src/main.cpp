#define MINIFLAC_IMPLEMENTATION
extern "C" {
#include "../lib/miniflac/miniflac.h"
}
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <vector>

#define SD_CS 5
#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22
#define FLAC_READ_BUFFER_SIZE 4096
#define FLAC_MAX_CHANNELS 8
#define I2S_DMA_BUF_COUNT 8
#define I2S_DMA_BUF_LEN 256*2
#define NEXT_BUTTON_PIN 33
#define NEXT_BUTTON_DEBOUNCE_MS 200

File flacFile;
miniflac_t* decoder = nullptr;
uint8_t* readBuffer = nullptr;
size_t readBufferLen = 0;
size_t readBufferPos = 0;
int32_t* pcm[FLAC_MAX_CHANNELS] = {nullptr};
uint8_t* i2sBuffer = nullptr;
size_t i2sBufferSize = 0;
uint32_t sampleRate = 0;
uint8_t channels = 0;
uint8_t bps = 0;
uint16_t blockSize = 0;
bool active = false;

std::vector<String> flacFiles;
int currentFileIndex = 0;
unsigned long lastButtonPress = 0;
bool nextRequested = false;

void findAllFlacFiles(File dir, std::vector<String>& files) {
    while (File entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
            findAllFlacFiles(entry, files);
        } else {
            String name = entry.name();
            if (name.endsWith(".flac") || name.endsWith(".FLAC")) {
                files.push_back(String(entry.path()));
            }
        }
        entry.close();
    }
}

void closeCurrentFile() {
    if (flacFile) flacFile.close();
    if (decoder) { free(decoder); decoder = nullptr; }
    if (readBuffer) { free(readBuffer); readBuffer = nullptr; }
    for (int i = 0; i < FLAC_MAX_CHANNELS; ++i) {
        if (pcm[i]) { free(pcm[i]); pcm[i] = nullptr; }
    }
    if (i2sBuffer) { free(i2sBuffer); i2sBuffer = nullptr; }
    active = false;
}

bool openFlacFile(const String& path) {
    closeCurrentFile();
    flacFile = SD.open(path);
    if (!flacFile) {
        Serial.printf("Failed to open FLAC file: %s\n", path.c_str());
        return false;
    }
    readBuffer = (uint8_t*)malloc(FLAC_READ_BUFFER_SIZE);
    if (!readBuffer) return false;
    decoder = (miniflac_t*)malloc(miniflac_size());
    if (!decoder) return false;
    miniflac_init(decoder, MINIFLAC_CONTAINER_UNKNOWN);
    readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
    readBufferPos = 0;
    uint32_t used = 0;
    if (miniflac_sync(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used) != MINIFLAC_OK) return false;
    readBufferPos += used;
    bool streaminfoFound = false;
    while (decoder->state == MINIFLAC_METADATA) {
        if (decoder->metadata.header.type == MINIFLAC_METADATA_STREAMINFO) {
            uint16_t min_block, max_block;
            uint32_t min_frame, max_frame, sr;
            uint8_t ch, bps_val;
            uint64_t total_samples;
            if (miniflac_streaminfo_min_block_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &min_block) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_max_block_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &max_block) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_min_frame_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &min_frame) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_max_frame_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &max_frame) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_sample_rate(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &sr) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_channels(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &ch) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_bps(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &bps_val) != MINIFLAC_OK) return false;
            readBufferPos += used;
            if (miniflac_streaminfo_total_samples(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &total_samples) != MINIFLAC_OK) return false;
            readBufferPos += used;
            sampleRate = sr;
            channels = ch;
            bps = bps_val;
            blockSize = max_block;
            Serial.printf("FLAC stream: %u Hz, %u ch, %u bps, blocksize %u\n", sampleRate, channels, bps, blockSize);
            for (int i = 0; i < FLAC_MAX_CHANNELS; ++i) {
                if (pcm[i]) free(pcm[i]);
                pcm[i] = nullptr;
            }
            for (int i = 0; i < channels; ++i) {
                pcm[i] = (int32_t*)malloc(sizeof(int32_t) * blockSize);
                if (!pcm[i]) return false;
            }
            if (i2sBuffer) free(i2sBuffer);
            i2sBufferSize = blockSize * channels * 4;
            i2sBuffer = (uint8_t*)malloc(i2sBufferSize);
            if (!i2sBuffer) return false;
            i2s_set_clk(I2S_NUM_0, sampleRate, (bps <= 16) ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_32BIT, (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
            streaminfoFound = true;
        }
        if (miniflac_sync(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used) != MINIFLAC_OK) {
            if (flacFile.available()) {
                readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
                readBufferPos = 0;
            } else {
                return false;
            }
        } else {
            readBufferPos += used;
        }
    }
    if (!streaminfoFound) return false;
    active = true;
    return true;
}

// Minimal function to turn off a single WS2812/NeoPixel LED on pin 21
void turnOffNeoPixel() {
    const int pin = 21;
    pinMode(pin, OUTPUT);
    noInterrupts();
    // Try all color orders: GRB, RGB, BRG
    uint8_t patterns[3][3] = {
        {0,0,0}, // GRB
        {0,0,0}, // RGB
        {0,0,0}  // BRG
    };
    for (int order = 0; order < 3; ++order) {
        for (int c = 0; c < 3; ++c) {
            for (int b = 7; b >= 0; --b) {
                if (patterns[order][c] & (1 << b)) {
                    digitalWrite(pin, HIGH);
                    asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
                    digitalWrite(pin, LOW);
                    asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
                } else {
                    digitalWrite(pin, HIGH);
                    asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
                    digitalWrite(pin, LOW);
                    asm volatile ("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop;");
                }
            }
        }
        delayMicroseconds(100); // Latch (longer)
    }
    digitalWrite(pin, LOW); // Ensure pin is low after
    interrupts();
}

void setup() {
    turnOffNeoPixel(); // First thing after startup
    Serial.begin(115150);
    Serial.println("Starting setup...");
    pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
        return;
    }
    Serial.println("SD Card initialized successfully");
    delay(1000);
    Serial.println("Scanning for FLAC files...");
    File root = SD.open("/");
    flacFiles.clear();
    findAllFlacFiles(root, flacFiles);
    root.close();
    if (flacFiles.empty()) {
        Serial.println("No FLAC files found on SD card!");
        return;
    }
    currentFileIndex = 0;
    Serial.printf("Found %d FLAC files.\n", (int)flacFiles.size());
    Serial.printf("Starting with: %s\n", flacFiles[currentFileIndex].c_str());
    Serial.println("Initializing I2S...");
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100, // will update after STREAMINFO
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // will update after STREAMINFO
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = I2S_DMA_BUF_COUNT,
        .dma_buf_len = I2S_DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("Failed to install I2S driver!");
        return;
    }
    if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) {
        Serial.println("Failed to set I2S pins!");
        return;
    }
    Serial.println("I2S driver and pins configured successfully");
    openFlacFile(flacFiles[currentFileIndex]);
}

void checkNextButton() {
    static bool lastState = HIGH;
    bool state = digitalRead(NEXT_BUTTON_PIN);
    if (lastState == HIGH && state == LOW) {
        unsigned long now = millis();
        if (now - lastButtonPress > NEXT_BUTTON_DEBOUNCE_MS) {
            nextRequested = true;
            lastButtonPress = now;
        }
    }
    lastState = state;
}

void playNextFile() {
    currentFileIndex = (currentFileIndex + 1) % flacFiles.size();
    Serial.printf("Switching to next file: %s\n", flacFiles[currentFileIndex].c_str());
    openFlacFile(flacFiles[currentFileIndex]);
}

void loop() {
    checkNextButton();
    if (!active) {
        playNextFile();
        return;
    }
    if (nextRequested) {
        nextRequested = false;
        playNextFile();
        return;
    }
    uint32_t used = 0;
    int res = miniflac_decode(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, pcm);
    readBufferPos += used;
    if (res == MINIFLAC_OK) {
        size_t blocksize = decoder->frame.header.block_size;
        uint8_t ch = decoder->frame.header.channels;
        uint8_t bits = decoder->frame.header.bps;
        size_t outLen = 0;
        // Interleave and pack samples for I2S
        if (bits <= 8) {
            for (size_t i = 0; i < blocksize; ++i) {
                for (uint8_t c = 0; c < ch; ++c) {
                    i2sBuffer[outLen++] = (uint8_t)(pcm[c][i] << (8 - bits));
                }
            }
        } else if (bits <= 16) {
            for (size_t i = 0; i < blocksize; ++i) {
                for (uint8_t c = 0; c < ch; ++c) {
                    int16_t sample = (int16_t)(pcm[c][i] << (16 - bits));
                    i2sBuffer[outLen++] = sample & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 8) & 0xFF;
                }
            }
        } else if (bits <= 24) {
            for (size_t i = 0; i < blocksize; ++i) {
                for (uint8_t c = 0; c < ch; ++c) {
                    int32_t sample = pcm[c][i] << (24 - bits);
                    i2sBuffer[outLen++] = sample & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 8) & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 16) & 0xFF;
                }
            }
        } else if (bits <= 32) {
            for (size_t i = 0; i < blocksize; ++i) {
                for (uint8_t c = 0; c < ch; ++c) {
                    int32_t sample = pcm[c][i];
                    i2sBuffer[outLen++] = sample & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 8) & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 16) & 0xFF;
                    i2sBuffer[outLen++] = (sample >> 24) & 0xFF;
                }
            }
        }
        size_t bytesWritten = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, i2sBuffer, outLen, &bytesWritten, portMAX_DELAY);
        if (err != ESP_OK) {
            Serial.printf("I2S write error: %d\n", err);
            active = false;
            return;
        }
        if (miniflac_sync(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used) != MINIFLAC_OK) {
            if (flacFile.available()) {
                readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
                readBufferPos = 0;
            } else {
                Serial.println("End of file or read error");
                active = false;
                return;
            }
        } else {
            readBufferPos += used;
        }
    } else if (res == 0) { // MINIFLAC_CONTINUE
        if (flacFile.available()) {
            readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
            readBufferPos = 0;
        } else {
            Serial.println("End of file or read error");
            active = false;
            return;
        }
    } else {
        Serial.println("FLAC decode error!");
        active = false;
        return;
    }
}
