#include "mobu/media/Audio.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mobu::media {
namespace {

using Microsoft::WRL::ComPtr;
constexpr DWORD AudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD AllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);

constexpr std::uint32_t Tag(char a, char b, char c, char d)
{
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(b) << 16)
        | (static_cast<std::uint32_t>(c) << 8) | static_cast<std::uint32_t>(d);
}

std::uint32_t Read32(std::span<const BYTE> data, std::size_t offset)
{
    if (offset > data.size() || data.size() - offset < 4) {
        throw std::runtime_error("The MP4 timeline metadata is truncated.");
    }
    return (static_cast<std::uint32_t>(data[offset]) << 24)
        | (static_cast<std::uint32_t>(data[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

std::uint64_t Read64(std::span<const BYTE> data, std::size_t offset)
{
    return (static_cast<std::uint64_t>(Read32(data, offset)) << 32) | Read32(data, offset + 4);
}

struct MetadataBox {
    std::uint32_t tag = 0;
    std::span<const BYTE> payload;
};

std::vector<MetadataBox> Boxes(std::span<const BYTE> data)
{
    std::vector<MetadataBox> result;
    std::size_t offset = 0;
    while (offset < data.size()) {
        if (data.size() - offset < 8 || result.size() >= 100000) {
            throw std::runtime_error("The MP4 timeline atom table is invalid or too large.");
        }
        const auto tag = Read32(data, offset + 4);
        std::uint64_t size = Read32(data, offset);
        std::size_t header = 8;
        if (size == 1) {
            size = Read64(data, offset + 8);
            header = 16;
        } else if (size == 0) {
            size = data.size() - offset;
        }
        if (size < header || size > data.size() - offset) {
            throw std::runtime_error("The MP4 timeline atom extends outside its parent.");
        }
        result.push_back({tag, data.subspan(offset + header, static_cast<std::size_t>(size) - header)});
        offset += static_cast<std::size_t>(size);
    }
    return result;
}

std::int64_t TimelineTicks(long double value)
{
    if (!std::isfinite(value) || value <= static_cast<long double>((std::numeric_limits<std::int64_t>::min)()) + 1
            || value >= static_cast<long double>((std::numeric_limits<std::int64_t>::max)()) - 1) {
        throw std::runtime_error("The media presentation time exceeds the supported timeline range.");
    }
    return std::llround(value);
}

std::uint32_t TimeScale(std::span<const BYTE> payload)
{
    if (payload.empty() || payload[0] > 1) {
        throw std::runtime_error("The MP4 timeline header has an unsupported version.");
    }
    const auto scale = Read32(payload, payload[0] == 0 ? 12 : 20);
    if (!scale) {
        throw std::runtime_error("The MP4 timeline has a zero time scale.");
    }
    return scale;
}

TrackPresentationTimeline ParseEdits(std::span<const BYTE> payload,
    std::uint32_t movieScale, std::uint32_t mediaScale)
{
    if (payload.empty() || payload[0] > 1) {
        throw std::runtime_error("The MP4 edit list has an unsupported version.");
    }
    const bool wide = payload[0] == 1;
    const auto count = Read32(payload, 4);
    const std::size_t entrySize = wide ? 20 : 12;
    if (count > 100000 || payload.size() < 8 || (payload.size() - 8) / entrySize < count) {
        throw std::runtime_error("The MP4 edit list is truncated or too large.");
    }
    if (!count) {
        return {};
    }
    long double emptyDuration = 0;
    TrackPresentationTimeline result;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t position = 8 + index * entrySize;
        const auto duration = wide ? Read64(payload, position) : Read32(payload, position);
        const std::int64_t mediaStart = wide ? static_cast<std::int64_t>(Read64(payload, position + 8))
            : static_cast<std::int32_t>(Read32(payload, position + 4));
        const auto rate = Read32(payload, position + (wide ? 16 : 8));
        if (rate != 0x10000 || result.hasEdit || mediaStart < -1) {
            throw std::runtime_error("This MP4 uses multiple media edits or a changed edit rate; its A/V timing cannot be preserved.");
        }
        const long double durationTicks = static_cast<long double>(duration) * 10000000.0L / movieScale;
        if (mediaStart == -1) {
            emptyDuration += durationTicks;
            continue;
        }
        result.hasEdit = true;
        result.offset100ns = TimelineTicks(emptyDuration - static_cast<long double>(mediaStart) * 10000000.0L / mediaScale);
        result.end100ns = TimelineTicks(emptyDuration + durationTicks);
    }
    if (!result.hasEdit) {
        throw std::runtime_error("The MP4 edit list contains no playable media segment.");
    }
    return result;
}

struct TimingRun {
    std::uint32_t count = 0;
    std::int64_t value = 0;
};

std::vector<TimingRun> ParseTiming(std::span<const BYTE> payload, bool composition)
{
    if (payload.empty() || payload[0] > (composition ? 1 : 0)) {
        throw std::runtime_error("The MP4 sample timing table has an unsupported version.");
    }
    const auto entries = Read32(payload, 4);
    if (payload.size() < 8 || (payload.size() - 8) / 8 < entries) {
        throw std::runtime_error("The MP4 sample timing table is truncated.");
    }
    std::vector<TimingRun> runs;
    std::uint64_t total = 0;
    for (std::uint32_t index = 0; index < entries; ++index) {
        const auto count = Read32(payload, 8 + static_cast<std::size_t>(index) * 8);
        const auto value = Read32(payload, 12 + static_cast<std::size_t>(index) * 8);
        total += count;
        if (!count || total > (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::runtime_error("The MP4 sample timing count exceeds the track sample limit.");
        }
        runs.push_back({count, composition && payload[0] == 1
            ? static_cast<std::int64_t>(static_cast<std::int32_t>(value)) : value});
    }
    return runs;
}

std::optional<std::int64_t> FirstCompositionTime(std::span<const BYTE> decode,
    std::span<const BYTE> composition)
{
    if (decode.empty()) {
        return {};
    }
    const auto decodeRuns = ParseTiming(decode, false);
    if (decodeRuns.empty()) {
        return {};
    }
    std::uint64_t totalDecode = 0;
    for (const auto& run : decodeRuns) {
        totalDecode += run.count;
    }
    auto compositionRuns = composition.empty() ? std::vector<TimingRun>{{static_cast<std::uint32_t>(totalDecode), 0}}
        : ParseTiming(composition, true);
    std::uint64_t totalComposition = 0;
    for (const auto& run : compositionRuns) {
        totalComposition += run.count;
    }
    if (totalComposition != totalDecode) {
        throw std::runtime_error("The MP4 decode and composition timing sample counts disagree.");
    }
    std::size_t decodeIndex = 0;
    std::size_t compositionIndex = 0;
    std::uint64_t decodeRemaining = decodeRuns[0].count;
    std::uint64_t compositionRemaining = compositionRuns[0].count;
    std::uint64_t decodeTime = 0;
    std::optional<std::int64_t> first;
    while (decodeIndex < decodeRuns.size() && compositionIndex < compositionRuns.size()) {
        const auto offset = compositionRuns[compositionIndex].value;
        if (decodeTime > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
                || (offset > 0 && decodeTime > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)() - offset))) {
            throw std::runtime_error("The MP4 composition timestamp exceeds the supported sample time range.");
        }
        const auto candidate = static_cast<std::int64_t>(decodeTime) + offset;
        first = first ? (std::min)(*first, candidate) : candidate;
        const auto count = (std::min)(decodeRemaining, compositionRemaining);
        const auto delta = static_cast<std::uint64_t>(decodeRuns[decodeIndex].value);
        if (delta && count > ((std::numeric_limits<std::uint64_t>::max)() - decodeTime) / delta) {
            throw std::runtime_error("The MP4 decode time exceeds the supported sample time range.");
        }
        decodeTime += count * delta;
        decodeRemaining -= count;
        compositionRemaining -= count;
        if (!decodeRemaining && ++decodeIndex < decodeRuns.size()) {
            decodeRemaining = decodeRuns[decodeIndex].count;
        }
        if (!compositionRemaining && ++compositionIndex < compositionRuns.size()) {
            compositionRemaining = compositionRuns[compositionIndex].count;
        }
    }
    return first;
}

PresentationTimeline ParseMovie(std::span<const BYTE> payload)
{
    const auto movie = Boxes(payload);
    std::uint32_t movieScale = 0;
    for (const auto& atom : movie) {
        if (atom.tag == Tag('m', 'v', 'h', 'd')) {
            movieScale = TimeScale(atom.payload);
        }
        if (atom.tag == Tag('c', 'm', 'o', 'v')) {
            throw std::runtime_error("Compressed MP4 timeline metadata is not supported.");
        }
    }
    PresentationTimeline result;
    bool audioFound = false;
    bool videoFound = false;
    for (const auto& atom : movie) {
        if (atom.tag != Tag('t', 'r', 'a', 'k')) {
            continue;
        }
        std::uint32_t handler = 0;
        std::uint32_t mediaScale = 0;
        std::span<const BYTE> edit;
        std::span<const BYTE> decode;
        std::span<const BYTE> composition;
        for (const auto& child : Boxes(atom.payload)) {
            if (child.tag == Tag('m', 'd', 'i', 'a')) {
                for (const auto& media : Boxes(child.payload)) {
                    if (media.tag == Tag('h', 'd', 'l', 'r')) {
                        handler = Read32(media.payload, 8);
                    } else if (media.tag == Tag('m', 'd', 'h', 'd')) {
                        mediaScale = TimeScale(media.payload);
                    } else if (media.tag == Tag('m', 'i', 'n', 'f')) {
                        for (const auto& mediaInformation : Boxes(media.payload)) {
                            if (mediaInformation.tag != Tag('s', 't', 'b', 'l')) {
                                continue;
                            }
                            for (const auto& timing : Boxes(mediaInformation.payload)) {
                                if (timing.tag == Tag('s', 't', 't', 's')) {
                                    decode = timing.payload;
                                } else if (timing.tag == Tag('c', 't', 't', 's')) {
                                    composition = timing.payload;
                                }
                            }
                        }
                    }
                }
            } else if (child.tag == Tag('e', 'd', 't', 's')) {
                for (const auto& edits : Boxes(child.payload)) {
                    if (edits.tag == Tag('e', 'l', 's', 't')) {
                        edit = edits.payload;
                    }
                }
            }
        }
        const bool audio = handler == Tag('s', 'o', 'u', 'n');
        const bool video = handler == Tag('v', 'i', 'd', 'e');
        if ((!audio && !video) || (audio && audioFound) || (video && videoFound)) {
            continue;
        }
        TrackPresentationTimeline timeline;
        if (!edit.empty()) {
            if (!movieScale || !mediaScale) {
                throw std::runtime_error("The MP4 edit list is missing its time scale.");
            }
            timeline = ParseEdits(edit, movieScale, mediaScale);
        }
        const auto firstComposition = FirstCompositionTime(decode, composition);
        if (firstComposition) {
            if (!mediaScale) {
                throw std::runtime_error("The MP4 sample timing is missing its media time scale.");
            }
            const auto firstTicks = TimelineTicks(static_cast<long double>(*firstComposition) * 10000000.0L / mediaScale);
            const auto firstMovie = TimelineTicks(static_cast<long double>(firstTicks) + timeline.offset100ns);
            if (video && timeline.hasEdit) {
                const auto version = edit[0];
                const auto count = Read32(edit, 4);
                const auto position = 8 + (static_cast<std::size_t>(count) - 1) * (version == 1 ? 20 : 12);
                const auto mediaStart = version == 1 ? static_cast<std::int64_t>(Read64(edit, position + 8))
                    : static_cast<std::int32_t>(Read32(edit, position + 4));
                // The OS source can expose preroll with a different PTS origin, so a guessed trim would lose sync.
                if (mediaStart > *firstComposition) {
                    throw std::runtime_error("This MP4 trims initial video samples with an edit list; its A/V timing cannot be preserved.");
                }
            }
            timeline.firstPresentation100ns = firstMovie;
        } else if (video && timeline.hasEdit) {
            throw std::runtime_error("The edited MP4 video has no static sample timing table; its A/V timing cannot be preserved.");
        }
        if (audio) {
            result.audio = timeline;
            audioFound = true;
        } else {
            result.video = timeline;
            videoFound = true;
        }
    }
    return result;
}

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex << std::setw(8)
                << std::setfill('0') << static_cast<unsigned long>(result) << ").";
        throw std::runtime_error(message.str());
    }
}

