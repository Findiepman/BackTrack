#include "linux/PipeWireCapture.h"

#include "linux/Log.h"
#include "linux/MonotonicClock.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

namespace bt::linux_app {

namespace {

constexpr Time100ns kNoPictureTimeout = 10 * kTicksPerSecond;

struct Image {
    std::vector<std::uint8_t> pixels;  // rows of width * 4 bytes, no padding
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Bgrx;
};

}  // namespace

// Three images change hands so neither thread ever copies a frame while holding the lock: the
// PipeWire thread fills `back` and swaps it into `front`; grab() swaps `front` into `held`.
struct PipeWireCapture::Impl {
    int fd = -1;
    std::uint32_t node = 0;
    int fps = 60;

    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    pw_stream_events stream_events{};

    // Only the PipeWire thread.
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::Bgrx;
    bool format_ok = false;
    bool streaming = false;
    Image back;

    std::mutex mutex;  // guards front, front_serial, failed, error
    Image front;
    std::uint64_t front_serial = 0;
    bool failed = false;
    std::string error;

    // Only the video thread.
    Time100ns started_at = 0;  // set by start()
    Image held;
    std::uint64_t held_serial = 0;

    void fail(std::string message) {
        std::lock_guard lock(mutex);
        if (!failed) {
            failed = true;
            error = std::move(message);
            log_warning("Screen capture: " + error);
        }
    }

    static void on_state_changed(void* data, pw_stream_state, pw_stream_state state, const char* message) {
        auto* self = static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            self->fail(std::string("PipeWire stream error: ") + (message != nullptr ? message : "unknown"));
        } else if (state == PW_STREAM_STATE_STREAMING) {
            self->streaming = true;
            log_info("Screen capture: PipeWire stream running");
        } else if (state == PW_STREAM_STATE_UNCONNECTED && self->streaming) {
            self->fail("Screen sharing was stopped");
        }
    }

    static void on_param_changed(void* data, std::uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(data);
        if (param == nullptr || id != SPA_PARAM_Format) {
            return;
        }
        std::uint32_t media_type = 0;
        std::uint32_t media_subtype = 0;
        if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
            media_subtype != SPA_MEDIA_SUBTYPE_raw) {
            return;
        }
        spa_video_info_raw raw{};
        if (spa_format_video_raw_parse(param, &raw) < 0) {
            return;
        }
        switch (raw.format) {
        case SPA_VIDEO_FORMAT_BGRx:
        case SPA_VIDEO_FORMAT_BGRA:
            self->layout = PixelLayout::Bgrx;
            break;
        case SPA_VIDEO_FORMAT_RGBx:
        case SPA_VIDEO_FORMAT_RGBA:
            self->layout = PixelLayout::Rgbx;
            break;
        default:
            self->fail(std::format("The compositor offers an unsupported pixel format ({})", static_cast<int>(raw.format)));
            return;
        }
        self->width = static_cast<int>(raw.size.width);
        self->height = static_cast<int>(raw.size.height);
        self->format_ok = self->width >= 2 && self->height >= 2;
        log_info(std::format("Screen capture: {}x{}", self->width, self->height));

