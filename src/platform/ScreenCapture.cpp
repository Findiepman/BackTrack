#include "platform/ScreenCapture.h"

#include "platform/QpcClock.h"

#include <avrt.h>
#include <d3d10.h>
#include <mferror.h>
#include <timeapi.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bt {
namespace {

// §3.7 retry schedule. Most losses (UAC, Ctrl+Alt+Del, alt-tab out of fullscreen) are over within
// a second, so the first retry is quick; a sleeping monitor is then polled once a second.
constexpr Time100ns kRetryInitialDelay = 100 * kTicksPerMillisecond;
constexpr Time100ns kRetryMaxDelay = 1000 * kTicksPerMillisecond;

// §3.5 NV12 sample pool. The hardware encoder keeps a few frames in flight and the hand-off to
// it buffers a few more; ten leaves headroom without wasting VRAM (one 1440p NV12 frame is 5.5 MB).
constexpr DWORD kAllocatorInitialSamples = 4;
constexpr DWORD kAllocatorMaxSamples = 10;

// More views than pool textures means the allocator replaced its textures; drop the stale ones.
constexpr std::size_t kMaxCachedOutputViews = kAllocatorMaxSamples * 2;

constexpr UINT kTimerResolutionMs = 1;

// §9: counters go to the log once a minute at debug level.
constexpr Time100ns kCounterLogInterval = 60 * kTicksPerSecond;

// After a driver crash (TDR) every call fails with one of these until the device is recreated,
// so treating them as anything softer would look like recording while nothing is recorded.
bool is_device_lost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// Failures that do not pass by waiting (desktop mode Desktop Duplication cannot handle, a format
// we cannot convert). Retried for a while in case they are a mode switch in progress, then fatal
// so the owner sees Failed instead of a frozen recording (§9).
constexpr Time100ns kPersistentFailureTimeout = 10 * kTicksPerSecond;

bool same_luid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// §3.2: a fixed tick needs a precise wait. Sleep(16) rounds to the 15.6 ms system tick and
// gives 58 or 64 fps depending on the machine.
class TickTimer {
public:
    explicit TickTimer(HANDLE stop_event) : stop_event_(stop_event) {
        timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
        if (!timer_) {
            // High resolution waitable timers need Windows 10 1803.
            log::info("High resolution timer unavailable (error {}), falling back to timeBeginPeriod(1) + Sleep",
                      GetLastError());
            raised_period_ = timeBeginPeriod(kTimerResolutionMs) == TIMERR_NOERROR;
        }
    }
    ~TickTimer() {
        if (raised_period_) {
            timeEndPeriod(kTimerResolutionMs);
        }
    }
    TickTimer(const TickTimer&) = delete;
    TickTimer& operator=(const TickTimer&) = delete;

    // Waits until the QPC time `deadline`. Returns false as soon as stop is signalled.
    bool wait_until(Time100ns deadline) {
        while (true) {
            const Time100ns remaining = deadline - qpc_now();
            if (remaining <= 0) {
                return WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0;
            }
            DWORD result = 0;
            if (timer_) {
                LARGE_INTEGER due{};
                due.QuadPart = -remaining;  // negative: relative time in 100 ns units
                BT_CHECK_WIN32(SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE));
                const HANDLE handles[] = {stop_event_, timer_.get()};
                result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            } else {
                const auto ms = static_cast<DWORD>((remaining + kTicksPerMillisecond - 1) / kTicksPerMillisecond);
                result = WaitForSingleObject(stop_event_, ms);
            }
            BT_CHECK_WIN32(result != WAIT_FAILED);
            if (result == WAIT_OBJECT_0) {
                return false;
            }
        }
    }

private:
    HANDLE stop_event_;
    UniqueHandle timer_;
    bool raised_period_ = false;
};

