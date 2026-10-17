#include "mobu/media/Decoder.h"

#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mobu::media {
namespace {

using Microsoft::WRL::ComPtr;
constexpr DWORD VideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD AllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD MediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (HRESULT 0x" << std::hex
                << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(result) << ").";
        throw std::runtime_error(message.str());
    }
}

std::int64_t AddTime(std::int64_t time, std::int64_t offset)
{
    if ((offset > 0 && time > (std::numeric_limits<std::int64_t>::max)() - offset)
            || (offset < 0 && time < (std::numeric_limits<std::int64_t>::min)() - offset)) {
        throw std::runtime_error("Video seek time overflows the native presentation time base.");
    }
    return time + offset;
}

std::int64_t RelativeTime(std::int64_t time, std::int64_t origin)
{
    if ((origin > 0 && time < (std::numeric_limits<std::int64_t>::min)() + origin)
            || (origin < 0 && time > (std::numeric_limits<std::int64_t>::max)() + origin)) {
        throw std::runtime_error("Video timestamp overflows the clip presentation time base.");
    }
    return time - origin;
}

class MediaRuntime {
public:
    MediaRuntime()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // MotionBuilder may already own a different COM apartment on this thread.
        if (result != RPC_E_CHANGED_MODE) {
            Check(result, "CoInitializeEx");
            ownsApartment_ = true;
        }
        const HRESULT startup = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(startup)) {
            if (ownsApartment_) {
                CoUninitialize();
            }
            Check(startup, "MFStartup (Windows media components must be installed)");
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

class BufferLock {
public:
    BufferLock(IMFMediaBuffer* buffer, LONG defaultStride, UINT32 height)
        : buffer_(buffer)
    {
        ComPtr<IMF2DBuffer2> buffer2;
        if (SUCCEEDED(buffer_->QueryInterface(IID_PPV_ARGS(&buffer2)))) {
            if (SUCCEEDED(buffer2->Lock2DSize(MF2DBuffer_LockFlags_Read,
                    &scanline_, &stride_, &start_, &length_))) {
                buffer2d_ = buffer2;
                hasBounds_ = true;
                return;
            }
        }
        ComPtr<IMF2DBuffer> buffer2d;
        if (SUCCEEDED(buffer_->QueryInterface(IID_PPV_ARGS(&buffer2d)))) {
            if (SUCCEEDED(buffer2d->Lock2D(&scanline_, &stride_))) {
                buffer2d_ = buffer2d;
                return;
            }
        }

        Check(buffer_->Lock(&start_, nullptr, &length_), "IMFMediaBuffer::Lock");
        lockedLinear_ = true;
        hasBounds_ = true;
        stride_ = defaultStride;
        scanline_ = start_;
        if (stride_ < 0 && height > 0) {
            const auto offset = static_cast<std::uint64_t>(-static_cast<std::int64_t>(stride_)) * (height - 1);
            if (offset >= length_) {
                buffer_->Unlock();
                lockedLinear_ = false;
                throw std::runtime_error("Decoded video buffer is shorter than its negative stride.");
            }
            scanline_ += static_cast<std::size_t>(offset);
        }
    }

    ~BufferLock()
    {
        if (buffer2d_) {
            buffer2d_->Unlock2D();
        } else if (lockedLinear_) {
            buffer_->Unlock();
        }
    }

    void CopyRows(VideoFrame& frame) const
    {
        const auto rowBytes = static_cast<std::size_t>(frame.width) * 4;
        const auto pitchMagnitude = stride_ < 0 ? -static_cast<std::int64_t>(stride_) : stride_;
        if (pitchMagnitude < static_cast<std::int64_t>(rowBytes)) {
            throw std::runtime_error("Decoded video stride is smaller than one BGRA row.");
        }
        if (!scanline_) {
            throw std::runtime_error("Decoded video has no pixel buffer.");
        }
        const auto startAddress = reinterpret_cast<std::uintptr_t>(start_);
        const auto scanlineAddress = reinterpret_cast<std::uintptr_t>(scanline_);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            const auto displacement = static_cast<std::int64_t>(stride_) * row;
            const auto address = displacement < 0
                ? scanlineAddress - static_cast<std::uintptr_t>(-displacement)
                : scanlineAddress + static_cast<std::uintptr_t>(displacement);
            if (hasBounds_ && (address < startAddress || address - startAddress > length_
                    || rowBytes > length_ - (address - startAddress))) {
                throw std::runtime_error("Decoded video row lies outside its pixel buffer.");
            }
            auto* destination = frame.bgra.data() + static_cast<std::size_t>(row) * rowBytes;
            std::memcpy(destination, reinterpret_cast<const void*>(address), rowBytes);
            // RGB32's fourth byte is unspecified, so exposing it as alpha could make video transparent.
            for (std::size_t offset = 3; offset < rowBytes; offset += 4) {
                destination[offset] = 255;
            }
        }
    }

    BufferLock(const BufferLock&) = delete;
    BufferLock& operator=(const BufferLock&) = delete;

private:
    ComPtr<IMFMediaBuffer> buffer_;
    ComPtr<IMF2DBuffer> buffer2d_;
    BYTE* scanline_ = nullptr;
    BYTE* start_ = nullptr;
    LONG stride_ = 0;
    DWORD length_ = 0;
    bool lockedLinear_ = false;
    bool hasBounds_ = false;
};