        // Memory buffers only (see the class comment), one plane.
        std::uint8_t buffer[512];
        spa_pod_builder builder{};
        spa_pod_builder_init(&builder, buffer, sizeof buffer);
        spa_pod_frame frame{};
        spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        spa_pod_builder_add(&builder,
                            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
                            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)),
                            0);
        const spa_pod* params[] = {static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame))};
        pw_stream_update_params(self->stream, params, 1);
    }

    static void on_process(void* data) {
        auto* self = static_cast<Impl*>(data);
        // Only the newest buffer matters; older queued ones go straight back.
        pw_buffer* newest = nullptr;
        while (pw_buffer* next = pw_stream_dequeue_buffer(self->stream)) {
            if (newest != nullptr) {
                pw_stream_queue_buffer(self->stream, newest);
            }
            newest = next;
        }
        if (newest == nullptr) {
            return;
        }
        self->copy(*newest->buffer);
        pw_stream_queue_buffer(self->stream, newest);
    }

    void copy(const spa_buffer& buffer) {
        if (!format_ok || buffer.n_datas < 1) {
            return;
        }
        const spa_data& d = buffer.datas[0];
        // size 0: KWin sent only a cursor update or nothing new.
        if (d.data == nullptr || d.chunk == nullptr || d.chunk->size == 0 ||
            (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0) {
            return;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
        const std::size_t stride = d.chunk->stride > 0 ? static_cast<std::size_t>(d.chunk->stride) : row_bytes;
        const std::size_t offset = d.chunk->offset;
        if (stride < row_bytes || offset >= d.maxsize ||
            offset + stride * (static_cast<std::size_t>(height) - 1) + row_bytes > d.maxsize) {
            return;  // a buffer smaller than the format says: skip it rather than read past it
        }
        const auto* src = static_cast<const std::uint8_t*>(d.data) + offset;
        back.pixels.resize(row_bytes * static_cast<std::size_t>(height));
        for (int y = 0; y < height; ++y) {
            std::memcpy(back.pixels.data() + row_bytes * static_cast<std::size_t>(y), src + stride * static_cast<std::size_t>(y),
                        row_bytes);
        }
        back.width = width;
        back.height = height;
        back.layout = layout;
        std::lock_guard lock(mutex);
        std::swap(back, front);
        ++front_serial;
    }

};

PipeWireCapture::PipeWireCapture(int pipewire_fd, std::uint32_t node, int fps, std::string description)
    : impl_(std::make_unique<Impl>()), description_(std::move(description)) {
    impl_->fd = pipewire_fd;
    impl_->node = node;
    impl_->fps = fps;
}

PipeWireCapture::~PipeWireCapture() {
    stop();
    if (impl_->fd >= 0) {
        close(impl_->fd);
    }
}

bool PipeWireCapture::start(std::string& error) {
    Impl& s = *impl_;
    if (s.fd < 0) {
        error = "The screen sharing connection is already closed";
        return false;
    }
    pw_init(nullptr, nullptr);
    s.loop = pw_thread_loop_new("bt-capture", nullptr);
    if (s.loop == nullptr) {
        error = "Cannot create the PipeWire thread";
        return false;
    }
    s.context = pw_context_new(pw_thread_loop_get_loop(s.loop), nullptr, 0);
    if (s.context == nullptr || pw_thread_loop_start(s.loop) < 0) {
        error = "Cannot start PipeWire";
        stop();
        return false;
    }

    pw_thread_loop_lock(s.loop);
    s.core = pw_context_connect_fd(s.context, s.fd, nullptr, 0);
    s.fd = -1;  // the core owns it now, also when the connection failed
    if (s.core == nullptr) {
        pw_thread_loop_unlock(s.loop);
        error = "Cannot connect to the screen sharing stream";
        stop();
        return false;
    }
    // No core listener: pw_core_add_listener is a GNU statement-expression macro in PipeWire 1.0
    // (Ubuntu 24.04), which -Wpedantic rejects. A lost connection ends the stream with an error
    // or UNCONNECTED state, which on_state_changed reports.

    s.stream = pw_stream_new(s.core, "Backtrack",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                               PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (s.stream == nullptr) {
        pw_thread_loop_unlock(s.loop);
        error = "Cannot create the PipeWire stream";
        stop();
        return false;
    }
    s.stream_events.version = PW_VERSION_STREAM_EVENTS;
    s.stream_events.state_changed = &Impl::on_state_changed;
    s.stream_events.param_changed = &Impl::on_param_changed;
    s.stream_events.process = &Impl::on_process;
    pw_stream_add_listener(s.stream, &s.stream_listener, &s.stream_events, &s);

    std::uint8_t buffer[1024];
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, buffer, sizeof buffer);
    spa_rectangle size_default{1920, 1080};
    spa_rectangle size_min{1, 1};
    spa_rectangle size_max{16384, 16384};
    spa_fraction rate_default{static_cast<std::uint32_t>(s.fps), 1};
    spa_fraction rate_min{0, 1};
    spa_fraction rate_max{1000, 1};
    spa_pod_frame frame{};
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&builder,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
                        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size_default, &size_min, &size_max),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate_default, &rate_min, &rate_max),
                        0);
    const spa_pod* params[] = {static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame))};
    const int result = pw_stream_connect(s.stream, PW_DIRECTION_INPUT, s.node,
                                         static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                         params, 1);
    pw_thread_loop_unlock(s.loop);
    s.started_at = now_100ns();
    if (result < 0) {
        error = std::format("Cannot connect to the screen (PipeWire error {})", result);
        stop();
        return false;
    }
    return true;
}

GrabResult PipeWireCapture::grab(FrameView& frame) {
    Impl& s = *impl_;
    {
        std::lock_guard lock(s.mutex);
        if (s.failed) {
            return GrabResult::Failed;
        }
        if (s.front_serial != s.held_serial) {
            std::swap(s.front, s.held);
            s.held_serial = s.front_serial;
        }
    }
    if (s.held.pixels.empty()) {
        // KWin sends a first image as soon as the stream runs; none after this long means the
        // stream never started (a refused format, a portal that handed over a dead node).
        if (s.started_at != 0 && now_100ns() - s.started_at > kNoPictureTimeout) {
            s.fail("No picture from the screen sharing stream");
            return GrabResult::Failed;
        }
        return GrabResult::NotYet;
    }
    frame.data = s.held.pixels.data();
    frame.width = s.held.width & ~1;  // the encoders want even sizes
    frame.height = s.held.height & ~1;
    frame.stride = s.held.width * 4;
    frame.layout = s.held.layout;
    return GrabResult::Frame;
}

void PipeWireCapture::stop() {
    Impl& s = *impl_;
    if (s.loop != nullptr) {
        pw_thread_loop_lock(s.loop);
        s.streaming = false;  // our own disconnect is not "the user stopped sharing"
        if (s.stream != nullptr) {
            pw_stream_disconnect(s.stream);
            pw_stream_destroy(s.stream);
            s.stream = nullptr;
        }
        if (s.core != nullptr) {
            pw_core_disconnect(s.core);
            s.core = nullptr;
        }
        pw_thread_loop_unlock(s.loop);
        pw_thread_loop_stop(s.loop);
    }
    if (s.context != nullptr) {
        pw_context_destroy(s.context);
        s.context = nullptr;
    }
    if (s.loop != nullptr) {
        pw_thread_loop_destroy(s.loop);
        s.loop = nullptr;
    }
}

std::string PipeWireCapture::error() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}

}  // namespace bt::linux_app
