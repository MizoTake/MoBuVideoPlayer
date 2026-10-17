#include <fbsdk/fbsdk.h>

#include <Windows.h>
#include <GL/gl.h>

#include "DecoderWorker.h"
#include "StoryIntegration.h"
#include "mobu/media/MediaCache.h"
#include "mobu/media/Playback.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* SourceProperty = "NativeVideo.Source";
constexpr const char* VersionProperty = "NativeVideo.Version";
constexpr const char* StatusProperty = "NativeVideo.Status";
constexpr const char* RetryProperty = "NativeVideo.Retry";
constexpr const char* PresentationProperty = "NativeVideo.PresentationSeconds";
std::atomic<DWORD> uiThreadId = 0;
thread_local std::string lastImportError;
thread_local std::string lastImportNotice;
bool importRunning = false;

class ImportScope final {
public:
    ImportScope() {
        if (importRunning) { throw std::runtime_error("A native media import is already running."); }
        importRunning = true;
    }
    ~ImportScope() { importRunning = false; }
};

template <typename T>
T GlFunction(const char* name) {
    const auto address = wglGetProcAddress(name);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (!address || value <= 3 || value == std::numeric_limits<std::uintptr_t>::max()) {
        throw std::runtime_error(std::string("OpenGL function is unavailable: ") + name);
    }
    return reinterpret_cast<T>(address);
}

struct GlFunctions {
    using BindBuffer = void(APIENTRY*)(GLenum, GLuint);
    using GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
    using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
    using DeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
    using FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using CheckFramebufferStatus = GLenum(APIENTRY*)(GLenum);
    using BlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    BindBuffer bindBuffer = GlFunction<BindBuffer>("glBindBuffer");
    GenFramebuffers genFramebuffers = GlFunction<GenFramebuffers>("glGenFramebuffers");
    BindFramebuffer bindFramebuffer = GlFunction<BindFramebuffer>("glBindFramebuffer");
    DeleteFramebuffers deleteFramebuffers = GlFunction<DeleteFramebuffers>("glDeleteFramebuffers");
    FramebufferTexture2D framebufferTexture2D = GlFunction<FramebufferTexture2D>("glFramebufferTexture2D");
    CheckFramebufferStatus checkFramebufferStatus = GlFunction<CheckFramebufferStatus>("glCheckFramebufferStatus");
    BlitFramebuffer blitFramebuffer = GlFunction<BlitFramebuffer>("glBlitFramebuffer");
};

const GlFunctions& Gl() {
    static const GlFunctions functions;
    return functions;
}

constexpr GLenum PixelUnpackBuffer = 0x88EC;
constexpr GLenum PixelUnpackBufferBinding = 0x88EF;
constexpr GLenum ReadFramebuffer = 0x8CA8;
constexpr GLenum DrawFramebuffer = 0x8CA9;
constexpr GLenum ReadFramebufferBinding = 0x8CAA;
constexpr GLenum DrawFramebufferBinding = 0x8CA6;
constexpr GLenum ColorAttachment0 = 0x8CE0;
constexpr GLenum FramebufferComplete = 0x8CD5;
constexpr GLenum FramebufferSrgb = 0x8DB9;
constexpr GLenum Rgba8 = 0x8058;
constexpr GLenum Bgra = 0x80E1;
constexpr GLenum ClampToEdge = 0x812F;
constexpr GLenum UnpackImageHeight = 0x806E;
constexpr GLenum UnpackSkipImages = 0x806D;

struct GlResources {
    GLuint texture = 0;
    GLuint upload = 0;
};

class GlFramebuffers final {
public:
    GlFramebuffers() {
        Gl().genFramebuffers(2, ids_);
    }
    ~GlFramebuffers() {
        Gl().deleteFramebuffers(2, ids_);
    }
    GLuint Read() const { return ids_[0]; }
    GLuint Draw() const { return ids_[1]; }
private:
    GLuint ids_[2] = {};
};

class GlUploadState final {
public:
    GlUploadState() {
        const auto& functions = Gl();
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_);
        glGetIntegerv(PixelUnpackBufferBinding, &unpackBuffer_);
        glGetIntegerv(ReadFramebufferBinding, &readFramebuffer_);
        glGetIntegerv(DrawFramebufferBinding, &drawFramebuffer_);
        scissor_ = glIsEnabled(GL_SCISSOR_TEST);
        srgb_ = glIsEnabled(FramebufferSrgb);
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        functions.bindBuffer(PixelUnpackBuffer, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(UnpackImageHeight, 0);
        glPixelStorei(UnpackSkipImages, 0);
        glDisable(GL_SCISSOR_TEST);
        glDisable(FramebufferSrgb);
    }

    ~GlUploadState() {
        const auto& functions = Gl();
        functions.bindFramebuffer(ReadFramebuffer, static_cast<GLuint>(readFramebuffer_));
        functions.bindFramebuffer(DrawFramebuffer, static_cast<GLuint>(drawFramebuffer_));
        glPopClientAttrib();
        functions.bindBuffer(PixelUnpackBuffer, static_cast<GLuint>(unpackBuffer_));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        if (scissor_) {
            glEnable(GL_SCISSOR_TEST);
        }
        if (srgb_) {
            glEnable(FramebufferSrgb);
        }
    }

private:
    GLint texture_ = 0;
    GLint unpackBuffer_ = 0;
    GLint readFramebuffer_ = 0;
    GLint drawFramebuffer_ = 0;
    GLboolean scissor_ = GL_FALSE;
    GLboolean srgb_ = GL_FALSE;
};

