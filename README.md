# ESP32MP3 — Open-Source High-Quality MP3/FLAC/WAV Audio Player

**ESP32MP3** is a fast, compact, and open-source audio player based on the ESP32. It supports MP3, FLAC, and WAV playback directly from SD card and is optimized for minimal latency, snappy controls, and excellent audio quality using an external DAC.

## Features

- 🎵 Supports **MP3**, **FLAC**, and **WAV** formats
- 🔁 **Shuffle playback** with persistent resume/bookmarking
- 🎚️ **Fixed volume steps** for precise control
- ⚡ **Snappy hardware button control** (volume, skip, previous)
- 💡 **LED feedback** for button actions
- 🪫 **Optimized for low power**
- 💾 Resume playback with last position even after reboot

## Hardware Requirements

- **ESP32 Dev Board** (e.g., ESP32-WROOM)
- **PCM5102A** DAC module (I2S output)
- **SD Card Module** (connected via SPI)
- **MAX4410** headphone amplifier (optional)
- 2x push buttons for volume and track control
- Onboard LED (GPIO2) for feedback
- Optional OLED display (planned)

### Wiring Overview

| ESP32 GPIO | Component    | Description     |
|------------|--------------|-----------------|
| GPIO5      | SD Card CS   | Chip Select     |
| GPIO18     | SD Card SCK  | SPI Clock       |
| GPIO19     | SD Card MISO | SPI MISO        |
| GPIO23     | SD Card MOSI | SPI MOSI        |
| GPIO22     | PCM5102A DIN | I2S Data        |
| GPIO25     | PCM5102A LRC | I2S L/R Clock   |
| GPIO26     | PCM5102A BCK | I2S Bit Clock   |
| GPIO27     | Button       | Volume Down     |
| GPIO33     | Button       | Volume Up       |
| GPIO2      | LED          | Feedback Blink  |

## Usage

1. Format an SD card as FAT32 and add your `.mp3`, `.wav`, or `.flac` files to the root directory (subfolders supported).
2. Flash the firmware using PlatformIO or Arduino IDE.
3. Press buttons to control:
   - **Short Press**:
     - Volume Up / Down in 16 predefined steps
   - **Long Press (>1.5s)**:
     - Volume Up → Skip track
     - Volume Down → Restart or go to previous track
4. The onboard LED will:
   - Blink **2×** for volume changes
   - Blink **5×** for track skips/restarts

## File System Details

- `shuffle.txt` — stores the current playback order and position
- `bookmark.txt` — stores current track index and byte offset for resume

## Building

This project uses **PlatformIO**. To build:

```bash
git clone https://github.com/GrafKnusprig/esp32mp3.git
cd esp32mp3
pio run
pio upload