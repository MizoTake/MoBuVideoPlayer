#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace mobu::media {

struct VideoInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t duration100ns = 0;
    // Preserve the first frame's raw native PTS because decoded clip time starts at zero.
    std::int64_t presentationOrigin100ns = 0;
    std::uint32_t frameRateNumerator = 0;
    std::uint32_t frameRateDenominator = 1;
    bool hardwareAccelerationRequested = false;
};

struct VideoFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Clip-relative PTS; add VideoInfo::presentationOrigin100ns to recover the native PTS.
    std::int64_t presentation100ns = 0;
    std::int64_t duration100ns = 0;
    std::vector<std::uint8_t> bgra;
};

struct FrameRange {
    std::int64_t first100ns = 0;
    std::int64_t last100ns = 0;
    std::int64_t end100ns = 0;
};

class IDecoder {
public:
    virtual ~IDecoder() = default;
    virtual void Open(const std::filesystem::path& path) = 0;
    virtual const VideoInfo& Info() const = 0;
    virtual VideoFrame DecodeAt(std::int64_t time100ns) = 0;
    virtual FrameRange ResolveFrameRange(std::int64_t startFrame, std::int64_t stopFrame) = 0;
    virtual std::vector<std::int64_t> FramePresentationTimes() = 0;
};

std::unique_ptr<IDecoder> CreateNativeDecoder();

}
