#pragma once

#include "mobu/media/Decoder.h"

#include <cstdint>
#include <optional>

namespace mobu::media {

struct PlaybackSettings {
    double timeOffsetSeconds = 0.0;
    double frameRate = 30.0;
    double playSpeed = 1.0;
    std::int64_t startFrame = 0;
    std::int64_t stopFrame = 0;
    std::int64_t duration100ns = 0;
    bool loop = false;
    std::optional<FrameRange> sourceRange;
};

std::int64_t MapTimelineToMediaTime(double timelineSeconds, const PlaybackSettings& settings);

}
