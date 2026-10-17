#include "mobu/media/Audio.h"
#include "mobu/media/Decoder.h"

#include <windows.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

using Microsoft::WRL::ComPtr;
constexpr DWORD AudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed: 0x" << std::hex << static_cast<unsigned long>(result);
        throw std::runtime_error(message.str());
    }
}

std::string GuidText(const GUID& value)
{
    wchar_t text[40]{};
    StringFromGUID2(value, text, 40);
    std::string result;
    for (const auto* character = text; *character; ++character) {
        result.push_back(static_cast<char>(*character));
    }
    return result;
}

std::string SampleSha256(const BYTE* data, DWORD length)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("Cannot open diagnostic sample digest provider.");
    }
    std::array<UCHAR, 32> digest{};
    const auto result = BCryptHash(algorithm, nullptr, 0, const_cast<BYTE*>(data), length,
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) { throw std::runtime_error("Cannot compute diagnostic sample digest."); }
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const auto value : digest) { text << std::setw(2) << static_cast<unsigned>(value); }
    return text.str();
}

void PrintAttributes(IMFAttributes* attributes)
{
    UINT32 count = 0;
    Check(attributes->GetCount(&count), "Count audio diagnostic attributes");
    std::cout << '[';
    for (UINT32 index = 0; index < count; ++index) {
        GUID key{};
        PROPVARIANT value;
        PropVariantInit(&value);
        Check(attributes->GetItemByIndex(index, &key, &value), "Read audio diagnostic attribute");
        if (index) { std::cout << ','; }
        std::cout << "{\"key\":\"" << GuidText(key) << "\",\"type\":" << value.vt;
        if (value.vt == VT_UI4) { std::cout << ",\"value\":" << value.ulVal; }
        else if (value.vt == VT_UI8) { std::cout << ",\"value\":" << value.uhVal.QuadPart; }
        else if (value.vt == VT_I8) { std::cout << ",\"value\":" << value.hVal.QuadPart; }
        else if (value.vt == VT_CLSID) { std::cout << ",\"value\":\"" << GuidText(*value.puuid) << '"'; }
        else if (value.vt == (VT_VECTOR | VT_UI1)) { std::cout << ",\"bytes\":" << value.caub.cElems; }
        std::cout << '}';
        PropVariantClear(&value);
    }
    std::cout << ']';
}

