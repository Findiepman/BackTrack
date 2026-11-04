// Development harness for M1 and M2 (build prompt §11). Records the configured monitor through the
// real capture, colour conversion and H.264 encoder into the ring buffer. The hotkey writes the
// last seconds from the buffer to a raw .h264 file (M2 proof); --dump-after dumps one NV12
// frame and checks its colours (M1 proof). M3 replaces this file with the tray application.
//
// Usage: backtrack.exe [--save-after <s>] [--dump-after <s>] [--exit-after <s>] [--lose-capture-every <s>]

#include "core/ClipBuffer.h"
#include "core/Config.h"
#include "core/H264.h"
#include "core/Hotkey.h"
#include "platform/Com.h"
#include "platform/Log.h"
#include "platform/QpcClock.h"
#include "platform/ScreenCapture.h"
#include "platform/VideoEncoder.h"
#include "tools/nv12_compare.h"

#include <mfapi.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// core/Hotkey uses the Win32 values without including <windows.h>; make sure they agree.
static_assert(bt::hotkey_mod::kAlt == MOD_ALT && bt::hotkey_mod::kCtrl == MOD_CONTROL &&
              bt::hotkey_mod::kShift == MOD_SHIFT && bt::hotkey_mod::kWin == MOD_WIN);
static_assert(VK_F1 == 0x70 && VK_F12 == 0x7B && VK_F24 == 0x87 && VK_NUMPAD0 == 0x60 && VK_HOME == 0x24 &&
              VK_END == 0x23 && VK_INSERT == 0x2D && VK_DELETE == 0x2E && VK_PRIOR == 0x21 && VK_NEXT == 0x22 &&
              VK_SPACE == 0x20 && VK_PAUSE == 0x13 && VK_SNAPSHOT == 0x2C);

constexpr UINT kMsgDumpReady = WM_APP + 1;
constexpr UINT kMsgCaptureFailed = WM_APP + 2;
constexpr int kHotkeyId = 1;
constexpr UINT kMillisecondsPerSecond = 1000;

// Windows kills the process 5 s after a close/logoff/shutdown event; leave margin for that.
constexpr DWORD kCloseWaitMs = 4000;

// §3.1 hard byte cap: room for the clip plus slack at twice the configured bitrate. CBR stays
// near the bitrate, so the cap is only reached when keyframes go missing.
constexpr std::size_t kByteCapSlackSeconds = 2;
constexpr std::size_t kByteCapBitrateFactor = 2;
constexpr std::size_t kBitsPerByte = 8;
constexpr std::size_t kBitsPerKilobit = 1000;

// Mean error per channel (0-255) below which the NV12 frame counts as matching the desktop.
// Chroma subsampling alone costs 1-3 on text-heavy screens; a wrong matrix or range costs 5+.
constexpr double kColourPassThreshold = 4.0;

DWORD g_main_thread = 0;
HANDLE g_finished = nullptr;  // set when run() has cleaned up

BOOL WINAPI on_console_ctrl(DWORD event) {
    if (!PostThreadMessageW(g_main_thread, WM_QUIT, 0, 0)) {
        return FALSE;  // let Windows apply its default (end the process) rather than lose the press
    }
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        // Returning ends the process at once; wait so capture stops and the log is complete.
        WaitForSingleObject(g_finished, kCloseWaitMs);
    }
    return TRUE;
}

std::string path_text(const fs::path& path) {
    // path::string() uses the ANSI code page and throws for names it cannot represent.
    return bt::to_utf8(path.wstring());
}

fs::path exe_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        BT_CHECK_WIN32(length != 0);
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        return std::nullopt;
    }
    return text.str();
}