std::string PathUtf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return std::string(encoded.begin(), encoded.end());
}

std::filesystem::path Utf8Path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

bool SameNumber(double left, double right) {
    return left == right || (std::isnan(left) && std::isnan(right));
}

template <typename T>
constexpr FBPropertyType PropertyType() {
    if constexpr (std::is_same_v<T, int>) {
        return kFBPT_int;
    } else if constexpr (std::is_same_v<T, bool>) {
        return kFBPT_bool;
    } else if constexpr (std::is_same_v<T, double>) {
        return kFBPT_double;
    } else if constexpr (std::is_same_v<T, FBTime>) {
        return kFBPT_Time;
    } else if constexpr (std::is_same_v<T, const char*>) {
        return kFBPT_charptr;
    } else {
        return kFBPT_unknown;
    }
}

template <typename T>
T Read(FBComponent* component, const char* name, T fallback) {
    if (auto* property = component->PropertyList.Find(name)) {
        if (property->GetPropertyType() == PropertyType<T>()) {
            property->GetData(&fallback, sizeof(fallback));
        }
    }
    return fallback;
}

std::string ReadString(FBComponent* component, const char* name) {
    if (auto* property = component->PropertyList.Find(name)) {
        if (property->GetPropertyType() != kFBPT_charptr) {
            return {};
        }
        const char* value = nullptr;
        property->GetData(&value, sizeof(value));
        return value ? value : "";
    }
    return {};
}

template <typename T>
void Set(FBComponent* component, const char* name, T value) {
    if (auto* property = component->PropertyList.Find(name)) {
        if (property->GetPropertyType() != PropertyType<T>()) {
            return;
        }
        const bool readOnly = property->GetPropertyFlag(kFBPropertyFlagReadOnly);
        property->ModifyPropertyFlag(kFBPropertyFlagReadOnly, false);
        property->SetData(&value);
        property->ModifyPropertyFlag(kFBPropertyFlagReadOnly, readOnly);
    }
}

void SetString(FBComponent* component, const char* name, const std::string& value) {
    const char* text = value.c_str();
    Set(component, name, text);
}

template <typename T>
void Add(FBComponent* component, const char* name, FBPropertyType type, T value) {
    if (component->PropertyList.Find(name)) {
        return;
    }
    auto* property = component->PropertyCreate(name, type, nullptr, false, true);
    if (!property) {
        throw std::runtime_error(std::string("Cannot create property: ") + name);
    }
    property->SetData(&value);
}

template <typename T>
void AddDiagnostic(FBComponent* component, const char* name, FBPropertyType type, T value) {
    Add(component, name, type, value);
    auto* property = component->PropertyList.Find(name);
    if (property->GetPropertyType() == type) {
        Set(component, name, value);
        property->ModifyPropertyFlag(kFBPropertyFlagReadOnly, true);
        property->ModifyPropertyFlag(kFBPropertyFlagNotSavable, true);
    }
}

void AddMetadata(FBTexture* texture, const std::string& path) {
    Add(texture, VersionProperty, kFBPT_int, 1);
    Add(texture, SourceProperty, kFBPT_charptr, path.c_str());
    Add(texture, "NativeVideo.TimeOffset", kFBPT_Time, FBTime(0));
    Add(texture, "NativeVideo.StartFrame", kFBPT_int, 0);
    Add(texture, "NativeVideo.StopFrame", kFBPT_int, -1);
    Add(texture, "NativeVideo.FrameRate", kFBPT_double, 0.0);
    Add(texture, "NativeVideo.PlaySpeed", kFBPT_double, 1.0);
    Add(texture, "NativeVideo.Loop", kFBPT_bool, false);
    Add(texture, RetryProperty, kFBPT_bool, false);
    AddDiagnostic(texture, StatusProperty, kFBPT_charptr, "Waiting for a viewport render");
    AddDiagnostic(texture, "NativeVideo.SourceFrameRate", kFBPT_double, 0.0);
    AddDiagnostic(texture, "NativeVideo.DurationSeconds", kFBPT_double, 0.0);
    AddDiagnostic(texture, "NativeVideo.Width", kFBPT_int, 0);
    AddDiagnostic(texture, "NativeVideo.Height", kFBPT_int, 0);
    AddDiagnostic(texture, PresentationProperty, kFBPT_double, 0.0);
}

std::filesystem::path ResolveSource(const std::string& source) {
    if (source.empty()) {
        throw std::runtime_error("NativeVideo.Source is empty.");
    }
    auto path = Utf8Path(source);
    if (path.is_relative()) {
        const char* sceneName = FBApplication().FBXFileName;
        if (!sceneName || !*sceneName) {
            throw std::runtime_error("Save the FBX scene before using a relative video path.");
        }
        path = Utf8Path(sceneName).parent_path() / path;
    }
    return std::filesystem::absolute(path).lexically_normal();
}

std::string PortableSource(const std::filesystem::path& source) {
    const char* sceneName = FBApplication().FBXFileName;
    if (sceneName && *sceneName) {
        std::error_code error;
        const auto relative = std::filesystem::relative(source,
            Utf8Path(sceneName).parent_path(), error);
        if (!error && !relative.empty()) {
            return PathUtf8(relative);
        }
    }
    return PathUtf8(source);
}

