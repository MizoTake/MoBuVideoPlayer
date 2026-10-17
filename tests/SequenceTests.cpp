#include "mobu/media/Sequence.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
using namespace mobu::media;

void Require(bool condition, const char* description)
{
    if (!condition) {
        throw std::runtime_error(description);
    }
}

class Source final : public IDecoder {
public:
    VideoInfo info{7, 3, 20'000'000, 0, 6, 1, false};
    std::vector<std::int64_t> requests;
    int failAt = -1;
    bool invalidPixels = false;
    std::vector<std::int64_t> presentationTimes;

    Source()
    {
        for (std::int64_t index = 0; index < 12; ++index) {
            presentationTimes.push_back(index * 10'000'000 / 6);
        }
    }

    VideoFrame Frame(std::int64_t time) const
    {
        VideoFrame frame;
        frame.width = info.width;
        frame.height = info.height;
        frame.presentation100ns = time;
        frame.bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
        for (std::size_t i = 0; i < frame.bgra.size(); i += 4) {
            frame.bgra[i] = static_cast<std::uint8_t>((i / 4 + time / 10'000) % 256);
            frame.bgra[i + 1] = static_cast<std::uint8_t>((i / 4) / info.width * 50);
            frame.bgra[i + 2] = static_cast<std::uint8_t>((i / 4) % info.width * 25);
            frame.bgra[i + 3] = 255;
        }
        return frame;
    }

private:
    void Open(const std::filesystem::path&) override {}
    const VideoInfo& Info() const override { return info; }
    VideoFrame DecodeAt(std::int64_t time) override
    {
        if (static_cast<int>(requests.size()) == failAt) {
            throw std::runtime_error("Procedural decode failure");
        }
        requests.push_back(time);
        auto selected = std::upper_bound(presentationTimes.begin(), presentationTimes.end(), time);
        auto frame = Frame(selected == presentationTimes.begin() ? presentationTimes.front() : *std::prev(selected));
        if (invalidPixels) {
            frame.bgra.pop_back();
        }
        return frame;
    }
    FrameRange ResolveFrameRange(std::int64_t, std::int64_t) override { return {}; }
    std::vector<std::int64_t> FramePresentationTimes() override { return presentationTimes; }
};

std::vector<std::uint8_t> ReadPixels(const std::filesystem::path& path, UINT width, UINT height)
{
    ComPtr<IWICImagingFactory> factory;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))), "OS image factory is available");
    ComPtr<IWICBitmapDecoder> decoder;
    Require(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnLoad, &decoder)), "PNG file decodes through the OS");
    ComPtr<IWICBitmapFrameDecode> frame;
    Require(SUCCEEDED(decoder->GetFrame(0, &frame)), "PNG has one image frame");
    UINT actualWidth = 0;
    UINT actualHeight = 0;
    frame->GetSize(&actualWidth, &actualHeight);
    Require(width == actualWidth && height == actualHeight, "PNG preserves the source dimensions");
    ComPtr<IWICFormatConverter> converter;
    Require(SUCCEEDED(factory->CreateFormatConverter(&converter)), "PNG pixels can be read as BGRA8");
    Require(SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)), "PNG BGRA8 conversion succeeds");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
    Require(SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bytes.size()), bytes.data())),
        "PNG BGRA8 pixels can be copied");
    return bytes;
}

template<typename Exception = std::exception, typename Callback>
void Rejects(Callback callback, const char* description)
{
    try {
        callback();
    }
    catch (const Exception&) {
        return;
    }
    throw std::runtime_error(description);
}