// Notepad saves as UTF-8, "Unicode" (UTF-16 LE), "Unicode big endian" or ANSI. The parser wants
// UTF-8, so the others are converted here instead of failing on every line.
std::string ini_text_as_utf8(const std::string& bytes) {
    const bool utf16le = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                         static_cast<unsigned char>(bytes[1]) == 0xFE;
    const bool utf16be = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
                         static_cast<unsigned char>(bytes[1]) == 0xFF;
    if (utf16le || utf16be) {
        std::wstring wide;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            const auto lo = static_cast<unsigned char>(bytes[utf16le ? i : i + 1]);
            const auto hi = static_cast<unsigned char>(bytes[utf16le ? i + 1 : i]);
            wide.push_back(static_cast<wchar_t>(lo | (hi << 8)));
        }
        bt::log::info("backtrack.ini is saved as UTF-16, reading it as such");
        return bt::to_utf8(wide);
    }
    const int valid_utf8 = bytes.empty() ? 1
        : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (valid_utf8 == 0) {
        // Not UTF-8: an "ANSI" save with an accented path. Read it in the system code page.
        const int length = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length);
        bt::log::info("backtrack.ini is not UTF-8, reading it in the system code page");
        return bt::to_utf8(wide);
    }
    return bytes;
}

bt::Settings load_config(const fs::path& ini_path) {
    std::string text;
    std::error_code ec;
    if (!fs::exists(ini_path, ec) && !ec) {
        std::ofstream out(ini_path, std::ios::binary);
        out << bt::default_ini_text();
        if (out) {
            bt::log::info("Wrote default settings to {}", path_text(ini_path));
        } else {
            bt::log::warn("Could not write default settings to {}, using built-in defaults", path_text(ini_path));
        }
        text = std::string(bt::default_ini_text());
    } else if (const auto bytes = read_file(ini_path)) {
        text = ini_text_as_utf8(*bytes);
    } else {
        // Never overwrite a file we merely failed to read (locked by a sync tool, no permission).
        bt::log::warn("Could not read {}, using built-in defaults for this run", path_text(ini_path));
    }
    bt::LoadedSettings loaded = bt::load_settings(text);
    for (const std::string& warning : loaded.warnings) {
        bt::log::warn("backtrack.ini {}", warning);
    }
    return loaded.settings;
}

struct Options {
    UINT save_after_s = 0;
    UINT dump_after_s = 0;
    UINT exit_after_s = 0;
    UINT lose_capture_every_s = 0;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring name = argv[i];
        const UINT value = static_cast<UINT>(std::wcstoul(argv[i + 1], nullptr, 10));
        if (name == L"--save-after") options.save_after_s = value;
        else if (name == L"--dump-after") options.dump_after_s = value;
        else if (name == L"--exit-after") options.exit_after_s = value;
        else if (name == L"--lose-capture-every") options.lose_capture_every_s = value;
        else bt::log::warn("Unknown option {}", bt::to_utf8(name));
    }
    return options;
}

struct FrameDump {
    std::vector<std::uint8_t> nv12;
    std::vector<std::uint8_t> bgra;
    bt::VideoSize size;
};

