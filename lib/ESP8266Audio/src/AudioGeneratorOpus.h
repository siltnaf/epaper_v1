/*
    AudioGeneratorOpus
    Audio output generator that plays Opus audio files

    Copyright (C) 2025  Earle F. Philhower, III

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

#ifndef _AUDIOGENERATOROPUS_H
#define _AUDIOGENERATOROPUS_H

#include <AudioGenerator.h>
#include "libopus/opus.h"

class AudioGeneratorOpus : public AudioGenerator {
public:
    AudioGeneratorOpus();
    virtual ~AudioGeneratorOpus() override;
    virtual bool begin(AudioFileSource *source, AudioOutput *output) override;
    virtual bool loop() override;
    virtual bool stop() override;
    virtual bool isRunning() override;
    // Reserve the largest allocation before callers create file/output objects
    // that can fragment a constrained internal heap. begin() also calls this,
    // so pre-reservation is optional for callers with sufficient memory.
    bool reserveDecoder();
    // Reserve decoded PCM after the playback task has claimed its stack but
    // before callers construct smaller file/output objects.
    bool reserveBuffers();
    // Retained for application diagnostics: 1 reserves the decoder, 2 reserves
    // PCM, 3 initializes output, and 4 is ready to decode.
    int getLastError() const { return lastError; }
    int getOpenStage() const { return openStage; }

private:
    OpusDecoder *od = nullptr;
    bool decoderUsesStaticWorkspace = false;

    // RFC 6716 limits an Opus audio packet to 1275 bytes. Keeping this in the
    // generator object removes a second heap allocation from constrained begin().
    uint8_t packet[1275] = {}; // Raw compressed, demuxed packet
    uint32_t packetOff;
    bool discardingTags = false;
    opus_int16 *buff; // Decoded PCM
    uint32_t buffPtr;
    uint32_t buffLen;

    bool demux();
    uint8_t hdr[27]; // Page header
    enum {WaitHeader, WaitSegment, ReadPacket} state;
    uint8_t type;
    uint64_t agp;
    uint32_t ssn;
    uint32_t psn;
    uint32_t pcs;
    uint16_t ps; // packet lacing segments
    uint16_t readPS;
    uint8_t seg[256]; // Packet lacing in the current page
    uint16_t curSeg;
    uint32_t lacingBytesToRead;
    void processPacket();
    // From the OpusHead
    uint16_t preskip;
    uint8_t channels;
    uint32_t samplerate;
    uint16_t gain;
    int lastError = 0;
    int openStage = 0;
};

#endif

