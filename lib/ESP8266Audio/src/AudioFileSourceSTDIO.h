/*
    AudioFileSourceSTDIO
    Input SPIFFS "file" to be used by AudioGenerator
    Only for host-based testing, not Arduino

    Copyright (C) 2017  Earle F. Philhower, III

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _AUDIOFILESOURCESTDIO_H
#define _AUDIOFILESOURCESTDIO_H

#include <Arduino.h>

#ifndef ARDUINO

#include "AudioFileSource.h"

class AudioFileSourceSTDIO : public AudioFileSource {
public:
    AudioFileSourceSTDIO();
    AudioFileSourceSTDIO(const char *filename);
    virtual ~AudioFileSourceSTDIO() override;

    virtual bool open(const char *filename) override;
    virtual uint32_t read(void *data, uint32_t len) override;
    virtual bool seek(int32_t pos, int dir) override;
    virtual bool close() override;
    virtual bool isOpen() override;
    virtual uint32_t getSize() override;
    virtual uint32_t getPos() override {
        if (!f) {
            return 0;
        } else {
            return (uint32_t)ftell(f);
        }
    };

private:
    FILE *f;
};

#endif // !ARDUINO

#endif

