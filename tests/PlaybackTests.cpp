#include "mobu/media/Playback.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using mobu::media::MapTimelineToMediaTime;
using mobu::media::PlaybackSettings;

int main()
{
    int checks = 0;
    auto expect = [&](double time, const PlaybackSettings& settings, std::int64_t ticks) {
        const auto actual = MapTimelineToMediaTime(time, settings);
        if (std::abs(actual - ticks) > 1) {
            std::cerr << "Expected " << ticks << " ticks at " << time << ", got " << actual << '\n';
            throw std::runtime_error("Timeline mapping failed");
        }
        ++checks;
    };
    auto rejects = [&](double time, const PlaybackSettings& settings) {
        try { MapTimelineToMediaTime(time, settings); }
        catch (const std::invalid_argument&) { ++checks; return; }
        throw std::runtime_error("Invalid playback settings were accepted");
    };
    try {
        PlaybackSettings settings;
        settings.stopFrame = 29;
        expect(-1.0, settings, 0);
        expect(0.5, settings, 5000000);
        expect(2.0, settings, 9999999);
        settings.loop = true;
        expect(1.0, settings, 0);
        expect(2.25, settings, 2500000);
        settings.startFrame = 3;
        settings.stopFrame = 8;
        settings.frameRate = 10;
        settings.timeOffsetSeconds = 2.0;
        expect(1.0, settings, 3000000);
        expect(2.2, settings, 5000000);
        expect(2.6, settings, 3000000);
        settings.playSpeed = 2.0;
        expect(2.15, settings, 6000000);
        settings.playSpeed = -1.0;
        expect(2.0, settings, 8000000);
        expect(2.1, settings, 7000000);
        expect(2.6, settings, 8000000);
        settings.loop = false;
        expect(20.0, settings, 3000000);
        settings.playSpeed = 0.0;
        expect(20.0, settings, 3000000);
        settings = {};
        settings.frameRate = 30000.0 / 1001.0;
        settings.stopFrame = 299;
        expect(0.0333667, settings, 333667);
        settings.stopFrame = -1;
        settings.duration100ns = 20000000;
        settings.frameRate = 5000000.0 / 833333.0;
        settings.loop = true;
        expect(2.0, settings, 0);
        expect(2.5, settings, 5000000);
        settings.startFrame = 3;
        expect(2.0, settings, 9999996);
        settings.loop = false;
        settings.sourceRange = mobu::media::FrameRange{5000000, 8333333, 10000000};
        expect(0.0, settings, 5000000);
        expect(0.2, settings, 7000000);
        settings.playSpeed = -1;
        expect(0.0, settings, 8333333);
        expect(0.1, settings, 7333333);
        settings.loop = true;
        expect(0.5, settings, 8333333);
        settings = {};
        settings.frameRate = 0;
        rejects(0, settings);
        settings.frameRate = 30;
        settings.stopFrame = -1;
        rejects(0, settings);
        settings.stopFrame = 10;
        settings.startFrame = -1;
        rejects(0, settings);
        settings.startFrame = 0;
        rejects(std::numeric_limits<double>::quiet_NaN(), settings);
        settings.playSpeed = std::numeric_limits<double>::infinity();
        rejects(0, settings);
        std::cout << "PASS " << checks << " playback behavior checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
