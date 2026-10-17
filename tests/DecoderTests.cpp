#include "mobu/media/Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct ExpectedFrame {
    std::int64_t presentation100ns = 0;
    std::int64_t duration100ns = 0;
    double blue = 0;
    double green = 0;
    double red = 0;
};

struct Fixture {
    std::string name;
    std::filesystem::path file;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t duration100ns = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    bool exactFrameRate = false;
    std::size_t expectedFrameCount = 0;
    std::vector<ExpectedFrame> frames;
};

void Require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::string> Split(const std::string& line)
{
    std::vector<std::string> result;
    std::istringstream input(line);
    std::string value;
    while (std::getline(input, value, '\t')) {
        result.push_back(value);
    }
    return result;
}

std::vector<Fixture> LoadFixtures(const std::filesystem::path& directory)
{
    std::ifstream input(directory / "manifest.tsv");
    Require(input.good(), "Cannot read fixture manifest; run run_decoder_integration.py first");
    std::map<std::string, Fixture> fixtures;
    std::string line;
    Require(static_cast<bool>(std::getline(input, line)) && line == "version\t1", "Unsupported manifest version");
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = Split(line);
        if (fields[0] == "fixture") {
            Require(fields.size() == 11, "Invalid fixture row");
            Fixture fixture;
            fixture.name = fields[1];
            fixture.file = directory / std::filesystem::u8path(fields[2]);
            fixture.width = static_cast<std::uint32_t>(std::stoul(fields[3]));
            fixture.height = static_cast<std::uint32_t>(std::stoul(fields[4]));
            fixture.duration100ns = std::stoll(fields[5]);
            fixture.frameRateNumerator = static_cast<std::uint32_t>(std::stoul(fields[6]));
            fixture.frameRateDenominator = static_cast<std::uint32_t>(std::stoul(fields[7]));
            fixture.exactFrameRate = fields[8] == "1";
            fixture.expectedFrameCount = std::stoull(fields[9]);
            Require(fields[10] == "BGRA", "Unsupported reference format");
            fixtures.emplace(fixture.name, std::move(fixture));
        } else if (fields[0] == "frame") {
            Require(fields.size() == 8, "Invalid frame row");
            auto& fixture = fixtures.at(fields[1]);
            ExpectedFrame frame;
            frame.presentation100ns = std::stoll(fields[2]);
            frame.duration100ns = std::stoll(fields[3]);
            frame.blue = std::stod(fields[4]);
            frame.green = std::stod(fields[5]);
            frame.red = std::stod(fields[6]);
            Require(std::stoull(fields[7]) == fixture.frames.size(), "Nonsequential reference frame index");
            fixture.frames.push_back(frame);
        } else {
            throw std::runtime_error("Unknown manifest row");
        }
    }
    std::vector<Fixture> result;
    for (auto& entry : fixtures) {
        Require(!entry.second.frames.empty(), "Fixture has no reference frames");
        Require(entry.second.frames.size() == entry.second.expectedFrameCount, "Reference frame count mismatch");
        result.push_back(std::move(entry.second));
    }
    Require(result.size() >= 4, "CFR, B-frame, VFR and Unicode fixtures are required");
    return result;
}

const ExpectedFrame& ExpectedAt(const Fixture& fixture, std::int64_t time100ns)
{
    const auto clamped = std::max<std::int64_t>(0, std::min(time100ns, fixture.duration100ns - 1));
    const auto next = std::upper_bound(fixture.frames.begin(), fixture.frames.end(), clamped,
        [](std::int64_t time, const ExpectedFrame& frame) { return time < frame.presentation100ns; });
    return next == fixture.frames.begin() ? fixture.frames.front() : *std::prev(next);
}