FBTexture* ImportNativeVideo(const char* utf8Path, int destination) {
    if (destination < 0 || destination > 2) {
        throw std::invalid_argument("Destination must be 0 (background), 1 (foreground), or 2 (texture only).");
    }
    if (!utf8Path || !*utf8Path) {
        throw std::invalid_argument("Video path is empty.");
    }
    const auto path = std::filesystem::absolute(Utf8Path(utf8Path)).lexically_normal();
    if (!std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument("Video path must name an existing local file.");
    }
    FBCamera* camera = nullptr;
    if (destination < 2) {
        FBRenderer* renderer = FBSystem().Renderer;
        // GetCameraInPane resolves Camera Switcher; its wrapper is not an FBCamera.
        camera = renderer->GetCameraInPane(renderer->GetSelectedPaneIndex());
        if (!camera || !camera->Is(FBCamera::TypeInfo)) {
            throw std::runtime_error("Select a viewport camera before importing a backplate.");
        }
    }
    const auto label = PathUtf8(path.stem());
    auto* texture = new FBTexture(label.c_str());
    try {
        AddMetadata(texture, PortableSource(path));
        if (camera && destination == 0) {
            camera->BackGroundTexture = texture;
            camera->ViewBackGroundPlaneMode = kFBViewPlaneAlways;
        } else if (camera && destination == 1) {
            camera->ForeGroundTexture = texture;
            camera->ViewForeGroundPlaneMode = kFBViewPlaneAlways;
        }
        texture->Selected = true;
    } catch (...) {
        texture->FBDelete();
        throw;
    }
    return texture;
}

class PreparationProgress final {
public:
    PreparationProgress() {
        progress_.Caption = "Native Video";
        progress_.ProgressBegin();
        progress_.Text = "Preparing standard Story media using Windows APIs. Hold ESC to cancel.";
        progress_.Percent = 0;
    }
    ~PreparationProgress() { progress_.ProgressDone(); }
    bool Cancelled() { return progress_.UserRequestCancell(); }
    void Percent(unsigned value) { progress_.Percent = static_cast<int>(value); }
private:
    FBProgress progress_;
};

void ImportNativeMedia(const char* utf8Path, int destination, const char* utf8CacheRoot, bool includeAudio) {
    if (destination < 0 || destination > 4 || !utf8Path || !*utf8Path) {
        throw std::invalid_argument("Choose an existing media file and destination 0 through 4.");
    }
    if (!utf8CacheRoot || !*utf8CacheRoot) {
        throw std::invalid_argument("Choose a durable cache folder for the PNG sequence and WAV audio.");
    }
    const auto path = std::filesystem::absolute(Utf8Path(utf8Path)).lexically_normal();
    const auto cacheRoot = std::filesystem::absolute(Utf8Path(utf8CacheRoot)).lexically_normal();
    if (!std::filesystem::is_regular_file(path)) {
        throw std::invalid_argument("Media path must name an existing local file.");
    }
    if (destination < 3 && !includeAudio) {
        ImportNativeVideo(utf8Path, destination);
        lastImportNotice = "Imported. Use Transport Controls and NativeVideo.* on the texture.";
        return;
    }
    mobu::media::MediaCacheOptions options;
    options.videoSequence = destination == 3;
    options.includeAudio = includeAudio || destination == 4;
    options.alignAudioToVideo = destination != 4;
    options.preserveMovieTimeline = destination == 3;
    std::atomic<bool> keepWorking = true;
    std::atomic<unsigned> percent = 0;
    options.continueExport = [&keepWorking] { return keepWorking.load(); };
    options.progress = [&percent](unsigned value) { percent = value; };
    PreparationProgress progress;
    auto preparation = std::async(std::launch::async, [path, cacheRoot, options] {
        return mobu::media::PrepareMediaCache(path, cacheRoot, options);
    });
    while (preparation.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) {
        progress.Percent(percent.load());
        if (progress.Cancelled()) { keepWorking = false; }
    }
    const auto prepared = preparation.get();
    if (destination == 4 && !prepared.audio.hasAudio) {
        throw std::runtime_error("This source has no audio stream.");
    }
    FBTime start = FBSystem().LocalTime;
    FBTexture* texture = nullptr;
    HdlFBPlugTemplate<FBCamera> camera;
    HdlFBPlugTemplate<FBTexture> previousTexture;
    FBCameraViewPlaneMode previousPlaneMode = kFBViewPlaneAlways;
    mobu::plugin::StoryImportResult story;
    try {
        if (destination < 3) {
            if (destination < 2) {
                FBRenderer* renderer = FBSystem().Renderer;
                camera = HdlFBPlugTemplate<FBCamera>(renderer->GetCameraInPane(renderer->GetSelectedPaneIndex()));
                if (camera.Ok()) {
                    previousTexture = HdlFBPlugTemplate<FBTexture>(destination == 0 ?
                        static_cast<FBTexture*>(camera->BackGroundTexture) : static_cast<FBTexture*>(camera->ForeGroundTexture));
                    previousPlaneMode = destination == 0 ? camera->ViewBackGroundPlaneMode : camera->ViewForeGroundPlaneMode;
                }
            }
            texture = ImportNativeVideo(utf8Path, destination);
            Set(texture, "NativeVideo.TimeOffset", start);
        }
        if (destination == 3 || prepared.audio.hasAudio) {
            story = mobu::plugin::CreateStoryMedia(prepared.video.firstFrame, path,
                prepared.video.duration100ns, prepared.audio.duration100ns,
                prepared.video.frameRateNumerator, prepared.video.frameRateDenominator,
                prepared.audioFile, start, prepared.videoStart100ns);
        }
        FBComponent* metadata = texture ? static_cast<FBComponent*>(texture) :
            static_cast<FBComponent*>(story.videoClip ? story.videoClip : story.audioClip);
        if (metadata) {
            Add(metadata, "NativeVideo.Cache", kFBPT_charptr, PortableSource(prepared.directory).c_str());
            Add(metadata, "NativeVideo.SourceSha256", kFBPT_charptr, prepared.sourceSha256.c_str());
        }
        if (story.audioClip) {
            Add(story.audioClip, "NativeVideo.AudioBits", kFBPT_int, static_cast<int>(prepared.audio.bitsPerSample));
            Add(story.audioClip, "NativeVideo.AudioPrecisionReduced", kFBPT_bool, prepared.audio.precisionReduced);
        }
        lastImportNotice = prepared.audio.precisionReduced ?
            "Imported. Audio was converted to 24-bit PCM because standard Story cannot load this source precision." :
            "Imported at current time. Use standard Story / Transport Controls; keep the media cache.";
    } catch (...) {
        mobu::plugin::CleanupStoryMedia(story);
        if (camera.Ok()) {
            if (destination == 0) {
                camera->BackGroundTexture = previousTexture.Ok() ? previousTexture() : nullptr;
                camera->ViewBackGroundPlaneMode = previousPlaneMode;
            } else if (destination == 1) {
                camera->ForeGroundTexture = previousTexture.Ok() ? previousTexture() : nullptr;
                camera->ViewForeGroundPlaneMode = previousPlaneMode;
            }
        }
        if (texture) { texture->FBDelete(); }
        throw;
    }
}