class WindowsDecoder final : public IDecoder {
public:
    WindowsDecoder() = default;
    ~WindowsDecoder() override = default;

private:
    void VerifyThread() const
    {
        if (threadId_ != GetCurrentThreadId()) {
            throw std::runtime_error("Native video decoder must be used and destroyed on the thread that created it.");
        }
    }

    void Open(const std::filesystem::path& path) override
    {
        VerifyThread();
        reader_.Reset();
        deviceManager_.Reset();
        device_.Reset();
        current_.reset();
        next_.reset();
        frameIndex_.clear();
        endOfStream_ = false;
        ready_ = false;
        info_ = {};
        if (path.empty() || !std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Video path must name an existing local file.");
        }
        path_ = path;

        try {
            CreateHardwareDevice();
            ConfigureReader(path, true);
            PrimeTimeline();
        } catch (const std::exception&) {
            // A codec or display adapter may reject DXVA while its OS software decoder still works.
            reader_.Reset();
            deviceManager_.Reset();
            device_.Reset();
            info_ = {};
            ConfigureReader(path, false);
            PrimeTimeline();
        }
        ready_ = true;
    }

    const VideoInfo& Info() const override
    {
        VerifyThread();
        return info_;
    }

    VideoFrame DecodeAt(std::int64_t time100ns) override
    {
        VerifyThread();
        if (!reader_ || !ready_) {
            throw std::runtime_error("Open a video before decoding it.");
        }
        try {
            return DecodeAtNative(time100ns);
        } catch (const std::exception&) {
            if (!info_.hardwareAccelerationRequested) {
                throw;
            }
            // Drivers can defer unsupported-format failures until the first actual decode request.
            ready_ = false;
            reader_.Reset();
            deviceManager_.Reset();
            device_.Reset();
            current_.reset();
            next_.reset();
            frameIndex_.clear();
            ConfigureReader(path_, false);
            PrimeTimeline();
            ready_ = true;
            return DecodeAtNative(time100ns);
        }
    }

    VideoFrame DecodeAtNative(std::int64_t time100ns)
    {
        std::int64_t target = (std::max)(std::int64_t{0}, time100ns);
        if (info_.duration100ns > 0) {
            target = (std::min)(target, info_.duration100ns - 1);
        }

        constexpr std::int64_t sequentialWindow = 2 * 10'000'000;
        if (!current_ || target < current_->presentation100ns
                || target - current_->presentation100ns > sequentialWindow) {
            Seek(target);
            current_ = ReadNext();
            // Source Reader seeks to a nearby key frame; a demuxer may land later than requested.
            if (target > 0 && (!current_ || current_->presentation100ns > target)) {
                Seek(0);
                current_ = ReadNext();
            }
        }
        if (!current_) {
            throw std::runtime_error("The OS decoder returned no video frames.");
        }

        for (;;) {
            if (!next_ && !endOfStream_) {
                next_ = ReadNext();
            }
            if (!next_) {
                return *current_;
            }
            if (next_->presentation100ns <= current_->presentation100ns) {
                throw std::runtime_error("The OS decoder returned non-increasing video presentation timestamps.");
            }
            if (next_->presentation100ns > target) {
                return *current_;
            }
            current_ = std::move(next_);
            next_.reset();
        }
    }

