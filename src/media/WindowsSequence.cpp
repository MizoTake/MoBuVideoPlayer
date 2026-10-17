#include "mobu/media/Sequence.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <intrin.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace mobu::media {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t TicksPerSecond = 10'000'000;

void Check(HRESULT status, const char* operation)
{
    if (FAILED(status)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << static_cast<std::uint32_t>(status) << ").";
        throw std::runtime_error(message.str());
    }
}

class ComScope {
public:
    ComScope() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization"); }
    ~ComScope() { CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

struct Division {
    std::uint64_t quotient;
    std::uint64_t remainder;
};

std::filesystem::path FramePath(const std::filesystem::path& directory, std::uint64_t index)
{
    std::wostringstream name;
    name << L"frame" << std::setfill(L'0') << std::setw(10) << index << L".png";
    return directory / name.str();
}

Division MultiplyDivide(std::uint64_t value, std::uint64_t multiplier, std::uint64_t divisor)
{
    std::uint64_t high = 0;
    const auto low = _umul128(value, multiplier, &high);
    if (!divisor || high >= divisor) {
        throw std::overflow_error("The video sequence timing is too large.");
    }
    std::uint64_t remainder = 0;
    const auto quotient = _udiv128(high, low, divisor, &remainder);
    return {quotient, remainder};
}

bool MatchesGrid(const std::vector<std::int64_t>& times,
    std::uint32_t numerator, std::uint32_t denominator, bool accumulatedTolerance)
{
    for (std::size_t index = 0; index < times.size(); ++index) {
        const auto expected = MultiplyDivide(index, TicksPerSecond * denominator, numerator).quotient;
        const auto actual = static_cast<std::uint64_t>(times[index]);
        const auto difference = expected > actual ? expected - actual : actual - expected;
        if (difference > (accumulatedTolerance ? index + 1 : 1)) {
            return false;
        }
    }
    return true;
}

void NormalizeConstantRate(const std::vector<std::int64_t>& times, SequenceResult& result)
{
    if (times.size() < 2 || times.back() <= 0) {
        return;
    }
    std::uint64_t numerator = (times.size() - 1) * TicksPerSecond;
    auto denominator = static_cast<std::uint64_t>(times.back());
    std::uint64_t previousNumerator = 1;
    std::uint64_t priorNumerator = 0;
    std::uint64_t previousDenominator = 0;
    std::uint64_t priorDenominator = 1;
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    while (denominator) {
        const auto term = numerator / denominator;
        if ((previousNumerator && term > (maximum - priorNumerator) / previousNumerator)
            || (previousDenominator && term > (maximum - priorDenominator) / previousDenominator)) {
            return;
        }
        const auto candidateNumerator = term * previousNumerator + priorNumerator;
        const auto candidateDenominator = term * previousDenominator + priorDenominator;
        if (candidateNumerator && candidateDenominator
            && MatchesGrid(times, static_cast<std::uint32_t>(candidateNumerator),
                static_cast<std::uint32_t>(candidateDenominator), false)) {
            result.frameRateNumerator = static_cast<std::uint32_t>(candidateNumerator);
            result.frameRateDenominator = static_cast<std::uint32_t>(candidateDenominator);
            return;
        }
        priorNumerator = previousNumerator;
        previousNumerator = candidateNumerator;
        priorDenominator = previousDenominator;
        previousDenominator = candidateDenominator;
        const auto remainder = numerator % denominator;
        numerator = denominator;
        denominator = remainder;
    }
}

struct OwnedDirectory {
    explicit OwnedDirectory(const std::filesystem::path& directory) : path(directory)
    {
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("The video sequence output directory already exists.");
        }
    }

    ~OwnedDirectory()
    {
        if (!committed) {
            std::error_code ignored;
            for (std::uint64_t index = 0; index < generatedFiles; ++index) {
                std::filesystem::remove(FramePath(path, index), ignored);
            }
            // A recursive removal could delete unrelated files created while conversion was running.
            std::filesystem::remove(path, ignored);
        }
    }

    OwnedDirectory(const OwnedDirectory&) = delete;
    OwnedDirectory& operator=(const OwnedDirectory&) = delete;

    std::filesystem::path path;
    std::uint64_t generatedFiles = 0;
    bool committed = false;
};

void EncodePng(IWICImagingFactory& factory, const VideoFrame& frame, const std::filesystem::path& path)
{
    ComPtr<IWICStream> stream;
    Check(factory.CreateStream(&stream), "PNG stream creation");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG file creation");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory.CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder creation");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG encoder initialization");
    ComPtr<IWICBitmapFrameEncode> output;
    ComPtr<IPropertyBag2> properties;
    Check(encoder->CreateNewFrame(&output, &properties), "PNG frame creation");
    Check(output->Initialize(properties.Get()), "PNG frame initialization");
    Check(output->SetSize(frame.width, frame.height), "PNG dimensions");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    Check(output->SetPixelFormat(&format), "PNG pixel format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        throw std::runtime_error("The OS PNG encoder did not accept lossless BGRA8 pixels.");
    }
    Check(output->WritePixels(frame.height, frame.width * 4,
        static_cast<UINT>(frame.bgra.size()), const_cast<BYTE*>(frame.bgra.data())), "PNG pixel write");
    Check(output->Commit(), "PNG frame commit");
    Check(encoder->Commit(), "PNG encoder commit");
}

}

