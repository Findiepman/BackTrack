#include "linux/Media.h"

#include "core/Clock.h"
#include "core/TrimPlan.h"
#include "linux/ClipFile.h"
#include "linux/Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

namespace bt::linux_app {

namespace {

constexpr AVRational kTicks{1, static_cast<int>(kTicksPerSecond)};
// Far beyond any clip, and small enough that seconds * 10^7 fits in an int64 with room to spare.
constexpr double kMaxSeconds = 100.0 * 24 * 3600;

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

struct InputDeleter {
    void operator()(AVFormatContext* context) const { avformat_close_input(&context); }
};
using Input = std::unique_ptr<AVFormatContext, InputDeleter>;

struct CodecDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
using Decoder = std::unique_ptr<AVCodecContext, CodecDeleter>;

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;

struct SwsDeleter {
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};

Input open_input(const std::string& path) {
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, path.c_str(), nullptr, nullptr) < 0) {
        return nullptr;
    }
    Input input(context);
    if (avformat_find_stream_info(input.get(), nullptr) < 0) {
        return nullptr;
    }
    return input;
}

double duration_of(const AVFormatContext* input, const AVStream* video) {
    if (input->duration > 0) {
        return static_cast<double>(input->duration) / AV_TIME_BASE;
    }
    if (video != nullptr && video->duration > 0) {
        return static_cast<double>(video->duration) * av_q2d(video->time_base);
    }
    return 0.0;
}

// The open video stream of a file and a decoder for it, for still pictures.
struct VideoReader {
    Input input;
    AVStream* stream = nullptr;
    Decoder decoder;
    double duration_s = 0.0;

    bool open(const std::string& path) {
        input = open_input(path);
        if (!input) {
            return false;
        }
        const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) {
            return false;
        }
        stream = input->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            return false;
        }
        decoder.reset(avcodec_alloc_context3(codec));
        if (!decoder || avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0) {
            return false;
        }
        // Frame threads hold frames back until several packets went in: one picture after a seek
        // would cost a whole GOP of decoding. Slices only.
        decoder->thread_type = FF_THREAD_SLICE;
        decoder->pkt_timebase = stream->time_base;
        if (avcodec_open2(decoder.get(), codec, nullptr) < 0) {
            return false;
        }
        duration_s = duration_of(input.get(), stream);
        return true;
    }

    // The first picture from the keyframe at or before `seconds`.
    Frame frame_at(double seconds) {
        const std::int64_t start = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        const std::int64_t target =
            start + av_rescale_q(static_cast<std::int64_t>(std::clamp(seconds, 0.0, kMaxSeconds) * kTicksPerSecond),
                                 kTicks, stream->time_base);
        if (av_seek_frame(input.get(), stream->index, target, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(input.get(), stream->index, start, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(decoder.get());
        Frame frame(av_frame_alloc());
        Packet packet(av_packet_alloc());
        if (!frame || !packet) {
            return nullptr;
        }
        bool flushed = false;
        while (true) {
            const int received = avcodec_receive_frame(decoder.get(), frame.get());
            if (received == 0) {
                return frame;
            }
            if (received != AVERROR(EAGAIN) || flushed) {
                return nullptr;
            }
            const int read = av_read_frame(input.get(), packet.get());
            if (read < 0) {
                avcodec_send_packet(decoder.get(), nullptr);  // the end: whatever the decoder still holds
                flushed = true;
                continue;
            }
            if (packet->stream_index == stream->index) {
                avcodec_send_packet(decoder.get(), packet.get());
            }
            av_packet_unref(packet.get());
        }
    }
};

Still scale_to_fill(AVFrame* frame, int width, int height) {
    Still still;
    if (frame->width <= 0 || frame->height <= 0 || width <= 0 || height <= 0) {
        return still;
    }
    // Cut off what sticks out at the sides (or top and bottom) so the picture fills the box.
    const double scale = std::max(static_cast<double>(width) / frame->width, static_cast<double>(height) / frame->height);
    const int shown_w = std::clamp(static_cast<int>(std::lround(width / scale)), 1, frame->width);
    const int shown_h = std::clamp(static_cast<int>(std::lround(height / scale)), 1, frame->height);
    frame->crop_left = static_cast<std::size_t>((frame->width - shown_w) / 2);
    frame->crop_right = static_cast<std::size_t>(frame->width - shown_w) - frame->crop_left;
    frame->crop_top = static_cast<std::size_t>((frame->height - shown_h) / 2);
    frame->crop_bottom = static_cast<std::size_t>(frame->height - shown_h) - frame->crop_top;
    if (av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED) < 0) {
        return still;
    }
    const std::unique_ptr<SwsContext, SwsDeleter> sws(
        sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), width, height,
                       AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr));
    if (!sws) {
        return still;
    }
    use_frame_colors(sws.get(), frame);
    still.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    std::uint8_t* planes[4] = {still.pixels.data(), nullptr, nullptr, nullptr};
    const int strides[4] = {width * 4, 0, 0, 0};
    if (sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, planes, strides) != height) {
        still.pixels.clear();
        return still;
    }
    for (std::size_t i = 3; i < still.pixels.size(); i += 4) {
        still.pixels[i] = 0xFF;  // RGB32 wants an opaque alpha byte
    }
    still.width = width;
    still.height = height;
    return still;
}

