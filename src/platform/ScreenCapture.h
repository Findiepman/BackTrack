#pragma once

// Records one monitor at a fixed frame rate and hands out NV12 frames on the GPU.
//
// Threads:
//   - The constructor (any thread, after MFStartup) creates the D3D11 device, the DXGI device
//     manager and the sample allocator. The device is multithread protected because the encoder
//     uses it from Media Foundation's own threads (§3.4).
//   - start() spawns the capture thread. It alone touches the duplication, the video processor,
//     the BGRA frame texture and the mouse pointer overlay, and it calls FrameCallback and
//     EventCallback.
//   - stop() and the destructor join the capture thread. Samples handed out may still be held
//     by the receiver; they return to the allocator when released, so release them before
//     destroying ScreenCapture.
//   - After Failed, start() may be called again (from the owner's thread). A Failed caused by a lost
//     or changed graphics device cannot be fixed that way: the owner builds a new ScreenCapture
//     (and a new encoder on its device).

#include "core/Clock.h"
#include "platform/Com.h"
#include "platform/CursorOverlay.h"
#include "platform/Log.h"

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bt {

struct VideoSize {
    int width = 0;
    int height = 0;
    bool operator==(const VideoSize&) const = default;
};

struct MonitorInfo {
    std::wstring device_name;  // "\\.\DISPLAY1", stable across duplication losses
    std::string adapter_name;
    VideoSize desktop_size;
    bool primary = false;
    LUID adapter_luid{};
};

// All monitors attached to the desktop, primary first, then in DXGI order. Index 0 of this list
// is what "monitor = 0" means in the ini.
std::vector<MonitorInfo> list_monitors();

struct CaptureStats {
    std::uint64_t ticks = 0;  // frames delivered or dropped
    std::uint64_t new_frames = 0;  // desktop changed since the previous tick
    std::uint64_t repeated_frames = 0;  // AcquireNextFrame timed out, previous image re-encoded (§3.2)
    std::uint64_t allocator_empty = 0;  // encoder behind, frame dropped (§3.5)
    std::uint64_t convert_failed = 0;  // BGRA to NV12 conversion failed, frame dropped
    std::uint64_t late_ticks_skipped = 0;  // the thread woke up more than a frame late
    std::uint64_t access_lost = 0;  // duplication lost and re-created (§3.7)
    std::uint64_t delivered = 0;  // frames handed to FrameCallback
};

struct CaptureOptions {
    int monitor = 0;  // index into list_monitors(); one that does not exist falls back to 0
    int fps = 60;
    int resolution = 0;  // Settings::resolution: 0 = the display's own size, else scaled_output_size()
    bool cursor = true;  // draw the mouse pointer into the frames
};

enum class CaptureEvent {
    FormatChanged,  // the first frame: from now on frame_size() is known. The size never changes
                    // afterwards; a later resolution change is scaled into it.
    Lost,  // duplication lost; the last good frame keeps being delivered
    Recovered,  // duplication re-created
    Failed,  // the capture thread hit a fatal error and stopped
};

class ScreenCapture {
public:
    // nv12_sample has its sample time and duration set. It comes from the allocator; holding on
    // to it keeps the texture out of the pool until released.
    using FrameCallback = std::function<void(IMFSample* nv12_sample, Time100ns pts)>;
    using EventCallback = std::function<void(CaptureEvent event, VideoSize size)>;

    ScreenCapture(const CaptureOptions& options, FrameCallback on_frame, EventCallback on_event);
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Must not be called from FrameCallback or EventCallback (it would join its own thread).
    // clock_offset is subtracted from every timestamp: the owner's recording clock stands still
    // while paused, so frames after a resume continue where the ones before the pause ended.
    void start(Time100ns clock_offset = 0);
    void stop();

    ID3D11Device* device() const { return device_.Get(); }
    IMFDXGIDeviceManager* device_manager() const { return device_manager_.Get(); }
    const MonitorInfo& monitor() const { return monitor_; }
    int fps() const { return fps_; }
    // The size of the NV12 frames: the display's size, or smaller for the resolution setting.
    VideoSize frame_size() const;
    CaptureStats stats() const;

    // Capture thread only (inside FrameCallback). Copies a frame to system memory: tightly
    // packed NV12 (Y plane then interleaved UV), for the software encoder, which needs system
    // memory input (§3.4).
    std::vector<std::uint8_t> read_nv12(IMFSample* nv12_sample);


    // Test hook: makes the next tick behave as if DXGI returned DXGI_ERROR_ACCESS_LOST.
    void simulate_access_lost() { simulate_lost_ = true; }

private:
    struct SourceFormat {
        UINT width = 0;
        UINT height = 0;
        DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY;
        bool operator==(const SourceFormat&) const = default;
    };

    struct OutputView {
        ComPtr<ID3D11Texture2D> texture;  // keeps the pointer valid as a cache key
        UINT subresource = 0;
        ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    struct Counters {
        std::atomic<std::uint64_t> ticks{0}, new_frames{0}, repeated_frames{0}, allocator_empty{0}, convert_failed{0},
            late_ticks_skipped{0}, access_lost{0}, delivered{0};
    };

    void create_device();
    void thread_main();
    void capture_loop();
    bool try_open_duplication();
    struct OutputLookup {
        ComPtr<IDXGIOutput> output;  // our monitor on our adapter
        std::string other_adapter;  // set when the monitor is only listed on another adapter
        std::string problem;  // set when DXGI could not be asked at all
    };
    OutputLookup find_output();
    void acquire_frame();
    void lose_duplication(HRESULT hr);
    void rebuild_pipeline(const SourceFormat& format);
    void deliver(Time100ns pts, Time100ns duration);
    bool draw_cursor();
    HRESULT output_view_for(ID3D11Texture2D* texture, UINT subresource, ID3D11VideoProcessorOutputView** view);
    void notify(CaptureEvent event);
    void log_counters();
    void check_monitor_assignment();

    const int fps_;
    const int resolution_;
    FrameCallback on_frame_;
    EventCallback on_event_;
    MonitorInfo monitor_;
    int requested_index_ = 0;  // the monitor setting, kept to re-resolve it
    bool fell_back_ = false;  // recording monitor 0 because the configured one was missing at startup
    std::set<std::wstring> known_names_;  // monitors that existed when recording started

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoContext1> video_context1_;  // null before Windows 10; colour space fallback
    ComPtr<IDXGIAdapter1> adapter_;
    UINT reset_token_ = 0;
    ComPtr<IMFDXGIDeviceManager> device_manager_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;

    // Capture thread state.
    ComPtr<IDXGIOutputDuplication> duplication_;
    SourceFormat source_{};  // what the pipeline is built for (the real texture size)
    SourceFormat opened_mode_{};  // the duplication mode the pipeline was last built from
    Time100ns persistent_failure_since_ = 0;  // 0 = no persistent failure in progress
    bool pipeline_built_ = false;
    bool have_frame_ = false;
    bool lost_ = false;
    bool open_failure_logged_ = false;
    bool allocator_ready_ = false;  // set up once, for the size that never changes
    Time100ns monitor_missing_since_ = 0;  // 0 = the monitor is there
    Time100ns next_monitor_check_ = 0;
    Time100ns next_retry_ = 0;
    Time100ns retry_delay_ = 0;
    Time100ns next_counter_log_ = 0;
    ComPtr<ID3D11Texture2D> bgra_frame_;
    ComPtr<ID3D11VideoProcessorEnumerator> processor_enum_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorInputView> input_view_;
    std::vector<OutputView> output_views_;
    // Null when the pointer is not drawn (setting off, or the overlay could not be set up).
    std::unique_ptr<CursorOverlay> cursor_;
    ComPtr<ID3D11VideoProcessorInputView> cursor_input_view_;  // reads the overlay's composite
    bool frame_changed_ = false;  // bgra_frame_ got a desktop image the overlay has not drawn on yet
    log::RateLimit cursor_error_log_{std::chrono::seconds(10)};
    log::RateLimit convert_error_log_{std::chrono::seconds(10)};
    ComPtr<ID3D11Texture2D> nv12_staging_;

    mutable std::mutex size_mutex_;
    VideoSize output_size_;
    VideoSize sized_for_;  // the desktop size output_size_ was chosen for

    Counters counters_;
    std::atomic<bool> simulate_lost_{false};
    std::atomic<bool> running_{false};  // capture thread alive; false once it has ended on its own
    Time100ns clock_offset_ = 0;  // written by start() before the thread exists
    UniqueHandle stop_event_;
    std::thread thread_;
};

}  // namespace bt
