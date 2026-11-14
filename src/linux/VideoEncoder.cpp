#include "linux/VideoEncoder.h"

#include "linux/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <climits>
#include <cstring>
#include <format>

namespace bt::linux_app {

namespace {

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

std::vector<std::string> candidates(EncoderPreference preference) {
    if (preference == EncoderPreference::Software) {
        return {"libx264", "libopenh264"};
    }
    // Hardware falls back to software too, with a warning: a recording beats none.
    return {"h264_nvenc", "h264_vaapi", "libx264", "libopenh264"};
}

const char* nvenc_preset(QualityPreset quality) {
    switch (quality) {
    case QualityPreset::Performance: return "p1";
    case QualityPreset::Quality: return "p6";
    case QualityPreset::Balanced: break;
    }
    return "p4";
}

// Screen capture leaves little processor time: x264's faster presets, one step slower for Quality.
const char* x264_preset(QualityPreset quality) {
    switch (quality) {
    case QualityPreset::Performance: return "ultrafast";
    case QualityPreset::Quality: return "veryfast";
    case QualityPreset::Balanced: break;
    }
    return "superfast";
}

}  // namespace

CodecParameters copy_parameters(const AVCodecContext* context) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    if (parameters == nullptr || avcodec_parameters_from_context(parameters, context) < 0) {
        avcodec_parameters_free(&parameters);
        return nullptr;
    }
    return CodecParameters(parameters, [](const AVCodecParameters* p) {
        auto* owned = const_cast<AVCodecParameters*>(p);
        avcodec_parameters_free(&owned);
    });
}

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() {
    close();
}

bool VideoEncoder::open(const VideoEncoderConfig& config, std::string& error) {
    close();
    config_ = config;
    const int src_w = config.source_width & ~1;
    const int src_h = config.source_height & ~1;
    if (src_w < 2 || src_h < 2) {
        error = "The screen has no size";
        return false;
    }
    out_width_ = src_w;
    out_height_ = src_h;
    if (config.resolution > 0 && config.resolution < src_h) {
        out_height_ = config.resolution & ~1;
        const std::int64_t scaled = (static_cast<std::int64_t>(src_w) * out_height_ + src_h / 2) / src_h;
        out_width_ = static_cast<int>(scaled) & ~1;
    }

    std::string reasons;
    for (const std::string& name : candidates(config.preference)) {
        std::string why;
        if (open_codec(name, why)) {
            if (config.preference == EncoderPreference::Hardware && !hardware_) {
                log_warning("No hardware encoder works here: recording with " + name + " on the processor");
            }
            log_info(std::format("Video encoder: {} {}x{} at {} fps, {} kbit/s", name, out_width_, out_height_, config.fps,
                                 config.bitrate_kbps));
            return true;
        }
        log_info("Encoder " + name + " not used: " + why);
        reasons += (reasons.empty() ? "" : "; ") + name + ": " + why;
        close();
    }
    error = "No H.264 encoder works (" + reasons + ")";
    return false;
}

