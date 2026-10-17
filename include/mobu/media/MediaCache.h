#pragma once

#include "Audio.h"
#include "Sequence.h"

#include <functional>
#include <string>

namespace mobu::media {

struct MediaCacheOptions {
    bool videoSequence = true;
    bool includeAudio = true;
    bool alignAudioToVideo = true;
    bool preserveMovieTimeline = false;
    std::function<bool()> continueExport;
    std::function<void(unsigned)> progress;
};

struct PreparedMedia {
    std::filesystem::path directory;
    std::filesystem::path audioFile;
    SequenceResult video;
    AudioExportResult audio;
    std::string sourceSha256;
    bool reused = false;
    std::int64_t videoStart100ns = 0;
};

PreparedMedia PrepareMediaCache(const std::filesystem::path& source,
    const std::filesystem::path& cacheRoot, const MediaCacheOptions& options = {});

}