void VerifyFrame(const Fixture& fixture, const ExpectedFrame& expected, const mobu::media::VideoFrame& actual)
{
    Require(actual.width == fixture.width && actual.height == fixture.height, "Decoded dimensions differ from reference");
    Require(actual.bgra.size() == static_cast<std::size_t>(actual.width) * actual.height * 4,
        "Decoded BGRA buffer is not tightly packed");
    Require(std::llabs(actual.presentation100ns - expected.presentation100ns) <= 100,
        "Presentation timestamp differs: expected " + std::to_string(expected.presentation100ns)
        + ", actual " + std::to_string(actual.presentation100ns));
    Require(actual.duration100ns > 0, "Decoded frame duration is not positive");
    const auto markerSize = std::max<std::uint32_t>(4, std::min(actual.width, actual.height) / 12);
    const auto markerOffset = (static_cast<std::size_t>(markerSize / 2) * actual.width + markerSize / 2) * 4;
    Require(actual.bgra[markerOffset] >= 220 && actual.bgra[markerOffset + 1] >= 220
            && actual.bgra[markerOffset + 2] >= 220,
        "Top-left orientation marker moved; decoded rows may be inverted");

    double blue = 0;
    double green = 0;
    double red = 0;
    std::size_t pixels = 0;
    for (std::uint32_t y = actual.height / 4; y < actual.height * 3 / 4; ++y) {
        for (std::uint32_t x = actual.width / 4; x < actual.width * 3 / 4; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * actual.width + x) * 4;
            blue += actual.bgra[offset];
            green += actual.bgra[offset + 1];
            red += actual.bgra[offset + 2];
            Require(actual.bgra[offset + 3] == 255, "Decoded alpha is not opaque");
            ++pixels;
        }
    }
    const auto tolerance = 12.0;
    Require(std::abs(blue / pixels - expected.blue) <= tolerance
            && std::abs(green / pixels - expected.green) <= tolerance
            && std::abs(red / pixels - expected.red) <= tolerance,
        "Decoded frame colors differ from FFmpeg reference at PTS " + std::to_string(expected.presentation100ns)
        + ": expected BGR " + std::to_string(expected.blue) + "," + std::to_string(expected.green)
        + "," + std::to_string(expected.red) + "; actual " + std::to_string(blue / pixels)
        + "," + std::to_string(green / pixels) + "," + std::to_string(red / pixels));
}

void DecodeAndVerify(mobu::media::IDecoder& decoder, const Fixture& fixture, std::int64_t time100ns)
{
    VerifyFrame(fixture, ExpectedAt(fixture, time100ns), decoder.DecodeAt(time100ns));
}

void VerifyRange(const Fixture& fixture, const mobu::media::FrameRange& range,
    std::size_t firstIndex, std::size_t lastIndex)
{
    const auto expectedEnd = lastIndex + 1 < fixture.frames.size()
        ? fixture.frames[lastIndex + 1].presentation100ns : fixture.duration100ns;
    Require(std::llabs(range.first100ns - fixture.frames[firstIndex].presentation100ns) <= 100,
        "Trim start does not match the indexed frame PTS");
    Require(std::llabs(range.last100ns - fixture.frames[lastIndex].presentation100ns) <= 100,
        "Inclusive trim stop does not match the indexed frame PTS");
    Require(std::llabs(range.end100ns - expectedEnd) <= 100,
        "Exclusive trim end does not match the next frame PTS or container end");
}

template <typename Function>
void RequireThrows(Function&& function, const std::string& message)
{
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    Require(threw, message);
}

class TestRunner {
public:
    void Run(const std::string& name, const std::function<void()>& test)
    {
        try {
            test();
            ++passed_;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }

    int Result() const
    {
        std::cout << "RESULT passed=" << passed_ << " failed=" << failed_ << '\n';
        return failed_ == 0 ? 0 : 1;
    }

private:
    unsigned passed_ = 0;
    unsigned failed_ = 0;
};

}