    FrameRange ResolveFrameRange(std::int64_t startFrame, std::int64_t stopFrame) override
    {
        VerifyThread();
        if (!reader_ || !ready_) {
            throw std::runtime_error("Open a video before resolving its frame range.");
        }
        if (startFrame < 0 || (stopFrame >= 0 && stopFrame < startFrame)) {
            throw std::invalid_argument("Video trim frame range is invalid.");
        }
        if (frameIndex_.empty()) {
            BuildFrameIndex();
        }
        if (static_cast<std::uint64_t>(startFrame) >= frameIndex_.size()) {
            throw std::out_of_range("Video trim begins after the last source frame.");
        }
        const auto first = static_cast<std::size_t>(startFrame);
        const auto last = stopFrame < 0 ? frameIndex_.size() - 1
            : static_cast<std::size_t>((std::min)(static_cast<std::uint64_t>(stopFrame),
                static_cast<std::uint64_t>(frameIndex_.size() - 1)));
        FrameRange range;
        range.first100ns = frameIndex_[first].presentation100ns;
        range.last100ns = frameIndex_[last].presentation100ns;
        range.end100ns = last + 1 < frameIndex_.size() ? frameIndex_[last + 1].presentation100ns
            : (std::max)(info_.duration100ns, AddTime(range.last100ns, frameIndex_[last].duration100ns));
        if (range.end100ns <= range.last100ns) {
            throw std::runtime_error("The final video frame has no positive presentation interval.");
        }
        return range;
    }

    std::vector<std::int64_t> FramePresentationTimes() override
    {
        VerifyThread();
        if (!reader_ || !ready_) {
            throw std::runtime_error("Open a video before reading its presentation times.");
        }
        if (frameIndex_.empty()) {
            BuildFrameIndex();
        }
        std::vector<std::int64_t> times;
        times.reserve(frameIndex_.size());
        for (const auto& frame : frameIndex_) {
            times.push_back(frame.presentation100ns);
        }
        return times;
    }