bool IsSceneOwnership(FBPlug* destination) {
    FBScene* scene = FBSystem().Scene;
    std::unordered_set<FBPlug*> visited;
    for (auto* plug = destination; plug && visited.insert(plug).second; plug = plug->GetOwner()) {
        if (plug == scene || (scene && plug->GetHIObject() == scene->GetHIObject()) ||
            plug->Is(FBScene::TypeInfo)) {
            return true;
        }
        if (plug->Is(FBComponent::TypeInfo)) {
            return false;
        }
    }
    return false;
}

bool HasVideoConsumers(FBVideo* video);

void DeleteUnusedRuntime(FBVideoMemory* video, const char* operation) {
    const bool consumers = HasVideoConsumers(video);
    FBScene* scene = FBSystem().Scene;
    FBTrace("[NativeVideo] %s: %s destinations=%d consumers=%d\n", operation,
        static_cast<const char*>(video->Name), video->GetDstCount(), consumers ? 1 : 0);
    for (int index = 0; index < video->GetDstCount(); ++index) {
        auto* destination = video->GetDst(index);
        if (destination) {
            FBTrace("[NativeVideo] destination class=%s isScene=%d isComponent=%d sceneIdentity=%d sceneOwnership=%d\n",
                destination->ClassName(), destination->Is(FBScene::TypeInfo) ? 1 : 0,
                destination->Is(FBComponent::TypeInfo) ? 1 : 0,
                scene && destination->GetHIObject() == scene->GetHIObject() ? 1 : 0,
                IsSceneOwnership(destination) ? 1 : 0);
        }
    }
    if (!consumers) {
        video->TextureOGLId = 0;
        AddDiagnostic(video, "NativeVideo.Lifecycle", kFBPT_charptr, "FBDelete requested");
        HdlFBPlugTemplate<FBVideoMemory> handle(video);
        video->FBDelete();
        if (handle.Ok()) {
            SetString(handle, "NativeVideo.Lifecycle", "FBDelete returned; object is still valid");
            FBTrace("[NativeVideo] FBDelete returned; %s is still valid\n", static_cast<const char*>(handle->Name));
        }
    }
}

bool HasVideoConsumers(FBVideo* video) {
    for (int index = 0; index < video->GetDstCount(); ++index) {
        auto* destination = video->GetDst(index);
        if (destination && !IsSceneOwnership(destination)) {
            return true;
        }
    }
    return false;
}

class TextureBinding final {
public:
    explicit TextureBinding(FBTexture* texture) : texture_(texture) {
        AddMetadata(texture, ReadString(texture, SourceProperty));
        const std::string label = std::string(static_cast<const char*>(texture->Name)) + " Video";
        FBVideo* previous = texture->Video;
        memory_ = HdlFBPlugTemplate<FBVideoMemory>(new FBVideoMemory(label.c_str()));
        Add(memory_, "NativeVideo.Runtime", kFBPT_bool, true);
        memory_->TextureOGLId = 0;
        texture->Video = memory_();
        if (previous && previous->Is(FBVideoMemory::TypeInfo)) {
            auto* oldMemory = static_cast<FBVideoMemory*>(previous);
            if (Read(oldMemory, "NativeVideo.Runtime", false)) {
                DeleteUnusedRuntime(oldMemory, "replace saved video");
            }
        }
    }

