#pragma once

#include "mobu/media/Decoder.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>

namespace mobu::media {

struct SequenceOptions {
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    std::optional<std::int64_t> duration100ns;
    std::function<bool(std::uint64_t completed, std::uint64_t total)> progress;
};

struct SequenceResult {
    std::filesystem::path directory;
    std::filesystem::path firstFrame;
    VideoInfo sourceInfo;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    std::uint64_t frameCount = 0;
    std::int64_t duration100ns = 0;
    bool variableFrameRateResampled = false;
};

class SequenceCancelled : public std::runtime_error {
public:
    SequenceCancelled() : std::runtime_error("Video sequence preparation was cancelled.") {}
};

// Existing directories are refused because a failed preparation must never remove existing media.
SequenceResult WriteVideoSequence(
    IDecoder& openDecoder,
    const std::filesystem::path& newOutputDirectory,
    const SequenceOptions& options = {});

SequenceResult PrepareVideoSequence(
    const std::filesystem::path& source,
    const std::filesystem::path& newOutputDirectory,
    const SequenceOptions& options = {});

}