SequenceResult WriteVideoSequence(
    IDecoder& openDecoder,
    const std::filesystem::path& newOutputDirectory,
    const SequenceOptions& options)
{
    SequenceResult result;
    result.sourceInfo = openDecoder.Info();
    result.duration100ns = options.duration100ns.value_or(result.sourceInfo.duration100ns);
    result.frameRateNumerator = options.frameRateNumerator
        ? options.frameRateNumerator : result.sourceInfo.frameRateNumerator;
    result.frameRateDenominator = options.frameRateNumerator
        ? options.frameRateDenominator : result.sourceInfo.frameRateDenominator;
    if (!result.frameRateNumerator || !result.frameRateDenominator
        || result.sourceInfo.duration100ns <= 0 || !result.sourceInfo.width || !result.sourceInfo.height) {
        throw std::invalid_argument("Video dimensions, duration, and sequence frame rate must be positive.");
    }
    if (result.duration100ns <= 0 || result.duration100ns > result.sourceInfo.duration100ns) {
        throw std::invalid_argument("The sequence duration must be positive and within the source duration.");
    }
    if (result.sourceInfo.width > std::numeric_limits<UINT>::max() / 4
        || result.sourceInfo.height > std::numeric_limits<UINT>::max() / (result.sourceInfo.width * 4)) {
        throw std::invalid_argument("The video frame is too large for the OS PNG encoder.");
    }
    const auto pixelBytes = static_cast<std::uint64_t>(result.sourceInfo.width)
        * result.sourceInfo.height * 4;
    const auto divisor = std::gcd(result.frameRateNumerator, result.frameRateDenominator);
    result.frameRateNumerator /= divisor;
    result.frameRateDenominator /= divisor;
    auto presentationTimes = openDecoder.FramePresentationTimes();
    if (presentationTimes.empty() || presentationTimes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || presentationTimes.front() != 0
        || !std::is_sorted(presentationTimes.begin(), presentationTimes.end())
        || std::adjacent_find(presentationTimes.begin(), presentationTimes.end()) != presentationTimes.end()) {
        throw std::runtime_error("Video frame presentation times must start at zero and increase strictly.");
    }
    const bool constantRate = result.sourceInfo.frameRateNumerator && result.sourceInfo.frameRateDenominator
        && MatchesGrid(presentationTimes, result.sourceInfo.frameRateNumerator,
            result.sourceInfo.frameRateDenominator, true);
    result.variableFrameRateResampled = !constantRate;
    if (!options.frameRateNumerator && constantRate
        && !MatchesGrid(presentationTimes, result.frameRateNumerator, result.frameRateDenominator, false)) {
        // Native rate attributes can encode a rounded period; fit actual PTS rather than dropping source frames.
        NormalizeConstantRate(presentationTimes, result);
    }
    const auto intervalNumerator = TicksPerSecond * result.frameRateDenominator;
    const auto count = MultiplyDivide(static_cast<std::uint64_t>(result.duration100ns),
        result.frameRateNumerator, intervalNumerator);
    result.frameCount = count.quotient;
    if (count.remainder != 0) {
        const auto boundary = MultiplyDivide(count.quotient, intervalNumerator,
            result.frameRateNumerator).quotient;
        const auto excessTicks = static_cast<std::uint64_t>(result.duration100ns) - boundary;
        const auto tolerance = options.duration100ns ? 0 : options.frameRateNumerator ? 1 : count.quotient;
        // Native nominal rates can contain a one-tick period rounding error accumulated across frames.
        if (!count.quotient || excessTicks > tolerance) {
            ++result.frameCount;
        }
    }
    if (!result.frameCount || result.frameCount > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("The video sequence exceeds MotionBuilder's supported frame count.");
    }
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)), "OS imaging factory creation");
    OwnedDirectory output(newOutputDirectory);
    result.directory = newOutputDirectory;
    if (options.progress && !options.progress(0, result.frameCount)) {
        throw SequenceCancelled();
    }
    for (std::uint64_t index = 0; index < result.frameCount; ++index) {
        auto ticks = MultiplyDivide(index, intervalNumerator, result.frameRateNumerator).quotient;
        if (!options.frameRateNumerator && constantRate && index < presentationTimes.size()) {
            ticks = static_cast<std::uint64_t>(presentationTimes[static_cast<std::size_t>(index)]);
        } else {
            const auto next = std::lower_bound(presentationTimes.begin(), presentationTimes.end(), static_cast<std::int64_t>(ticks));
            const auto tolerance = options.frameRateNumerator ? 1 : index + 1;
            // An ideal rational boundary can precede its quantized source PTS by a native clock tick.
            if (next != presentationTimes.end() && static_cast<std::uint64_t>(*next) - ticks <= tolerance) {
                ticks = static_cast<std::uint64_t>(*next);
            }
        }
        ticks = (std::min)(ticks, static_cast<std::uint64_t>(result.duration100ns - 1));
        auto frame = openDecoder.DecodeAt(static_cast<std::int64_t>(ticks));
        if (frame.width != result.sourceInfo.width || frame.height != result.sourceInfo.height
            || frame.bgra.size() != pixelBytes) {
            throw std::runtime_error("The decoder returned incompatible video pixels during sequence preparation.");
        }
        const auto file = FramePath(output.path, index);
        const auto reserved = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reserved == INVALID_HANDLE_VALUE) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "Reserve new PNG file");
        }
        CloseHandle(reserved);
        ++output.generatedFiles;
        EncodePng(*factory.Get(), frame, file);
        if (index == 0) {
            result.firstFrame = file;
        }
        if (options.progress && !options.progress(index + 1, result.frameCount)) {
            throw SequenceCancelled();
        }
    }
    output.committed = true;
    return result;
}

SequenceResult PrepareVideoSequence(
    const std::filesystem::path& source,
    const std::filesystem::path& newOutputDirectory,
    const SequenceOptions& options)
{
    auto decoder = CreateNativeDecoder();
    decoder->Open(source);
    return WriteVideoSequence(*decoder, newOutputDirectory, options);
}

}