class MediaRuntime {
public:
    MediaRuntime()
    {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (apartment != RPC_E_CHANGED_MODE) {
            Check(apartment, "CoInitializeEx");
            ownsApartment_ = true;
        }
        const HRESULT startup = MFStartup(MF_VERSION);
        if (FAILED(startup)) {
            if (ownsApartment_) {
                CoUninitialize();
            }
            Check(startup, "MFStartup");
        }
    }

    ~MediaRuntime()
    {
        MFShutdown();
        if (ownsApartment_) {
            CoUninitialize();
        }
    }

    MediaRuntime(const MediaRuntime&) = delete;
    MediaRuntime& operator=(const MediaRuntime&) = delete;

private:
    bool ownsApartment_ = false;
};

class LockedBuffer {
public:
    explicit LockedBuffer(IMFMediaBuffer* buffer) : buffer_(buffer)
    {
        Check(buffer_->Lock(&data_, nullptr, &size_), "IMFMediaBuffer::Lock (audio)");
    }

    ~LockedBuffer() { buffer_->Unlock(); }
    const BYTE* Data() const { return data_; }
    DWORD Size() const { return size_; }
    LockedBuffer(const LockedBuffer&) = delete;
    LockedBuffer& operator=(const LockedBuffer&) = delete;

private:
    IMFMediaBuffer* buffer_;
    BYTE* data_ = nullptr;
    DWORD size_ = 0;
};