// A game keeps every core busy. The MMCSS "Capture" class keeps the tick thread on schedule
// without admin rights; above-normal priority is the fallback.
class CaptureThreadPriority {
public:
    CaptureThreadPriority() {
        DWORD task_index = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Capture", &task_index);
        if (handle_ == nullptr) {
            log::info("MMCSS unavailable (error {}), using above normal thread priority", GetLastError());
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }
    ~CaptureThreadPriority() {
        if (handle_ != nullptr) {
            AvRevertMmThreadCharacteristics(handle_);
        }
    }
    CaptureThreadPriority(const CaptureThreadPriority&) = delete;
    CaptureThreadPriority& operator=(const CaptureThreadPriority&) = delete;

private:
    HANDLE handle_ = nullptr;
};

// UNVERIFIED on real hardware: the test machine has no rotated monitor. The mapping follows the
// Desktop Duplication sample, where ROTATE90 means the image must be turned 90 degrees clockwise.
D3D11_VIDEO_PROCESSOR_ROTATION processor_rotation(DXGI_MODE_ROTATION rotation) {
    switch (rotation) {
        case DXGI_MODE_ROTATION_ROTATE90: return D3D11_VIDEO_PROCESSOR_ROTATION_90;
        case DXGI_MODE_ROTATION_ROTATE180: return D3D11_VIDEO_PROCESSOR_ROTATION_180;
        case DXGI_MODE_ROTATION_ROTATE270: return D3D11_VIDEO_PROCESSOR_ROTATION_270;
        default: return D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    }
}

bool is_sideways(DXGI_MODE_ROTATION rotation) {
    return rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270;
}

void sample_texture(IMFSample* sample, ComPtr<ID3D11Texture2D>& texture, UINT& subresource) {
    ComPtr<IMFMediaBuffer> buffer;
    BT_CHECK_HR(sample->GetBufferByIndex(0, &buffer));
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    BT_CHECK_HR(buffer.As(&dxgi_buffer));
    BT_CHECK_HR(dxgi_buffer->GetResource(IID_PPV_ARGS(&texture)));
    BT_CHECK_HR(dxgi_buffer->GetSubresourceIndex(&subresource));
}

}  // namespace

std::vector<MonitorInfo> list_monitors() {
    ComPtr<IDXGIFactory1> factory;
    BT_CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    std::vector<MonitorInfo> monitors;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_hr = factory->EnumAdapters1(a, &adapter);
        if (adapter_hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        BT_CHECK_HR(adapter_hr);
        DXGI_ADAPTER_DESC1 adapter_desc{};
        BT_CHECK_HR(adapter->GetDesc1(&adapter_desc));
        if (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;  // Microsoft Basic Render Driver: no real outputs
        }
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_hr = adapter->EnumOutputs(o, &output);
            if (output_hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            BT_CHECK_HR(output_hr);
            DXGI_OUTPUT_DESC desc{};
            BT_CHECK_HR(output->GetDesc(&desc));
            if (!desc.AttachedToDesktop) {
                continue;
            }
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            const bool primary = GetMonitorInfoW(desc.Monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY);
            const RECT& r = desc.DesktopCoordinates;
            monitors.push_back(MonitorInfo{desc.DeviceName, to_utf8(adapter_desc.Description),
                                           VideoSize{r.right - r.left, r.bottom - r.top}, primary,
                                           adapter_desc.AdapterLuid});
        }
    }
    std::stable_partition(monitors.begin(), monitors.end(), [](const MonitorInfo& m) { return m.primary; });
    return monitors;
}

ScreenCapture::ScreenCapture(int monitor_index, int fps, FrameCallback on_frame, EventCallback on_event)
    : fps_(fps), on_frame_(std::move(on_frame)), on_event_(std::move(on_event)) {
    const std::vector<MonitorInfo> monitors = list_monitors();
    if (monitors.empty()) {
        throw std::runtime_error("No monitor is attached to the desktop, there is nothing to record");
    }
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        const MonitorInfo& m = monitors[i];
        log::info("Monitor {}: {} {}x{}{} on {}", i, to_utf8(m.device_name), m.desktop_size.width,
                  m.desktop_size.height, m.primary ? " (primary)" : "", m.adapter_name);
    }
    if (monitor_index < 0 || static_cast<std::size_t>(monitor_index) >= monitors.size()) {
        log::warn("monitor = {} but only {} monitor(s) found, recording monitor 0", monitor_index, monitors.size());
        monitor_index = 0;
    }
    monitor_ = monitors[static_cast<std::size_t>(monitor_index)];
    log::info("Recording monitor {} ({}) at {} fps", monitor_index, to_utf8(monitor_.device_name), fps_);

    stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    BT_CHECK_WIN32(static_cast<bool>(stop_event_));
    create_device();
}