const char* problem_text(TrimProblem problem) {
    switch (problem) {
        case TrimProblem::NoVideo: return "the file has no video frames";
        case TrimProblem::NoKeyframe: return "the video has no keyframe to start from";
        case TrimProblem::Reordered:
            return "this video uses B-frames, which cannot be trimmed without re-encoding (Backtrack's own clips can)";
        case TrimProblem::EmptyRange: return "the selection is empty";
        case TrimProblem::StartPastEnd: return "the selection starts after the end of the video";
        case TrimProblem::None: break;
    }
    return "";
}

std::int64_t packet_time(const AVPacket* packet) {
    return packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
}

struct OutputDeleter {
    void operator()(AVFormatContext* context) const {
        if (context->pb != nullptr) {
            avio_closep(&context->pb);
        }
        avformat_free_context(context);
    }
};

}  // namespace

void use_frame_colors(SwsContext* sws, const AVFrame* frame) {
    int matrix = SWS_CS_DEFAULT;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: matrix = SWS_CS_ITU601; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: matrix = SWS_CS_BT2020; break;
        case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_FCC: matrix = SWS_CS_FCC; break;
        default:
            // Untagged: what players assume, HD is BT.709 and SD is BT.601.
            matrix = frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
            break;
    }
    const int full_range = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    // RGB out is always full range; brightness, contrast and saturation stay neutral.
    sws_setColorspaceDetails(sws, sws_getCoefficients(matrix), full_range, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);
}

MediaFacts probe_media(const std::string& path) {
    MediaFacts facts;
    const Input input = open_input(path);
    if (!input) {
        return facts;
    }
    const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        return facts;
    }
    const AVStream* video = input->streams[index];
    facts.duration_s = duration_of(input.get(), video);
    facts.width = video->codecpar->width;
    facts.height = video->codecpar->height;
    facts.ok = facts.width > 0 && facts.height > 0 && facts.duration_s > 0.0;
    return facts;
}

Still read_still(const std::string& path, double fraction, int width, int height) {
    VideoReader reader;
    if (!reader.open(path)) {
        return {};
    }
    Frame frame = reader.frame_at(std::clamp(fraction, 0.0, 1.0) * reader.duration_s);
    return frame ? scale_to_fill(frame.get(), width, height) : Still{};
}

