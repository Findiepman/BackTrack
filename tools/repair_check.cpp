// Development tool for the repair of clips with the microphone on a second sound track. Not shipped.
//
//   repair_check check <clip.mp4 or folder>
//     prints check_for_repair for the clip, or for every .mp4 in the folder (not its subfolders)
//     with a count and how long it took. Only reads.
//   repair_check repair <source.mp4> <destination.mp4> [--cancel-after <s>]
//     runs repair_clip exactly as the app does and prints the progress and the result; check the
//     file with ffprobe / ffmpeg.

#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Repair.h"
#include "platform/Text.h"

#include <mfapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int usage() {
    std::puts("usage: repair_check check <clip or folder>\n"
              "       repair_check repair <source> <destination> [--cancel-after s]");
    return 2;
}

int check(const std::filesystem::path& target) {
    std::vector<std::filesystem::path> clips;
    std::error_code ec;
    if (std::filesystem::is_directory(target, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(target, ec)) {
            std::wstring extension = entry.path().extension().wstring();
            for (wchar_t& c : extension) c = static_cast<wchar_t>(towlower(c));
            if (entry.is_regular_file(ec) && extension == L".mp4") {
                clips.push_back(entry.path());
            }
        }
    } else {
        clips.push_back(target);
    }
    int needed = 0;
    int not_needed = 0;
    int unreadable = 0;
    const auto started = std::chrono::steady_clock::now();
    for (const std::filesystem::path& clip : clips) {
        const auto result = bt::check_for_repair(clip);
        const std::string name = bt::to_utf8(clip.filename().wstring());
        if (!result) {
            ++unreadable;
            std::printf("unreadable  %s\n", name.c_str());
            continue;
        }
        (result->needed ? needed : not_needed)++;
        std::printf("%-11s %s: %d sound track%s. %s\n", result->needed ? "needed" : "not needed", name.c_str(),
                    result->audio_tracks, result->audio_tracks == 1 ? "" : "s", result->reason.c_str());
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("%zu clips: %d need a repair, %d do not, %d unreadable; %lld ms\n", clips.size(), needed, not_needed,
                unreadable, static_cast<long long>(ms));
    return 0;
}

int repair(const std::filesystem::path& source, const std::filesystem::path& destination, double cancel_after) {
    std::atomic<bool> cancel{false};
    std::jthread canceller;
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
    const auto started = std::chrono::steady_clock::now();
    const bt::RepairResult result = bt::repair_clip(
        source, destination,
        [&](double fraction) {
            const int tenth = static_cast<int>(fraction * 10);
            if (tenth != last_tenth) {
                last_tenth = tenth;
                std::printf("progress %.0f %%\n", fraction * 100.0);
            }
        },
        cancel);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::printf("%s%s\n%s\ntook %lld ms\n", result.ok ? "ok" : "failed", result.cancelled ? " (cancelled)" : "",
                result.message.c_str(), static_cast<long long>(ms));
    return result.ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return usage();
    bt::log::set_console_echo(true);
    bt::log::set_level(bt::LogLevel::Debug);
    const std::wstring command = argv[1];
    if (command == L"check" && argc == 3) {
        return check(argv[2]);
    }
    if (command != L"repair" || argc < 4) return usage();
    double cancel_after = -1.0;
    for (int i = 4; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cancel-after" && i + 1 < argc) {
            cancel_after = _wtof(argv[++i]);
        } else {
            return usage();
        }
    }
    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    const int code = repair(argv[2], argv[3], cancel_after);
    MFShutdown();
    return code;
}