void write_dump(const fs::path& folder, const FrameDump& dump) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::wstring stem = std::format(L"frame_{:04}{:02}{:02}_{:02}{:02}{:02}_{}x{}", t.wYear, t.wMonth, t.wDay,
                                          t.wHour, t.wMinute, t.wSecond, dump.size.width, dump.size.height);
    const fs::path nv12_path = folder / (stem + L".nv12");
    {
        std::ofstream file(nv12_path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(dump.nv12.data()), static_cast<std::streamsize>(dump.nv12.size()));
        if (!file) {
            bt::log::error("Could not write {}", path_text(nv12_path));
            return;
        }
    }
    const fs::path decoded_path = folder / (stem + L"_decoded.bmp");
    if (!bt::tools::write_bmp24(decoded_path, bt::tools::nv12_to_bgr(dump.nv12, dump.size.width, dump.size.height),
                                dump.size.width, dump.size.height)) {
        bt::log::error("Could not write {}", path_text(decoded_path));
        return;
    }
    bt::log::info("Dumped {} (raw NV12, {}x{}) and {} (decoded as BT.709 limited)", path_text(nv12_path),
                  dump.size.width, dump.size.height, path_text(decoded_path));

    if (dump.bgra.empty()) {
        bt::log::info("No reference image (rotated monitor), colour check skipped");
        return;
    }
    const fs::path reference_path = folder / (stem + L"_desktop.bmp");
    if (!bt::tools::write_bmp24(reference_path, bt::tools::bgra_to_bgr(dump.bgra), dump.size.width, dump.size.height)) {
        bt::log::error("Could not write {}", path_text(reference_path));
    }
    const auto report = bt::tools::compare_nv12_with_bgra(dump.nv12, dump.bgra, dump.size.width, dump.size.height);
    if (report.fits.empty()) {
        bt::log::error("Colour check FAILED: frame buffers do not match {}x{} (NV12 {} bytes, BGRA {} bytes)",
                       dump.size.width, dump.size.height, dump.nv12.size(), dump.bgra.size());
        return;
    }
    for (const auto& fit : report.fits) {
        bt::log::info("  as {:<15} mean error R {:.2f} G {:.2f} B {:.2f}", fit.interpretation->name,
                      fit.mean_abs_error[0], fit.mean_abs_error[1], fit.mean_abs_error[2]);
    }
    const auto& best = report.fits.front();
    const bool pass = std::string_view(best.interpretation->name) == "BT.709 limited" &&
                      best.overall < kColourPassThreshold && report.luma_min >= 16 && report.luma_max <= 235;
    bt::log::info("Colour check {}: best match {}, mean error {:.2f}, luma range {}-{} (limited range is 16-235)",
                  pass ? "PASSED" : "FAILED", best.interpretation->name, best.overall, report.luma_min,
                  report.luma_max);
}