void Append16(std::vector<BYTE>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<BYTE>(value));
    bytes.push_back(static_cast<BYTE>(value >> 8));
}

void Append32(std::vector<BYTE>& bytes, std::uint32_t value)
{
    Append16(bytes, static_cast<std::uint16_t>(value));
    Append16(bytes, static_cast<std::uint16_t>(value >> 16));
}

std::vector<BYTE> WaveHeader(const AudioExportResult& info, std::uint32_t dataBytes)
{
    const bool extensible = info.channels > 2 || (info.channelMask != 0
        && info.channelMask != (info.channels == 1 ? 4U : 3U));
    const std::uint32_t formatSize = extensible ? 40U : 16U;
    const auto block = static_cast<std::uint16_t>(info.channels * (info.bitsPerSample / 8));
    std::vector<BYTE> result;
    result.insert(result.end(), {'R', 'I', 'F', 'F'});
    Append32(result, dataBytes + (dataBytes & 1U) + 20 + formatSize);
    result.insert(result.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Append32(result, formatSize);
    Append16(result, extensible ? 0xFFFE : 1);
    Append16(result, static_cast<std::uint16_t>(info.channels));
    Append32(result, info.sampleRate);
    Append32(result, info.sampleRate * block);
    Append16(result, block);
    Append16(result, static_cast<std::uint16_t>(info.bitsPerSample));
    if (extensible) {
        Append16(result, 22);
        Append16(result, static_cast<std::uint16_t>(info.bitsPerSample));
        Append32(result, info.channelMask);
        Append32(result, 1);
        Append16(result, 0);
        Append16(result, 0x0010);
        result.insert(result.end(), {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
    }
    result.insert(result.end(), {'d', 'a', 't', 'a'});
    Append32(result, dataBytes);
    return result;
}

class PendingWave {
public:
    PendingWave(const std::filesystem::path& destination, const AudioExportResult& info)
        : destination_(destination)
    {
        if (std::filesystem::exists(destination)) {
            throw std::runtime_error("Audio output already exists; it will not be overwritten.");
        }
        GUID identifier{};
        Check(CoCreateGuid(&identifier), "CoCreateGuid");
        wchar_t suffix[40]{};
        if (StringFromGUID2(identifier, suffix, 40) == 0) {
            throw std::runtime_error("Cannot allocate a unique audio output name.");
        }
        temporary_ = destination;
        temporary_ += std::wstring(L".partial.") + suffix;
        handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "CreateFileW (audio output)");
        }
        try {
            const auto header = WaveHeader(info, 0);
            Write(header.data(), header.size());
        } catch (...) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            DeleteFileW(temporary_.c_str());
            throw;
        }
    }

    ~PendingWave()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (!committed_) {
            DeleteFileW(temporary_.c_str());
        }
    }

    void Write(const BYTE* data, std::size_t size)
    {
        while (size) {
            const DWORD requested = static_cast<DWORD>((std::min)(size, std::size_t{1024 * 1024}));
            DWORD written = 0;
            if (!WriteFile(handle_, data, requested, &written, nullptr)) {
                Check(HRESULT_FROM_WIN32(GetLastError()), "WriteFile (audio output)");
            }
            if (written != requested) {
                throw std::runtime_error("Audio output write was incomplete.");
            }
            data += written;
            size -= written;
        }
    }

    void Silence(std::uint64_t bytes)
    {
        const std::array<BYTE, 65536> zeros{};
        while (bytes) {
            const auto size = static_cast<std::size_t>((std::min)(bytes,
                static_cast<std::uint64_t>(zeros.size())));
            Write(zeros.data(), size);
            bytes -= size;
        }
    }

    void Commit(const AudioExportResult& info, std::uint32_t bytes)
    {
        if (bytes & 1U) {
            const BYTE padding = 0;
            Write(&padding, 1);
        }
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(handle_, beginning, nullptr, FILE_BEGIN)) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "SetFilePointerEx (audio header)");
        }
        const auto header = WaveHeader(info, bytes);
        Write(header.data(), header.size());
        if (!FlushFileBuffers(handle_)) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "FlushFileBuffers (audio output)");
        }
        if (!CloseHandle(handle_)) {
            handle_ = INVALID_HANDLE_VALUE;
            Check(HRESULT_FROM_WIN32(GetLastError()), "CloseHandle (audio output)");
        }
        handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH)) {
            Check(HRESULT_FROM_WIN32(GetLastError()), "MoveFileExW (audio output)");
        }
        committed_ = true;
    }

    PendingWave(const PendingWave&) = delete;
    PendingWave& operator=(const PendingWave&) = delete;

