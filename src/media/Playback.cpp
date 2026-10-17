#include "mobu/media/Playback.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mobu::media {

std::int64_t MapTimelineToMediaTime(double timelineSeconds, const PlaybackSettings& settings)
{
    if (!std::isfinite(timelineSeconds) || !std::isfinite(settings.timeOffsetSeconds)
            || !std::isfinite(settings.playSpeed) || !std::isfinite(settings.frameRate)
            || settings.frameRate <= 0.0 || settings.startFrame < 0
            || settings.stopFrame < -1
            || (settings.stopFrame == -1 && settings.duration100ns <= 0)
            || (settings.stopFrame >= 0 && settings.stopFrame < settings.startFrame)) {
        throw std::invalid_argument("Invalid native video playback settings.");
    }
    const long double first = settings.sourceRange
        ? static_cast<long double>(settings.sourceRange->first100ns) / 10000000.0L
        : static_cast<long double>(settings.startFrame) / settings.frameRate;
    const long double end = settings.sourceRange
        ? static_cast<long double>(settings.sourceRange->end100ns) / 10000000.0L
        : settings.stopFrame == -1
        ? static_cast<long double>(settings.duration100ns) / 10000000.0L
        : (static_cast<long double>(settings.stopFrame) + 1.0L) / settings.frameRate;
    if (first < 0 || end <= first) {
        throw std::invalid_argument("Native video trim begins past the end of the source.");
    }
    const long double last = settings.sourceRange
        ? static_cast<long double>(settings.sourceRange->last100ns) / 10000000.0L
        : std::max(first, end - 1.0L / settings.frameRate);
    if (last < first || last >= end) {
        throw std::invalid_argument("Native video frame bounds are inconsistent.");
    }
    const long double period = end - first;
    const long double elapsed = std::max(0.0L,
        static_cast<long double>(timelineSeconds) - settings.timeOffsetSeconds);
    const long double distance = elapsed * std::abs(static_cast<long double>(settings.playSpeed));
    long double source;
    if (settings.loop) {
        const long double phase = std::fmod(distance, period);
        if (settings.playSpeed < 0.0) {
            // The last frame's interval must be included when wrapping backwards.
            const long double reverse = std::fmod(last - first - phase + period, period);
            source = first + reverse;
        } else {
            source = first + phase;
        }
    } else {
        // Keep the final frame's entire timestamp interval, including variable frame rate tails.
        const long double maximum = first + period - 0.0000001L;
        source = settings.playSpeed < 0.0
            ? std::max(first, last - distance)
            : std::min(maximum, first + distance);
    }
    const long double ticks = source * 10000000.0L;
    if (!std::isfinite(ticks) || ticks < 0
            || ticks > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("Native video time exceeds the supported range.");
    }
    return static_cast<std::int64_t>(std::round(ticks));
}

}
