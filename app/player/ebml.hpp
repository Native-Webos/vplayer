// SPDX-License-Identifier: GPL-3.0-or-later
//
// Just enough EBML (Matroska) reading for what VLC 3 does not hand the app: the Dolby Vision
// configuration record (dovi_detect.hpp) and attached fonts (mkv_attachments.hpp). Reads element
// headers and values from a FILE at given offsets; no allocation beyond what the caller asks for.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vplayer::ebml {

constexpr uint64_t kUnknownSize = ~uint64_t{0};

// Matroska element ids (with their marker bits).
constexpr uint32_t kHeader = 0x1A45DFA3, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB,
                   kSeekId = 0x53AB, kSeekPosition = 0x53AC, kTracks = 0x1654AE6B, kTrackEntry = 0xAE,
                   kTrackType = 0x83, kCluster = 0x1F43B675, kAttachments = 0x1941A469, kAttachedFile = 0x61A7,
                   kFileName = 0x466E, kFileMimeType = 0x4660, kFileData = 0x465C;

struct Element {
    uint32_t id = 0;
    uint64_t size = 0;   // kUnknownSize when the writer did not know it
    uint64_t data = 0;   // offset of the data
    uint64_t end() const { return size == kUnknownSize ? kUnknownSize : data + size; }
};

bool read_at(std::FILE *f, uint64_t at, void *buf, size_t n);
// The element whose header starts at `at`.
bool header(std::FILE *f, uint64_t at, Element *out);
uint64_t read_uint(std::FILE *f, const Element &e);
std::string read_string(std::FILE *f, const Element &e, size_t max = 4096);
bool read_bytes(std::FILE *f, const Element &e, std::vector<uint8_t> *out, size_t max);

// The Segment's data start and end (end clamped to the file), after the EBML header. False if the
// file is not Matroska/WebM.
bool segment(std::FILE *f, uint64_t file_size, uint64_t *data, uint64_t *end);

}  // namespace vplayer::ebml