private:
    std::filesystem::path destination_;
    std::filesystem::path temporary_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool committed_ = false;
};

std::int64_t SamplePosition(std::int64_t time, std::int64_t origin, std::uint32_t rate)
{
    const long double frames = (static_cast<long double>(time) - origin) * rate / 10000000.0L;
    if (frames <= static_cast<long double>((std::numeric_limits<std::int64_t>::min)()) + 1
            || frames >= static_cast<long double>((std::numeric_limits<std::int64_t>::max)()) - 1) {
        throw std::runtime_error("Audio presentation timestamp exceeds the PCM time range.");
    }
    return std::llround(frames);
}

void VerifyType(IMFMediaType* type, const AudioExportResult& info)
{
    GUID subtype{};
    Check(type->GetGUID(MF_MT_SUBTYPE, &subtype), "GetGUID (audio subtype)");
    UINT32 channels = 0;
    UINT32 rate = 0;
    UINT32 bits = 0;
    UINT32 block = 0;
    UINT32 channelMask = info.channelMask;
    Check(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), "GetUINT32 (audio channels)");
    Check(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), "GetUINT32 (audio sample rate)");
    Check(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits), "GetUINT32 (audio bit depth)");
    Check(type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &block), "GetUINT32 (audio alignment)");
    type->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &channelMask);
    if (subtype != MFAudioFormat_PCM || channels != info.channels || rate != info.sampleRate
            || bits != info.bitsPerSample || block != channels * (bits / 8) || channelMask != info.channelMask) {
        throw std::runtime_error("The audio decoder changed the PCM format during export.");
    }
}

}

