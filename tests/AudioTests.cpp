#include "mobu/media/Audio.h"
#include "mobu/media/Decoder.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--timeline") {
            const auto timeline = mobu::media::ReadPresentationTimeline(argv[2]);
            std::cout << "{\"audioOffset100ns\":" << timeline.audio.offset100ns
                      << ",\"videoOffset100ns\":" << timeline.video.offset100ns
                      << ",\"audioEnd100ns\":" << timeline.audio.end100ns.value_or(-1)
                      << ",\"videoEnd100ns\":" << timeline.video.end100ns.value_or(-1)
                      << ",\"audioFirst100ns\":" << timeline.audio.firstPresentation100ns.value_or(-1)
                      << ",\"videoFirst100ns\":" << timeline.video.firstPresentation100ns.value_or(-1) << "}\n";
            return EXIT_SUCCESS;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--video-origin") {
            auto decoder = mobu::media::CreateNativeDecoder();
            decoder->Open(argv[2]);
            const auto timeline = mobu::media::ReadPresentationTimeline(argv[2]);
            const auto native = decoder->Info().presentationOrigin100ns;
            std::cout << "{\"nativeVideoOrigin100ns\":" << native
                      << ",\"videoOffset100ns\":" << timeline.video.offset100ns
                      << ",\"movieVideoOrigin100ns\":"
                      << timeline.video.firstPresentation100ns.value_or(native + timeline.video.offset100ns) << "}\n";
            return EXIT_SUCCESS;
        }
        if (argc < 3 || argc > 6) {
            throw std::runtime_error("Expected: audio_tests source output [timelineOrigin100ns] [maxDataBytes] [cancelAtFrame]");
        }
        mobu::media::AudioExportOptions options;
        if (argc > 3) {
            options.timelineOrigin100ns = std::stoll(argv[3]);
        }
        if (argc > 4) {
            options.maxDataBytes = std::stoull(argv[4]);
        }
        if (argc > 5) {
            const auto cancellationFrame = std::stoull(argv[5]);
            options.continueExport = [cancellationFrame](std::uint64_t frames) {
                return frames < cancellationFrame;
            };
        }
        const auto result = mobu::media::ExportAudioWave(argv[1], argv[2], options);
        std::cout << "{\"hasAudio\":" << (result.hasAudio ? "true" : "false")
                  << ",\"precisionReduced\":" << (result.precisionReduced ? "true" : "false")
                  << ",\"sampleRate\":" << result.sampleRate
                  << ",\"channels\":" << result.channels
                  << ",\"bitsPerSample\":" << result.bitsPerSample
                  << ",\"channelMask\":" << result.channelMask
                  << ",\"frameCount\":" << result.frameCount
                  << ",\"firstPresentation100ns\":" << result.firstPresentation100ns
                  << ",\"duration100ns\":" << result.duration100ns << "}\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
