#include "mobu/media/MediaCache.h"

#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace mobu::media {
namespace {

constexpr unsigned CacheRevision = 4;

std::filesystem::path AudioFilename(const std::string& digest, const MediaCacheOptions& options) {
    return "a" + std::to_string(CacheRevision) + "-" + digest +
        (options.preserveMovieTimeline ? "-movie" : options.alignAudioToVideo ? "-clip" : "-raw") + ".wav";
}

void Progress(const MediaCacheOptions& options, unsigned value) {
    if (options.progress) { options.progress(value); }
}

void CheckCancelled(const MediaCacheOptions& options) {
    if (options.continueExport && !options.continueExport()) {
        throw std::runtime_error("Media preparation was cancelled.");
    }
}

class Sha256 final {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            throw std::runtime_error("Cannot open the Windows SHA256 provider.");
        }
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("Cannot create the Windows SHA256 hash.");
        }
    }
    ~Sha256() {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    std::string File(const std::filesystem::path& path, const MediaCacheOptions& options) {
        std::ifstream input(path, std::ios::binary);
        if (!input) { throw std::runtime_error("Cannot read a media/cache file for hashing."); }
        std::vector<char> buffer(1024 * 1024);
        while (input) {
            CheckCancelled(options);
            input.read(buffer.data(), buffer.size());
            if (input.gcount() && BCryptHashData(hash_, reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(input.gcount()), 0) < 0) {
                throw std::runtime_error("Windows SHA256 hashing failed.");
            }
        }
        if (!input.eof()) { throw std::runtime_error("A media/cache file read failed."); }
        std::array<unsigned char, 32> digest;
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            throw std::runtime_error("Windows SHA256 finalization failed.");
        }
        std::ostringstream output;
        for (const auto value : digest) {
            output << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(value);
        }
        return output.str();
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string Hash(const std::filesystem::path& path, const MediaCacheOptions& options) {
    return Sha256().File(path, options);
}

std::filesystem::path FramePath(const std::filesystem::path& directory, std::uint64_t index) {
    std::ostringstream name;
    name << "frame" << std::setfill('0') << std::setw(10) << index << ".png";
    return directory / "video" / name.str();
}

void WriteFileRecord(std::ostream& stream, const std::filesystem::path& file,
    const MediaCacheOptions& options) {
    stream << std::filesystem::file_size(file) << ' ' << Hash(file, options) << '\n';
}

void VerifyFileRecord(std::istream& stream, const std::filesystem::path& file,
    const MediaCacheOptions& options) {
    std::uint64_t size;
    std::string digest;
    if (!(stream >> size >> digest) || !std::filesystem::is_regular_file(file) ||
        std::filesystem::file_size(file) != size || Hash(file, options) != digest) {
        throw std::runtime_error("The media cache is incomplete or damaged. Select a new cache folder to rebuild it.");
    }
}

void WriteManifest(const PreparedMedia& prepared, const MediaCacheOptions& options) {
    std::ofstream manifest(prepared.directory / "complete.txt", std::ios::binary);
    const auto& video = prepared.video;
    const auto& info = video.sourceInfo;
    const auto& audio = prepared.audio;
    manifest << "NativeMediaCache " << CacheRevision << ' ' << prepared.sourceSha256 << '\n'
        << options.videoSequence << ' ' << options.includeAudio << ' ' << options.alignAudioToVideo << ' '
        << options.preserveMovieTimeline << ' ' << prepared.videoStart100ns << '\n'
        << info.width << ' ' << info.height << ' ' << info.duration100ns << ' '
        << info.presentationOrigin100ns << ' ' << info.frameRateNumerator << ' ' << info.frameRateDenominator << '\n'
        << video.frameRateNumerator << ' ' << video.frameRateDenominator << ' ' << video.frameCount << ' '
        << video.variableFrameRateResampled << ' ' << video.duration100ns << '\n'
        << audio.hasAudio << ' ' << audio.sampleRate << ' ' << audio.channels << ' ' << audio.bitsPerSample << ' '
        << audio.channelMask << ' ' << audio.frameCount << ' ' << audio.firstPresentation100ns << ' '
        << audio.duration100ns << ' ' << audio.precisionReduced << '\n';
    for (std::uint64_t index = 0; index < video.frameCount; ++index) {
        WriteFileRecord(manifest, FramePath(prepared.directory, index), options);
    }
    if (audio.hasAudio) { WriteFileRecord(manifest, prepared.audioFile, options); }
    manifest.flush();
    if (!manifest) { throw std::runtime_error("Cannot finish the media cache manifest."); }
}

PreparedMedia ReadManifest(const std::filesystem::path& directory, const std::string& digest,
    const MediaCacheOptions& options) {
    PreparedMedia prepared;
    prepared.directory = directory;
    prepared.sourceSha256 = digest;
    prepared.reused = true;
    std::ifstream manifest(directory / "complete.txt", std::ios::binary);
    std::string magic, storedDigest;
    unsigned version = 0;
    bool videoSequence = false, includeAudio = false, alignAudio = false, preserveMovie = false;
    auto& video = prepared.video;
    auto& info = video.sourceInfo;
    auto& audio = prepared.audio;
    if (!(manifest >> magic >> version >> storedDigest >> videoSequence >> includeAudio >> alignAudio
        >> preserveMovie >> prepared.videoStart100ns
        >> info.width >> info.height >> info.duration100ns >> info.presentationOrigin100ns
        >> info.frameRateNumerator >> info.frameRateDenominator
        >> video.frameRateNumerator >> video.frameRateDenominator >> video.frameCount >> video.variableFrameRateResampled >> video.duration100ns
        >> audio.hasAudio >> audio.sampleRate >> audio.channels >> audio.bitsPerSample
        >> audio.channelMask >> audio.frameCount >> audio.firstPresentation100ns >> audio.duration100ns >> audio.precisionReduced) ||
        magic != "NativeMediaCache" || version != CacheRevision || storedDigest != digest ||
        videoSequence != options.videoSequence || includeAudio != options.includeAudio ||
        alignAudio != options.alignAudioToVideo ||
        preserveMovie != options.preserveMovieTimeline || prepared.videoStart100ns < 0 ||
        (videoSequence && (!video.frameCount || !video.frameRateNumerator || !video.frameRateDenominator))) {
        throw std::runtime_error("The media cache manifest is invalid. Select a new cache folder to rebuild it.");
    }
    // Filenames are generated here because a cache manifest must not choose arbitrary read paths.
    for (std::uint64_t index = 0; index < video.frameCount; ++index) {
        VerifyFileRecord(manifest, FramePath(directory, index), options);
    }
    if (videoSequence) {
        video.directory = directory / "video";
        video.firstFrame = FramePath(directory, 0);
    }
    if (audio.hasAudio) {
        prepared.audioFile = directory / AudioFilename(digest, options);
        VerifyFileRecord(manifest, prepared.audioFile, options);
    }
    std::string trailing;
    if (manifest >> trailing) { throw std::runtime_error("Unexpected media cache manifest data."); }
    return prepared;
}

std::string UniqueSuffix() {
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) { throw std::runtime_error("Cannot create a cache transaction identifier."); }
    wchar_t text[40];
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    std::string result;
    for (const auto value : std::wstring_view(text)) { result.push_back(static_cast<char>(value)); }
    return result;
}

}