PresentationTimeline ReadPresentationTimeline(const std::filesystem::path& source)
{
    const auto length = std::filesystem::file_size(source);
    if (length < 8) {
        return {};
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read media presentation metadata.");
    }
    std::uint64_t offset = 0;
    bool identified = false;
    while (offset + 8 <= length) {
        std::array<BYTE, 16> header{};
        input.seekg(static_cast<std::streamoff>(offset));
        input.read(reinterpret_cast<char*>(header.data()), 8);
        if (!input) {
            throw std::runtime_error("Cannot read the media container atom table.");
        }
        const auto tag = Read32(header, 4);
        if (!identified) {
            identified = tag == Tag('f', 't', 'y', 'p') || tag == Tag('m', 'o', 'o', 'v')
                || tag == Tag('m', 'd', 'a', 't') || tag == Tag('w', 'i', 'd', 'e')
                || tag == Tag('f', 'r', 'e', 'e') || tag == Tag('s', 'k', 'i', 'p');
            if (!identified) {
                return {};
            }
        }
        std::uint64_t size = Read32(header, 0);
        std::uint64_t headerSize = 8;
        if (size == 1) {
            input.read(reinterpret_cast<char*>(header.data() + 8), 8);
            if (!input) {
                throw std::runtime_error("The MP4 extended atom header is truncated.");
            }
            size = Read64(header, 8);
            headerSize = 16;
        } else if (size == 0) {
            size = length - offset;
        }
        if (size < headerSize || size > length - offset) {
            throw std::runtime_error("The MP4 container atom extends beyond the file.");
        }
        if (tag == Tag('m', 'o', 'o', 'v')) {
            const auto metadataSize = size - headerSize;
            if (metadataSize > 64ULL * 1024 * 1024) {
                throw std::runtime_error("The MP4 timeline metadata exceeds the 64 MiB safety limit.");
            }
            std::vector<BYTE> metadata(static_cast<std::size_t>(metadataSize));
            input.read(reinterpret_cast<char*>(metadata.data()), static_cast<std::streamsize>(metadata.size()));
            if (!input) {
                throw std::runtime_error("The MP4 movie metadata is truncated.");
            }
            return ParseMovie(metadata);
        }
        offset += size;
    }
    return {};
}

