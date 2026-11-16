#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <complex>
#include <mutex>
#include <thread>

struct RoomAudioFrame
{
    float bass = 0, mid = 0, high = 0, level = 0, beat = 0;
    std::array<float, 16> spectrum{};
    std::array<float, 48> waveform{};
};

// Playback loopback: no microphone, recordings or files. The worker owns COM and
// releases capture while disabled. Reopening also follows default-device changes.
class RoomAudio
{
    std::atomic<bool> enabled{false}, quitting{false};
    std::atomic<int> status{0};
    std::mutex mutex;
    RoomAudioFrame frame;
    std::thread worker;
    void Run()
    {
        using Microsoft::WRL::ComPtr;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::array<float, 1024> samples{};
        size_t at = 0;
        float average = .01f, envelope = 0;
        while (!quitting)
        {
            if (!enabled) { Publish({}); Sleep(20); continue; }
            status=1;
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioCaptureClient> capture;
            WAVEFORMATEX* format = nullptr;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
            if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
            bool floating = format && (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
            if (SUCCEEDED(hr) && (!format->nChannels || (!floating && format->wBitsPerSample != 16 && format->wBitsPerSample != 32))) hr = E_FAIL;
            if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, format, nullptr);
            if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
            if (SUCCEEDED(hr)) hr = client->Start();
            status=SUCCEEDED(hr)?2:-1;
            at = 0;
            auto lastPacket = std::chrono::steady_clock::now(), checkDevice = lastPacket;
            while (SUCCEEDED(hr) && enabled && !quitting)
            {
                UINT32 available = 0;
                hr = capture->GetNextPacketSize(&available);
                while (SUCCEEDED(hr) && available)
                {
                    BYTE* data = nullptr; UINT32 count = 0; DWORD flags = 0;
                    hr = capture->GetBuffer(&data, &count, &flags, nullptr, nullptr);
                    if (FAILED(hr)) break;
                    if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) at = 0;
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        float value = 0;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                            for (int c = 0; c < format->nChannels; ++c)
                            {
                                size_t j = (size_t)i * format->nChannels + c;
                                value += floating ? reinterpret_cast<float*>(data)[j] : format->wBitsPerSample == 16 ?
                                    reinterpret_cast<short*>(data)[j] / 32768.f : reinterpret_cast<int*>(data)[j] / 2147483648.f;
                            }
                        samples[at++] = std::isfinite(value) ? value / format->nChannels : 0;
                        if (at == samples.size())
                        {
                            RoomAudioFrame f = Analyse(samples, format->nSamplesPerSec);
                            average += .02f * (f.bass - average);
                            envelope = std::max(envelope * .86f, f.bass > average * 1.45f && f.level > .008f ? 1.f : 0.f);
                            f.beat = envelope; Publish(f); at = 0;
                        }
                    }
                    capture->ReleaseBuffer(count);
                    lastPacket = std::chrono::steady_clock::now();
                    hr = capture->GetNextPacketSize(&available);
                }
                auto now = std::chrono::steady_clock::now();
                if (now - lastPacket > std::chrono::milliseconds(150)) { Publish({}); at = 0; envelope = 0; }
                if (now - checkDevice > std::chrono::seconds(1))
                {
                    ComPtr<IMMDevice> next; LPWSTR a = nullptr, b = nullptr;
                    bool changed = FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &next));
                    if (!changed && SUCCEEDED(device->GetId(&a)) && SUCCEEDED(next->GetId(&b))) changed = wcscmp(a, b) != 0;
                    CoTaskMemFree(a); CoTaskMemFree(b); checkDevice = now;
                    if (changed) break;
                }
                Sleep(5);
            }
            if (client) client->Stop();
            CoTaskMemFree(format); Publish({});
            for (int i = 0; i < 20 && enabled && !quitting; ++i) Sleep(10);
        }
        CoUninitialize();
    }
    void Publish(const RoomAudioFrame& f) { std::lock_guard<std::mutex> lock(mutex); frame = f; }
public:
    RoomAudio() : worker([this] { Run(); }) {}
    ~RoomAudio() { quitting = true; worker.join(); }
    RoomAudio(const RoomAudio&) = delete;
    void Enable(bool on) { enabled = on; if(!on) status=0; }
    int Status() const { return status.load(); }
    RoomAudioFrame Read() { std::lock_guard<std::mutex> lock(mutex); return enabled ? frame : RoomAudioFrame{}; }
    static RoomAudioFrame Analyse(const std::array<float, 1024>& samples, unsigned rate)
    {
        RoomAudioFrame f;
        std::array<std::complex<float>, 1024> bins;
        float energy = 0;
        for (int i = 0; i < 1024; ++i) { energy += samples[i] * samples[i]; bins[i] = samples[i] * (.5f - .5f * cosf(6.2831853f * i / 1023)); }
        f.level = std::min(1.f, sqrtf(energy / 1024) * 4);
        for(int i=0;i<48;++i) f.waveform[i]=std::clamp(samples[i*1023/47]*3.f,-1.f,1.f);
        for (unsigned i = 1, j = 0; i < 1024; ++i)
        {
            unsigned bit = 512;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit; if (i < j) std::swap(bins[i], bins[j]);
        }
        for (int n = 2; n <= 1024; n *= 2)
        {
            auto step = std::polar(1.f, -6.2831853f / n);
            for (int i = 0; i < 1024; i += n)
            {
                std::complex<float> w(1, 0);
                for (int j = 0; j < n / 2; ++j) { auto a = bins[i+j], b = w * bins[i+j+n/2]; bins[i+j] = a+b; bins[i+j+n/2] = a-b; w *= step; }
            }
        }
        for (int b = 0; b < 16; ++b)
        {
            float lo = 35 * powf(16000.f / 35, b / 16.f), hi = 35 * powf(16000.f / 35, (b+1) / 16.f), power = 0;
            for (int i = 1; i < 512; ++i) if (i * rate / 1024.f >= lo && i * rate / 1024.f < hi) power += std::norm(bins[i]);
            f.spectrum[b] = std::min(1.f, sqrtf(power) / 70);
            if (b < 5) f.bass = std::max(f.bass, f.spectrum[b]);
            else if (b < 11) f.mid = std::max(f.mid, f.spectrum[b]);
            else f.high = std::max(f.high, f.spectrum[b]);
        }
        return f;
    }
};
