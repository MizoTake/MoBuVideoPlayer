#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace mobu::media {

struct TrackPresentationTimeline {
    bool hasEdit = false;
    std::int64_t offset100ns = 0;
    std::optional<std::int64_t> end100ns;
    std::optional<std::int64_t> firstPresentation100ns;
};

struct PresentationTimeline {
    TrackPresentationTimeline audio;
    TrackPresentationTimeline video;
};

PresentationTimeline ReadPresentationTimeline(const std::filesystem::path& source);

struct AudioExportOptions {
    std::int64_t timelineOrigin100ns = 0;
    std::uint64_t maxDataBytes = 0xFFFFFFB0ULL;
    std::function<bool(std::uint64_t frames)> continueExport;
};

struct AudioExportResult {
    bool hasAudio = false;
    bool precisionReduced = false;
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint32_t channelMask = 0;
    std::uint64_t frameCount = 0;
    std::int64_t firstPresentation100ns = 0;
    std::int64_t duration100ns = 0;
};

AudioExportResult ExportAudioWave(const std::filesystem::path& source,
    const std::filesystem::path& destination, const AudioExportOptions& options = {});

}
