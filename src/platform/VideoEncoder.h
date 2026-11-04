#pragma once

// H.264 encoder: the GPU vendor's Media Foundation encoder, with Microsoft's software encoder as
// the fallback (§3.4).
//
// Threads:
//   - Constructor and destructor: the thread that calls encode() (or any thread while nobody
//     calls it), after MFStartup. Creating one can take a few hundred milliseconds.
//   - encode() / encode_system_memory(): the capture thread. Never blocks on the encoder: a full
//     queue drops the frame and counts it.
//   - Hardware: Media Foundation work queue threads deliver the encoder's events to on_event(),
//     one at a time. ProcessOutput and the packet callback run there.
//   - Software: one worker thread owned by this class runs the synchronous MFT and the callback.
//   - The packet callback must be thread safe with respect to the owner (ClipBuffer is).

#include "core/ClipBuffer.h"
#include "core/Config.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/ScreenCapture.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

struct VideoEncoderSettings {
    VideoSize size;
    int fps = 60;
    int bitrate_kbps = 20000;
    EncoderPreference preference = EncoderPreference::Auto;
    LUID adapter_luid{};  // the hardware encoder must run on the GPU that holds the frames
};

struct VideoEncoderStats {
    std::uint64_t frames_in = 0;
    std::uint64_t frames_dropped = 0;  // input queue full: the encoder is behind
    std::uint64_t input_errors = 0;
    std::uint64_t packets_out = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t bytes_out = 0;
    Time100ns min_keyframe_interval = 0;  // between consecutive keyframes, 0 until two were seen
    Time100ns max_keyframe_interval = 0;
    std::uint64_t non_increasing_pts = 0;  // would mean B-frames or a broken encoder
};

class VideoEncoder {
public:
    // output_type: the encoder's current output type, which this packet belongs to (§7 needs it
    // for the MP4). It only changes after a format renegotiation.
    using PacketCallback = std::function<void(Packet&& packet, IMFMediaType* output_type)>;

    VideoEncoder(const VideoEncoderSettings& settings, IMFDXGIDeviceManager* device_manager, PacketCallback on_packet);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool hardware() const { return hardware_; }
    const std::string& name() const { return name_; }
    VideoSize size() const { return settings_.size; }

    // The software encoder cannot read GPU textures; feed it encode_system_memory() instead.
    bool needs_system_memory() const { return !hardware_; }

    // Hardware path: the NV12 sample from ScreenCapture, with sample time and duration set.
    void encode(IMFSample* nv12_gpu_sample);
    // Software path: tightly packed NV12 from ScreenCapture::read_nv12.
    void encode_system_memory(std::vector<std::uint8_t> nv12, Time100ns pts, Time100ns duration);

    // The encoder's current output type once the first packet came out (it carries the sequence
    // header the MP4 writer needs, §7), else null.
    ComPtr<IMFMediaType> output_type() const;

    VideoEncoderStats stats() const;

    // True once the encoder stopped working (event queue error, MEError, worker crash). The owner
    // rebuilds it; packets stop until then.
    bool failed() const { return failed_; }

private:
    class EventCallback;
    struct CpuFrame {
        std::vector<std::uint8_t> nv12;
        Time100ns pts = 0;
        Time100ns duration = 0;
    };

    void reset_for_attempt();
    bool try_hardware(IMFActivate* activate, IMFDXGIDeviceManager* device_manager);
    void setup_software();
    void configure(IMFTransform* transform);
    void read_stream_info();

    void on_event(IMFAsyncResult* result);
    void feed_locked();
    void process_input_locked(IMFSample* sample);
    // Returns false when the encoder needs more input. deliver = false discards the packet (drain).
    bool process_output(bool deliver);
    void emit_packet(IMFSample* sample);
    void software_loop();
    void shutdown();

    VideoEncoderSettings settings_;
    PacketCallback on_packet_;
    bool hardware_ = false;
    std::string name_;

    ComPtr<IMFActivate> activate_;
    ComPtr<IMFTransform> transform_;
    DWORD input_id_ = 0;
    DWORD output_id_ = 0;
    bool encoder_allocates_output_ = false;
    DWORD output_buffer_size_ = 0;

    // Hardware event loop.
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<EventCallback> callback_;  // non-null only while the event loop is armed
    UniqueHandle event_loop_done_;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> failed_{false};
    std::mutex input_mutex_;  // serialises ProcessInput between the capture thread and events
    std::deque<ComPtr<IMFSample>> pending_;
    int need_input_ = 0;  // METransformNeedInput events not yet answered
    int consecutive_input_errors_ = 0;

    // Software worker.
    std::thread worker_;
    std::condition_variable worker_wake_;
    std::deque<CpuFrame> cpu_pending_;
    bool stop_worker_ = false;

    // Output side (event thread or worker only, except output_type_ under its mutex).
    mutable std::mutex output_type_mutex_;
    ComPtr<IMFMediaType> output_type_;
    bool have_last_pts_ = false;
    Time100ns last_pts_ = 0;
    bool have_keyframe_ = false;
    Time100ns last_keyframe_pts_ = 0;
    int keyframe_intervals_logged_ = 0;
    log::RateLimit input_error_log_{std::chrono::seconds(10)};
    log::RateLimit timestamp_error_log_{std::chrono::seconds(10)};
    log::RateLimit type_error_log_{std::chrono::seconds(30)};
    bool clean_point_fallback_logged_ = false;

    struct Counters {
        std::atomic<std::uint64_t> frames_in{0}, frames_dropped{0}, input_errors{0}, packets_out{0}, keyframes{0},
            bytes_out{0}, non_increasing_pts{0};
        std::atomic<Time100ns> min_keyframe_interval{0}, max_keyframe_interval{0};
    } counters_;
};

}  // namespace bt
