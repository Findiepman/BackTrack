// Development tool: runs the recording engine (bt::Recorder) without the window, saves one clip and
// exits. Not shipped. It has no single-instance lock, so sound changes can be checked on a PC
// where Backtrack itself is running (the two record side by side).
//
//   record_check <backtrack.ini> <log file> <save after s> <exit after s> [lose capture at s]
//
// The clip goes to the ini's output_dir. Prints the sound meters and the microphone's state every
// second, then the save result. The optional last argument makes the capture behave once as if
// Windows took the screen away (a UAC prompt), to check what recovers afterwards.

#include "core/Config.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/Recorder.h"

#include <mfapi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace {

const char* status_name(bt::MicStatus status) {
    switch (status) {
        case bt::MicStatus::Off: return "off";
        case bt::MicStatus::Starting: return "starting";
        case bt::MicStatus::Recording: return "recording";
        case bt::MicStatus::NoDevice: return "no device";
        case bt::MicStatus::Blocked: return "blocked";
        case bt::MicStatus::Failed: return "failed";
        case bt::MicStatus::OnlyBluetooth: return "only bluetooth";
    }
    return "?";
}

int run(int argc, wchar_t** argv) {
    if (argc != 5 && argc != 6) {
        std::printf("usage: record_check <backtrack.ini> <log file> <save after s> <exit after s> [lose capture at s]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // DuplicateOutput1 needs it
    std::ifstream file{std::filesystem::path(argv[1])};
    std::stringstream text;
    text << file.rdbuf();
    const bt::LoadedSettings loaded = bt::load_settings(text.str());
    bt::log::set_console_echo(true);
    bt::log::init(argv[2], loaded.settings.log_level);
    for (const std::string& warning : loaded.warnings) {
        bt::log::warn("{}", warning);
    }
    const int save_after = _wtoi(argv[3]);
    const int exit_after = _wtoi(argv[4]);
    const int lose_at = argc == 6 ? _wtoi(argv[5]) : 0;

    for (const bt::MicDevice& mic : bt::list_microphones()) {
        std::printf("microphone: %s%s%s\n", mic.name.c_str(), mic.is_default ? " (default)" : "",
                    mic.bluetooth ? " (bluetooth)" : "");
    }

    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    {
        bt::Recorder recorder(loaded.settings, std::filesystem::path(argv[1]).parent_path(),
                              [](const bt::SaveResult& result) {
                                  std::printf("SAVE %s: %s\n", result.ok ? "ok" : "failed", result.message.c_str());
                                  if (result.ok) {
                                      std::printf("CLIP %s\n", bt::to_utf8(result.path.wstring()).c_str());
                                  }
                              });
        recorder.start();
        for (int second = 1; second <= exit_after; ++second) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const bt::AudioLevels levels = recorder.audio_levels();
            std::printf("%2d s: system %.2f, mic %.2f (%s, %s, gate %s%s%s) | %s\n", second, levels.system, levels.mic,
                        status_name(levels.mic_status), levels.mic_name.c_str(), levels.mic_gate_open ? "open" : "closed",
                        levels.mic_fallback ? ", fallback" : "", levels.ptt_blocked ? ", ptt blocked" : "",
                        recorder.status_text().c_str());
            if (second == lose_at) {
                recorder.simulate_capture_loss();
            }
            if (second == save_after) {
                std::string reason;
                if (!recorder.save_clip(reason)) {
                    std::printf("SAVE refused: %s\n", reason.c_str());
                }
            }
        }
        recorder.stop();
    }
    BT_CHECK_HR(MFShutdown());
    bt::log::shutdown();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
}