ScreenCapture::~ScreenCapture() {
    stop();
    output_views_.clear();
    if (allocator_) {
        const HRESULT hr = allocator_->UninitializeSampleAllocator();
        if (FAILED(hr)) {
            log::warn("UninitializeSampleAllocator failed: {}", hresult_text(hr));
        }
    }
}

void ScreenCapture::create_device() {
    ComPtr<IDXGIFactory1> factory;
    BT_CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        BT_CHECK_HR(hr);
        DXGI_ADAPTER_DESC1 desc{};
        BT_CHECK_HR(adapter->GetDesc1(&desc));
        if (same_luid(desc.AdapterLuid, monitor_.adapter_luid)) {
            adapter_ = adapter;
            break;
        }
    }
    if (!adapter_) {
        throw std::runtime_error("The graphics adapter of the selected monitor disappeared during startup");
    }

    // The device lives on the adapter that drives the monitor: Desktop Duplication only works
    // there. VIDEO_SUPPORT is needed for the video processor and the encoder (§3.4).
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    BT_CHECK_HR(D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                  static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device_, nullptr,
                                  &context_));

    // §3.4: the hardware encoder calls into this device from Media Foundation's threads; without
    // this it corrupts the immediate context state.
    ComPtr<ID3D10Multithread> multithread;
    BT_CHECK_HR(device_.As(&multithread));
    multithread->SetMultithreadProtected(TRUE);

    BT_CHECK_HR(device_.As(&video_device_));
    BT_CHECK_HR(context_.As(&video_context_));
    if (FAILED(context_.As(&video_context1_))) {
        log::info("ID3D11VideoContext1 unavailable, using the legacy colour space API only");
    }

    BT_CHECK_HR(MFCreateDXGIDeviceManager(&reset_token_, &device_manager_));
    BT_CHECK_HR(device_manager_->ResetDevice(device_.Get(), reset_token_));
    BT_CHECK_HR(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&allocator_)));
    BT_CHECK_HR(allocator_->SetDirectXManager(device_manager_.Get()));
}

void ScreenCapture::start() {
    if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
        // Joining ourselves would deadlock; the owner must restart from its own thread.
        throw std::logic_error("ScreenCapture::start() called from a capture callback");
    }
    if (thread_.joinable()) {
        if (running_) {
            return;
        }
        thread_.join();  // a previous run ended on its own (Failed); restart cleanly
    }
    BT_CHECK_WIN32(ResetEvent(stop_event_.get()));
    running_ = true;
    thread_ = std::thread(&ScreenCapture::thread_main, this);
}

void ScreenCapture::stop() {
    if (!thread_.joinable()) {
        return;
    }
    SetEvent(stop_event_.get());
    thread_.join();
}

VideoSize ScreenCapture::frame_size() const {
    std::lock_guard lock(size_mutex_);
    return output_size_;
}

CaptureStats ScreenCapture::stats() const {
    CaptureStats s;
    s.ticks = counters_.ticks;
    s.new_frames = counters_.new_frames;
    s.repeated_frames = counters_.repeated_frames;
    s.allocator_empty = counters_.allocator_empty;
    s.convert_failed = counters_.convert_failed;
    s.late_ticks_skipped = counters_.late_ticks_skipped;
    s.access_lost = counters_.access_lost;
    s.delivered = counters_.delivered;
    return s;
}

void ScreenCapture::thread_main() {
    // §9: a thread never dies silently. The owner gets Failed and shuts the pipeline down.
    bool failed = false;
    try {
        ComApartment com(COINIT_MULTITHREADED);
        CaptureThreadPriority priority;
        capture_loop();
    } catch (const std::exception& e) {
        log::error("Capture stopped: {}", e.what());
        failed = true;
    } catch (...) {
        log::error("Capture stopped: unknown exception");
        failed = true;
    }
    // Release the duplication so a stopped capture does not hold one of the few duplication
    // slots Windows hands out, and so a restart opens a fresh one.
    duplication_.Reset();
    running_ = false;
    if (failed) {
        notify(CaptureEvent::Failed);
    }
}