void VerifyNativeFixtures(const std::filesystem::path& directory, const std::filesystem::path& root, int& checks)
{
    std::ifstream manifest(directory / "manifest.tsv");
    Require(manifest.good(), "Native sequence fixtures have a reference manifest");
    std::string line;
    std::size_t fixtures = 0;
    while (std::getline(manifest, line)) {
        if (!line.starts_with("fixture\t")) {
            continue;
        }
        std::istringstream row(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(row, field, '\t')) {
            fields.push_back(field);
        }
        Require(fields.size() == 11, "Native fixture metadata is complete");
        const std::u8string sourceName(fields[2].begin(), fields[2].end());
        const auto sourcePath = directory / std::filesystem::path(sourceName);
        const auto output = root / ("native-" + fields[1]);
        const auto result = PrepareVideoSequence(sourcePath, output);
        Require(result.variableFrameRateResampled == (fields[8] != "1"),
            "Source timestamp analysis distinguishes real CFR and VFR fixtures");
        const auto expectedCount = result.variableFrameRateResampled ? std::stoull(fields[9])
            : (std::stoull(fields[5]) * std::stoull(fields[6])
                + 10'000'000 * std::stoull(fields[7]) - 1) / (10'000'000 * std::stoull(fields[7]));
        std::cout << "INFO native sequence " << fields[1] << " expected_frames=" << expectedCount
                  << " actual_frames=" << result.frameCount << " fps=" << result.frameRateNumerator
                  << '/' << result.frameRateDenominator << " duration_ticks=" << result.sourceInfo.duration100ns << '\n';
        Require(result.frameCount == expectedCount, "Sequence frame count preserves source presentation duration");
        auto decoder = CreateNativeDecoder();
        decoder->Open(sourcePath);
        const auto times = decoder->FramePresentationTimes();
        for (std::uint64_t index = 0; index < result.frameCount; ++index) {
            const auto sample = !result.variableFrameRateResampled
                ? times[static_cast<std::size_t>((std::min)(index, static_cast<std::uint64_t>(times.size() - 1)))]
                : static_cast<std::int64_t>(index * 10'000'000 * result.frameRateDenominator / result.frameRateNumerator);
            const auto frame = decoder->DecodeAt(sample);
            std::wstring name = L"frame0000000000.png";
            const auto digits = std::to_wstring(index);
            name.replace(15 - digits.size(), digits.size(), digits);
            Require(ReadPixels(output / name, frame.width, frame.height) == frame.bgra,
                "Native source-to-PNG conversion preserves every selected BGRA pixel exactly");
            ++checks;
        }
        checks += 2;
        ++fixtures;
        std::cout << "PASS native sequence " << fields[1] << " frames=" << result.frameCount
                  << " fps=" << result.frameRateNumerator << '/' << result.frameRateDenominator
                  << " vfr_resampled=" << result.variableFrameRateResampled << '\n';
    }
    Require(fixtures >= 7, "Native sequence coverage includes CFR, VFR, B-frames, padded stride, offset, audio tail, and Unicode");
    ++checks;
}

}

