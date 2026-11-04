// Development tool for the audio/video sync check (milestone 4). Shows a square in the top-left
// corner of the primary monitor that turns white while a 1 kHz tone plays, every two seconds.
// tools/check_sync.ps1 then finds each flash and each tone in a saved clip and reports the offset.
//
// The tone is written into a WASAPI stream on the default playback device, and the square is
// flipped when IAudioClock says the device is playing the tone's first sample. So flash and tone
// really happen together at the speaker, whatever the device latency (Bluetooth adds 150+ ms);
// any offset left in the clip is Backtrack's. The flash shows up at the next screen refresh, so
// the picture may trail the sound by up to one frame.
//
// Usage: sync_beacon.exe <seconds>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSquarePixels = 300;
constexpr double kPeriodSeconds = 2.0;
constexpr double kToneSeconds = 0.2;
constexpr double kFirstToneSeconds = 1.0;
constexpr double kToneHz = 1000.0;
constexpr float kToneAmplitude = 0.25f;  // loud enough to detect, not to startle
constexpr REFERENCE_TIME kBufferDuration = 50 * 10'000;  // 50 ms in 100 ns units
constexpr UINT kMsgBright = WM_APP + 1;  // wParam: 1 white, 0 black
constexpr DWORD kPollMs = 1;

std::atomic<bool> g_stop{false};
bool g_white = false;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(g_white ? WHITE_BRUSH : BLACK_BRUSH)));
            EndPaint(window, &paint);
            return 0;
        }
        case kMsgBright:
            g_white = wparam != 0;
            InvalidateRect(window, nullptr, FALSE);
            UpdateWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool is_float_format(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// Plays silence with a tone burst every period and flips the square when the device clock reaches
// each burst. Returns false when the audio device cannot be used. COM must be initialised; every
// COM object here is released before this returns.
bool play_beacon(HWND window, double seconds) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    WAVEFORMATEX* format = nullptr;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    UINT32 buffer_frames = 0;
    UINT64 clock_frequency = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()))) ||
        FAILED(client->GetMixFormat(&format)) ||
        FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, format, nullptr)) ||
        FAILED(client->GetBufferSize(&buffer_frames)) || FAILED(client->GetService(IID_PPV_ARGS(&render))) ||
        FAILED(client->GetService(IID_PPV_ARGS(&clock))) || FAILED(clock->GetFrequency(&clock_frequency))) {
        CoTaskMemFree(format);
        return false;
    }
    const bool is_float = is_float_format(format);
    if (!is_float && format->wBitsPerSample != 16) {
        CoTaskMemFree(format);
        return false;  // shared-mode mix formats are float in practice; anything else is not written
    }
    const UINT32 rate = format->nSamplesPerSec;
    const UINT32 channels = format->nChannels;
    const auto period = static_cast<std::uint64_t>(kPeriodSeconds * rate);
    const auto tone = static_cast<std::uint64_t>(kToneSeconds * rate);
    const auto first = static_cast<std::uint64_t>(kFirstToneSeconds * rate);
    const auto end = static_cast<std::uint64_t>(seconds * rate);

    auto sample_at = [&](std::uint64_t frame) -> float {
        if (frame < first || frame >= end) return 0.0f;
        const std::uint64_t in_period = (frame - first) % period;
        if (in_period >= tone) return 0.0f;
        return kToneAmplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * kToneHz * in_period / rate));
    };

    std::uint64_t written = 0;
    std::uint64_t next_flash = first;
    std::uint64_t flash_off = 0;
    bool bright = false;
    client->Start();
    while (!g_stop) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        const UINT32 free_frames = buffer_frames - padding;
        BYTE* data = nullptr;
        if (free_frames > 0 && SUCCEEDED(render->GetBuffer(free_frames, &data))) {
            for (UINT32 i = 0; i < free_frames; ++i) {
                const float value = sample_at(written + i);
                for (UINT32 c = 0; c < channels; ++c) {
                    if (is_float) {
                        reinterpret_cast<float*>(data)[i * channels + c] = value;
                    } else {
                        reinterpret_cast<std::int16_t*>(data)[i * channels + c] =
                            static_cast<std::int16_t>(value * 32767.0f);
                    }
                }
            }
            render->ReleaseBuffer(free_frames, 0);
            written += free_frames;
        }
        UINT64 position = 0;
        if (SUCCEEDED(clock->GetPosition(&position, nullptr))) {
            const std::uint64_t played = position * rate / clock_frequency;
            if (!bright && played >= next_flash && next_flash < end) {
                bright = true;
                flash_off = next_flash + tone;
                next_flash += period;
                PostMessageW(window, kMsgBright, 1, 0);
            } else if (bright && played >= flash_off) {
                bright = false;
                PostMessageW(window, kMsgBright, 0, 0);
            }
            if (played >= end) {
                break;
            }
        }
        Sleep(kPollMs);
    }
    client->Stop();
    CoTaskMemFree(format);
    return true;
}

bool run_audio(HWND window, double seconds) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return false;
    }
    const bool ok = play_beacon(window, seconds);
    CoUninitialize();
    return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    constexpr double kMinimumSeconds = 1.0;
    const double requested = argc > 1 ? std::wcstod(argv[1], nullptr) : 20.0;
    constexpr double kMaximumSeconds = 86400.0;  // a day; keeps seconds * rate inside uint64
    const double seconds = requested >= kMinimumSeconds ? std::min(requested, kMaximumSeconds) : kMinimumSeconds;  // NaN -> minimum
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"BacktrackSyncBeacon";
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&window_class);
    const HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class.lpszClassName,
                                        L"Backtrack sync beacon", WS_POPUP, 0, 0, kSquarePixels, kSquarePixels,
                                        nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        return 1;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);

    int result = 0;
    std::thread audio([&] {
        if (!run_audio(window, seconds)) {
            result = 2;
        }
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        DispatchMessageW(&msg);
    }
    g_stop = true;
    audio.join();
    timeEndPeriod(1);
    return result;
}
