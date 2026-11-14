#include "linux/ClipFile.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>

namespace bt::linux_app {

namespace {

constexpr AVRational kTicks{1, static_cast<int>(kTicksPerSecond)};

std::string av_error(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

struct Output {
    AVFormatContext* context = nullptr;
    bool opened = false;
    ~Output() {
        if (context != nullptr) {
            if (opened) {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
        }
    }
};

}  // namespace

bool write_clip(const std::string& path, const CodecParameters& video, const CodecParameters& audio,
                const std::vector<Packet>& video_packets, const std::vector<Packet>& audio_packets, const ClipPlan& plan,
                std::string& error) {
    if (!video || plan.samples.empty()) {
        error = "Nothing to write";
        return false;
    }
    const bool with_audio = audio && plan.audio_samples > 0;
    const std::filesystem::path final_path(path);
    const std::filesystem::path temp_path = final_path.parent_path() / ("." + final_path.filename().string() + ".part");

    Output out;
    int result = avformat_alloc_output_context2(&out.context, nullptr, "mp4", temp_path.c_str());
    if (result < 0 || out.context == nullptr) {
        error = "Cannot make an MP4 (" + av_error(result) + ")";
        return false;
    }
    AVStream* video_stream = avformat_new_stream(out.context, nullptr);
    AVStream* audio_stream = with_audio ? avformat_new_stream(out.context, nullptr) : nullptr;
    if (video_stream == nullptr || (with_audio && audio_stream == nullptr) ||
        avcodec_parameters_copy(video_stream->codecpar, video.get()) < 0 ||
        (with_audio && avcodec_parameters_copy(audio_stream->codecpar, audio.get()) < 0)) {
        error = "Out of memory";
        return false;
    }
    video_stream->codecpar->codec_tag = 0;
    video_stream->time_base = AVRational{1, 90000};
    if (with_audio) {
        audio_stream->codecpar->codec_tag = 0;
        audio_stream->time_base = AVRational{1, audio->sample_rate > 0 ? audio->sample_rate : 48000};
    }

    result = avio_open(&out.context->pb, temp_path.c_str(), AVIO_FLAG_WRITE);
    if (result < 0) {
        error = "Cannot write in the clips folder (" + av_error(result) + ")";
        return false;
    }
    out.opened = true;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    result = avformat_write_header(out.context, &options);
    av_dict_free(&options);
    if (result < 0) {
        error = "Cannot write the MP4 header (" + av_error(result) + ")";
        std::filesystem::remove(temp_path);
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    bool ok = packet != nullptr;
    for (const ClipSample& sample : plan.samples) {
        if (!ok) {
            break;
        }
        if (sample.audio && (!with_audio || sample.track != 0)) {
            continue;  // one sound track on Linux
        }
        const Packet& source = sample.audio ? audio_packets[sample.index] : video_packets[sample.index];
        AVStream* stream = sample.audio ? audio_stream : video_stream;
        if (av_new_packet(packet, static_cast<int>(source.size())) < 0) {
            ok = false;
            error = "Out of memory";
            break;
        }
        std::memcpy(packet->data, source.data->data(), source.size());
        packet->stream_index = stream->index;
        // The muxer may have changed time_base in write_header; rescale against the final one.
        packet->pts = packet->dts = av_rescale_q(sample.time, kTicks, stream->time_base);
        packet->duration = av_rescale_q(sample.duration, kTicks, stream->time_base);
        packet->flags = sample.keyframe ? AV_PKT_FLAG_KEY : 0;
        result = av_interleaved_write_frame(out.context, packet);  // takes the packet's data
        if (result < 0) {
            ok = false;
            error = "Writing the clip failed (" + av_error(result) + ")";
        }
    }
    av_packet_free(&packet);
    if (ok) {
        result = av_write_trailer(out.context);
        if (result < 0) {
            ok = false;
            error = "Finishing the clip failed (" + av_error(result) + ")";
        }
    }
    avio_closep(&out.context->pb);
    out.opened = false;
    std::error_code ec;
    if (ok) {
        // link() never replaces an existing file (a clip saved in the same second by another
        // save); rename() is the fallback for file systems without hard links (exFAT, FAT32).
        if (link(temp_path.c_str(), final_path.c_str()) == 0) {
            std::filesystem::remove(temp_path, ec);
        } else if (errno == EEXIST) {
            ok = false;
            error = "A clip with this name appeared while saving";
        } else {
            std::filesystem::rename(temp_path, final_path, ec);
            if (ec) {
                ok = false;
                error = "Cannot rename the finished clip: " + ec.message();
            }
        }
    }
    if (!ok) {
        std::filesystem::remove(temp_path, ec);
    }
    return ok;
}

}  // namespace bt::linux_app