PreparedMedia PrepareMediaCache(const std::filesystem::path& source,
    const std::filesystem::path& cacheRoot, const MediaCacheOptions& options) {
    if (!options.videoSequence && !options.includeAudio) {
        throw std::invalid_argument("Choose video or audio media preparation.");
    }
    if (cacheRoot.empty() || !std::filesystem::is_regular_file(source)) {
        throw std::invalid_argument("An existing source file and a cache folder are required.");
    }
    const auto originalSize = std::filesystem::file_size(source);
    const auto originalTime = std::filesystem::last_write_time(source);
    const auto verifySourceUnchanged = [&] {
        if (std::filesystem::file_size(source) != originalSize ||
            std::filesystem::last_write_time(source) != originalTime) {
            throw std::runtime_error("The source file changed while preparing media. Import it again after writing finishes.");
        }
    };
    PreparedMedia prepared;
    prepared.sourceSha256 = Hash(source, options);
    const auto root = std::filesystem::absolute(cacheRoot).lexically_normal();
    const auto key = "v" + std::to_string(CacheRevision) + "-" + prepared.sourceSha256 + (options.videoSequence ? "-v" : "") +
        (options.includeAudio ? "-a" : "") + (options.alignAudioToVideo ? "-aligned" : "") +
        (options.preserveMovieTimeline ? "-movie" : "");
    const auto finalDirectory = root / key;
    if (std::filesystem::exists(finalDirectory)) {
        const auto result = ReadManifest(finalDirectory, prepared.sourceSha256, options);
        verifySourceUnchanged();
        Progress(options, 100);
        return result;
    }
    std::filesystem::create_directories(root);
    prepared.directory = root / (".partial-" + UniqueSuffix());
    if (!std::filesystem::create_directory(prepared.directory)) {
        throw std::runtime_error("Cannot create a new cache transaction folder.");
    }
    try {
        CheckCancelled(options);
        Progress(options, 5);
        std::int64_t origin = 0;
        const auto timeline = (options.includeAudio || options.preserveMovieTimeline) ?
            ReadPresentationTimeline(source) : PresentationTimeline{};
        if (options.videoSequence) {
            SequenceOptions sequenceOptions;
            sequenceOptions.progress = [&options](std::uint64_t done, std::uint64_t total) {
                Progress(options, 5 + static_cast<unsigned>(total ? done * 75 / total : 0));
                return !options.continueExport || options.continueExport();
            };
            auto decoder = CreateNativeDecoder();
            decoder->Open(source);
            origin = decoder->Info().presentationOrigin100ns;
            if (options.preserveMovieTimeline) {
                prepared.videoStart100ns = timeline.video.firstPresentation100ns.value_or(origin);
                if (prepared.videoStart100ns < 0) {
                    throw std::runtime_error("A negative movie video presentation origin is not supported.");
                }
                if (timeline.video.end100ns) {
                    if (*timeline.video.end100ns <= prepared.videoStart100ns) {
                        throw std::runtime_error("The movie edit does not contain a positive video interval.");
                    }
                    sequenceOptions.duration100ns = *timeline.video.end100ns - prepared.videoStart100ns;
                }
            }
            prepared.video = WriteVideoSequence(*decoder, prepared.directory / "video", sequenceOptions);
        } else if (options.alignAudioToVideo) {
            auto decoder = CreateNativeDecoder();
            decoder->Open(source);
            prepared.video.sourceInfo = decoder->Info();
            origin = prepared.video.sourceInfo.presentationOrigin100ns;
        }
        CheckCancelled(options);
        Progress(options, 85);
        if (options.includeAudio) {
            AudioExportOptions audioOptions;
            if (options.alignAudioToVideo &&
                ((timeline.video.offset100ns > 0 && origin > std::numeric_limits<std::int64_t>::max() - timeline.video.offset100ns) ||
                 (timeline.video.offset100ns < 0 && origin < std::numeric_limits<std::int64_t>::min() - timeline.video.offset100ns))) {
                throw std::runtime_error("Video presentation origin overflows the audio time base.");
            }
            audioOptions.timelineOrigin100ns = options.alignAudioToVideo && !options.preserveMovieTimeline ?
                timeline.video.firstPresentation100ns.value_or(origin + timeline.video.offset100ns) : 0;
            audioOptions.continueExport = [&options](std::uint64_t) {
                return !options.continueExport || options.continueExport();
            };
            prepared.audioFile = prepared.directory / AudioFilename(prepared.sourceSha256, options);
            prepared.audio = ExportAudioWave(source, prepared.audioFile, audioOptions);
            if (!prepared.audio.hasAudio) { prepared.audioFile.clear(); }
        }
        verifySourceUnchanged();
        WriteManifest(prepared, options);
        Progress(options, 95);
        CheckCancelled(options);
        std::error_code renameError;
        std::filesystem::rename(prepared.directory, finalDirectory, renameError);
        if (renameError) {
            if (!std::filesystem::is_directory(finalDirectory)) {
                throw std::filesystem::filesystem_error("Cannot commit prepared media", renameError);
            }
            const auto existing = ReadManifest(finalDirectory, prepared.sourceSha256, options);
            verifySourceUnchanged();
            std::filesystem::remove_all(prepared.directory);
            return existing;
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(prepared.directory, ignored);
        throw;
    }
    auto result = ReadManifest(finalDirectory, prepared.sourceSha256, options);
    verifySourceUnchanged();
    result.reused = false;
    Progress(options, 100);
    return result;
}

}