    void BuildFrameIndex()
    {
        ComPtr<IMFSourceReader> indexReader;
        Check(MFCreateSourceReaderFromURL(path_.c_str(), nullptr, &indexReader), "Open video timestamp index");
        Check(indexReader->SetStreamSelection(AllStreams, FALSE), "Disable unused timestamp-index streams");
        Check(indexReader->SetStreamSelection(VideoStream, TRUE), "Select timestamp-index video stream");
        ComPtr<IMFMediaType> nativeType;
        Check(indexReader->GetNativeMediaType(VideoStream, 0, &nativeType), "Read timestamp-index native format");
        Check(indexReader->SetCurrentMediaType(VideoStream, nullptr, nativeType.Get()), "Preserve encoded sample timestamps");

        std::vector<IndexedFrame> index;
        unsigned emptySamples = 0;
        for (;;) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            Check(indexReader->ReadSample(VideoStream, 0, nullptr, &flags, &timestamp, &sample), "Read video timestamp index");
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("Media Foundation reported a video timestamp-index error.");
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                ComPtr<IMFMediaType> changedType;
                Check(indexReader->GetCurrentMediaType(VideoStream, &changedType), "Read changed timestamp-index format");
                ValidatePresentation(changedType.Get());
                UINT32 width = 0;
                UINT32 height = 0;
                if (SUCCEEDED(MFGetAttributeSize(changedType.Get(), MF_MT_FRAME_SIZE, &width, &height))
                        && (width != info_.width || height != info_.height)) {
                    throw std::runtime_error("Video changes dimensions during its timestamp index.");
                }
            }
            if (sample) {
                emptySamples = 0;
                const auto relative = RelativeTime(timestamp, info_.presentationOrigin100ns);
                if (relative >= 0) {
                    LONGLONG duration = 0;
                    if (FAILED(sample->GetSampleDuration(&duration))) {
                        duration = 0;
                    }
                    index.push_back({ relative, (std::max)(LONGLONG{0}, duration) });
                }
            } else if (++emptySamples > 4096) {
                throw std::runtime_error("Media Foundation returned too many empty timestamp-index samples.");
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                break;
            }
        }
        if (index.empty()) {
            throw std::runtime_error("The video contains no indexable source frames.");
        }
        // Encoded B frames arrive in decode order, which cannot define frame-number trim boundaries.
        std::sort(index.begin(), index.end(), [](const IndexedFrame& left, const IndexedFrame& right) {
            return left.presentation100ns < right.presentation100ns;
        });
        for (std::size_t position = 1; position < index.size(); ++position) {
            if (index[position - 1].presentation100ns == index[position].presentation100ns) {
                throw std::runtime_error("Video has duplicate source presentation timestamps; frame-number trimming is ambiguous.");
            }
        }
        frameIndex_ = std::move(index);
    }

    void CreateHardwareDevice()
    {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        constexpr UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, nullptr);
        if (result == E_INVALIDARG) {
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels + 1, ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, &device_, nullptr, nullptr);
        }
        Check(result, "D3D11CreateDevice");
        ComPtr<ID3D10Multithread> multithread;
        Check(device_.As(&multithread), "ID3D10Multithread");
        multithread->SetMultithreadProtected(TRUE);
        UINT resetToken = 0;
        Check(MFCreateDXGIDeviceManager(&resetToken, &deviceManager_), "MFCreateDXGIDeviceManager");
        Check(deviceManager_->ResetDevice(device_.Get(), resetToken), "IMFDXGIDeviceManager::ResetDevice");
    }

    void ConfigureReader(const std::filesystem::path& path, bool useHardware)
    {
        ComPtr<IMFAttributes> attributes;
        Check(MFCreateAttributes(&attributes, 4), "MFCreateAttributes");
        Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE), "Enable native video processing");
        Check(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, useHardware ? TRUE : FALSE), "Configure hardware transforms");
        if (useHardware) {
            Check(attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, deviceManager_.Get()), "Configure DXVA device");
        } else {
            Check(attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE), "Configure software decoding");
        }
        Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader_), "Open video with Media Foundation");
        Check(reader_->SetStreamSelection(AllStreams, FALSE), "Disable unused media streams");
        Check(reader_->SetStreamSelection(VideoStream, TRUE), "Select video stream");

        ComPtr<IMFMediaType> nativeType;
        Check(reader_->GetNativeMediaType(VideoStream, 0, &nativeType), "Read native video format");
        ValidatePresentation(nativeType.Get());
        UINT32 numerator = 0;
        UINT32 denominator = 1;
        if (SUCCEEDED(MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)) && denominator != 0) {
            info_.frameRateNumerator = numerator;
            info_.frameRateDenominator = denominator;
        }

        ComPtr<IMFMediaType> outputType;
        Check(MFCreateMediaType(&outputType), "MFCreateMediaType");
        Check(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set video output type");
        Check(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set BGRA-compatible output");
        const GUID presentationAttributes[] = {
            MF_MT_FRAME_SIZE, MF_MT_FRAME_RATE, MF_MT_PIXEL_ASPECT_RATIO, MF_MT_INTERLACE_MODE
        };
        for (const auto& attribute : presentationAttributes) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(nativeType->GetItem(attribute, &value))) {
                const HRESULT copied = outputType->SetItem(attribute, value);
                PropVariantClear(&value);
                Check(copied, "Preserve native video presentation format");
            } else {
                PropVariantClear(&value);
            }
        }
        Check(reader_->SetCurrentMediaType(VideoStream, nullptr, outputType.Get()), "Configure native RGB decoder");
        DisableFrameRateConversion();
        ReadOutputFormat(false);

        PROPVARIANT duration;
        PropVariantInit(&duration);
        const HRESULT result = reader_->GetPresentationAttribute(MediaSource, MF_PD_DURATION, &duration);
        if (SUCCEEDED(result)) {
            LONGLONG value = 0;
            const HRESULT converted = PropVariantToInt64(duration, &value);
            PropVariantClear(&duration);
            Check(converted, "Read video duration");
            info_.duration100ns = (std::max)(LONGLONG{0}, value);
        } else {
            PropVariantClear(&duration);
            Check(result, "Read video duration");
        }
        info_.hardwareAccelerationRequested = useHardware;
    }

    void DisableFrameRateConversion()
    {
        ComPtr<IMFSourceReaderEx> reader;
        Check(reader_.As(&reader), "Configure frame-preserving native reader");
        for (DWORD index = 0;; ++index) {
            GUID category = GUID_NULL;
            ComPtr<IMFTransform> transform;
            const HRESULT result = reader->GetTransformForStream(VideoStream, index, &category, &transform);
            if (result == MF_E_INVALIDINDEX) {
                return;
            }
            Check(result, "Inspect native video processing chain");
            if (category == MFT_CATEGORY_VIDEO_PROCESSOR) {
                ComPtr<IMFAttributes> attributes;
                Check(transform->GetAttributes(&attributes), "Read video processor attributes");
                // XVP otherwise resamples VFR timestamps and may duplicate or drop source frames.
                Check(attributes->SetUINT32(MF_XVP_DISABLE_FRC, TRUE), "Disable native frame-rate conversion");
            }
        }
    }

    void ValidatePresentation(IMFMediaType* mediaType) const
    {
        UINT32 rotation = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)) && rotation != 0) {
            throw std::runtime_error("Video display rotation is not supported; use a video with physically oriented pixels.");
        }
        UINT32 numerator = 1;
        UINT32 denominator = 1;
        if (SUCCEEDED(MFGetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, &numerator, &denominator))
                && numerator != denominator) {
            throw std::runtime_error("Non-square video pixels are not supported.");
        }
        UINT32 transfer = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer))
                && (transfer == MFVideoTransFunc_2084 || transfer == MFVideoTransFunc_HLG)) {
            throw std::runtime_error("HDR video requires a color-managed output and is not supported by this BGRA8 texture decoder.");
        }
    }

    void PrimeTimeline()
    {
        info_.presentationOrigin100ns = 0;
        Seek(0);
        current_ = ReadNext();
        if (!current_) {
            throw std::runtime_error("The OS decoder returned no video frames.");
        }
        // MP4 sources can shift B-frame decode timestamps to zero; the clip must still start at its first frame.
        info_.presentationOrigin100ns = current_->presentation100ns;
        current_->presentation100ns = 0;
    }

    void ReadOutputFormat(bool rejectResize)
    {
        ComPtr<IMFMediaType> output;
        Check(reader_->GetCurrentMediaType(VideoStream, &output), "Read decoded video format");
        UINT32 width = 0;
        UINT32 height = 0;
        Check(MFGetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, &width, &height), "Read video dimensions");
        if (width == 0 || height == 0 || width > static_cast<UINT32>((std::numeric_limits<LONG>::max)() / 4)
                || static_cast<std::uint64_t>(width) * height > (std::numeric_limits<std::size_t>::max)() / 4) {
            throw std::runtime_error("Video dimensions cannot be represented as a BGRA8 image.");
        }
        if (rejectResize && (width != info_.width || height != info_.height)) {
            throw std::runtime_error("Video changes dimensions during playback; dynamic texture dimensions are unsupported.");
        }
        info_.width = width;
        info_.height = height;
        UINT32 stride = 0;
        if (SUCCEEDED(output->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            stride_ = static_cast<LONG>(stride);
        } else {
            Check(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride_), "Read default video stride");
        }
    }

    void Seek(std::int64_t target)
    {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = AddTime(target, info_.presentationOrigin100ns);
        Check(reader_->SetCurrentPosition(GUID_NULL, position), "Seek native video source");
        current_.reset();
        next_.reset();
        endOfStream_ = false;
    }

    std::optional<VideoFrame> ReadNext()
    {
        for (unsigned skipped = 0; skipped < 4096; ++skipped) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            Check(reader_->ReadSample(VideoStream, 0, nullptr,
                &flags, &timestamp, &sample), "Decode native video sample");
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                throw std::runtime_error("Media Foundation reported a video decode error.");
            }
            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                ReadOutputFormat(true);
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                endOfStream_ = true;
            }
            if (!sample) {
                if (endOfStream_) {
                    return std::nullopt;
                }
                continue;
            }

            VideoFrame frame;
            frame.width = info_.width;
            frame.height = info_.height;
            frame.presentation100ns = RelativeTime(timestamp, info_.presentationOrigin100ns);
            LONGLONG duration = 0;
            if (SUCCEEDED(sample->GetSampleDuration(&duration))) {
                frame.duration100ns = (std::max)(LONGLONG{0}, duration);
            }
            frame.bgra.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
            ComPtr<IMFMediaBuffer> buffer;
            DWORD bufferCount = 0;
            Check(sample->GetBufferCount(&bufferCount), "Read decoded buffer count");
            if (bufferCount == 1) {
                Check(sample->GetBufferByIndex(0, &buffer), "Read decoded buffer");
            } else {
                Check(sample->ConvertToContiguousBuffer(&buffer), "Join decoded buffers");
            }
            BufferLock lock(buffer.Get(), stride_, frame.height);
            lock.CopyRows(frame);
            return frame;
        }
        throw std::runtime_error("Media Foundation returned too many empty samples without video frames.");
    }

    struct IndexedFrame {
        std::int64_t presentation100ns = 0;
        std::int64_t duration100ns = 0;
    };

    const DWORD threadId_ = GetCurrentThreadId();
    MediaRuntime runtime_;
    ComPtr<ID3D11Device> device_;
    ComPtr<IMFDXGIDeviceManager> deviceManager_;
    ComPtr<IMFSourceReader> reader_;
    std::filesystem::path path_;
    VideoInfo info_;
    LONG stride_ = 0;
    std::optional<VideoFrame> current_;
    std::optional<VideoFrame> next_;
    std::vector<IndexedFrame> frameIndex_;
    bool endOfStream_ = false;
    bool ready_ = false;
};

}

std::unique_ptr<IDecoder> CreateNativeDecoder()
{
    return std::make_unique<WindowsDecoder>();
}

}