void InspectAudioReader(const std::filesystem::path& source, bool decode)
{
    ComPtr<IMFSourceReader> reader;
    Check(MFCreateSourceReaderFromURL(source.c_str(), nullptr, &reader), "Open audio diagnostic reader");
    Check(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE), "Disable diagnostic streams");
    Check(reader->SetStreamSelection(AudioStream, TRUE), "Select diagnostic audio stream");
    ComPtr<IMFMediaType> native;
    Check(reader->GetNativeMediaType(AudioStream, 0, &native), "Read diagnostic native audio format");
    UINT32 channels = 0;
    UINT32 rate = 0;
    Check(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels), "Read diagnostic audio channels");
    Check(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate), "Read diagnostic audio rate");
    if (decode) {
        ComPtr<IMFMediaType> output;
        Check(MFCreateMediaType(&output), "Create diagnostic PCM format");
        Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "Set diagnostic audio major type");
        Check(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "Set diagnostic PCM subtype");
        Check(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "Set diagnostic PCM precision");
        Check(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels), "Set diagnostic PCM channels");
        Check(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate), "Set diagnostic PCM rate");
        Check(reader->SetCurrentMediaType(AudioStream, nullptr, output.Get()), "Configure diagnostic PCM reader");
    } else {
        Check(reader->SetCurrentMediaType(AudioStream, nullptr, native.Get()), "Preserve diagnostic compressed audio");
    }
    std::cout << "{\"decoded\":" << (decode ? "true" : "false") << ",\"native_attributes\":";
    PrintAttributes(native.Get());
    std::cout << ",\"presentation_attributes\":[";
    const GUID presentationKeys[] = {MF_SD_AUDIO_ENCODER_DELAY, MF_SD_AUDIO_ENCODER_PADDING};
    for (unsigned index = 0; index < 2; ++index) {
        if (index) { std::cout << ','; }
        PROPVARIANT value;
        PropVariantInit(&value);
        const auto result = reader->GetPresentationAttribute(AudioStream, presentationKeys[index], &value);
        std::cout << "{\"key\":\"" << GuidText(presentationKeys[index]) << "\",\"result\":" << result
                  << ",\"type\":" << value.vt;
        if (value.vt == VT_UI4) { std::cout << ",\"value\":" << value.ulVal; }
        if (value.vt == VT_UI8) { std::cout << ",\"value\":" << value.uhVal.QuadPart; }
        std::cout << '}';
        PropVariantClear(&value);
    }
    std::cout << ']';
    ComPtr<IMFMediaType> current;
    Check(reader->GetCurrentMediaType(AudioStream, &current), "Read diagnostic output format");
    std::cout << ",\"current_attributes\":";
    PrintAttributes(current.Get());
    std::cout << ",\"transforms\":[";
    ComPtr<IMFSourceReaderEx> extended;
    if (SUCCEEDED(reader.As(&extended))) {
        for (DWORD index = 0;; ++index) {
            GUID category{};
            ComPtr<IMFTransform> transform;
            const auto result = extended->GetTransformForStream(AudioStream, index, &category, &transform);
            if (result == MF_E_INVALIDINDEX) { break; }
            Check(result, "Inspect diagnostic audio transforms");
            if (index) { std::cout << ','; }
            std::cout << "{\"category\":\"" << GuidText(category) << '"';
            MFT_INPUT_STREAM_INFO input{};
            if (SUCCEEDED(transform->GetInputStreamInfo(0, &input))) {
                std::cout << ",\"maximum_latency100ns\":" << input.hnsMaxLatency
                          << ",\"lookahead_bytes\":" << input.cbMaxLookahead << ",\"input_flags\":" << input.dwFlags;
            }
            ComPtr<IMFAttributes> attributes;
            if (SUCCEEDED(transform->GetAttributes(&attributes))) {
                std::cout << ",\"attributes\":";
                PrintAttributes(attributes.Get());
            }
            std::cout << '}';
        }
    }
    std::cout << "],\"samples\":[";
    std::uint64_t totalBytes = 0;
    unsigned sampleCount = 0;
    LONGLONG lastTimestamp = 0;
    LONGLONG lastDuration = 0;
    bool finished = false;
    for (unsigned index = 0; index < 5; ++index) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        Check(reader->ReadSample(AudioStream, 0, nullptr, &flags, &timestamp, &sample), "Read diagnostic audio sample");
        if (index) { std::cout << ','; }
        std::cout << "{\"timestamp100ns\":" << timestamp << ",\"flags\":" << flags;
        if (sample) {
            LONGLONG duration = 0;
            sample->GetSampleDuration(&duration);
            DWORD bytes = 0;
            Check(sample->GetTotalLength(&bytes), "Read diagnostic audio sample size");
            totalBytes += bytes;
            ++sampleCount;
            lastTimestamp = timestamp;
            lastDuration = duration;
            std::cout << ",\"duration100ns\":" << duration << ",\"bytes\":" << bytes << ",\"attributes\":";
            PrintAttributes(sample.Get());
            ComPtr<IMFMediaBuffer> buffer;
            Check(sample->ConvertToContiguousBuffer(&buffer), "Read diagnostic sample buffer");
            BYTE* data = nullptr;
            DWORD length = 0;
            Check(buffer->Lock(&data, nullptr, &length), "Lock diagnostic sample buffer");
            std::cout << ",\"sha256\":\"" << SampleSha256(data, length) << '"';
            if (decode) {
                std::cout << ",\"frames\":" << bytes / (channels * 2);
                std::cout << ",\"head_samples\":[";
                const auto values = reinterpret_cast<const std::int16_t*>(data);
                for (DWORD offset = 0; offset < (std::min)(DWORD{32}, length / 2); ++offset) {
                    if (offset) { std::cout << ','; }
                    std::cout << values[offset];
                }
                std::cout << ']';
            }
            Check(buffer->Unlock(), "Unlock diagnostic sample buffer");
        }
        std::cout << '}';
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) { finished = true; break; }
    }
    while (!finished) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        Check(reader->ReadSample(AudioStream, 0, nullptr, &flags, &timestamp, &sample), "Read diagnostic audio tail");
        if (sample) {
            DWORD bytes = 0;
            Check(sample->GetTotalLength(&bytes), "Read diagnostic audio tail size");
            sample->GetSampleDuration(&lastDuration);
            lastTimestamp = timestamp;
            totalBytes += bytes;
            ++sampleCount;
        }
        finished = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (sampleCount > 100000) { throw std::runtime_error("Diagnostic audio sample count exceeded its bound."); }
    }
    std::cout << "],\"sample_count\":" << sampleCount << ",\"total_bytes\":" << totalBytes
              << ",\"total_pcm_frames\":" << (decode ? totalBytes / (channels * 2) : 0)
              << ",\"last_timestamp100ns\":" << lastTimestamp << ",\"last_duration100ns\":" << lastDuration << '}';
}

void InspectAudio(const std::filesystem::path& source)
{
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize diagnostic COM");
    Check(MFStartup(MF_VERSION), "Initialize diagnostic Media Foundation");
    std::cout << "{\"readers\":[";
    InspectAudioReader(source, false);
    std::cout << ',';
    InspectAudioReader(source, true);
    std::cout << "]}\n";
    MFShutdown();
    CoUninitialize();
}

}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect-audio") {
            InspectAudio(argv[2]);
            return EXIT_SUCCESS;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--timeline") {
            const auto timeline = mobu::media::ReadPresentationTimeline(argv[2]);
            std::cout << "{\"audioOffset100ns\":" << timeline.audio.offset100ns
                      << ",\"videoOffset100ns\":" << timeline.video.offset100ns
                      << ",\"audioEnd100ns\":" << timeline.audio.end100ns.value_or(-1)
                      << ",\"videoEnd100ns\":" << timeline.video.end100ns.value_or(-1)
                      << ",\"audioFirst100ns\":" << timeline.audio.firstPresentation100ns.value_or(-1)
                      << ",\"videoFirst100ns\":" << timeline.video.firstPresentation100ns.value_or(-1)
                      << ",\"audioHasEdit\":" << (timeline.audio.hasEdit ? "true" : "false")
                      << ",\"videoHasEdit\":" << (timeline.video.hasEdit ? "true" : "false") << "}\n";
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