    ~TextureBinding() {
        try {
            if (memory_.Ok()) {
                memory_->TextureOGLId = 0;
                DetachOwner();
                DeleteUnusedRuntime(memory_, "release binding");
            }
        } catch (...) {
            // Other scene objects may still hold a video connection during scene teardown.
        }
    }

    FBTexture* Texture() const { return texture_(); }
    GlResources Resources() const { return resources_; }

    bool Valid() const { return texture_.Ok() && memory_.Ok(); }

    void DetachOwner() {
        if (texture_.Ok() && memory_.Ok() && static_cast<FBVideo*>(texture_->Video) == memory_()) {
            texture_->Video = nullptr;
        }
    }

    bool HasConsumers() const { return memory_.Ok() && HasVideoConsumers(memory_); }

    bool OwnsVideo(FBVideo* video) const {
        return memory_.Ok() && memory_->GetHIObject() == video->GetHIObject();
    }

    void RefreshSettings() {
        if (!Valid()) {
            return;
        }
        if (static_cast<FBVideo*>(texture_->Video) != memory_()) {
            texture_->Video = memory_();
        }
        source_ = ReadString(texture_, SourceProperty);
        bool sourceValid = true;
        try {
            auto* sourceProperty = texture_->PropertyList.Find(SourceProperty);
            if (!sourceProperty || sourceProperty->GetPropertyType() != kFBPT_charptr) {
                throw std::runtime_error("NativeVideo.Source must be a String property.");
            }
            resolvedSource_ = PathUtf8(ResolveSource(source_));
        } catch (const std::exception& error) {
            resolvedSource_.clear();
            sourceValid = false;
            status_ = error.what();
            failed_ = true;
        }
        const auto previousSettings = settings_;
        settings_.timeOffsetSeconds = Read(texture_, "NativeVideo.TimeOffset", FBTime(0)).GetSecondDouble();
        settings_.startFrame = Read(texture_, "NativeVideo.StartFrame", 0);
        settings_.stopFrame = Read(texture_, "NativeVideo.StopFrame", -1);
        settings_.frameRate = Read(texture_, "NativeVideo.FrameRate", 0.0);
        settings_.playSpeed = Read(texture_, "NativeVideo.PlaySpeed", 1.0);
        settings_.loop = Read(texture_, "NativeVideo.Loop", false);
        const bool settingsChanged = !SameNumber(settings_.timeOffsetSeconds, previousSettings.timeOffsetSeconds) ||
            settings_.startFrame != previousSettings.startFrame || settings_.stopFrame != previousSettings.stopFrame ||
            !SameNumber(settings_.frameRate, previousSettings.frameRate) ||
            !SameNumber(settings_.playSpeed, previousSettings.playSpeed) ||
            settings_.loop != previousSettings.loop;
        if (settingsChanged) {
            sourceRange_.reset();
        }
        if (Read(texture_, RetryProperty, false) || openedSource_ != resolvedSource_ || (failed_ && settingsChanged)) {
            worker_.reset();
            openedSource_ = resolvedSource_;
            failed_ = !sourceValid;
            initialized_ = false;
            sourceRange_.reset();
            lastRequested_ = -1;
            memory_->TextureOGLId = 0;
            Set(texture_, RetryProperty, false);
            if (sourceValid) {
                status_ = "Waiting for a viewport render";
            }
        }
        SetString(texture_, StatusProperty, status_);
        if (initialized_) {
            Set(texture_, "NativeVideo.SourceFrameRate", SourceFrameRate());
            Set(texture_, "NativeVideo.DurationSeconds", info_.duration100ns / 10000000.0);
            Set(texture_, "NativeVideo.Width", static_cast<int>(info_.width));
            Set(texture_, "NativeVideo.Height", static_cast<int>(info_.height));
            Set(texture_, PresentationProperty, presentationSeconds_);
        }
    }

    void Render(double timelineSeconds) noexcept {
        if (!memory_.Ok() || failed_ || resolvedSource_.empty()) {
            return;
        }
        try {
            if (!worker_) {
                worker_ = std::make_unique<mobu::plugin::DecoderWorker>(
                    Utf8Path(resolvedSource_));
            }
            if (!initialized_) {
                auto result = worker_->DecodeAt(0);
                info_ = result.info;
                initialized_ = true;
            }
            auto settings = settings_;
            if (settings.frameRate == 0.0) {
                settings.frameRate = SourceFrameRate();
            }
            settings.duration100ns = info_.duration100ns;
            if (settings_.frameRate == 0.0 &&
                (settings.startFrame > 0 || settings.stopFrame >= 0 || settings.playSpeed < 0.0)) {
                if (!sourceRange_) {
                    sourceRange_ = worker_->ResolveFrameRange(settings.startFrame, settings.stopFrame);
                }
                settings.sourceRange = sourceRange_;
            }
            const auto requested = std::min(mobu::media::MapTimelineToMediaTime(timelineSeconds, settings),
                std::max<std::int64_t>(0, info_.duration100ns - 1));
            if (requested == lastRequested_ && resources_.texture != 0) {
                return;
            }
            auto result = worker_->DecodeAt(requested);
            Upload(result.frame);
            presentationSeconds_ = result.frame.presentation100ns / 10000000.0;
            lastRequested_ = requested;
            status_ = "Ready - OS native decoder; audio is not enabled";
        } catch (const std::exception& error) {
            failed_ = true;
            status_ = std::string("Error: ") + error.what() + ". Set NativeVideo.Retry to retry.";
        } catch (...) {
            failed_ = true;
            status_ = "Error: unknown native video failure. Set NativeVideo.Retry to retry.";
        }
    }

private:
    double SourceFrameRate() const {
        return info_.frameRateDenominator && info_.frameRateNumerator
            ? static_cast<double>(info_.frameRateNumerator) / info_.frameRateDenominator : 30.0;
    }

