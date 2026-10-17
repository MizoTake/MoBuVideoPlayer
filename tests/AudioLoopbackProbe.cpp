#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {
void Check(HRESULT result, const char* action) {
    if (FAILED(result)) { throw std::runtime_error(std::string(action) + " HRESULT=" + std::to_string(result)); }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2 || argc > 4) { return 2; }
        const double seconds = argc >= 3 ? std::stod(argv[2]) : 2.0;
        const double frequency = argc == 4 ? std::stod(argv[3]) : 440.0;
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)), "Device enumeration");
        ComPtr<IMMDevice> device;
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Default render device");
        ComPtr<IAudioClient> client;
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())), "Audio client activation");
        WAVEFORMATEX* format = nullptr;
        Check(client->GetMixFormat(&format), "Device mix format");
        const auto releaseFormat = std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>(format, CoTaskMemFree);
        bool floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format);
            floating = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            if (!floating && !IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
                throw std::runtime_error("Unsupported loopback mix encoding.");
            }
        } else if (!floating && format->wFormatTag != WAVE_FORMAT_PCM) {
            throw std::runtime_error("Unsupported loopback mix encoding.");
        }
        if (!format->nChannels || (floating && format->wBitsPerSample != 32) ||
            (!floating && format->wBitsPerSample != 16 && format->wBitsPerSample != 32)) {
            throw std::runtime_error("Unsupported loopback sample width.");
        }
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
            10000000, 0, format, nullptr), "Loopback initialization");
        ComPtr<IAudioCaptureClient> capture;
        Check(client->GetService(IID_PPV_ARGS(&capture)), "Loopback capture service");
        Check(client->Start(), "Loopback start");
        {
            std::ofstream ready(std::filesystem::path(std::wstring(argv[1]) + L".ready"), std::ios::binary);
            ready << "ready\n";
            if (!ready) { throw std::runtime_error("Cannot write the loopback readiness marker."); }
        }
        std::cout << "READY\n" << std::flush;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        double squares = 0, peak = 0, sine = 0, cosine = 0;
        std::uint64_t frames = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            UINT32 packetFrames = 0;
            Check(capture->GetNextPacketSize(&packetFrames), "Loopback packet size");
            while (packetFrames) {
                BYTE* bytes = nullptr;
                UINT32 count = 0;
                DWORD flags = 0;
                Check(capture->GetBuffer(&bytes, &count, &flags, nullptr, nullptr), "Loopback buffer");
                for (UINT32 frame = 0; frame < count; ++frame) {
                    double mono = 0;
                    for (unsigned channel = 0; channel < format->nChannels; ++channel) {
                        double value = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                            const auto* sample = bytes + frame * format->nBlockAlign +
                                channel * (format->wBitsPerSample / 8);
                            if (floating) {
                                float data;
                                std::memcpy(&data, sample, sizeof(data));
                                value = data;
                            } else if (format->wBitsPerSample == 16) {
                                std::int16_t data;
                                std::memcpy(&data, sample, sizeof(data));
                                value = data / 32768.0;
                            } else {
                                std::int32_t data;
                                std::memcpy(&data, sample, sizeof(data));
                                value = data / 2147483648.0;
                            }
                        }
                        mono += value / format->nChannels;
                    }
                    squares += mono * mono;
                    peak = (std::max)(peak, std::abs(mono));
                    const double angle = 2 * std::numbers::pi * frequency * frames / format->nSamplesPerSec;
                    sine += mono * std::sin(angle);
                    cosine += mono * std::cos(angle);
                    ++frames;
                }
                Check(capture->ReleaseBuffer(count), "Loopback buffer release");
                Check(capture->GetNextPacketSize(&packetFrames), "Next loopback packet");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(client->Stop(), "Loopback stop");
        std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary);
        output << "{\"frames\":" << frames << ",\"sampleRate\":" << format->nSamplesPerSec
            << ",\"channels\":" << format->nChannels << ",\"rms\":"
            << (frames ? std::sqrt(squares / frames) : 0) << ",\"peak\":" << peak
            << ",\"toneFrequency\":" << frequency
            << ",\"toneAmplitude\":" << (frames ? 2 * std::hypot(sine, cosine) / frames : 0)
            << ",\"tone440\":" << (frames && frequency == 440 ? 2 * std::hypot(sine, cosine) / frames : 0) << "}\n";
        if (!output) { throw std::runtime_error("Cannot write loopback result."); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