int wmain(int argc, wchar_t** argv)
{
    int checks = 0;
    std::filesystem::path root;
    try {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Test COM initialization succeeds");
        root = std::filesystem::temp_directory_path() / (L"MoBuSequenceTests_" + std::to_wstring(GetCurrentProcessId()));
        Require(std::filesystem::create_directory(root), "Tests use a new output root");
        Source source;
        const auto output = root / L"\u52d5\u753b\u30b7\u30fc\u30b1\u30f3\u30b9";
        std::vector<std::uint64_t> progress;
        SequenceOptions options;
        options.progress = [&](std::uint64_t completed, std::uint64_t total) {
            Require(total == 12, "Progress reports the expected total frame count");
            progress.push_back(completed);
            return true;
        };
        const auto result = WriteVideoSequence(source, output, options);
        Require(result.frameCount == 12, "Two seconds at six FPS produces twelve images");
        Require(result.duration100ns == source.info.duration100ns, "The default sequence duration preserves source duration");
        Require(result.frameRateNumerator == 6 && result.frameRateDenominator == 1, "The source rational frame rate is preserved");
        Require(result.firstFrame == output / L"frame0000000000.png", "The returned sequence starts with frame zero");
        Require(progress.size() == 13 && progress.front() == 0 && progress.back() == 12, "Progress covers preparation through completion");
        checks += 5;
        for (std::size_t index = 0; index < source.requests.size(); ++index) {
            const auto expectedTime = static_cast<std::int64_t>(index * 10'000'000 / 6);
            Require(source.requests[index] == expectedTime, "The sequence samples at the requested CFR boundaries");
            std::wstring name = L"frame0000000000.png";
            auto digits = std::to_wstring(index);
            name.replace(15 - digits.size(), digits.size(), digits);
            Require(ReadPixels(output / name, source.info.width, source.info.height) == source.Frame(expectedTime).bgra,
                "PNG preserves every BGRA byte, row orientation, and opaque alpha");
            checks += 2;
        }
        Rejects([&] { WriteVideoSequence(source, output); }, "Existing output directories are rejected");
        Require(ReadPixels(result.firstFrame, source.info.width, source.info.height) == source.Frame(0).bgra,
            "Rejecting an existing output preserves all existing media");
        checks += 2;
        source.requests.clear();
        options = {};
        options.frameRateNumerator = 30000;
        options.frameRateDenominator = 1001;
        auto rational = WriteVideoSequence(source, root / L"rational", options);
        Require(rational.frameCount == 60, "Two seconds at 30000/1001 FPS keeps the partial final interval");
        Require(source.requests[1] == 333666 && source.requests[59] == 19686333,
            "NTSC frame times use exact integer rational division");
        checks += 2;
        source.requests.clear();
        source.failAt = 2;
        Rejects([&] { WriteVideoSequence(source, root / L"failure"); }, "Decode failures propagate");
        Require(!std::filesystem::exists(root / L"failure"), "Decode failures remove their partial sequence");
        checks += 2;
        source.requests.clear();
        source.failAt = -1;
        source.invalidPixels = true;
        Rejects([&] { WriteVideoSequence(source, root / L"invalid-pixels"); }, "Invalid decoded frame sizes are rejected");
        Require(!std::filesystem::exists(root / L"invalid-pixels"), "Invalid decoded frames leave no incomplete output");
        checks += 2;
        source.invalidPixels = false;
        source.requests.clear();
        options = {};
        options.progress = [](auto completed, auto) { return completed < 2; };
        Rejects<SequenceCancelled>([&] { WriteVideoSequence(source, root / L"cancel", options); }, "Cancellation uses the specific cancelled error");
        Require(!std::filesystem::exists(root / L"cancel"), "Cancellation removes its partial sequence");
        checks += 2;
        options = {};
        options.progress = [&](auto completed, auto) {
            if (completed == 1) {
                std::ofstream(root / L"unrelated" / L"keep.txt") << "keep";
                return false;
            }
            return true;
        };
        Rejects<SequenceCancelled>([&] { WriteVideoSequence(source, root / L"unrelated", options); }, "Cancellation after an external addition propagates");
        Require(std::filesystem::is_regular_file(root / L"unrelated" / L"keep.txt"), "Cleanup preserves unrelated files added to its output");
        Require(!std::filesystem::exists(root / L"unrelated" / L"frame0000000000.png"), "Cleanup removes only the generated images");
        checks += 3;
        options = {};
        options.frameRateNumerator = 30;
        options.frameRateDenominator = 0;
        Rejects<std::invalid_argument>([&] { WriteVideoSequence(source, root / L"invalid-rate", options); }, "A zero frame-rate denominator is rejected");
        Require(!std::filesystem::exists(root / L"invalid-rate"), "Invalid options create no directory");
        checks += 2;
        options.frameRateNumerator = std::numeric_limits<std::uint32_t>::max();
        options.frameRateDenominator = 1;
        source.info.duration100ns = std::numeric_limits<std::int64_t>::max();
        Rejects([&] { WriteVideoSequence(source, root / L"overflow", options); }, "Unrepresentable frame counts are rejected");
        Require(!std::filesystem::exists(root / L"overflow"), "Timing overflow creates no directory");
        checks += 2;
        source.info = {7, 3, 20'000'000, 0, 6, 1, false};
        source.requests.clear();
        options = {};
        options.frameRateNumerator = 12;
        options.frameRateDenominator = 2;
        rational = WriteVideoSequence(source, root / L"reduced", options);
        Require(rational.frameRateNumerator == 6 && rational.frameRateDenominator == 1, "Equivalent rates are reduced to a canonical rational");
        ++checks;
        source.requests.clear();
        options = {};
        options.duration100ns = 10'000'000;
        rational = WriteVideoSequence(source, root / L"shortened", options);
        Require(rational.duration100ns == 10'000'000 && rational.sourceInfo.duration100ns == 20'000'000
                && rational.frameCount == 6,
            "A one-second duration override produces six frames and preserves original source metadata");
        Require(source.requests.size() == 6 && source.requests.back() < *options.duration100ns,
            "Duration-limited output never samples at or beyond its effective end");
        checks += 2;
        options.duration100ns = 10'000'001;
        rational = WriteVideoSequence(source, root / L"one-tick-boundary", options);
        Require(rational.frameCount == 7, "An explicit duration preserves even a one-tick partial final interval");
        ++checks;
        options.duration100ns = 11'000'000;
        rational = WriteVideoSequence(source, root / L"partial-end", options);
        Require(rational.frameCount == 7 && rational.duration100ns == 11'000'000,
            "An effective duration with a partial final interval includes its last source frame");
        ++checks;
        for (const auto invalidDuration : {std::int64_t{0}, std::int64_t{-1}, std::int64_t{20'000'001}}) {
            options.duration100ns = invalidDuration;
            Rejects<std::invalid_argument>([&] { WriteVideoSequence(source, root / L"invalid-duration", options); },
                "Zero, negative, and longer-than-source sequence durations are rejected");
            Require(!std::filesystem::exists(root / L"invalid-duration"), "Invalid durations create no output directory");
            checks += 2;
        }
        source.info.frameRateNumerator = 5'000'000;
        source.info.frameRateDenominator = 833'333;
        options = {};
        rational = WriteVideoSequence(source, root / L"native-rounding", options);
        Require(rational.frameCount == 12 && rational.frameRateNumerator == 6 && rational.frameRateDenominator == 1,
            "Native period rounding is normalized from actual CFR timestamps without an extra frame");
        Require(std::equal(source.presentationTimes.begin(), source.presentationTimes.end(), source.requests.end() - 12),
            "Native nominal rounding still samples every actual CFR frame timestamp");
        checks += 2;
        options.frameRateNumerator = 5'000'000;
        options.frameRateDenominator = 833'333;
        rational = WriteVideoSequence(source, root / L"specified-rounding", options);
        Require(rational.frameCount == 13, "An explicit fractional rate retains a partial frame beyond the one-tick boundary tolerance");
        ++checks;
        source.info = {7, 3, 20'000'000, 0, 2, 1, false};
        source.presentationTimes = {0, 2'000'000, 8'000'000, 15'000'000};
        source.requests.clear();
        options = {};
        rational = WriteVideoSequence(source, root / L"variable", options);
        Require(rational.frameCount == 4 && rational.variableFrameRateResampled,
            "Variable frame durations are explicitly reported as CFR resampling");
        Require(source.requests == std::vector<std::int64_t>{0, 5'000'000, 10'000'000, 15'000'000},
            "VFR source time is sampled on the specified constant-rate output grid");
        const std::vector<std::int64_t> expectedVariable = {0, 2'000'000, 8'000'000, 15'000'000};
        for (std::size_t index = 0; index < 4; ++index) {
            std::wstring name = L"frame0000000000.png";
            name[14] = static_cast<wchar_t>(L'0' + index);
            Require(ReadPixels(rational.directory / name, source.info.width, source.info.height)
                    == source.Frame(expectedVariable[index]).bgra,
                "VFR resampling preserves the selected source pixels exactly");
        }
        checks += 6;
        source.presentationTimes = {0, 5'000'001, 10'000'000, 15'000'000};
        source.requests.clear();
        options.frameRateNumerator = 2;
        rational = WriteVideoSequence(source, root / L"quantized", options);
        Require(source.requests[1] == 5'000'001 && !rational.variableFrameRateResampled,
            "An output frame boundary includes its source timestamp rounded upward by one tick");
        ++checks;
        if (argc == 3 && std::wstring(argv[1]) == L"--fixture-dir") {
            VerifyNativeFixtures(argv[2], root, checks);
        } else if (argc == 2) {
            source.info = {64, 32, 20'000'000, 0, 6, 1, false};
            source.presentationTimes.clear();
            for (std::int64_t index = 0; index < 12; ++index) {
                source.presentationTimes.push_back(index * 10'000'000 / 6);
            }
            source.requests.clear();
            options = {};
            const auto probe = WriteVideoSequence(source, argv[1]);
            std::wcout << L"Story probe first image: " << probe.firstFrame << L'\n';
        }
        std::filesystem::remove_all(root);
        CoUninitialize();
        std::cout << "PASS " << checks << " sequence checks\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        if (!root.empty()) {
            std::cerr << "Test output retained for inspection.\n";
        }
        return 1;
    }
}