void ScreenCapture::capture_loop() {
    TickTimer timer(stop_event_.get());
    // A restart after stop() or Failed begins clean: no frame from before the stop is repeated,
    // no stale "lost" state, and persistent failures get their full grace period again.
    have_frame_ = false;
    lost_ = false;
    open_failure_logged_ = false;
    persistent_failure_since_ = 0;
    const Time100ns start = qpc_now();
    next_retry_ = start;
    retry_delay_ = kRetryInitialDelay;
    next_counter_log_ = start + kCounterLogInterval;

    // §3.2: one frame per tick, whether or not the desktop changed. Tick times come from the
    // index so they never drift, and they are the frames' timestamps on the QPC clock (§3.3).
    std::int64_t index = 0;
    while (timer.wait_until(start + frame_offset(index, fps_))) {
        const Time100ns now = qpc_now();
        // More than a whole frame late (system stall, debugger): skip the missed ticks instead of
        // delivering a burst of frames whose timestamps are all in the past.
        const std::int64_t due = (now - start) * fps_ / kTicksPerSecond;
        if (due > index) {
            counters_.late_ticks_skipped += static_cast<std::uint64_t>(due - index);
            index = due;
        }
        const Time100ns pts = start + frame_offset(index, fps_);
        const Time100ns duration = frame_offset(index + 1, fps_) - frame_offset(index, fps_);
        ++counters_.ticks;

        if (!duplication_ && now >= next_retry_) {
            try_open_duplication();
        }
        if (duplication_) {
            acquire_frame();
        }
        // §3.7: while the duplication is lost, the last good frame keeps the timeline going.
        if (have_frame_) {
            deliver(pts, duration);
        }
        if (now >= next_counter_log_) {
            log_counters();
            next_counter_log_ = now + kCounterLogInterval;
        }
        ++index;
    }
    log_counters();
}

bool ScreenCapture::try_open_duplication() {
    auto retry_later = [this](const std::string& why) {
        if (!open_failure_logged_) {
            log::info("Cannot capture {} yet ({}), retrying", to_utf8(monitor_.device_name), why);
            open_failure_logged_ = true;
        } else {
            log::debug("Still cannot capture {} ({})", to_utf8(monitor_.device_name), why);
        }
        next_retry_ = qpc_now() + retry_delay_;
        retry_delay_ = std::min(retry_delay_ * 2, kRetryMaxDelay);
        return false;
    };
    // For failures waiting does not normally fix: retried for kPersistentFailureTimeout in case a
    // mode switch is still in progress, then fatal with the exact reason.
    auto persistent_failure = [&](const std::string& why) {
        const Time100ns now = qpc_now();
        if (persistent_failure_since_ == 0) {
            persistent_failure_since_ = now;
        } else if (now - persistent_failure_since_ > kPersistentFailureTimeout) {
            throw std::runtime_error(std::format("Cannot capture {}: {}", to_utf8(monitor_.device_name), why));
        }
        return retry_later(why);
    };

    const OutputLookup lookup = find_output();
    if (!lookup.problem.empty()) {
        return retry_later(lookup.problem);
    }
    if (!lookup.output && !lookup.other_adapter.empty()) {
        // Our D3D device cannot duplicate an output on another GPU (dock, hybrid laptop). Given
        // the grace period in case it is a wake-up glitch, then the owner must rebuild the pipeline.
        return persistent_failure("the monitor is now on another graphics adapter (" + lookup.other_adapter + ")");
    }
    const ComPtr<IDXGIOutput> output = lookup.output;
    if (!output) {
        persistent_failure_since_ = 0;
        return retry_later("the monitor is asleep or disconnected");
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    HRESULT hr = E_NOINTERFACE;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output.As(&output5))) {
        // §3.6: offering only BGRA makes Windows tone map an HDR desktop for us instead of
        // handing out R16G16B16A16_FLOAT frames.
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(device_.Get(), 0, static_cast<UINT>(std::size(formats)), formats, &duplication);
    }
    if (FAILED(hr) && hr != E_ACCESSDENIED && hr != DXGI_ERROR_SESSION_DISCONNECTED &&
        hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE && !is_device_lost(hr)) {
        // IDXGIOutput5 missing (before Windows 10 1703) or DuplicateOutput1 unsupported.
        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (SUCCEEDED(hr)) {
            hr = output1->DuplicateOutput(device_.Get(), &duplication);
        }
    }
    if (is_device_lost(hr)) {
        lose_duplication(hr);  // throws
    }
    if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        // Secure desktop (UAC, lock screen), disconnected session, or too many other programs
        // capturing. All pass on their own, however long they take.
        persistent_failure_since_ = 0;
        return retry_later("DuplicateOutput " + hresult_text(hr));
    }
    if (FAILED(hr)) {
        // E_INVALIDARG, DXGI_ERROR_UNSUPPORTED and the like.
        return persistent_failure("DuplicateOutput " + hresult_text(hr));
    }

    DXGI_OUTDUPL_DESC desc{};
    duplication->GetDesc(&desc);
    if (desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        return persistent_failure(std::format("desktop format {} is not BGRA", static_cast<int>(desc.ModeDesc.Format)));
    }
    persistent_failure_since_ = 0;
    const DXGI_MODE_ROTATION rotation =
        desc.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED ? DXGI_MODE_ROTATION_IDENTITY : desc.Rotation;
    const SourceFormat mode{desc.ModeDesc.Width, desc.ModeDesc.Height, rotation};
    // Compared with the mode the pipeline was opened for, not with source_: acquire_frame may have
    // corrected source_ from the real texture size, and comparing against that would rebuild (and
    // lose the last good frame) on every recovery.
    if (!pipeline_built_ || !(mode == opened_mode_)) {
        rebuild_pipeline(mode);
        opened_mode_ = mode;
    }
    duplication_ = duplication;
    open_failure_logged_ = false;
    retry_delay_ = kRetryInitialDelay;
    if (lost_) {
        lost_ = false;
        log::info("Capture recovered");
        notify(CaptureEvent::Recovered);
    }
    return true;
}

