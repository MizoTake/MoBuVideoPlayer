#include "mobu/media/MediaCache.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) { return 2; }
    const std::filesystem::path fixtures(argv[1]);
    const auto root = std::filesystem::path(argv[2]) /
        (L"cache-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    unsigned checks = 0;
    auto expect = [&checks](bool condition, const char* name) {
        if (!condition) { throw std::runtime_error(name); }
        ++checks;
        std::cout << "PASS " << name << '\n';
    };
    auto rejects = [&expect](auto operation, const char* name) {
        bool rejected = false;
        try { operation(); } catch (const std::exception&) { rejected = true; }
        expect(rejected, name);
    };
    try {
        mobu::media::MediaCacheOptions video;
        video.includeAudio = false;
        const auto prepared = mobu::media::PrepareMediaCache(fixtures / "cfr.mp4", root, video);
        std::cout << "prepared: reused=" << prepared.reused << " frames=" << prepared.video.frameCount << '\n';
        expect(!prepared.reused && prepared.video.frameCount == 12, "first preparation has twelve source frames");
        expect(std::filesystem::is_regular_file(prepared.video.firstFrame), "first PNG frame is available");
        expect(prepared.audioFile.empty() && !prepared.audio.hasAudio, "video-only preparation creates no WAV");
        const auto again = mobu::media::PrepareMediaCache(fixtures / "cfr.mp4", root, video);
        expect(again.reused && again.directory == prepared.directory, "unchanged content reuses validated media");
        const auto copy = root / L"同じ動画.mp4";
        std::filesystem::copy_file(fixtures / "cfr.mp4", copy);
        const auto renamed = mobu::media::PrepareMediaCache(copy, root, video);
        expect(renamed.reused && renamed.directory == prepared.directory, "source filename does not affect content identity");
        const auto mutating = root / "mutating.mp4";
        std::filesystem::copy_file(fixtures / "cfr.mp4", mutating);
        unsigned validationCalls = 0;
        auto concurrentChange = video;
        concurrentChange.continueExport = [&] {
            if (++validationCalls == 2) {
                std::ofstream append(mutating, std::ios::binary | std::ios::app);
                append.put('\0');
            }
            return true;
        };
        rejects([&] { mobu::media::PrepareMediaCache(mutating, root, concurrentChange); },
            "a source changed during cache validation is rejected");
        {
            std::ofstream changed(copy, std::ios::binary | std::ios::app);
            changed.put('\0');
        }
        const auto changed = mobu::media::PrepareMediaCache(copy, root, video);
        expect(changed.directory != prepared.directory, "source content changes create a distinct media cache");
        {
            std::ofstream damaged(prepared.video.firstFrame, std::ios::binary | std::ios::app);
            damaged.put('\0');
        }
        rejects([&] { mobu::media::PrepareMediaCache(fixtures / "cfr.mp4", root, video); },
            "damaged cached output is rejected rather than reused");
        mobu::media::MediaCacheOptions audio;
        audio.videoSequence = false;
        const auto withAudio = mobu::media::PrepareMediaCache(fixtures / "audio-tail.mp4", root, audio);
        expect(withAudio.audio.hasAudio && withAudio.audio.frameCount > 0 &&
            std::filesystem::is_regular_file(withAudio.audioFile), "video-aligned audio prepares a standard WAV");
        expect(withAudio.video.frameCount == 0, "audio-only cache contains no PNG sequence");
        const auto silent = mobu::media::PrepareMediaCache(fixtures / "cfr.mp4", root, audio);
        expect(!silent.audio.hasAudio && silent.audioFile.empty(), "silent video reports no audio without an empty WAV");
        unsigned cancellationCalls = 0;
        auto cancelled = video;
        cancelled.continueExport = [&] { return ++cancellationCalls <= 3; };
        rejects([&] { mobu::media::PrepareMediaCache(fixtures / "vfr.mp4", root, cancelled); },
            "cancelled media preparation does not commit a cache");
        expect(std::none_of(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator(),
            [](const auto& item) { return item.path().filename().wstring().find(L".partial-") != std::wstring::npos; }),
            "cancelled transactions leave no partial directory");
        expect(std::filesystem::is_regular_file(copy), "cleanup preserves existing source files");
        mobu::media::MediaCacheOptions movie;
        movie.preserveMovieTimeline = true;
        const auto tail = mobu::media::PrepareMediaCache(fixtures / "audio-tail.mp4", root, movie);
        expect(tail.video.duration100ns == 20'000'000 && tail.video.frameCount == 12,
            "Story video ends at its own edit boundary before the audio tail");
        expect(tail.audio.duration100ns == 40'000'000, "Story audio retains the full audio tail");
        const auto delayedVideo = mobu::media::PrepareMediaCache(fixtures / "offset.mp4", root, movie);
        expect(delayedVideo.videoStart100ns == 10'000'000 && delayedVideo.video.duration100ns == 20'000'000,
            "Story keeps an initial empty video edit as a placement offset");
        if (argc == 4) {
            const auto av = mobu::media::PrepareMediaCache(std::filesystem::path(argv[3]) / "video-delayed.mp4", root, movie);
            expect(av.videoStart100ns == 5'000'000 && av.video.duration100ns == 10'000'000,
                "delayed B-frame video is placed half a second after movie zero");
            expect(av.audio.frameCount == 48000 && av.audio.duration100ns == 10'000'000,
                "Story preserves the audio before the first video frame");
        }
        std::cout << "PASS " << checks << " cache checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