    void Upload(const mobu::media::VideoFrame& frame) {
        if (!frame.width || !frame.height ||
            frame.bgra.size() != static_cast<std::size_t>(frame.width) * frame.height * 4) {
            throw std::runtime_error("Native decoder returned an invalid BGRA video frame.");
        }
        const auto& functions = Gl();
        GlUploadState savedState;
        if (!resources_.texture) {
            glGenTextures(1, &resources_.texture);
            glGenTextures(1, &resources_.upload);
        }
        glBindTexture(GL_TEXTURE_2D, resources_.upload);
        if (width_ != frame.width || height_ != frame.height) {
            AllocateTexture(frame.width, frame.height, frame.bgra.data());
            glBindTexture(GL_TEXTURE_2D, resources_.texture);
            AllocateTexture(frame.width, frame.height, nullptr);
            width_ = frame.width;
            height_ = frame.height;
            memory_->SetObjectImageSize(static_cast<int>(width_), static_cast<int>(height_));
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(frame.width),
                static_cast<GLsizei>(frame.height), Bgra, GL_UNSIGNED_BYTE, frame.bgra.data());
        }
        // Framebuffer objects are not shared by interactive and offscreen OpenGL contexts.
        GlFramebuffers framebuffers;
        functions.bindFramebuffer(ReadFramebuffer, framebuffers.Read());
        functions.framebufferTexture2D(ReadFramebuffer, ColorAttachment0, GL_TEXTURE_2D, resources_.upload, 0);
        functions.bindFramebuffer(DrawFramebuffer, framebuffers.Draw());
        functions.framebufferTexture2D(DrawFramebuffer, ColorAttachment0, GL_TEXTURE_2D, resources_.texture, 0);
        if (functions.checkFramebufferStatus(ReadFramebuffer) != FramebufferComplete ||
            functions.checkFramebufferStatus(DrawFramebuffer) != FramebufferComplete) {
            throw std::runtime_error("OpenGL could not create the native video framebuffers.");
        }
        // Camera backplates can bypass FBTexture UV transforms, so invert on the GPU instead.
        functions.blitFramebuffer(0, 0, static_cast<GLint>(width_), static_cast<GLint>(height_),
            0, static_cast<GLint>(height_), static_cast<GLint>(width_), 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        memory_->TextureOGLId = static_cast<int>(resources_.texture);
    }

    static void AllocateTexture(std::uint32_t width, std::uint32_t height, const void* pixels) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ClampToEdge);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, ClampToEdge);
        glTexImage2D(GL_TEXTURE_2D, 0, Rgba8, static_cast<GLsizei>(width),
            static_cast<GLsizei>(height), 0, Bgra, GL_UNSIGNED_BYTE, pixels);
    }

    HdlFBPlugTemplate<FBTexture> texture_;
    HdlFBPlugTemplate<FBVideoMemory> memory_;
    std::string source_;
    std::string resolvedSource_;
    std::string openedSource_;
    std::string status_ = "Waiting for a viewport render";
    mobu::media::PlaybackSettings settings_;
    mobu::media::VideoInfo info_;
    std::optional<mobu::media::FrameRange> sourceRange_;
    std::unique_ptr<mobu::plugin::DecoderWorker> worker_;
    bool failed_ = false;
    bool initialized_ = false;
    std::int64_t lastRequested_ = -1;
    double presentationSeconds_ = 0;
    GlResources resources_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}

extern "C" __declspec(dllexport) int MoBuNativeVideoImport(const char* utf8Path, int destination) {
    try {
        lastImportError.clear();
        if (!uiThreadId.load() || GetCurrentThreadId() != uiThreadId.load()) {
            throw std::runtime_error("Native video import must run on the MotionBuilder UI thread after initialization.");
        }
        ImportScope importScope;
        ImportNativeVideo(utf8Path, destination);
        return 1;
    } catch (const std::exception& error) {
        lastImportError = error.what();
    } catch (...) {
        lastImportError = "Unknown native video import failure.";
    }
    return 0;
}

extern "C" __declspec(dllexport) const char* MoBuNativeVideoLastError() {
    return lastImportError.c_str();
}

extern "C" __declspec(dllexport) const char* MoBuNativeMediaLastNotice() {
    return lastImportNotice.c_str();
}

extern "C" __declspec(dllexport) int MoBuNativeMediaImport(
    const char* utf8Path, int destination, const char* utf8CacheRoot, int includeAudio) {
    try {
        lastImportError.clear();
        lastImportNotice.clear();
        if (!uiThreadId.load() || GetCurrentThreadId() != uiThreadId.load()) {
            throw std::runtime_error("Native media import must run on the MotionBuilder UI thread after initialization.");
        }
        if (includeAudio != 0 && includeAudio != 1) {
            throw std::invalid_argument("includeAudio must be 0 or 1.");
        }
        ImportScope importScope;
        ImportNativeMedia(utf8Path, destination, utf8CacheRoot, includeAudio != 0);
        return 1;
    } catch (const std::exception& error) {
        lastImportError = error.what();
    } catch (...) {
        lastImportError = "Unknown native media import failure.";
    }
    return 0;
}