bool VideoEncoder::open_codec(const std::string& codec_name, std::string& error) {
    const AVCodec* codec = avcodec_find_encoder_by_name(codec_name.c_str());
    if (codec == nullptr) {
        error = "not in this FFmpeg";
        return false;
    }
    const bool vaapi = codec_name == "h264_vaapi";
    const bool nvenc = codec_name == "h264_nvenc";
    hardware_ = vaapi || nvenc;
    name_ = codec_name;

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
        error = "out of memory";
        return false;
    }
    const int fps = config_.fps;
    context_->width = out_width_;
    context_->height = out_height_;
    context_->time_base = AVRational{1, fps};
    context_->framerate = AVRational{fps, 1};
    context_->gop_size = fps;  // a keyframe every second: a clip can start at any second
    context_->max_b_frames = 0;
    context_->bit_rate = static_cast<std::int64_t>(config_.bitrate_kbps) * 1000;
    context_->rc_max_rate = context_->bit_rate;
    context_->rc_buffer_size = static_cast<int>(std::min<std::int64_t>(context_->bit_rate, INT32_MAX));
    context_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // SPS/PPS in extradata, for the MP4 header
    context_->color_range = AVCOL_RANGE_MPEG;
    context_->colorspace = AVCOL_SPC_BT709;
    context_->color_primaries = AVCOL_PRI_BT709;
    context_->color_trc = AVCOL_TRC_BT709;

    AVPixelFormat sw_format = AV_PIX_FMT_YUV420P;
    AVDictionary* options = nullptr;
    if (nvenc) {
        sw_format = AV_PIX_FMT_NV12;
        context_->pix_fmt = AV_PIX_FMT_NV12;
        av_dict_set(&options, "preset", nvenc_preset(config_.quality), 0);
        av_dict_set(&options, "tune", "ll", 0);
        av_dict_set(&options, "rc", "vbr", 0);
        av_dict_set(&options, "zerolatency", "1", 0);
        av_dict_set(&options, "forced-idr", "1", 0);  // a frame marked I (the first real one) is an IDR
    } else if (vaapi) {
        sw_format = AV_PIX_FMT_NV12;
        int result = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
        if (result < 0) {
            error = "no VA-API device (" + av_error(result) + ")";
            return false;
        }
        hw_frames_ = av_hwframe_ctx_alloc(hw_device_);
        if (hw_frames_ == nullptr) {
            error = "out of memory";
            return false;
        }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(hw_frames_->data);
        frames->format = AV_PIX_FMT_VAAPI;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = out_width_;
        frames->height = out_height_;
        frames->initial_pool_size = 8;
        result = av_hwframe_ctx_init(hw_frames_);
        if (result < 0) {
            error = "VA-API cannot make NV12 surfaces (" + av_error(result) + ")";
            return false;
        }
        context_->pix_fmt = AV_PIX_FMT_VAAPI;
        context_->hw_frames_ctx = av_buffer_ref(hw_frames_);
    } else if (codec_name == "libx264") {
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
        av_dict_set(&options, "preset", x264_preset(config_.quality), 0);
        av_dict_set(&options, "tune", "zerolatency", 0);
    } else {
        context_->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    const int opened = avcodec_open2(context_, codec, &options);
    av_dict_free(&options);
    if (opened < 0) {
        error = av_error(opened);
        return false;
    }

    sw_frame_ = av_frame_alloc();
    hw_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (sw_frame_ == nullptr || hw_frame_ == nullptr || packet_ == nullptr) {
        error = "out of memory";
        return false;
    }
    sw_frame_->format = sw_format;
    sw_frame_->width = out_width_;
    sw_frame_->height = out_height_;
    if (av_frame_get_buffer(sw_frame_, 0) < 0) {
        error = "out of memory";
        return false;
    }
    sws_format_ = sw_format;
    if (!send_test_frame(error)) {
        return false;
    }
    parameters_ = copy_parameters(context_);
    if (!parameters_ || parameters_->extradata_size <= 0) {
        error = "the encoder gave no SPS/PPS";
        return false;
    }
    return true;
}

bool VideoEncoder::send_test_frame(std::string& error) {
    // Black. A driver that opens but cannot encode fails here instead of on the first real frame.
    // Its index is not in times_, so whatever comes out for it is dropped.
    const int y_size = sw_frame_->linesize[0] * out_height_;
    std::memset(sw_frame_->data[0], 16, static_cast<std::size_t>(y_size));
    // NV12: one interleaved UV plane; YUV420P: U and V planes. Both half height.
    for (int plane = 1; plane <= (sws_format_ == AV_PIX_FMT_NV12 ? 1 : 2); ++plane) {
        std::memset(sw_frame_->data[plane], 128, static_cast<std::size_t>(sw_frame_->linesize[plane]) * (out_height_ / 2));
    }
    AVFrame* send = sw_frame_;
    if (hw_frames_ != nullptr) {
        av_frame_unref(hw_frame_);
        int result = av_hwframe_get_buffer(hw_frames_, hw_frame_, 0);
        if (result >= 0) {
            result = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
        }
        if (result < 0) {
            error = "cannot upload to the graphics card (" + av_error(result) + ")";
            return false;
        }
        send = hw_frame_;
    }
    send->pts = next_index_++;
    const int result = avcodec_send_frame(context_, send);
    av_frame_unref(hw_frame_);
    if (result < 0) {
        error = "test frame failed (" + av_error(result) + ")";
        return false;
    }
    std::vector<Packet> ignored;
    return receive(ignored, error);
}