AudioExportResult ExportAudioWave(const std::filesystem::path& source,
    const std::filesystem::path& destination, const AudioExportOptions& options)
{
    if (!std::filesystem::is_regular_file(source)) {
        throw std::runtime_error("The audio source must be an existing local file.");
    }
    if (destination.empty() || destination.filename().empty()) {
        throw std::runtime_error("The audio output must have a file name.");
    }
    MediaRuntime runtime;
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader), "MFCreateSourceReaderFromURL (audio)");
    ComPtr<IMFMediaType> nativeType;
    const HRESULT nativeResult = reader->GetNativeMediaType(AudioStream, 0, &nativeType);
    if (nativeResult == MF_E_INVALIDSTREAMNUMBER || nativeResult == MF_E_NO_MORE_TYPES) {
        return {};
    }
    Check(nativeResult, "GetNativeMediaType (audio)");
    const auto timeline = ReadPresentationTimeline(source).audio;
    AudioExportResult result;
    Check(nativeType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &result.channels), "GetUINT32 (native audio channels)");
    Check(nativeType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &result.sampleRate), "GetUINT32 (native audio sample rate)");
    GUID nativeSubtype{};
    Check(nativeType->GetGUID(MF_MT_SUBTYPE, &nativeSubtype), "GetGUID (native audio subtype)");
    UINT32 nativeBits = 16;
    nativeType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &nativeBits);
    if (!nativeBits && nativeSubtype != MFAudioFormat_PCM && nativeSubtype != MFAudioFormat_Float) {
        nativeBits = 16;
    }
    UINT32 validBits = nativeBits;
    nativeType->GetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, &validBits);
    if (nativeSubtype == MFAudioFormat_PCM && nativeBits != 8 && nativeBits != 16
            && nativeBits != 24 && nativeBits != 32) {
        throw std::runtime_error("The native PCM sample size is not supported by the standard audio cache.");
    }
    if (!validBits || validBits > nativeBits) {
        throw std::runtime_error("The native audio precision exceeds its sample container size.");
    }
    if (nativeSubtype == MFAudioFormat_Float) {
        // The standard Story loader rejects IEEE float WAVE, so this cache is a declared playback conversion.
        result.bitsPerSample = 24;
        result.precisionReduced = true;
    } else if (nativeBits > 24) {
        result.bitsPerSample = result.channels == 1 ? 32 : 24;
        result.precisionReduced = validBits > result.bitsPerSample;
    } else {
        result.bitsPerSample = nativeBits > 16 ? 24 : 16;
    }
    const std::uint64_t bytesPerSample = result.bitsPerSample / 8;
    if (!result.channels || result.channels > (std::numeric_limits<std::uint16_t>::max)() / bytesPerSample
            || !result.sampleRate || static_cast<std::uint64_t>(result.sampleRate) * result.channels * bytesPerSample
                > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::runtime_error("The audio format exceeds the PCM WAVE format limits.");
    }
    nativeType->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &result.channelMask);
    Check(reader->SetStreamSelection(AllStreams, FALSE), "SetStreamSelection (audio only)");
    Check(reader->SetStreamSelection(AudioStream, TRUE), "SetStreamSelection (audio)");
    ComPtr<IMFMediaType> requested;
    Check(MFCreateMediaType(&requested), "MFCreateMediaType (PCM)");
    Check(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "SetGUID (audio major type)");
    Check(requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "SetGUID (PCM)");
    Check(requested->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, result.bitsPerSample), "SetUINT32 (PCM bit depth)");
    Check(requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, result.channels), "SetUINT32 (PCM channels)");
    Check(requested->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, result.sampleRate), "SetUINT32 (PCM sample rate)");
    Check(reader->SetCurrentMediaType(AudioStream, nullptr, requested.Get()), "SetCurrentMediaType (PCM)");
    ComPtr<IMFMediaType> outputType;
    Check(reader->GetCurrentMediaType(AudioStream, &outputType), "GetCurrentMediaType (PCM)");
    outputType->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &result.channelMask);
    VerifyType(outputType.Get(), result);
    const std::uint64_t block = static_cast<std::uint64_t>(result.channels) * bytesPerSample;
    const auto headerSize = WaveHeader(result, 0).size();
    const std::uint64_t maximumRiffData = 0xFFFFFFFFULL - (headerSize - 8) - 1;
    const std::uint64_t maximumFrames = (std::min)(maximumRiffData, options.maxDataBytes) / block;
    if (!maximumFrames) {
        throw std::runtime_error("The audio output size limit cannot hold one PCM frame.");
    }
    PendingWave output(destination, result);
    bool foundSample = false;
    unsigned emptyReads = 0;
    while (true) {
        if (options.continueExport && !options.continueExport(result.frameCount)) {
            throw std::runtime_error("Audio preparation was cancelled.");
        }
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        Check(reader->ReadSample(AudioStream, 0, nullptr, &flags, &timestamp, &sample), "ReadSample (audio)");
        if (flags & MF_SOURCE_READERF_ERROR) {
            throw std::runtime_error("The audio source reported a streaming error.");
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            outputType.Reset();
            Check(reader->GetCurrentMediaType(AudioStream, &outputType), "GetCurrentMediaType (audio changed)");
            VerifyType(outputType.Get(), result);
        }
        if (sample) {
            emptyReads = 0;
            if (!foundSample) {
                foundSample = true;
                result.firstPresentation100ns = TimelineTicks(static_cast<long double>(timestamp) + timeline.offset100ns);
            }
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(&buffer), "ConvertToContiguousBuffer (audio)");
            LockedBuffer data(buffer.Get());
            if (data.Size() % block) {
                throw std::runtime_error("The audio decoder returned an incomplete PCM frame.");
            }
            const auto movieTimestamp = TimelineTicks(static_cast<long double>(timestamp) + timeline.offset100ns);
            const auto position = SamplePosition(movieTimestamp, options.timelineOrigin100ns, result.sampleRate);
            std::uint64_t sampleFrames = data.Size() / block;
            if (timeline.end100ns) {
                const auto available = SamplePosition(*timeline.end100ns, movieTimestamp, result.sampleRate);
                sampleFrames = available <= 0 ? 0 : (std::min)(sampleFrames, static_cast<std::uint64_t>(available));
            }
            const std::uint64_t skip = position < 0
                ? (std::min)(sampleFrames, static_cast<std::uint64_t>(-(position + 1)) + 1)
                : static_cast<std::uint64_t>(position) < result.frameCount
                    ? (std::min)(sampleFrames, result.frameCount - static_cast<std::uint64_t>(position)) : 0;
            const std::uint64_t destinationFrame = position < 0 ? 0 : static_cast<std::uint64_t>(position);
            const std::uint64_t frames = sampleFrames - skip;
            const std::uint64_t gap = destinationFrame > result.frameCount ? destinationFrame - result.frameCount : 0;
            if (gap > maximumFrames - result.frameCount
                    || frames > maximumFrames - result.frameCount - gap) {
                throw std::runtime_error("Decoded audio exceeds the configured PCM WAVE size limit.");
            }
            if (frames) {
                output.Silence(gap * block);
                output.Write(data.Data() + static_cast<std::size_t>(skip * block),
                    static_cast<std::size_t>(frames * block));
                result.frameCount += gap + frames;
            }
        } else if (!(flags & MF_SOURCE_READERF_ENDOFSTREAM) && ++emptyReads > 100000) {
            throw std::runtime_error("The audio source stopped producing PCM samples.");
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            break;
        }
    }
    if (!foundSample || !result.frameCount) {
        throw std::runtime_error("The audio stream has no PCM samples in the requested timeline range.");
    }
    if (options.continueExport && !options.continueExport(result.frameCount)) {
        throw std::runtime_error("Audio preparation was cancelled.");
    }
    result.hasAudio = true;
    result.duration100ns = static_cast<std::int64_t>(std::llround(
        static_cast<long double>(result.frameCount) * 10000000.0L / result.sampleRate));
    output.Commit(result, static_cast<std::uint32_t>(result.frameCount * block));
    return result;
}

}