class MoBuNativeVideoManager final : public FBCustomManager {
    FBCustomManagerDeclare(MoBuNativeVideoManager);
public:
    bool FBCreate() override { return true; }
    bool Init() override { return true; }
    bool Open() override {
        uiThreadId = GetCurrentThreadId();
        FBSystem().OnUIIdle.Add(this, static_cast<FBCallback>(&MoBuNativeVideoManager::OnIdle));
        FBEvaluateManager::TheOne().OnRenderingPipelineEvent.Add(this,
            static_cast<FBCallback>(&MoBuNativeVideoManager::OnRender));
        return true;
    }
    bool Clear() override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& binding : bindings_) {
            retiredResources_.push_back(binding->Resources());
        }
        bindings_.clear();
        return true;
    }
    bool Close() override {
        uiThreadId = 0;
        FBSystem().OnUIIdle.Remove(this, static_cast<FBCallback>(&MoBuNativeVideoManager::OnIdle));
        FBEvaluateManager::TheOne().OnRenderingPipelineEvent.Remove(this,
            static_cast<FBCallback>(&MoBuNativeVideoManager::OnRender));
        return Clear();
    }
    void FBDestroy() override {}

private:
    void OnIdle(HISender, HKEvent) {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            std::unordered_set<FBTexture*> seen;
            FBScene* scene = FBSystem().Scene;
            for (int index = 0; index < scene->Textures.GetCount(); ++index) {
                auto* texture = scene->Textures[index];
                if (!texture || Read(texture, VersionProperty, 0) != 1) {
                    continue;
                }
                seen.insert(texture);
                auto found = std::find_if(bindings_.begin(), bindings_.end(),
                    [texture](const auto& binding) { return binding->Valid() && binding->Texture() == texture; });
                if (found == bindings_.end()) {
                    bindings_.push_back(std::make_unique<TextureBinding>(texture));
                    found = std::prev(bindings_.end());
                }
                (*found)->RefreshSettings();
            }
            bindings_.erase(std::remove_if(bindings_.begin(), bindings_.end(), [this, &seen](const auto& binding) {
                if (!binding->Valid() || seen.count(binding->Texture()) == 0) {
                    binding->DetachOwner();
                    if (binding->HasConsumers()) {
                        return false;
                    }
                    retiredResources_.push_back(binding->Resources());
                    return true;
                }
                return false;
            }), bindings_.end());
            std::vector<HdlFBPlugTemplate<FBVideoMemory>> orphans;
            for (int index = 0; index < scene->Components.GetCount(); ++index) {
                auto* component = scene->Components[index];
                if (!component || !component->Is(FBVideoMemory::TypeInfo)) {
                    continue;
                }
                auto* video = static_cast<FBVideoMemory*>(component);
                const bool owned = std::any_of(bindings_.begin(), bindings_.end(),
                    [video](const auto& binding) { return binding->OwnsVideo(video); });
                if (Read(video, "NativeVideo.Runtime", false) && !owned && !HasVideoConsumers(video)) {
                    orphans.emplace_back(video);
                }
            }
            for (const auto& orphan : orphans) {
                if (orphan.Ok() && !HasVideoConsumers(orphan)) {
                    DeleteUnusedRuntime(orphan, "release unbound saved video");
                }
            }
        } catch (...) {
            // An SDK event boundary must not unwind into the host.
        }
    }

    void OnRender(HISender, HKEvent event) {
        try {
            FBEventEvalGlobalCallback callback(event);
            if (callback.GetTiming() != kFBGlobalEvalCallbackBeforeRender || !wglGetCurrentContext()) {
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& resources : retiredResources_) {
                if (resources.texture) {
                    glDeleteTextures(1, &resources.texture);
                }
                if (resources.upload) {
                    glDeleteTextures(1, &resources.upload);
                }
            }
            retiredResources_.clear();
            FBTime time = callback.GetEvaluateInfo() ? callback.GetEvaluateInfo()->GetLocalTime() : FBSystem().LocalTime;
            for (const auto& binding : bindings_) {
                binding->Render(time.GetSecondDouble());
            }
        } catch (...) {
            // The render callback runs inside MotionBuilder's OpenGL pipeline.
        }
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<TextureBinding>> bindings_;
    std::vector<GlResources> retiredResources_;
};

FBCustomManagerImplementation(MoBuNativeVideoManager);
FBRegisterCustomManager(MoBuNativeVideoManager);