bool VideoEncoder::convert(const FrameView& frame, AVFrame* target) {
    const AVPixelFormat source = frame.layout == PixelLayout::Rgbx ? AV_PIX_FMT_RGB0 : AV_PIX_FMT_BGR0;
    const bool changed = sws_ == nullptr || frame.width != sws_width_ || frame.height != sws_height_ || source != sws_source_;
    if (changed) {
        sws_freeContext(sws_);
        sws_ = sws_getContext(frame.width, frame.height, source, out_width_, out_height_,
                              static_cast<AVPixelFormat>(sws_format_), SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_ == nullptr) {
            return false;
        }
        // Full-range RGB in, BT.709 limited range out, matching what the context declares.
        sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_ITU709), 1, sws_getCoefficients(SWS_CS_ITU709), 0, 0,
                                 1 << 16, 1 << 16);
        sws_width_ = frame.width;
        sws_height_ = frame.height;
        sws_source_ = source;
    }
    const std::uint8_t* const planes[] = {frame.data};
    const int strides[] = {frame.stride};
    return sws_scale(sws_, planes, strides, 0, frame.height, target->data, target->linesize) == out_height_;
}

bool VideoEncoder::encode(const FrameView& frame, Time100ns time, std::vector<Packet>& out, std::string& error) {
    if (context_ == nullptr) {
        error = "The encoder is not open";
        return false;
    }
    // The encoder may still hold the last image (lookahead): give it a fresh buffer then.
    if (av_frame_make_writable(sw_frame_) < 0 || !convert(frame, sw_frame_)) {
        error = "Cannot convert the screen image";
        return false;
    }
    AVFrame* send = sw_frame_;
    if (hw_frames_ != nullptr) {
        av_frame_unref(hw_frame_);
        int result = av_hwframe_get_buffer(hw_frames_, hw_frame_, 0);
        if (result >= 0) {
            result = av_hwframe_transfer_data(hw_frame_, sw_frame_, 0);
        }
        if (result < 0) {
            error = "Cannot upload the image to the graphics card (" + av_error(result) + ")";
            return false;
        }
        send = hw_frame_;
    }
    const std::int64_t index = next_index_++;
    send->pts = index;
    // The test frame took the encoder's first keyframe and its packet was dropped: ask for a new
    // one now, or nothing could be kept until the next keyframe a second later.
    send->pict_type = first_frame_ ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    first_frame_ = false;
    times_[index] = time;
    const int result = avcodec_send_frame(context_, send);
    av_frame_unref(hw_frame_);
    if (result < 0) {
        times_.erase(index);
        error = "The encoder failed (" + av_error(result) + ")";
        return false;
    }
    if (!receive(out, error)) {
        return false;
    }
    // An encoder that takes frames but never returns them: stop instead of growing forever.
    if (times_.size() > static_cast<std::size_t>(config_.fps) * 4) {
        error = "The encoder stopped returning video";
        return false;
    }
    return true;
}

bool VideoEncoder::receive(std::vector<Packet>& out, std::string& error) {
    const Time100ns frame_duration = kTicksPerSecond / config_.fps;
    for (;;) {
        const int result = avcodec_receive_packet(context_, packet_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (result < 0) {
            error = "The encoder failed (" + av_error(result) + ")";
            return false;
        }
        const auto it = times_.find(packet_->pts);
        if (it != times_.end()) {
            Packet packet;
            packet.pts = it->second;
            packet.duration = frame_duration;
            packet.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
            packet.data = make_packet_bytes(std::vector<std::uint8_t>(packet_->data, packet_->data + packet_->size));
            out.push_back(std::move(packet));
            // Without B-frames packets come in order: everything up to here is done.
            times_.erase(times_.begin(), std::next(it));
        }
        av_packet_unref(packet_);
    }
}

void VideoEncoder::close() {
    avcodec_free_context(&context_);
    av_buffer_unref(&hw_frames_);
    av_buffer_unref(&hw_device_);
    av_frame_free(&sw_frame_);
    av_frame_free(&hw_frame_);
    av_packet_free(&packet_);
    sws_freeContext(sws_);
    sws_ = nullptr;
    sws_format_ = -1;
    sws_width_ = 0;
    sws_height_ = 0;
    next_index_ = 0;
    first_frame_ = true;
    times_.clear();
    parameters_.reset();
    hardware_ = false;
    name_.clear();
}

}  // namespace bt::linux_app