ScreenCapture::OutputLookup ScreenCapture::find_output() {
    // A DXGI factory is a snapshot of the display topology. A DisplayPort monitor that slept or
    // was replugged only shows up in a new factory, so every attempt starts from a fresh one.
    OutputLookup result;
    ComPtr<IDXGIFactory1> factory;
    const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factory_hr)) {
        result.problem = "CreateDXGIFactory1 " + hresult_text(factory_hr);
        return result;
    }
    // Every adapter is searched before deciding: while a monitor wakes up it can briefly be
    // listed under another adapter (e.g. the Basic Display Adapter) as well as under ours.
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) != S_OK) {
            break;
        }
        DXGI_ADAPTER_DESC1 adapter_desc{};
        if (FAILED(adapter->GetDesc1(&adapter_desc))) {
            continue;
        }
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) != S_OK) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop || monitor_.device_name != desc.DeviceName) {
                continue;
            }
            if (same_luid(adapter_desc.AdapterLuid, monitor_.adapter_luid)) {
                result.output = output;
                return result;
            }
            // The same GPU under a new LUID means its driver restarted (update, TDR, disable/enable).
            result.other_adapter = to_utf8(adapter_desc.Description) == monitor_.adapter_name
                ? to_utf8(adapter_desc.Description) + ", same GPU after a driver restart"
                : to_utf8(adapter_desc.Description);
        }
    }
    return result;
}

void ScreenCapture::acquire_frame() {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr =
        simulate_lost_.exchange(false) ? DXGI_ERROR_ACCESS_LOST : duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        ++counters_.repeated_frames;  // §3.2: nothing changed, the previous image is encoded again
        return;
    }
    if (FAILED(hr)) {
        lose_duplication(hr);
        return;
    }

    // §3.2: copy into our own texture and release right away. Holding the frame across ticks
    // stops the desktop from updating.
    bool copied = false;
    try {
        if (info.LastPresentTime.QuadPart != 0 || !have_frame_) {
            ComPtr<ID3D11Texture2D> texture;
            BT_CHECK_HR(resource.As(&texture));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width != source_.width || desc.Height != source_.height) {
                // The surface is the authority on its own size (the mode changed after the
                // duplication was created, or ModeDesc reports it in another orientation).
                rebuild_pipeline(SourceFormat{desc.Width, desc.Height, source_.rotation});
            }
            context_->CopyResource(bgra_frame_.Get(), texture.Get());
            copied = true;
        }
    } catch (...) {
        duplication_->ReleaseFrame();
        throw;
    }
    const HRESULT release_hr = duplication_->ReleaseFrame();
    if (copied) {
        have_frame_ = true;
        ++counters_.new_frames;
    } else {
        ++counters_.repeated_frames;
    }
    if (FAILED(release_hr)) {
        lose_duplication(release_hr);
    }
}