class MoBuNativeVideoTool final : public FBTool {
    FBToolDeclare(MoBuNativeVideoTool, FBTool);
public:
    bool FBCreate() override {
        StartSize[0] = 700;
        StartSize[1] = 250;
        AddRegion("Destination", "Destination", 10, kFBAttachLeft, "", 1,
            10, kFBAttachTop, "", 1, -10, kFBAttachRight, "", 1, 25, kFBAttachNone, "", 1);
        AddRegion("Audio", "Audio", 10, kFBAttachLeft, "", 1,
            42, kFBAttachTop, "", 1, -10, kFBAttachRight, "", 1, 24, kFBAttachNone, "", 1);
        AddRegion("Cache", "Cache", 10, kFBAttachLeft, "", 1,
            75, kFBAttachTop, "", 1, -110, kFBAttachRight, "", 1, 25, kFBAttachNone, "", 1);
        AddRegion("Browse", "Browse", -100, kFBAttachRight, "", 1,
            75, kFBAttachTop, "", 1, 90, kFBAttachNone, "", 1, 25, kFBAttachNone, "", 1);
        AddRegion("Import", "Import", 10, kFBAttachLeft, "", 1,
            110, kFBAttachTop, "", 1, -10, kFBAttachRight, "", 1, 30, kFBAttachNone, "", 1);
        AddRegion("Hint", "Hint", 10, kFBAttachLeft, "", 1,
            150, kFBAttachTop, "", 1, -10, kFBAttachRight, "", 1, 24, kFBAttachNone, "", 1);
        AddRegion("Status", "Status", 10, kFBAttachLeft, "", 1,
            180, kFBAttachTop, "", 1, -10, kFBAttachRight, "", 1, 40, kFBAttachNone, "", 1);
        SetControl("Destination", destination_);
        SetControl("Audio", audio_);
        SetControl("Cache", cache_);
        SetControl("Browse", browse_);
        SetControl("Import", import_);
        SetControl("Hint", hint_);
        SetControl("Status", status_);
        destination_.Style = kFBDropDownList;
        destination_.Items.Add("Current camera background");
        destination_.Items.Add("Current camera foreground");
        destination_.Items.Add("Texture only");
        destination_.Items.Add("Story video + audio (standard clips)");
        destination_.Items.Add("Story audio only");
        destination_.ItemIndex = 3;
        audio_.Style = kFBCheckbox;
        audio_.Caption = "Include audio as a standard Story audio clip";
        audio_.State = 1;
        browse_.Caption = "Cache folder...";
        browse_.OnClick.Add(this, static_cast<FBCallback>(&MoBuNativeVideoTool::OnBrowse));
        import_.Caption = "Import using Windows APIs...";
        import_.OnClick.Add(this, static_cast<FBCallback>(&MoBuNativeVideoTool::OnImport));
        hint_.Caption = "Story: edit standard clips, then drag the video track onto a camera video plane.";
        status_.Caption = "Cache: chosen folder, or NativeVideoMedia beside the FBX/source. Keep it with the scene.";
        return true;
    }

    void FBDestroy() override {
        import_.OnClick.Remove(this, static_cast<FBCallback>(&MoBuNativeVideoTool::OnImport));
        browse_.OnClick.Remove(this, static_cast<FBCallback>(&MoBuNativeVideoTool::OnBrowse));
    }

private:
    void OnBrowse(HISender, HKEvent) {
        FBFolderPopup folder;
        folder.Caption = "Choose a durable media cache folder";
        folder.Path = static_cast<const char*>(cache_.Text);
        if (folder.Execute()) { cache_.Text = static_cast<const char*>(folder.Path); }
    }

    void OnImport(HISender, HKEvent) {
        try {
            FBFilePopup popup;
            popup.Caption = "Import native video/audio";
            popup.Style = kFBFilePopupOpen;
            popup.Filter = "*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.m4a;*.mp3;*.wav;*.wma";
            if (!popup.Execute()) {
                return;
            }
            std::string cache = static_cast<const char*>(cache_.Text);
            if (cache.empty()) {
                const char* sceneName = FBApplication().FBXFileName;
                const auto parent = sceneName && *sceneName ? Utf8Path(sceneName).parent_path() :
                    Utf8Path(static_cast<const char*>(popup.FullFilename)).parent_path();
                cache = PathUtf8(parent / "NativeVideoMedia");
                cache_.Text = cache.c_str();
            }
            if (!MoBuNativeMediaImport(static_cast<const char*>(popup.FullFilename), destination_.ItemIndex,
                cache.c_str(), audio_.State ? 1 : 0)) {
                throw std::runtime_error(MoBuNativeVideoLastError());
            }
            status_.Caption = MoBuNativeMediaLastNotice();
        } catch (const std::exception& error) {
            status_.Caption = error.what();
            FBMessageBox("Native Video", error.what(), "OK");
        } catch (...) {
            status_.Caption = "Native video import failed.";
        }
    }

    FBList destination_;
    FBButton audio_;
    FBEdit cache_;
    FBButton browse_;
    FBButton import_;
    FBLabel hint_;
    FBLabel status_;
};

FBToolImplementation(MoBuNativeVideoTool);
FBRegisterTool(MoBuNativeVideoTool, "Native Video", "Video playback with the operating system decoder", FB_DEFAULT_SDK_ICON);

FBLibraryDeclare(MoBuNativeVideo)
{
    FBLibraryRegister(MoBuNativeVideoManager);
    FBLibraryRegister(MoBuNativeVideoTool);
}
FBLibraryDeclareEnd;

bool FBLibrary::LibInit() { return true; }
bool FBLibrary::LibOpen() { return true; }
bool FBLibrary::LibReady() { return true; }
bool FBLibrary::LibClose() { return true; }
bool FBLibrary::LibRelease() { return true; }