std::vector<Still> read_filmstrip(const std::string& path, int count, int width, int height) {
    std::vector<Still> stills;
    VideoReader reader;
    if (count <= 0 || !reader.open(path) || reader.duration_s <= 0.0) {
        return stills;
    }
    for (int i = 0; i < count; ++i) {
        Frame frame = reader.frame_at((i + 0.5) / count * reader.duration_s);
        Still still = frame ? scale_to_fill(frame.get(), width, height) : Still{};
        if (still.pixels.empty()) {
            break;
        }
        stills.push_back(std::move(still));
    }
    return stills;
}

TrimResult trim_clip(const std::string& source, const std::string& destination, double in_s, double out_s) {
    TrimResult result;
    if (!std::isfinite(in_s) || !std::isfinite(out_s)) {
        result.error = problem_text(TrimProblem::EmptyRange);
        return result;
    }
    const Time100ns in = static_cast<Time100ns>(std::clamp(in_s, 0.0, kMaxSeconds) * kTicksPerSecond);
    const Time100ns out = static_cast<Time100ns>(std::clamp(out_s, 0.0, kMaxSeconds) * kTicksPerSecond);

    // Pass 1: every packet's time, to plan the cut the same way as on Windows.
    Input input = open_input(source);
    if (!input) {
        result.error = "the clip could not be opened";
        return result;
    }
    const int video_index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0) {
        result.error = problem_text(TrimProblem::NoVideo);
        return result;
    }
    const int audio_index = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, video_index, nullptr, 0);
    AVStream* video = input->streams[video_index];
    AVStream* audio = audio_index >= 0 ? input->streams[audio_index] : nullptr;
    // Times relative to the start of the file, as the player and the trim handles count them.
    const std::int64_t video_zero = video->start_time != AV_NOPTS_VALUE ? video->start_time : 0;
    const Time100ns zero = av_rescale_q(video_zero, video->time_base, kTicks);

    std::vector<TrimFrame> frames;
    std::vector<Time100ns> frame_ends;
    std::vector<Time100ns> sound;
    {
        Packet packet(av_packet_alloc());
        if (!packet) {
            result.error = "out of memory";
            return result;
        }
        while (av_read_frame(input.get(), packet.get()) >= 0) {
            const std::int64_t at = packet_time(packet.get());
            if (packet->stream_index == video_index && at != AV_NOPTS_VALUE) {
                const Time100ns pts = av_rescale_q(at, video->time_base, kTicks) - zero;
                frames.push_back(TrimFrame{pts, (packet->flags & AV_PKT_FLAG_KEY) != 0});
                frame_ends.push_back(pts + av_rescale_q(packet->duration, video->time_base, kTicks));
            } else if (audio != nullptr && packet->stream_index == audio_index && at != AV_NOPTS_VALUE) {
                // A packet without a time cannot be placed: it is not counted here nor copied below.
                sound.push_back(av_rescale_q(at, audio->time_base, kTicks) - zero);
            }
            av_packet_unref(packet.get());
        }
    }
    const TrimPlan plan = plan_trim(frames, in, out);
    if (plan.problem != TrimProblem::None) {
        result.error = problem_text(plan.problem);
        return result;
    }
    const TrimRange sound_range = trim_audio(sound, plan.start, out);

    // Pass 2: copy the chosen packets as they are, moved so the cut starts at 0.
    input = open_input(source);
    if (!input) {
        result.error = "the clip could not be opened";
        return result;
    }
    video = input->streams[video_index];
    audio = audio_index >= 0 ? input->streams[audio_index] : nullptr;
    const bool with_audio = audio != nullptr && sound_range.end > sound_range.first;

    const std::filesystem::path final_path(destination);
    const std::string temp_path = (final_path.parent_path() / ("." + final_path.filename().string() + ".part")).string();
    AVFormatContext* raw = nullptr;
    int code = avformat_alloc_output_context2(&raw, nullptr, "mp4", temp_path.c_str());
    if (code < 0 || raw == nullptr) {
        result.error = "cannot make an MP4 (" + av_error(code) + ")";
        return result;
    }
    const std::unique_ptr<AVFormatContext, OutputDeleter> output(raw);
    AVStream* video_out = avformat_new_stream(output.get(), nullptr);
    AVStream* audio_out = with_audio ? avformat_new_stream(output.get(), nullptr) : nullptr;
    if (video_out == nullptr || (with_audio && audio_out == nullptr) ||
        avcodec_parameters_copy(video_out->codecpar, video->codecpar) < 0 ||
        (with_audio && avcodec_parameters_copy(audio_out->codecpar, audio->codecpar) < 0)) {
        result.error = "out of memory";
        return result;
    }
    video_out->codecpar->codec_tag = 0;
    video_out->time_base = video->time_base;
    if (with_audio) {
        audio_out->codecpar->codec_tag = 0;
        audio_out->time_base = audio->time_base;
    }
    code = avio_open(&output->pb, temp_path.c_str(), AVIO_FLAG_WRITE);
    if (code < 0) {
        result.error = "cannot write in the clips folder (" + av_error(code) + ")";
        return result;
    }
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    code = avformat_write_header(output.get(), &options);
    av_dict_free(&options);
    std::error_code ec;
    if (code < 0) {
        result.error = "cannot write the MP4 header (" + av_error(code) + ")";
        avio_closep(&output->pb);
        std::filesystem::remove(temp_path, ec);
        return result;
    }

    const Time100ns shift = plan.start + zero;  // back on the source's own timeline
    Packet packet(av_packet_alloc());
    std::size_t video_seen = 0;
    std::size_t sound_seen = 0;
    bool ok = packet != nullptr;
    while (ok && (video_seen < plan.end || (with_audio && sound_seen < sound_range.end)) &&
           av_read_frame(input.get(), packet.get()) >= 0) {
        AVStream* to = nullptr;
        AVRational from_base{};
        if (packet->stream_index == video_index && packet_time(packet.get()) != AV_NOPTS_VALUE) {
            const std::size_t i = video_seen++;
            if (i >= plan.first && i < plan.end) {
                to = video_out;
                from_base = video->time_base;
            }
        } else if (audio != nullptr && packet->stream_index == audio_index && packet_time(packet.get()) != AV_NOPTS_VALUE) {
            const std::size_t i = sound_seen++;
            if (with_audio && i >= sound_range.first && i < sound_range.end) {
                to = audio_out;
                from_base = audio->time_base;
            }
        }
        if (to == nullptr) {
            av_packet_unref(packet.get());
            continue;
        }
        const std::int64_t moved = av_rescale_q(shift, kTicks, from_base);
        if (packet->pts != AV_NOPTS_VALUE) {
            packet->pts -= moved;
        }
        if (packet->dts != AV_NOPTS_VALUE) {
            packet->dts -= moved;
        }
        av_packet_rescale_ts(packet.get(), from_base, to->time_base);
        packet->stream_index = to->index;
        packet->pos = -1;
        code = av_interleaved_write_frame(output.get(), packet.get());
        if (code < 0) {
            ok = false;
            result.error = "writing the trimmed clip failed (" + av_error(code) + ")";
        }
    }
    if (ok) {
        code = av_write_trailer(output.get());
        if (code < 0) {
            ok = false;
            result.error = "finishing the trimmed clip failed (" + av_error(code) + ")";
        }
    }
    avio_closep(&output->pb);
    if (!ok) {
        std::filesystem::remove(temp_path, ec);
        return result;
    }
    if (!publish_file(temp_path, destination, result.error)) {
        return result;
    }
    result.ok = true;
    result.start_s = static_cast<double>(plan.start) / kTicksPerSecond;
    const Time100ns last_end = std::max(frame_ends[plan.end - 1], frames[plan.end - 1].pts);
    result.seconds = static_cast<double>(last_end - plan.start) / kTicksPerSecond;
    result.bytes = std::filesystem::file_size(destination, ec);
    if (ec) {
        result.bytes = 0;
    }
    return result;
}

}  // namespace bt::linux_app