// M2 proof: the last clip_seconds from the ring buffer, written as a raw H.264 stream that any
// player can open. Also checks what §3.1 promises: starts on a keyframe with SPS/PPS, one keyframe
// per second, timestamps always increasing.
void save_clip_or_throw(const fs::path& folder, const bt::ClipBuffer& buffer, int clip_seconds) {
    const bt::Time100ns now = bt::qpc_now();
    const auto copy_started = std::chrono::steady_clock::now();
    const std::vector<bt::Packet> clip = buffer.extract(now - bt::seconds_to_100ns(clip_seconds), now);
    const auto copy_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - copy_started).count();
    if (clip.empty()) {
        bt::log::warn("Nothing to save yet: the buffer has no keyframe");
        return;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const fs::path path = folder / std::format(L"clip_{:04}{:02}{:02}_{:02}{:02}{:02}.h264", t.wYear, t.wMonth,
                                               t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::size_t bytes = 0;
    {
        std::ofstream file(path, std::ios::binary);
        for (const bt::Packet& p : clip) {
            file.write(reinterpret_cast<const char*>(p.data.data()), static_cast<std::streamsize>(p.data.size()));
            bytes += p.data.size();
        }
        if (!file) {
            bt::log::error("Could not write {}", path_text(path));
            return;
        }
    }

    int keyframes = 0;
    bool increasing = true;
    bt::Time100ns min_gap = 0;
    bt::Time100ns max_gap = 0;
    bt::Time100ns last_keyframe = 0;
    for (std::size_t i = 0; i < clip.size(); ++i) {
        if (i > 0 && clip[i].pts <= clip[i - 1].pts) {
            increasing = false;
        }
        if (clip[i].keyframe) {
            if (keyframes > 0) {
                const bt::Time100ns gap = clip[i].pts - last_keyframe;
                min_gap = keyframes == 1 ? gap : std::min(min_gap, gap);
                max_gap = keyframes == 1 ? gap : std::max(max_gap, gap);
            }
            last_keyframe = clip[i].pts;
            ++keyframes;
        }
    }
    const bt::h264::AccessUnitInfo first = bt::h264::inspect(clip.front().data);
    const bt::Time100ns length = clip.back().pts + clip.back().duration - clip.front().pts;
    const auto as_seconds = [](bt::Time100ns v) { return static_cast<double>(v) / bt::kTicksPerSecond; };
    bt::log::info("Saved {}", path_text(path));
    bt::log::info("  {:.3f} s ({} asked), {} packets, {:.1f} MB, copied out of the buffer in {} ms", as_seconds(length),
                  clip_seconds, clip.size(), static_cast<double>(bytes) / (1024.0 * 1024.0), copy_ms);
    bt::log::info("  starts on a keyframe: {}, with SPS {} and PPS {}; {} keyframes, every {:.3f} to {:.3f} s; "
                  "timestamps always increasing: {}",
                  clip.front().keyframe && first.has_idr, first.has_sps, first.has_pps, keyframes,
                  as_seconds(min_gap), as_seconds(max_gap), increasing);
}

// A failed save (out of memory copying a long clip, disk full) is reported, never fatal.
void save_clip(const fs::path& folder, const bt::ClipBuffer& buffer, int clip_seconds) {
    try {
        save_clip_or_throw(folder, buffer, clip_seconds);
    } catch (const std::exception& e) {
        bt::log::error("Saving the clip failed: {}", e.what());
    }
}

void disable_quick_edit() {
    // With QuickEdit on, clicking the console pauses all output; the capture thread would then
    // block on its next log line and stop ticking.
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        SetConsoleMode(input, (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    }
}

int run(int argc, wchar_t** argv) {
    // DuplicateOutput1 refuses processes that are not per-monitor DPI aware, and DPI virtualisation
    // would report scaled monitor sizes.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        bt::log::warn("SetProcessDpiAwarenessContext failed (error {})", GetLastError());
    }
    SetConsoleOutputCP(CP_UTF8);
    disable_quick_edit();
    bt::log::set_console_echo(true);

    // The message queue must exist before Ctrl+C can post WM_QUIT to it.
    MSG peek{};
    PeekMessageW(&peek, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_main_thread = GetCurrentThreadId();
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);

    const fs::path exe_dir = exe_directory();
    const bt::Settings settings = load_config(exe_dir / L"backtrack.ini");
    bt::log::init(exe_dir / L"backtrack.log", settings.log_level);
    bt::log::info("Backtrack development harness starting");
    const Options options = parse_options(argc, argv);

    bt::ComApartment com(COINIT_MULTITHREADED);
    BT_CHECK_HR(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    struct MfShutdown {
        ~MfShutdown() { MFShutdown(); }
    } mf_shutdown;

    const bt::Hotkey hotkey = *bt::parse_hotkey(settings.save_hotkey).hotkey;
    const bool hotkey_ok = RegisterHotKey(nullptr, kHotkeyId, hotkey.modifiers | MOD_NOREPEAT, hotkey.virtual_key);
    if (hotkey_ok) {
        bt::log::info("Press {} to save the last {} seconds as .h264, Ctrl+C to quit", settings.save_hotkey,
                      settings.clip_seconds);
    } else {
        const HRESULT error = HRESULT_FROM_WIN32(GetLastError());
        bt::log::warn("Cannot register {} as a hotkey: {}. Use --dump-after instead", settings.save_hotkey,
                      bt::hresult_text(error));
    }

    std::atomic<bool> dump_requested{false};
    std::mutex dump_mutex;
    std::optional<FrameDump> pending_dump;
    bt::ScreenCapture* capture_ptr = nullptr;

    // At the config maximum (300 s at 100 Mbit/s) this is 7.5 GB, so it needs a 64-bit size_t;
    // CMakeLists.txt refuses 32-bit builds.
    static_assert(sizeof(std::size_t) == 8);
    const std::size_t byte_cap = (static_cast<std::size_t>(settings.clip_seconds) + kByteCapSlackSeconds) *
                                 static_cast<std::size_t>(settings.video_bitrate_kbps) * kBitsPerKilobit / kBitsPerByte *
                                 kByteCapBitrateFactor;
    bt::ClipBuffer video_buffer(bt::seconds_to_100ns(settings.clip_seconds), byte_cap);
    std::unique_ptr<bt::VideoEncoder> encoder;  // created, used and replaced on the capture thread
    bool encoding_failed = false;  // capture thread only
    bt::EncoderPreference preference = settings.encoder;  // capture thread only; drops to Software on failure

    auto on_frame = [&](IMFSample* sample, bt::Time100ns pts) {
        if (!encoding_failed) {
            try {
                const bt::VideoSize size = capture_ptr->frame_size();
                if (encoder && encoder->failed()) {
                    if (!encoder->hardware()) {
                        throw std::runtime_error("the software video encoder stopped working");
                    }
                    // A hardware encoder that breaks mid-session (driver reset, NVENC session
                    // limit) is replaced by the software one so recording continues.
                    bt::log::error("{} stopped working, switching to the software encoder", encoder->name());
                    preference = bt::EncoderPreference::Software;
                    encoder.reset();
                }
                if (!encoder || !(encoder->size() == size)) {
                    encoder.reset();
                    video_buffer.clear();  // an MP4 cannot change resolution or encoder in the middle of a clip
                    const bt::VideoEncoderSettings encoder_settings{size, settings.fps, settings.video_bitrate_kbps,
                                                                    preference, capture_ptr->monitor().adapter_luid};
                    encoder = std::make_unique<bt::VideoEncoder>(encoder_settings, capture_ptr->device_manager(),
                                                                 [&](bt::Packet&& p) { video_buffer.push(std::move(p)); });
                }
                if (encoder->needs_system_memory()) {
                    LONGLONG duration = 0;
                    BT_CHECK_HR(sample->GetSampleDuration(&duration));
                    encoder->encode_system_memory(capture_ptr->read_nv12(sample), pts, duration);
                } else {
                    encoder->encode(sample);
                }
            } catch (const std::exception& e) {
                bt::log::error("Encoding stopped: {}", e.what());
                encoding_failed = true;
                PostThreadMessageW(g_main_thread, kMsgCaptureFailed, 0, 0);
            }
        }
        if (!dump_requested.exchange(false)) {
            return;
        }
        try {
            FrameDump dump;
            dump.nv12 = capture_ptr->read_nv12(sample);
            dump.bgra = capture_ptr->read_last_bgra();
            dump.size = capture_ptr->frame_size();
            std::lock_guard lock(dump_mutex);
            pending_dump = std::move(dump);
            PostThreadMessageW(g_main_thread, kMsgDumpReady, 0, 0);
        } catch (const std::exception& e) {
            bt::log::error("Frame dump failed: {}", e.what());
        }
    };
    auto on_event = [&](bt::CaptureEvent event, bt::VideoSize size) {
        if (event == bt::CaptureEvent::FormatChanged) {
            bt::log::info("Frames are now {}x{}", size.width, size.height);
        } else if (event == bt::CaptureEvent::Failed) {
            PostThreadMessageW(g_main_thread, kMsgCaptureFailed, 0, 0);
        }
    };

    int exit_code = 0;
    {
        bt::ScreenCapture capture(settings.monitor, settings.fps, on_frame, on_event);
        capture_ptr = &capture;
        capture.start();

        const UINT_PTR save_timer = options.save_after_s
            ? SetTimer(nullptr, 0, options.save_after_s * kMillisecondsPerSecond, nullptr) : 0;
        const UINT_PTR dump_timer = options.dump_after_s
            ? SetTimer(nullptr, 0, options.dump_after_s * kMillisecondsPerSecond, nullptr) : 0;
        const UINT_PTR exit_timer = options.exit_after_s
            ? SetTimer(nullptr, 0, options.exit_after_s * kMillisecondsPerSecond, nullptr) : 0;
        const UINT_PTR lose_timer = options.lose_capture_every_s
            ? SetTimer(nullptr, 0, options.lose_capture_every_s * kMillisecondsPerSecond, nullptr) : 0;

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
                save_clip(exe_dir / L"frames", video_buffer, settings.clip_seconds);
            } else if (msg.message == WM_TIMER && save_timer != 0 && msg.wParam == save_timer) {
                KillTimer(nullptr, save_timer);
                save_clip(exe_dir / L"frames", video_buffer, settings.clip_seconds);
            } else if (msg.message == WM_TIMER && dump_timer != 0 && msg.wParam == dump_timer) {
                KillTimer(nullptr, dump_timer);
                dump_requested = true;
            } else if (msg.message == WM_TIMER && exit_timer != 0 && msg.wParam == exit_timer) {
                break;
            } else if (msg.message == WM_TIMER && lose_timer != 0 && msg.wParam == lose_timer) {
                bt::log::info("Simulating a lost capture");
                capture.simulate_access_lost();
            } else if (msg.message == kMsgDumpReady) {
                std::optional<FrameDump> dump;
                {
                    std::lock_guard lock(dump_mutex);
                    dump.swap(pending_dump);
                }
                if (dump) {
                    write_dump(exe_dir / L"frames", *dump);
                }
            } else if (msg.message == kMsgCaptureFailed) {
                exit_code = 1;
                break;
            }
        }
        capture.stop();
        if (encoder) {
            const bt::VideoEncoderStats e = encoder->stats();
            bt::log::info("Encoder counters: {} frames in, {} dropped (encoder behind), {} input errors, {} packets "
                          "out, {} keyframes (every {:.3f} to {:.3f} s), {:.1f} MB, {} timestamps going backwards",
                          e.frames_in, e.frames_dropped, e.input_errors, e.packets_out, e.keyframes,
                          static_cast<double>(e.min_keyframe_interval) / bt::kTicksPerSecond,
                          static_cast<double>(e.max_keyframe_interval) / bt::kTicksPerSecond,
                          static_cast<double>(e.bytes_out) / (1024.0 * 1024.0), e.non_increasing_pts);
            encoder.reset();  // before the capture that owns its device and input textures
        }
        const bt::ClipBufferStats b = video_buffer.stats();
        bt::log::info("Buffer: {} packets, {:.1f} MB, {:.1f} s, dropped {} out of order, {} waiting for a keyframe, {} "
                      "by the byte cap",
                      b.packets, static_cast<double>(b.bytes) / (1024.0 * 1024.0),
                      static_cast<double>(b.newest_pts - b.oldest_pts) / bt::kTicksPerSecond, b.dropped_out_of_order,
                      b.dropped_waiting_for_keyframe, b.dropped_by_byte_cap);
        const bt::CaptureStats s = capture.stats();
        bt::log::info("Capture counters: {} ticks, {} new frames, {} repeated, {} dropped (encoder behind), {} "
                      "conversion failures, {} late ticks skipped, {} capture losses, {} delivered",
                      s.ticks, s.new_frames, s.repeated_frames, s.allocator_empty, s.convert_failed,
                      s.late_ticks_skipped, s.access_lost, s.delivered);
    }
    if (hotkey_ok) {
        UnregisterHotKey(nullptr, kHotkeyId);
    }
    bt::log::info("Backtrack development harness stopped");
    return exit_code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int code = 1;
    try {
        code = run(argc, argv);
    } catch (const std::exception& e) {
        bt::log::error("Fatal: {}", e.what());
    } catch (...) {
        bt::log::error("Fatal: unknown exception");
    }
    bt::log::shutdown();
    if (g_finished != nullptr) {
        SetEvent(g_finished);
    }
    return code;
}