int wmain(int argc, wchar_t* argv[])
{
    try {
        Require(argc == 3 && std::wstring(argv[1]) == L"--fixture-dir",
            "Usage: native_decoder_tests --fixture-dir <directory>");
        const auto directory = std::filesystem::path(argv[2]);
        const auto fixtures = LoadFixtures(directory);
        TestRunner runner;
        runner.Run("decoder operations before open fail", [] {
            const auto decoder = mobu::media::CreateNativeDecoder();
            RequireThrows([&] { decoder->DecodeAt(0); }, "DecodeAt before Open did not fail");
            RequireThrows([&] { decoder->ResolveFrameRange(0, -1); }, "ResolveFrameRange before Open did not fail");
            RequireThrows([&] { decoder->FramePresentationTimes(); }, "FramePresentationTimes before Open did not fail");
        });

        for (const auto& fixture : fixtures) {
            const auto decoder = mobu::media::CreateNativeDecoder();
            runner.Run(fixture.name + ": metadata", [&] {
                decoder->Open(fixture.file);
                const auto& info = decoder->Info();
                Require(info.width == fixture.width && info.height == fixture.height, "Metadata dimensions differ");
                Require(std::llabs(info.duration100ns - fixture.duration100ns) <= 10000,
                    "Metadata duration differs: expected " + std::to_string(fixture.duration100ns)
                    + ", actual " + std::to_string(info.duration100ns));
                Require(info.frameRateNumerator > 0 && info.frameRateDenominator > 0, "Invalid nominal frame rate");
                const auto actualRate = static_cast<double>(info.frameRateNumerator) / info.frameRateDenominator;
                std::cout << "INFO " << fixture.name << " nominal_fps=" << info.frameRateNumerator
                          << '/' << info.frameRateDenominator << " hardware_requested="
                          << info.hardwareAccelerationRequested << " presentation_origin_100ns="
                          << info.presentationOrigin100ns << '\n';
                if (fixture.exactFrameRate) {
                    const auto expectedRate = static_cast<double>(fixture.frameRateNumerator) / fixture.frameRateDenominator;
                    Require(std::abs(actualRate - expectedRate) <= expectedRate * 0.0001,
                        "CFR frame rate differs: expected " + std::to_string(expectedRate)
                        + ", actual " + std::to_string(actualRate));
                }
            });
            runner.Run(fixture.name + ": forward frame identity and PTS", [&] {
                decoder->Open(fixture.file);
                for (const auto& frame : fixture.frames) {
                    DecodeAndVerify(*decoder, fixture, frame.presentation100ns + 100);
                }
            });
            runner.Run(fixture.name + ": reverse frame identity and PTS", [&] {
                decoder->Open(fixture.file);
                for (auto frame = fixture.frames.rbegin(); frame != fixture.frames.rend(); ++frame) {
                    DecodeAndVerify(*decoder, fixture, frame->presentation100ns + 100);
                }
            });
            runner.Run(fixture.name + ": requests between frame timestamps", [&] {
                decoder->Open(fixture.file);
                for (std::size_t index = 0; index + 1 < fixture.frames.size(); ++index) {
                    const auto midpoint = fixture.frames[index].presentation100ns
                        + (fixture.frames[index + 1].presentation100ns - fixture.frames[index].presentation100ns) / 2;
                    DecodeAndVerify(*decoder, fixture, midpoint);
                }
            });
            runner.Run(fixture.name + ": repeated requests retain pixels", [&] {
                decoder->Open(fixture.file);
                const auto time = fixture.frames[fixture.frames.size() / 2].presentation100ns + 100;
                const auto first = decoder->DecodeAt(time);
                VerifyFrame(fixture, ExpectedAt(fixture, time), first);
                for (int count = 0; count < 3; ++count) {
                    const auto repeated = decoder->DecodeAt(time);
                    Require(first.presentation100ns == repeated.presentation100ns && first.bgra == repeated.bgra,
                        "Repeated request changed frame identity or pixels");
                }
            });
            runner.Run(fixture.name + ": negative and end times clamp", [&] {
                decoder->Open(fixture.file);
                for (const auto time : {std::numeric_limits<std::int64_t>::min(), std::int64_t(-1), std::int64_t(0),
                         fixture.duration100ns - 1, fixture.duration100ns, std::numeric_limits<std::int64_t>::max()}) {
                    DecodeAndVerify(*decoder, fixture, time);
                }
            });
            runner.Run(fixture.name + ": seeking backward after the final frame preserves frame identity", [&] {
                decoder->Open(fixture.file);
                DecodeAndVerify(*decoder, fixture, fixture.duration100ns - 1);
                const auto penultimate = fixture.frames.size() - 2;
                DecodeAndVerify(*decoder, fixture, fixture.frames[penultimate].presentation100ns + 100);
                DecodeAndVerify(*decoder, fixture, fixture.duration100ns - 1);
            });
            runner.Run(fixture.name + ": trim indices select the exact source frames", [&] {
                decoder->Open(fixture.file);
                const auto probeTime = fixture.frames[fixture.frames.size() / 2].presentation100ns + 100;
                const auto beforeRange = decoder->DecodeAt(probeTime);
                const auto range = decoder->ResolveFrameRange(3, 5);
                const auto afterRange = decoder->DecodeAt(probeTime);
                Require(beforeRange.presentation100ns == afterRange.presentation100ns
                        && beforeRange.bgra == afterRange.bgra,
                    "Resolving frame ranges changed the current decoded frame");
                VerifyRange(fixture, range, 3, 5);
                VerifyFrame(fixture, fixture.frames[3], decoder->DecodeAt(range.first100ns));
                VerifyFrame(fixture, fixture.frames[5], decoder->DecodeAt(range.last100ns));
                VerifyFrame(fixture, fixture.frames[5], decoder->DecodeAt(range.end100ns - 1));
            });
            runner.Run(fixture.name + ": presentation index preserves frame identity and current decode state", [&] {
                decoder->Open(fixture.file);
                const auto probeTime = fixture.frames[fixture.frames.size() / 2].presentation100ns + 100;
                const auto beforeIndex = decoder->DecodeAt(probeTime);
                auto times = decoder->FramePresentationTimes();
                Require(times.size() == fixture.frames.size(), "Presentation index frame count differs from the source");
                for (std::size_t index = 0; index < times.size(); ++index) {
                    Require(std::llabs(times[index] - fixture.frames[index].presentation100ns) <= 100,
                        "Presentation index timestamp differs from the source");
                }
                const auto afterIndex = decoder->DecodeAt(probeTime);
                Require(beforeIndex.presentation100ns == afterIndex.presentation100ns && beforeIndex.bgra == afterIndex.bgra,
                    "Reading the presentation index changed the current decoded frame");
                const auto originalFirst = times.front();
                times.front() = -1;
                Require(decoder->FramePresentationTimes().front() == originalFirst,
                    "A returned index copy can change the decoder's source timestamps");
            });
            runner.Run(fixture.name + ": whole, single and overflowing stop ranges retain the container end", [&] {
                decoder->Open(fixture.file);
                const auto lastIndex = fixture.frames.size() - 1;
                const auto all = decoder->ResolveFrameRange(0, -1);
                VerifyRange(fixture, all, 0, lastIndex);
                VerifyFrame(fixture, fixture.frames.front(), decoder->DecodeAt(all.first100ns));
                VerifyFrame(fixture, fixture.frames.back(), decoder->DecodeAt(all.end100ns - 1));
                const auto last = decoder->ResolveFrameRange(static_cast<std::int64_t>(lastIndex),
                    static_cast<std::int64_t>(lastIndex));
                VerifyRange(fixture, last, lastIndex, lastIndex);
                const auto overflow = decoder->ResolveFrameRange(3, std::numeric_limits<std::int64_t>::max());
                VerifyRange(fixture, overflow, 3, lastIndex);
                const auto trimAgain = decoder->ResolveFrameRange(3, 5);
                VerifyRange(fixture, trimAgain, 3, 5);
                VerifyFrame(fixture, fixture.frames[3], decoder->DecodeAt(trimAgain.first100ns));
            });
            runner.Run(fixture.name + ": invalid frame ranges fail", [&] {
                decoder->Open(fixture.file);
                RequireThrows([&] { decoder->ResolveFrameRange(-1, -1); }, "Negative start index was accepted");
                RequireThrows([&] { decoder->ResolveFrameRange(static_cast<std::int64_t>(fixture.frames.size()), -1); },
                    "Start index after the final source frame was accepted");
                RequireThrows([&] { decoder->ResolveFrameRange(std::numeric_limits<std::int64_t>::max(), -1); },
                    "Overflowing start index was accepted");
                RequireThrows([&] { decoder->ResolveFrameRange(5, 3); }, "Stop index before start was accepted");
            });
        }

        runner.Run("missing, corrupt, directory and audio-only inputs fail; reopening recovers", [&] {
            const auto decoder = mobu::media::CreateNativeDecoder();
            decoder->Open(fixtures.front().file);
            RequireThrows([&] { decoder->Open(directory / "does-not-exist.mp4"); }, "Missing file opened successfully");
            decoder->Open(fixtures.front().file);
            DecodeAndVerify(*decoder, fixtures.front(), 0);
            RequireThrows([&] { decoder->Open(directory / "corrupt.mp4"); }, "Corrupt file opened successfully");
            RequireThrows([&] { decoder->Open(directory); }, "Directory opened successfully");
            RequireThrows([&] { decoder->Open(directory / "audio-only.m4a"); }, "Audio-only file opened successfully");
            decoder->Open(fixtures.front().file);
            DecodeAndVerify(*decoder, fixtures.front(), 0);
        });
        runner.Run("reopening changes dimensions and frame state", [&] {
            const auto decoder = mobu::media::CreateNativeDecoder();
            for (const auto& fixture : fixtures) {
                decoder->Open(fixture.file);
                DecodeAndVerify(*decoder, fixture, fixture.duration100ns - 1);
                DecodeAndVerify(*decoder, fixture, 0);
            }
        });
        return runner.Result();
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n';
        return 2;
    }
}
