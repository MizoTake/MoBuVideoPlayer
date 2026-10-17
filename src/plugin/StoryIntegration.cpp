#include "StoryIntegration.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace mobu::plugin {
namespace {

std::string PathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::filesystem::path Utf8Path(const char* value) {
    const std::string text = value ? value : "";
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string PortablePath(const std::filesystem::path& path) {
    const char* sceneName = FBApplication().FBXFileName;
    if (sceneName && *sceneName) {
        std::error_code error;
        const auto relative = std::filesystem::relative(path, Utf8Path(sceneName).parent_path(), error);
        if (!error && !relative.empty()) {
            return PathUtf8(relative);
        }
    }
    return PathUtf8(path);
}

FBTime MediaTime(std::int64_t ticks100ns) {
    FBTime result;
    result.SetSecondDouble(static_cast<double>(ticks100ns) / 10'000'000.0);
    return result;
}

void StringProperty(FBComponent* object, const char* name, const std::string& value) {
    auto* property = object->PropertyCreate(name, kFBPT_charptr, nullptr, false, true);
    if (!property) {
        throw std::runtime_error(std::string("Cannot create Story media property: ") + name);
    }
    const char* text = value.c_str();
    property->SetData(&text);
}

void Metadata(FBStoryClip* clip, const std::filesystem::path& source,
    const std::filesystem::path& cache, const char* cacheKind) {
    StringProperty(clip, "NativeVideo.OriginalSource", PortablePath(source));
    StringProperty(clip, "NativeVideo.PreparedMedia", PortablePath(cache));
    StringProperty(clip, "NativeVideo.PreparedMediaKind", cacheKind);
}

template <typename T>
void Delete(HdlFBPlugTemplate<T>& handle) noexcept {
    try {
        if (handle.Ok()) {
            handle->FBDelete();
        }
    } catch (...) {
        // A cleanup exception must not prevent releasing the remaining import objects.
    }
}

void CheckTrackClip(FBStoryTrack* track, FBStoryClip* clip) {
    for (int index = 0; index < track->Clips.GetCount(); ++index) {
        auto* candidate = track->Clips[index];
        if (candidate && candidate->GetHIObject() == clip->GetHIObject()) {
            return;
        }
    }
    throw std::runtime_error("MotionBuilder could not add the prepared media to a standard Story track.");
}

}

void CleanupStoryMedia(StoryImportResult& result) noexcept {
    Delete(result.ownedAudioClip);
    Delete(result.ownedVideoClip);
    Delete(result.ownedAudioTrack);
    Delete(result.ownedVideoTrack);
    Delete(result.ownedAudio);
    Delete(result.ownedVideo);
    result = {};
}

StoryImportResult CreateStoryMedia(
    const std::filesystem::path& firstImagePath,
    const std::filesystem::path& sourcePath,
    std::int64_t videoDuration100ns,
    std::int64_t audioDuration100ns,
    std::uint32_t frameRateNumerator,
    std::uint32_t frameRateDenominator,
    const std::filesystem::path& wavPath,
    FBTime start,
    std::int64_t videoStartOffset100ns) {
    if (videoStartOffset100ns < 0) {
        throw std::invalid_argument("Story video start offset cannot be negative; preroll is unsupported.");
    }
    if (firstImagePath.empty() && wavPath.empty()) {
        throw std::invalid_argument("There is no prepared video or audio to import.");
    }
    if (!firstImagePath.empty() && (!std::filesystem::is_regular_file(firstImagePath) ||
        videoDuration100ns <= 0 || frameRateNumerator == 0 || frameRateDenominator == 0)) {
        throw std::invalid_argument("Story video requires an existing image sequence, positive duration and frame rate.");
    }
    if (!wavPath.empty() && (!std::filesystem::is_regular_file(wavPath) || audioDuration100ns <= 0)) {
        throw std::invalid_argument("Story audio requires an existing PCM WAVE file and positive duration.");
    }

    FBTime videoStart = start + MediaTime(videoStartOffset100ns);
    const auto label = PathUtf8(sourcePath.stem());
    HdlFBPlugTemplate<FBVideoClipImage> video;
    HdlFBPlugTemplate<FBAudioClip> audio;
    HdlFBPlugTemplate<FBStoryTrack> videoTrack;
    HdlFBPlugTemplate<FBStoryTrack> audioTrack;
    HdlFBPlugTemplate<FBStoryClip> videoClip;
    HdlFBPlugTemplate<FBStoryClip> audioClip;
    StoryImportResult result;
    try {
        if (!firstImagePath.empty()) {
            const auto path = PathUtf8(std::filesystem::absolute(firstImagePath));
            video = HdlFBPlugTemplate<FBVideoClipImage>(new FBVideoClipImage(path.c_str()));
            if (!video.Ok()) {
                throw std::runtime_error("MotionBuilder could not create the prepared image sequence.");
            }
            // The concrete image constructor does not dispatch the file loader used by FBVideoClip(path).
            video->Filename = path.c_str();
            if (!video->IsValid() || video->Width <= 0 || video->Height <= 0) {
                throw std::runtime_error("MotionBuilder could not open the prepared image sequence.");
            }
            const double rate = static_cast<double>(frameRateNumerator) / frameRateDenominator;
            if (!std::isfinite(rate) || rate <= 0.0) {
                throw std::invalid_argument("Story image sequence frame rate is invalid.");
            }
            video->ImageSequence = true;
            // Changing only the Story clip rate leaves the source's initial system-rate duration in place.
            video->UseSystemFrameRate = false;
            video->FrameRate = rate;
            video->TimeOffset = FBTime(0);
            video->Loop = false;
            videoTrack = HdlFBPlugTemplate<FBStoryTrack>(new FBStoryTrack(kFBStoryTrackVideo));
            videoTrack->Name = (label + " Video").c_str();
            videoClip = HdlFBPlugTemplate<FBStoryClip>(new FBStoryClip(video(), videoTrack(), videoStart));
            CheckTrackClip(videoTrack(), videoClip());
            videoClip->ImageSequence = true;
            videoClip->UseSystemFrameRate = false;
            videoClip->FrameRate = rate;
            videoClip->FirstLoopMarkIn = FBTime(0);
            videoClip->LastLoopMarkOut = MediaTime(videoDuration100ns);
            videoClip->Start = videoStart;
            videoClip->Stop = videoStart + MediaTime(videoDuration100ns);
            videoClip->Speed = 1.0;
            if (!videoClip->ImageSequence) {
                throw std::runtime_error("MotionBuilder did not enable the standard Story image sequence.");
            }
            videoClip->Name = label.c_str();
            Metadata(videoClip(), sourcePath, firstImagePath, "PNG image sequence");
            result.videoTrack = videoTrack();
            result.videoClip = videoClip();
            result.trackVideo = videoTrack->TrackVideo;
        }
        if (!wavPath.empty()) {
            const auto path = PathUtf8(std::filesystem::absolute(wavPath));
            audio = HdlFBPlugTemplate<FBAudioClip>(new FBAudioClip(path.c_str(), false));
            if (!audio.Ok() || !audio->IsMediaReady()) {
                throw std::runtime_error("MotionBuilder could not open the prepared PCM WAVE audio.");
            }
            audioTrack = HdlFBPlugTemplate<FBStoryTrack>(new FBStoryTrack(kFBStoryTrackAudio));
            audioTrack->Name = (label + " Audio").c_str();
            audioClip = HdlFBPlugTemplate<FBStoryClip>(new FBStoryClip(audio(), audioTrack(), start));
            CheckTrackClip(audioTrack(), audioClip());
            audioClip->FirstLoopMarkIn = FBTime(0);
            audioClip->LastLoopMarkOut = MediaTime(audioDuration100ns);
            audioClip->Start = start;
            audioClip->Stop = start + MediaTime(audioDuration100ns);
            audioClip->Speed = 1.0;
            audioClip->Name = label.c_str();
            Metadata(audioClip(), sourcePath, wavPath, "PCM WAVE audio");
            result.audioTrack = audioTrack();
            result.audioClip = audioClip();
        }
        result.ownedVideo = video;
        result.ownedAudio = audio;
        result.ownedVideoTrack = videoTrack;
        result.ownedAudioTrack = audioTrack;
        result.ownedVideoClip = videoClip;
        result.ownedAudioClip = audioClip;
        return result;
    } catch (...) {
        Delete(audioClip);
        Delete(videoClip);
        Delete(audioTrack);
        Delete(videoTrack);
        Delete(audio);
        Delete(video);
        throw;
    }
}

}