void ScreenCapture::lose_duplication(HRESULT hr) {
    duplication_.Reset();
    if (is_device_lost(hr)) {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        throw HresultError(hr, std::format("The graphics device was lost ({}), reason {}", hresult_text(hr),
                                           hresult_text(reason)));
    }
    ++counters_.access_lost;
    if (!lost_) {
        lost_ = true;
        log::info("Capture lost ({}), retrying while the last frame keeps recording", hresult_text(hr));
        notify(CaptureEvent::Lost);
    }
    // §3.7: wait a little before re-creating; the desktop switch is usually still in progress.
    retry_delay_ = kRetryInitialDelay;
    next_retry_ = qpc_now() + retry_delay_;
}

void ScreenCapture::rebuild_pipeline(const SourceFormat& format) {
    // §3.6: NV12 needs even dimensions. Drop the odd column or row, never stretch.
    const UINT crop_width = format.width & ~1u;
    const UINT crop_height = format.height & ~1u;
    if (crop_width == 0 || crop_height == 0) {
        throw std::runtime_error(std::format("Desktop size {}x{} is too small to record", format.width, format.height));
    }
    const bool sideways = is_sideways(format.rotation);
    const VideoSize out{static_cast<int>(sideways ? crop_height : crop_width),
                        static_cast<int>(sideways ? crop_width : crop_height)};

    output_views_.clear();
    input_view_.Reset();
    processor_.Reset();
    processor_enum_.Reset();
    bgra_frame_.Reset();
    nv12_staging_.Reset();
    bgra_staging_.Reset();
    have_frame_ = false;
    pipeline_built_ = false;

    D3D11_TEXTURE2D_DESC frame_desc{};
    frame_desc.Width = format.width;
    frame_desc.Height = format.height;
    frame_desc.MipLevels = 1;
    frame_desc.ArraySize = 1;
    frame_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frame_desc.SampleDesc.Count = 1;
    frame_desc.Usage = D3D11_USAGE_DEFAULT;
    frame_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    BT_CHECK_HR(device_->CreateTexture2D(&frame_desc, nullptr, &bgra_frame_));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = DXGI_RATIONAL{static_cast<UINT>(fps_), 1};
    content.InputWidth = format.width;
    content.InputHeight = format.height;
    content.OutputFrameRate = DXGI_RATIONAL{static_cast<UINT>(fps_), 1};
    content.OutputWidth = static_cast<UINT>(out.width);
    content.OutputHeight = static_cast<UINT>(out.height);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    BT_CHECK_HR(video_device_->CreateVideoProcessorEnumerator(&content, &processor_enum_));

    UINT support = 0;
    BT_CHECK_HR(processor_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        throw std::runtime_error("The GPU video processor cannot read BGRA frames");
    }
    BT_CHECK_HR(processor_enum_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support));
    if (!(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        throw std::runtime_error("The GPU video processor cannot write NV12 frames");
    }
    BT_CHECK_HR(video_device_->CreateVideoProcessor(processor_enum_.Get(), 0, &processor_));

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    BT_CHECK_HR(video_device_->CreateVideoProcessorInputView(bgra_frame_.Get(), processor_enum_.Get(), &input_desc,
                                                             &input_view_));

    ID3D11VideoContext* vc = video_context_.Get();
    ID3D11VideoProcessor* vp = processor_.Get();
    const RECT source_rect{0, 0, static_cast<LONG>(crop_width), static_cast<LONG>(crop_height)};
    const RECT target_rect{0, 0, out.width, out.height};
    vc->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &source_rect);
    vc->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &target_rect);
    vc->VideoProcessorSetOutputTargetRect(vp, TRUE, &target_rect);
    // Drivers may apply their own "enhancements" (AMD: colour vibrance, sharpening) when auto
    // processing is on. A recording must look exactly like the screen.
    vc->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    vc->VideoProcessorSetStreamRotation(vp, 0, format.rotation != DXGI_MODE_ROTATION_IDENTITY,
                                        processor_rotation(format.rotation));

    // §3.6: explicit colour spaces. Defaults give washed out or crushed blacks next to ShadowPlay.
    // Input: desktop RGB, full range 0-255. Output: BT.709 YCbCr, limited range 16-235.
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
    input_space.RGB_Range = 0;  // 0 = full range 0-255
    input_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
    output_space.YCbCr_Matrix = 1;  // 1 = BT.709
    output_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    vc->VideoProcessorSetStreamColorSpace(vp, 0, &input_space);
    vc->VideoProcessorSetOutputColorSpace(vp, &output_space);
    if (video_context1_) {
        // The DXGI colour space form is what current drivers honour; it states the same thing.
        video_context1_->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        video_context1_->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    }

    // §3.5: the encoder's input textures come from Media Foundation's allocator, which only hands
    // out a texture once the encoder has released it.
    BT_CHECK_HR(allocator_->UninitializeSampleAllocator());
    ComPtr<IMFAttributes> attributes;
    BT_CHECK_HR(MFCreateAttributes(&attributes, 2));
    BT_CHECK_HR(attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));  // video processor output
    BT_CHECK_HR(attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT));
    ComPtr<IMFMediaType> type;
    BT_CHECK_HR(MFCreateMediaType(&type));
    BT_CHECK_HR(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    BT_CHECK_HR(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    BT_CHECK_HR(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(out.width),
                                   static_cast<UINT32>(out.height)));
    BT_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps_), 1));
    BT_CHECK_HR(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    BT_CHECK_HR(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    BT_CHECK_HR(allocator_->InitializeSampleAllocatorEx(kAllocatorInitialSamples, kAllocatorMaxSamples,
                                                        attributes.Get(), type.Get()));

    source_ = format;
    pipeline_built_ = true;
    bool size_changed = false;
    {
        std::lock_guard lock(size_mutex_);
        size_changed = !(output_size_ == out);
        output_size_ = out;
    }
    log::info("Capture format: desktop {}x{}, rotation {}, NV12 frames {}x{}", format.width, format.height,
              static_cast<int>(format.rotation), out.width, out.height);
    if (size_changed) {
        notify(CaptureEvent::FormatChanged);
    }
}

void ScreenCapture::deliver(Time100ns pts, Time100ns duration) {
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) {
        ++counters_.allocator_empty;  // §3.5: the encoder is behind. Drop, never block.
        return;
    }

    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi_buffer;
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    ID3D11VideoProcessorOutputView* view = nullptr;
    if (SUCCEEDED(hr)) hr = sample->GetBufferByIndex(0, &buffer);
    if (SUCCEEDED(hr)) hr = buffer.As(&dxgi_buffer);
    if (SUCCEEDED(hr)) hr = dxgi_buffer->GetResource(IID_PPV_ARGS(&texture));
    if (SUCCEEDED(hr)) hr = dxgi_buffer->GetSubresourceIndex(&subresource);
    if (SUCCEEDED(hr)) hr = output_view_for(texture.Get(), subresource, &view);
    if (SUCCEEDED(hr)) {
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view_.Get();
        hr = video_context_->VideoProcessorBlt(processor_.Get(), view, 0, 1, &stream);
    }
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(pts);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (is_device_lost(hr)) {
        lose_duplication(hr);  // throws
    }
    if (FAILED(hr)) {
        ++counters_.convert_failed;
        if (convert_error_log_.allow()) {
            log::error("Converting a frame to NV12 failed: {}", hresult_text(hr));
        }
        return;
    }
    ++counters_.delivered;
    on_frame_(sample.Get(), pts);
}

