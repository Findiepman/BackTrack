// Development tool for the exporter. Not shipped.
//
//   export_check <source.mp4> <destination file or folder> [options]
//     --gif                       animated GIF instead of MP4
//     --height <0|1440|1080|720|480>
//     --quality <high|balanced|small|discord>
//     --budget <bytes>            Fits Discord's limit (default 10000000)
//     --no-audio
//     --in <s> --out <s>          the range (out 0: to the end)
//     --software                  no graphics card
//     --cancel-after <s>          sets the cancel flag after that many seconds
//
// Runs export_clip exactly as the app does and prints the plan, the progress and the result;
// check the file with ffprobe / ffmpeg.

#include "core/ExportPlan.h"
#include "platform/Com.h"
#include "platform/Exporter.h"
#include "platform/Log.h"
#include "platform/Text.h"

#include <mfapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace {

int usage() {
    std::puts("usage: export_check <source> <destination> [--gif] [--height N] [--quality high|balanced|small|discord]\n"
              "                    [--budget bytes] [--no-audio] [--in s] [--out s] [--software] [--cancel-after s]");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return usage();
    bt::log::set_console_echo(true);
    bt::log::set_level(bt::LogLevel::Debug);
    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));

    bt::ExportOptions options;
    double cancel_after = -1.0;
    for (int i = 3; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == L"--gif") {
            options.format = bt::ExportFormat::Gif;
        } else if (arg == L"--no-audio") {
            options.audio = false;
        } else if (arg == L"--software") {
            options.hardware = false;
        } else if (arg == L"--height" && has_value) {
            options.height = _wtoi(argv[++i]);
        } else if (arg == L"--budget" && has_value) {
            options.byte_budget = _wcstoui64(argv[++i], nullptr, 10);
        } else if (arg == L"--in" && has_value) {
            options.in_seconds = _wtof(argv[++i]);
        } else if (arg == L"--out" && has_value) {
            options.out_seconds = _wtof(argv[++i]);
        } else if (arg == L"--cancel-after" && has_value) {
            cancel_after = _wtof(argv[++i]);
        } else if (arg == L"--quality" && has_value) {
            const std::wstring q = argv[++i];
            if (q == L"high") options.quality = bt::ExportQuality::High;
            else if (q == L"balanced") options.quality = bt::ExportQuality::Balanced;
            else if (q == L"small") options.quality = bt::ExportQuality::Small;
            else if (q == L"discord") options.quality = bt::ExportQuality::FitsDiscord;
            else return usage();
        } else {
            return usage();
        }
    }

    // A folder as the destination: the app's naming, "<clip> (export).mp4", "<clip> (export 2).mp4", ...
    std::filesystem::path destination = argv[2];
    std::error_code ec;
    if (std::filesystem::is_directory(destination, ec)) {
        destination = bt::free_export_path(destination, std::filesystem::path(argv[1]).stem().wstring(), options.format);
        std::printf("destination: %s\n", bt::to_utf8(destination.wstring()).c_str());
    }

    const auto source = bt::read_export_source(argv[1]);
    if (!source) {
        std::puts("failed: cannot read the source");
        MFShutdown();
        return 1;
    }
    std::printf("source: %dx%d, %.3f fps, %.3f s, %s, %d kbit/s video\n", source->width, source->height, source->fps,
                source->duration_s, source->has_audio ? "sound" : "no sound", source->video_kbps);
    const bt::ExportPlan plan = bt::plan_export(options, *source);
    std::printf("plan: %dx%d at %.2f fps%s, %d + %d kbit/s%s, %.3f to %.3f s, estimate %llu bytes%s%s%s%s\n",
                plan.width, plan.height, plan.fps, plan.every_frame ? "" : " (frames dropped)", plan.video_kbps,
                plan.audio_kbps, plan.constant_bitrate ? " CBR" : "", plan.start_s, plan.end_s,
                static_cast<unsigned long long>(plan.estimated_bytes), plan.note.empty() ? "" : "\n  note: ",
                plan.note.c_str(), plan.problem.empty() ? "" : "\n  problem: ", plan.problem.c_str());

    std::atomic<bool> cancel{false};
    std::jthread canceller;
    const auto started = std::chrono::steady_clock::now();
    if (cancel_after >= 0.0) {
        canceller = std::jthread([&cancel, cancel_after](std::stop_token stop) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(cancel_after);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < until) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!stop.stop_requested()) {
                std::puts("cancelling");
                cancel = true;
            }
        });
    }
    int last_tenth = -1;
    const bt::ExportResult r = bt::export_clip(
        argv[1], destination, options,
        [&](double fraction) {
            const int tenth = static_cast<int>(fraction * 10);
            if (tenth != last_tenth) {
                last_tenth = tenth;
                std::printf("progress %.0f%%\n", fraction * 100);
            }
        },
        cancel);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (canceller.joinable()) {
        canceller.request_stop();
        canceller.join();
    }
    std::printf("%s: %s\n", r.ok ? "ok" : (r.cancelled ? "cancelled" : "failed"), r.message.c_str());
    if (r.ok) {
        std::printf("file: %s, %llu bytes, %dx%d, %.2f fps, %.3f s, %s\n", bt::to_utf8(r.path.wstring()).c_str(),
                    static_cast<unsigned long long>(r.bytes), r.width, r.height, r.fps, r.seconds,
                    r.hardware ? "hardware" : "software");
    }
    std::printf("took %lld ms\n", static_cast<long long>(ms));
    MFShutdown();
    return r.ok ? 0 : 1;
}
