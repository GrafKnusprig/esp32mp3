#define MINIFLAC_IMPLEMENTATION
extern "C" {
#include "../lib/miniflac/miniflac.h"
}
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>

#define SD_CS 5
#define I2S_BCLK 26
#define I2S_LRC 25
#define I2S_DOUT 22
#define FLAC_READ_BUFFER_SIZE 4096
#define FLAC_MAX_CHANNELS 8

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

String findFirstFlacFile(File dir) {
    while (File entry = dir.openNextFile()) {
        if (entry.isDirectory()) {
            String found = findFirstFlacFile(entry);
            entry.close();
            if (found != "") {
                return found;
            }
        } else {
            String name = entry.name();
            if (name.endsWith(".flac") || name.endsWith(".FLAC")) {
                String path = String(entry.path());
                entry.close();
                return path;
            }
        }
        entry.close();
    }
    return "";
}

void setup() {
    Serial.begin(115200);
    Serial.println("Starting setup...");
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card initialization failed!");
        return;
    }
    Serial.println("SD Card initialized successfully");
    delay(1000);
    Serial.println("Searching for first FLAC file...");
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("Failed to open root directory!");
        return;
    }
    String currentFile = findFirstFlacFile(root);
    root.close();
    if (currentFile == "") {
        Serial.println("No FLAC file found on SD card!");
        return;
    }
    Serial.printf("First FLAC file found: %s\n", currentFile.c_str());
    Serial.println("Initializing I2S...");
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100, // will update after STREAMINFO
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // will update after STREAMINFO
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 64,
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
    flacFile = SD.open(currentFile);
    if (!flacFile) {
        Serial.println("Failed to open FLAC file!");
        return;
    }
    readBuffer = (uint8_t*)malloc(FLAC_READ_BUFFER_SIZE);
    if (!readBuffer) {
        Serial.println("Failed to allocate read buffer!");
        return;
    }
    decoder = (miniflac_t*)malloc(miniflac_size());
    if (!decoder) {
        Serial.println("Failed to allocate decoder!");
        return;
    }
    miniflac_init(decoder, MINIFLAC_CONTAINER_UNKNOWN);
    readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
    readBufferPos = 0;
    uint32_t used = 0;
    // Sync to first metadata block
    if (miniflac_sync(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used) != MINIFLAC_OK) {
        Serial.println("FLAC sync error!");
        return;
    }
    readBufferPos += used;
    // Parse metadata blocks, allocate buffers after STREAMINFO
    bool streaminfoFound = false;
    while (decoder->state == MINIFLAC_METADATA) {
        if (decoder->metadata.header.type == MINIFLAC_METADATA_STREAMINFO) {
            // Parse streaminfo
            uint16_t min_block, max_block;
            uint32_t min_frame, max_frame, sr;
            uint8_t ch, bps_val;
            uint64_t total_samples;
            if (miniflac_streaminfo_min_block_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &min_block) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_max_block_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &max_block) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_min_frame_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &min_frame) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_max_frame_size(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &max_frame) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_sample_rate(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &sr) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_channels(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &ch) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_bps(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &bps_val) != MINIFLAC_OK) return;
            readBufferPos += used;
            if (miniflac_streaminfo_total_samples(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used, &total_samples) != MINIFLAC_OK) return;
            readBufferPos += used;
            sampleRate = sr;
            channels = ch;
            bps = bps_val;
            blockSize = max_block;
            Serial.printf("FLAC stream: %u Hz, %u ch, %u bps, blocksize %u\n", sampleRate, channels, bps, blockSize);
            // (Re)allocate PCM and I2S buffers
            for (int i = 0; i < FLAC_MAX_CHANNELS; ++i) {
                if (pcm[i]) free(pcm[i]);
                pcm[i] = nullptr;
            }
            for (int i = 0; i < channels; ++i) {
                pcm[i] = (int32_t*)malloc(sizeof(int32_t) * blockSize);
                if (!pcm[i]) {
                    Serial.printf("Failed to allocate PCM buffer for channel %d!\n", i);
                    return;
                }
            }
            i2sBufferSize = blockSize * channels * 4;
            if (i2sBuffer) free(i2sBuffer);
            i2sBuffer = (uint8_t*)malloc(i2sBufferSize);
            if (!i2sBuffer) {
                Serial.println("Failed to allocate I2S buffer!");
                return;
            }
            // Update I2S config
            i2s_set_clk(I2S_NUM_0, sampleRate, (bps <= 16) ? I2S_BITS_PER_SAMPLE_16BIT : I2S_BITS_PER_SAMPLE_32BIT, (channels == 2) ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);
            streaminfoFound = true;
        }
        // Skip rest of metadata block
        if (miniflac_sync(decoder, &readBuffer[readBufferPos], readBufferLen - readBufferPos, &used) != MINIFLAC_OK) {
            // If not enough data, refill buffer
            if (flacFile.available()) {
                readBufferLen = flacFile.read(readBuffer, FLAC_READ_BUFFER_SIZE);
                readBufferPos = 0;
            } else {
                Serial.println("End of file or read error during metadata!");
                return;
            }
        } else {
            readBufferPos += used;
        }
    }
    if (!streaminfoFound) {
        Serial.println("STREAMINFO block not found!");
        return;
    }
    active = true;
}

void loop() {
    if (!active) return;
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
        // Sync to next frame
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