HRESULT ScreenCapture::output_view_for(ID3D11Texture2D* texture, UINT subresource,
                                       ID3D11VideoProcessorOutputView** view) {
    for (const OutputView& entry : output_views_) {
        if (entry.texture.Get() == texture && entry.subresource == subresource) {
            *view = entry.view.Get();
            return S_OK;
        }
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_desc{};
    if (desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        view_desc.Texture2DArray.FirstArraySlice = subresource;
        view_desc.Texture2DArray.ArraySize = 1;
    } else {
        view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    }
    OutputView entry{texture, subresource, nullptr};
    const HRESULT hr =
        video_device_->CreateVideoProcessorOutputView(texture, processor_enum_.Get(), &view_desc, &entry.view);
    if (FAILED(hr)) {
        return hr;
    }
    if (output_views_.size() >= kMaxCachedOutputViews) {
        output_views_.clear();
    }
    *view = entry.view.Get();
    output_views_.push_back(std::move(entry));
    return S_OK;
}

std::vector<std::uint8_t> ScreenCapture::read_nv12(IMFSample* nv12_sample) {
    ComPtr<ID3D11Texture2D> texture;
    UINT subresource = 0;
    sample_texture(nv12_sample, texture, subresource);
    const VideoSize size = frame_size();
    const auto width = static_cast<std::size_t>(size.width);
    const auto height = static_cast<std::size_t>(size.height);

    if (!nv12_staging_) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        BT_CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &nv12_staging_));
    }
    D3D11_TEXTURE2D_DESC staging_desc{};
    nv12_staging_->GetDesc(&staging_desc);

    std::vector<std::uint8_t> out(width * height * 3 / 2);
    context_->CopySubresourceRegion(nv12_staging_.Get(), 0, 0, 0, 0, texture.Get(), subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    BT_CHECK_HR(context_->Map(nv12_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto* y_plane = static_cast<const std::uint8_t*>(mapped.pData);
    // A mapped NV12 texture stores the UV plane right after RowPitch * Height bytes of luma.
    const std::uint8_t* uv_plane = y_plane + static_cast<std::size_t>(mapped.RowPitch) * staging_desc.Height;
    for (std::size_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + y * width, y_plane + y * mapped.RowPitch, width);
    }
    for (std::size_t y = 0; y < height / 2; ++y) {
        std::memcpy(out.data() + width * height + y * width, uv_plane + y * mapped.RowPitch, width);
    }
    context_->Unmap(nv12_staging_.Get(), 0);
    return out;
}

std::vector<std::uint8_t> ScreenCapture::read_last_bgra() {
    if (source_.rotation != DXGI_MODE_ROTATION_IDENTITY || !bgra_frame_) {
        return {};
    }
    if (!bgra_staging_) {
        D3D11_TEXTURE2D_DESC desc{};
        bgra_frame_->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        BT_CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &bgra_staging_));
    }
    const VideoSize size = frame_size();
    const std::size_t row_bytes = static_cast<std::size_t>(size.width) * 4;
    std::vector<std::uint8_t> out(row_bytes * static_cast<std::size_t>(size.height));
    context_->CopyResource(bgra_staging_.Get(), bgra_frame_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    BT_CHECK_HR(context_->Map(bgra_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < size.height; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * row_bytes, src + static_cast<std::size_t>(y) * mapped.RowPitch,
                    row_bytes);
    }
    context_->Unmap(bgra_staging_.Get(), 0);
    return out;
}

void ScreenCapture::notify(CaptureEvent event) {
    if (!on_event_) {
        return;
    }
    try {
        on_event_(event, frame_size());
    } catch (const std::exception& e) {
        log::error("Capture event handler failed: {}", e.what());
    } catch (...) {
        log::error("Capture event handler failed: unknown exception");
    }
}

void ScreenCapture::log_counters() {
    const CaptureStats s = stats();
    log::debug("Capture counters: {} ticks, {} new frames, {} repeated, {} dropped (encoder behind), {} conversion "
               "failures, {} late ticks skipped, {} capture losses, {} delivered",
               s.ticks, s.new_frames, s.repeated_frames, s.allocator_empty, s.convert_failed, s.late_ticks_skipped,
               s.access_lost, s.delivered);
}

}  // namespace bt
