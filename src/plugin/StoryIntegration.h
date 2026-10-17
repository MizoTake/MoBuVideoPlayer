#pragma once

#include <fbsdk/fbsdk.h>

#include <cstdint>
#include <filesystem>

namespace mobu::plugin {

struct StoryImportResult {
    FBStoryTrack* videoTrack = nullptr;
    FBStoryClip* videoClip = nullptr;
    FBStoryTrack* audioTrack = nullptr;
    FBStoryClip* audioClip = nullptr;
    FBVideo* trackVideo = nullptr;
    HdlFBPlugTemplate<FBVideoClipImage> ownedVideo;
    HdlFBPlugTemplate<FBAudioClip> ownedAudio;
    HdlFBPlugTemplate<FBStoryTrack> ownedVideoTrack;
    HdlFBPlugTemplate<FBStoryTrack> ownedAudioTrack;
    HdlFBPlugTemplate<FBStoryClip> ownedVideoClip;
    HdlFBPlugTemplate<FBStoryClip> ownedAudioClip;
};

void CleanupStoryMedia(StoryImportResult& result) noexcept;

StoryImportResult CreateStoryMedia(
    const std::filesystem::path& firstImagePath,
    const std::filesystem::path& sourcePath,
    std::int64_t videoDuration100ns,
    std::int64_t audioDuration100ns,
    std::uint32_t frameRateNumerator,
    std::uint32_t frameRateDenominator,
    const std::filesystem::path& wavPath,
    FBTime start,
    std::int64_t videoStartOffset100ns = 0);

}
