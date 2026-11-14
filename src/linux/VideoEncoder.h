#pragma once

#include "core/ClipBuffer.h"
#include "core/Config.h"
#include "linux/FrameSource.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVCodecParameters;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace bt::linux_app {

// Copy of an encoder's output parameters (codec, size, SPS/PPS or AudioSpecificConfig), which the
// clip writer needs. Shared because a save may still use it after the encoder was reopened.
using CodecParameters = std::shared_ptr<const AVCodecParameters>;

CodecParameters copy_parameters(const AVCodecContext* context);

struct VideoEncoderConfig {
    int source_width = 0;
    int source_height = 0;
    PixelLayout layout = PixelLayout::Bgrx;
    int fps = 60;
    int bitrate_kbps = 20000;
    int resolution = 0;  // the `resolution` setting: output height, 0 = the source's own (never scaled up)
    EncoderPreference preference = EncoderPreference::Auto;
    QualityPreset quality = QualityPreset::Balanced;
};

// H.264 through FFmpeg's libavcodec: NVENC (NVIDIA), then VA-API (AMD, Intel), then x264 or
// OpenH264 on the processor. A candidate only counts once it encoded a test frame, so a driver
// that is installed but broken falls through to the next one.
//
// Settings for a replay buffer: a keyframe every second (a clip starts at a keyframe), no
// B-frames (decode order = display order, so a packet's pts is its dts), constant bitrate-ish.
class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    bool open(const VideoEncoderConfig& config, std::string& error);
    void close();
    bool is_open() const { return context_ != nullptr; }

    // Encodes one screen image shown at `time`. The packets that come out (possibly for earlier
    // frames, hardware encoders run a few frames behind) are appended to `out`.
    bool encode(const FrameView& frame, Time100ns time, std::vector<Packet>& out, std::string& error);

    std::string name() const { return name_; }  // "h264_vaapi"
    bool hardware() const { return hardware_; }
    int width() const { return out_width_; }
    int height() const { return out_height_; }
    const VideoEncoderConfig& config() const { return config_; }
    CodecParameters parameters() const { return parameters_; }

private:
    bool open_codec(const std::string& codec_name, std::string& error);
    bool convert(const FrameView& frame, AVFrame* target);
    bool receive(std::vector<Packet>& out, std::string& error);
    bool send_test_frame(std::string& error);

    VideoEncoderConfig config_;
    int out_width_ = 0;
    int out_height_ = 0;
    std::string name_;
    bool hardware_ = false;
    AVCodecContext* context_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVBufferRef* hw_frames_ = nullptr;
    AVFrame* sw_frame_ = nullptr;  // the converted image (NV12 or YUV420P)
    AVFrame* hw_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    int sws_format_ = -1;  // what sws_ converts to
    int sws_width_ = 0;  // what sws_ converts from
    int sws_height_ = 0;
    int sws_source_ = -1;
    std::int64_t next_index_ = 0;
    bool first_frame_ = true;  // the next encode() is the first after the test frame
    std::map<std::int64_t, Time100ns> times_;  // frame index -> time, until its packet comes out
    CodecParameters parameters_;
};

}  // namespace bt::linux_app
