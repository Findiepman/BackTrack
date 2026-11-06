// The Record page (backtrack.html #/record): which display Instant Replay records, the save
// button, sound, the replay buffer and how full it is, Backtrack's own load and the latest clips.
// Long recordings and screenshots are not there yet: their buttons are greyed with "Soon".

#include "platform/Text.h"
#include "ui/App.h"
#include "ui/Theme.h"

#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace bt::ui {
namespace {

using namespace theme;

constexpr std::size_t kHistory = 30;  // samples in a sparkline, one per second

std::uint64_t filetime_ticks(const FILETIME& t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

// "Today, 4:25 PM", "Yesterday, 9:02 AM", "Mon 2 Nov, 12:25 PM".
std::wstring when_text(std::int64_t unix_seconds, std::int64_t now) {
    const std::int64_t ticks = (unix_seconds + 11'644'473'600LL) * 10'000'000LL;
    FILETIME utc{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    FILETIME local{};
    SYSTEMTIME t{};
    if (unix_seconds <= 0 || !FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &t)) return L"";
    wchar_t time[64] = {};
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, time, 64);
    SYSTEMTIME today{};
    GetLocalTime(&today);
    if (t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay) return std::format(L"Today, {}", time);
    // Yesterday by the calendar: the local date of this time yesterday.
    const std::int64_t yesterday_ticks = (now - 86400 + 11'644'473'600LL) * 10'000'000LL;
    FILETIME y_utc{static_cast<DWORD>(yesterday_ticks & 0xFFFFFFFF), static_cast<DWORD>(yesterday_ticks >> 32)};
    FILETIME y_local{};
    SYSTEMTIME y{};
    if (FileTimeToLocalFileTime(&y_utc, &y_local) && FileTimeToSystemTime(&y_local, &y) && t.wYear == y.wYear &&
        t.wMonth == y.wMonth && t.wDay == y.wDay) {
        return std::format(L"Yesterday, {}", time);
    }
    wchar_t date[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"ddd d MMM", date, 64, nullptr);
    return std::format(L"{}, {}", date, time);
}

}  // namespace

void App::sample_load() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    const auto now = std::chrono::steady_clock::now();
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return;
    const std::uint64_t cpu = filetime_ticks(kernel) + filetime_ticks(user);  // 100 ns units, all cores
    if (last_cpu_time_ != 0) {
        const double wall = std::chrono::duration<double>(now - last_cpu_sample_).count() * 1e7;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const double cores = std::max<DWORD>(info.dwNumberOfProcessors, 1);
        const double percent = wall > 0 ? static_cast<double>(cpu - last_cpu_time_) / wall / cores * 100.0 : 0.0;
        cpu_history_.push_back(static_cast<float>(std::clamp(percent, 0.0, 100.0)));
        if (cpu_history_.size() > kHistory) cpu_history_.pop_front();
    }
    last_cpu_time_ = cpu;
    last_cpu_sample_ = now;
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        memory_history_.push_back(static_cast<float>(static_cast<double>(memory.PrivateUsage) / 1e6));
        if (memory_history_.size() > kHistory) memory_history_.pop_front();
    }
}

void App::run_record(const std::string& what, const Rect& anchor) {
    if (what == "save-length") {
        run_setting("saveLen", anchor);  // the same list as Settings > Instant Replay
    } else if (what.starts_with("display:")) {
        const int index = std::stoi(what.substr(8));
        if (index != settings_.monitor) change_settings([index](Settings& s) { s.monitor = index; });
    } else if (what == "replay") {
        toggle_pause();
    } else if (what == "audio") {
        change_settings([](Settings& s) { s.audio_enabled = !s.audio_enabled; });
    } else if (what == "mic") {
        change_settings([](Settings& s) { s.mic_enabled = !s.mic_enabled; });
    } else if (what == "mute") {
        toggle_mic_mute();
    } else if (what == "mic-privacy") {
        run_setting("mic-privacy", Rect{});
    } else if (what == "save") {
        save_clip();
    } else if (what == "all") {
        set_view(LibraryView::All);
    } else if (what.starts_with("open:")) {
        open_clip(what.substr(5));
    }
    invalidate();
}

void App::paint_record_page(const Rect& area) {
    Graphics& g = *graphics_;
    const Rect page = begin_page(area);
    const float x0 = page.x + kPad;
    const float cw = page.w - 2 * kPad;
    float y = page.y + kPageTop;
    g.text(L"Record", Rect{x0, y, 300, 38}, TextStyle{28, Weight::Bold, true}, kInk);
    g.text(L"Instant Replay records the whole screen and saves the moments that just happened.",
           Rect{x0, y + 40, cw, 20}, TextStyle{13.5f}, kMute);
    y += 60 + 18;

    // .rgrid: 1.5fr and minmax(300, 1fr), 22 apart; one column when narrow.
    const bool one_column = cw < 760;
    const float left_w = one_column ? cw : std::floor((cw - 22) * 0.6f);
    const float right_w = one_column ? cw : cw - 22 - left_w;
    const float right_x = one_column ? x0 : x0 + left_w + 22;
    const auto card = [&](const Rect& r) { g.fill_round(r, kPanelRadius, kCard); };
    const auto heading = [&](float x, float top, const wchar_t* title, const std::wstring& text, float width) {
        g.text(title, Rect{x, top, width, 20}, TextStyle{15, Weight::Bold, true}, kInk);
        if (!text.empty()) g.text(text, Rect{x, top + 22, width, 18}, TextStyle{12.5f}, kMute);
    };
    const bool paused = recorder_->paused();
    const BufferStatus buffer = recorder_->buffer_status();

    // Screen.
    float left_bottom = y;
    {
        const float px = x0 + 20;
        const float pw = left_w - 40;
        // As tall as the displays drawn side by side need (4 displays make a short row), at most 16:9.
        float screen_h = std::floor(pw * 9.0f / 16.0f);
        if (monitors_.size() > 1) {
            const float tile_w = (pw - 12 - 6 * static_cast<float>(monitors_.size() - 1)) / static_cast<float>(monitors_.size());
            float tallest = 0;
            for (const MonitorInfo& m : monitors_) {
                if (m.desktop_size.width > 0) {
                    tallest = std::max(tallest, tile_w * static_cast<float>(m.desktop_size.height) /
                                                    static_cast<float>(m.desktop_size.width));
                }
            }
            if (tallest > 0) screen_h = std::min(screen_h, std::floor(tallest + 12));
        }
        const Rect box{x0, y, left_w, 18 + 40 + 12 + screen_h + 16 + 56 + 18};
        card(box);
        // Out of range (unplugged) records monitor 0, like the recorder does.
        const int selected =
            settings_.monitor >= 0 && settings_.monitor < static_cast<int>(monitors_.size()) ? settings_.monitor : 0;
        const MonitorInfo* m = monitors_.empty() ? nullptr : &monitors_[static_cast<std::size_t>(selected)];
        float picker_w = 0;
        if (monitors_.size() > 1) {
            std::vector<std::wstring> names;
            for (std::size_t i = 0; i < monitors_.size(); ++i) names.push_back(std::format(L"Display {}", i + 1));
            picker_w = segmented(px + pw, y + 18 + 20, names, selected, Action::Record, "display") + 12;
        }
        heading(px, y + 18, L"Screen",
                m != nullptr ? std::format(L"Display {}, {}×{} · whole screen, no window picking", selected + 1,
                                           m->desktop_size.width, m->desktop_size.height)
                             : std::wstring(L"Whole screen, no window picking"),
                pw - picker_w);
        // .screen: the displays side by side, the recorded one framed in the accent colour.
        const Rect screen{px, y + 18 + 40 + 12, pw, screen_h};
        g.fill_gradient(screen, 12, Color::hex(0x12302b), Color::hex(0x1a1a3c));
        g.stroke_round(screen, 12, kLine);
        const std::size_t count = std::max<std::size_t>(monitors_.size(), 1);
        const float tile_w = (screen.w - 12 - 6 * static_cast<float>(count - 1)) / static_cast<float>(count);
        for (std::size_t i = 0; i < count; ++i) {
            // Each display keeps its own shape, centred in its share of the screen area.
            float tile_h = screen.h - 12;
            if (i < monitors_.size() && monitors_[i].desktop_size.width > 0) {
                tile_h = std::min(tile_h, tile_w * static_cast<float>(monitors_[i].desktop_size.height) /
                                              static_cast<float>(monitors_[i].desktop_size.width));
            }
            const Rect tile{screen.x + 6 + static_cast<float>(i) * (tile_w + 6), screen.y + (screen.h - tile_h) / 2, tile_w,
                            tile_h};
            const bool on = static_cast<int>(i) == selected;
            g.push_round_clip(tile, 8);
            g.fill_gradient(tile, 0, Color::hex(on ? 0x0f3b34 : 0x1d2228), Color::hex(on ? 0x12254a : 0x14171c));
            const Color glass = Color::hex(0xffffff, on ? 0.13f : 0.07f);
            const Rect w1{tile.x + tile.w * 0.10f, tile.y + tile.h * 0.16f, tile.w * 0.42f, tile.h * 0.46f};
            const Rect w2{tile.x + tile.w * 0.44f, tile.y + tile.h * 0.40f, tile.w * 0.44f, tile.h * 0.40f};
            g.fill_round(w1, 6, glass);
            g.stroke_round(w1, 6, Color::hex(0xffffff, 0.25f));
            g.fill_round(w2, 6, glass);
            g.stroke_round(w2, 6, Color::hex(0xffffff, 0.22f));
            g.fill(Rect{tile.x, tile.bottom() - tile.h * 0.06f, tile.w, tile.h * 0.06f}, Color::hex(0x000000, 0.5f));
            g.pop_clip();
            if (on) g.stroke_round(tile, 8, kAccent, 2);
            std::wstring label = std::format(L"Display {}{}", i + 1,
                                             i < monitors_.size() && monitors_[i].primary ? L" (main)" : L"");
            if (g.text_width(label, TextStyle{11.5f}) + 16 > tile.w - 16) label = std::to_wstring(i + 1);  // narrow tiles
            const float lw = std::min(g.text_width(label, TextStyle{11.5f}) + 16, tile.w - 16);
            const Rect pill{tile.x + 8, tile.y + 8, lw, 20};
            g.fill_round(pill, 10, Color::hex(0x000000, 0.5f));
            g.text(label, pill, TextStyle{11.5f}, Color::hex(0xffffff), Align::Center);
            if (monitors_.size() > 1) add_hit(tile, Action::Record, std::format("display:{}", i));
        }
        // .ctl: the big record button (later), the timer, screenshot (later), save.
        const float cy = screen.bottom() + 16;
        {
            const Rect rec{px, cy, 190, 56};
            g.fill_round(rec, 28, kRec.with_alpha(0.42f));
            g.fill_circle(rec.x + 30, rec.y + 28, 10, Color::hex(0xffffff, 0.6f));
            g.text(L"Start recording", Rect{rec.x + 52, rec.y, rec.w - 60, rec.h}, TextStyle{16, Weight::Bold},
                   Color::hex(0xffffff, 0.6f));
            add_hit(rec, Action::Disabled);
            soon_badge(rec.right() + 10, rec.y + 28);
        }
        const int save_seconds = saved_seconds(settings_);
        const float save_w = button_width(std::format(L"Save last {}", to_wide(format_duration(save_seconds))),
                                          glyph::kBolt, false);
        const std::wstring save_label = save_seconds < 60
                                            ? std::format(L"Save last {} s", save_seconds)
                                            : std::format(L"Save last {}", to_wide(format_duration(save_seconds)));
        const float sw2 = button_width(save_label, glyph::kBolt, false);
        (void)save_w;
        push_button(px + pw - sw2, cy + 9, save_label, glyph::kBolt, ButtonStyle::Primary, false, Action::Record, "save",
                    paused && buffer.seconds <= 0);
        const float shot_w = button_width(L"Screenshot", glyph::kCamera, false);
        if (px + 190 + 60 < px + pw - sw2 - 12 - shot_w) {
            push_button(px + pw - sw2 - 12 - shot_w, cy + 9, L"Screenshot", glyph::kCamera, ButtonStyle::Line, false,
                        Action::Record, "shot", true);
        }
        left_bottom = box.bottom();
    }

    // Audio.
    {
        const float top = left_bottom + 22;
        const float px = x0 + 20;
        const float pw = left_w - 40;
        const Rect box{x0, top, left_w, 18 + 40 + 12 + 56 + 1 + 56 + 10};
        card(box);
        heading(px, top + 18, L"Audio", L"What goes into your clips, mixed into one sound track", pw);
        const AudioLevels levels = recorder_->audio_levels();
        const bool muted = recorder_->mic_muted();
        float ry = top + 18 + 40 + 12;
        // .lm: 22 bars, the loud end in the warning colour.
        const auto meter = [&](float right, float level, bool live) {
            constexpr int kBars = 22;
            const Rect lm{right - 120, ry + 21, 120, 14};
            const float bw = (lm.w - 2 * (kBars - 1)) / kBars;
            // Loudness scale like any sound meter: -60 dB at the left end, 0 dB (full scale) at the right.
            const float db = 20.0f * std::log10(std::max(level, 1e-6f));
            const int lit = live ? static_cast<int>(std::lround(std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f) * kBars)) : 0;
            for (int i = 0; i < kBars; ++i) {
                const Color on = i > 17 ? kWarn : kAccent;
                const Color c = i < lit ? on : kLine;
                g.fill_round(Rect{lm.x + static_cast<float>(i) * (bw + 2), lm.y, bw, lm.h}, 2, live ? c : c.with_alpha(0.55f));
            }
        };
        const auto audio_row = [&](wchar_t glyph, const wchar_t* title, const std::wstring& text, Color text_color,
                                   bool on, const char* arg, float level, bool live, bool with_mute) {
            // .aud: an icon tile, two lines, the level meter, the switch.
            const Rect icon{px, ry + 10, 36, 36};
            g.fill_round(icon, 10, on ? kSoft : kInk.with_alpha(0.08f));
            g.icon(glyph, icon, 15, on ? kAccent : kMute);
            g.text(title, Rect{px + 48, ry + 9, pw * 0.5f, 20}, TextStyle{14, Weight::Semibold}, kInk);
            const float right = px + pw - 42 - 16;
            const float mute_w = with_mute ? 34.0f + 8.0f : 0.0f;
            g.text(text, Rect{px + 48, ry + 29, std::max(40.0f, right - 120 - 16 - mute_w - (px + 48)), 18}, TextStyle{12.5f},
                   text_color);
            if (pw > 380) meter(right - mute_w, level, live);
            if (with_mute) {
                icon_button(Rect{right - 34, ry + 11, 34, 34}, muted ? glyph::kMute : glyph::kMicrophone, 14, Action::Record,
                            "mute", false, muted ? kWarn : App::kNoColor);
            }
            toggle(px + pw, ry + 28, on, Action::Record, arg);
            ry += 56;
        };
        // The microphone's state in words.
        std::wstring mic_text = L"Off";
        Color mic_color = kMute;
        if (settings_.mic_enabled) {
            switch (levels.mic_status) {
                case MicStatus::Off:
                    // Asked for, but the recorder has no sound at all (see backtrack.log).
                    mic_text = L"Sound recording is not available right now";
                    mic_color = kWarn;
                    break;
                case MicStatus::Starting: mic_text = L"Starting..."; break;
                case MicStatus::NoDevice: mic_text = L"No microphone found. Plug one in."; break;
                case MicStatus::OnlyBluetooth:
                    mic_text = L"Only a Bluetooth headset mic: pick it in Settings > Audio to use it (call-quality sound).";
                    break;
                case MicStatus::Blocked:
                    mic_text = L"Windows blocks microphone access. Click to open its privacy settings.";
                    mic_color = kWarn;
                    break;
                case MicStatus::Failed: mic_text = L"The microphone could not be opened"; mic_color = kWarn; break;
                case MicStatus::Recording: {
                    // The state first: a long device name must not push it out of sight.
                    const std::wstring name = to_wide(levels.mic_name);
                    if (muted) {
                        mic_text = (settings_.mic_mute_hotkey.empty()
                                        ? std::wstring(L"Muted")
                                        : std::format(L"Muted ({} unmutes)", to_wide(settings_.mic_mute_hotkey))) +
                                   L" \u00b7 " + name;
                        mic_color = kWarn;
                    } else if (settings_.push_to_talk && levels.ptt_blocked) {
                        // Windows hides keys pressed in an admin program from normal programs.
                        mic_text = L"Push-to-talk can't see keys while an admin program is in front";
                        mic_color = kWarn;
                    } else if (settings_.push_to_talk) {
                        mic_text = (levels.mic_gate_open ? std::wstring(L"Talking")
                                                         : std::format(L"Hold {} to talk", to_wide(settings_.push_to_talk_key))) +
                                   L" \u00b7 " + (levels.mic_fallback ? L"chosen mic not connected, using " : L"") + name;
                        if (levels.mic_fallback) mic_color = kWarn;
                    } else if (levels.mic_fallback) {
                        mic_text = L"Your chosen mic is not connected: using " + name;
                        mic_color = kWarn;
                    } else {
                        mic_text = name + (settings_.noise_suppression ? L" \u00b7 Noise suppression on" : L"");
                    }
                    break;
                }
            }
        }
        const float mic_row_y = ry;
        audio_row(muted ? glyph::kMute : glyph::kMicrophone, L"Microphone", mic_text, mic_color, settings_.mic_enabled,
                  "mic", levels.mic, settings_.mic_enabled && levels.mic_status == MicStatus::Recording && !muted,
                  settings_.mic_enabled);
        if (settings_.mic_enabled && levels.mic_status == MicStatus::Blocked) {
            add_hit(Rect{px + 48, mic_row_y + 27, pw * 0.5f, 22}, Action::Record, "mic-privacy");
        }
        g.fill(Rect{px, ry, pw, 1}, kLine);
        ry += 1;
        audio_row(glyph::kSpeaker, L"System audio", L"Default playback device: games, voice chat, music", kMute,
                  settings_.audio_enabled, "audio", levels.system, settings_.audio_enabled, false);
        left_bottom = box.bottom();
    }

    // Instant Replay.
    float right_top = one_column ? left_bottom + 22 : y;
    float right_bottom = right_top;
    {
        const float top = right_top;
        const float px = right_x + 20;
        const float pw = right_w - 40;
        const Rect box{right_x, top, right_w, 18 + 40 + 4 + 3 * 44 + 8 + 8 + 6 + 18 + 18};
        card(box);
        heading(px, top + 18, L"Instant Replay", L"Always keeps the last moments, ready to save", pw - 54);
        toggle(px + pw, top + 18 + 14, !paused, Action::Record, "replay");
        float ry = top + 18 + 44;
        const auto krow = [&](const wchar_t* label) {
            g.text(label, Rect{px, ry, pw * 0.4f, 44}, TextStyle{13.5f}, kInk);
        };
        // Buffer length: the same slider as in Settings.
        krow(L"Buffer length");
        {
            const bool previewing = drag_preview_ && drag_preview_->first == "set:replayLen";
            const float value = previewing ? drag_preview_->second : static_cast<float>(settings_.clip_seconds - 5) / 295.0f;
            const int seconds = previewing ? 5 + 5 * static_cast<int>(std::lround(value * 59.0f)) : settings_.clip_seconds;
            const std::wstring text = seconds < 60 ? std::format(L"{} s", seconds)
                                      : seconds % 60 == 0 ? std::format(L"{} min", seconds / 60)
                                                          : std::format(L"{} min {} s", seconds / 60, seconds % 60);
            g.text(text, Rect{px + pw - 74, ry, 74, 44}, TextStyle{13.5f, Weight::Bold, false, true}, kInk, Align::End);
            const float slider_w = std::max(80.0f, std::min(200.0f, pw - 74 - 12 - pw * 0.4f));
            slider(Rect{px + pw - 74 - 12 - slider_w, ry + 10, slider_w, 24}, value, "set:replayLen");
        }
        ry += 44;
        krow(L"Keep buffer in");
        segmented(px + pw, ry + 22, {L"RAM", L"Disk"}, 0, Action::Record, "store", true);
        ry += 44;
        krow(L"Save button saves");
        {
            const int save_seconds = saved_seconds(settings_);
            const std::wstring text = L"Last " + (save_seconds < 60 ? std::format(L"{} s", save_seconds)
                                                                     : to_wide(format_duration(save_seconds)));
            select_box(Rect{px + pw - 130, ry + 4, 130, 36}, text, Action::Record, "save-length");
        }
        ry += 44 + 8;
        // .fillbar: how much of the buffer is there (it fills up after a start or a settings change).
        const float fill = static_cast<float>(std::clamp(buffer.seconds / std::max(1, settings_.clip_seconds), 0.0, 1.0));
        const Rect bar{px, ry, pw, 8};
        g.fill_round(bar, 4, kLine);
        if (fill > 0) g.fill_round(Rect{bar.x, bar.y, std::max(8.0f, bar.w * fill), bar.h}, 4, paused ? kOff : kAccent);
        const std::wstring held = std::format(L"Holding {} of {}, about {} MB of memory",
                                              to_wide(format_duration(std::min<double>(buffer.seconds, settings_.clip_seconds))),
                                              to_wide(format_duration(settings_.clip_seconds)),
                                              std::lround(static_cast<double>(buffer.bytes) / 1e6));
        g.text(paused ? L"Paused: holding what was recorded before the pause" : held, Rect{px, ry + 14, pw, 18},
               TextStyle{12.5f}, kMute);
        right_bottom = box.bottom();
    }

    // Performance.
    {
        const float top = right_bottom + 22;
        const float px = right_x + 20;
        const float pw = right_w - 40;
        const Rect box{right_x, top, right_w, 18 + 40 + 12 + 16 + 26 + 8 + 34 + 18};
        card(box);
        heading(px, top + 18, L"Performance", L"Backtrack's own load while capturing", pw);
        const float colw = (pw - 24) / 3;
        const float cy = top + 18 + 40 + 12;
        const auto stat = [&](float x, const wchar_t* label, const std::wstring& value, const std::deque<float>* history,
                              float scale) {
            g.text(label, Rect{x, cy, colw, 16}, TextStyle{12}, kMute);
            g.text(value, Rect{x, cy + 16, colw, 26}, TextStyle{20, Weight::Bold, true, true}, kInk);
            // .spark: bars, the newest one solid.
            const Rect spark{x, cy + 16 + 26 + 8, colw, 34};
            if (history == nullptr || history->empty()) {
                g.fill(Rect{spark.x, spark.bottom() - 2, spark.w, 2}, kAccent.with_alpha(0.3f));
                return;
            }
            const float bw = (spark.w - 2 * static_cast<float>(kHistory - 1)) / static_cast<float>(kHistory);
            const std::size_t n = history->size();
            for (std::size_t i = 0; i < n; ++i) {
                const float v = std::clamp((*history)[i] / scale, 0.0f, 1.0f);
                const float h = std::max(2.0f, spark.h * v);
                const float bx = spark.x + static_cast<float>(kHistory - n + i) * (bw + 2);
                g.fill(Rect{bx, spark.bottom() - h, bw, h}, kAccent.with_alpha(i + 1 == n ? 1.0f : 0.55f));
            }
        };
        const float cpu = cpu_history_.empty() ? 0.0f : cpu_history_.back();
        const float mem = memory_history_.empty() ? 0.0f : memory_history_.back();
        float cpu_scale = 10.0f;
        for (const float v : cpu_history_) cpu_scale = std::max(cpu_scale, v);
        float mem_scale = 1.0f;
        for (const float v : memory_history_) mem_scale = std::max(mem_scale, v);
        stat(px, L"CPU", std::format(L"{:.1f}%", cpu), &cpu_history_, cpu_scale);
        stat(px + colw + 12, L"Memory", std::format(L"{} MB", std::lround(mem)), &memory_history_, mem_scale * 1.15f);
        g.text(L"Video encoder", Rect{px + 2 * (colw + 12), cy, colw, 16}, TextStyle{12}, kMute);
        g.text(buffer.encoder_running ? (buffer.hardware_encoder ? L"GPU" : L"CPU") : L"—",
               Rect{px + 2 * (colw + 12), cy + 16, colw, 26}, TextStyle{20, Weight::Bold, true}, kInk);
        g.text(buffer.encoder_running ? (buffer.hardware_encoder ? L"Graphics card" : L"Software") : L"Starting",
               Rect{px + 2 * (colw + 12), cy + 16 + 26 + 8, colw, 18}, TextStyle{12.5f}, kMute);
        right_bottom = box.bottom();
    }

    // Recent captures.
    {
        const float top = right_bottom + 22;
        const float px = right_x + 20;
        const float pw = right_w - 40;
        LibraryQuery q;
        q.view = LibraryView::All;
        q.sort = ClipSort::Newest;
        q.now = now_unix();
        std::vector<std::size_t> recent = library_.query(q);
        if (recent.size() > 4) recent.resize(4);
        const float rows_h = recent.empty() ? 30.0f : static_cast<float>(recent.size()) * 58.0f;
        const Rect box{right_x, top, right_w, 18 + 30 + 8 + rows_h + 12};
        card(box);
        g.text(L"Recent captures", Rect{px, top + 18, 300, 30}, TextStyle{15, Weight::Bold, true}, kInk);
        const float vw = button_width(L"View all", 0, true);
        push_button(px + pw - vw, top + 18, L"View all", 0, ButtonStyle::Ghost, true, Action::Record, "all");
        float ry = top + 18 + 30 + 8;
        if (recent.empty()) {
            g.text(std::format(L"Press {} in a game to save your first clip.", to_wide(settings_.save_hotkey)),
                   Rect{px, ry, pw, 22}, TextStyle{13}, kMute);
        }
        for (const std::size_t index : recent) {
            const ClipEntry& clip = library_.clips()[index];
            const Rect row{px - 8, ry, pw + 16, 54};
            const std::string arg = "open:" + clip.file;
            if (hovered(Action::Record, arg)) g.fill_round(row, 10, kHover);
            paint_thumbnail(clip, Rect{px, ry + 5, 78, 44}, 6);
            g.text(to_wide(display_title(clip)), Rect{px + 90, ry + 8, pw - 90, 20}, TextStyle{13.5f, Weight::Semibold}, kInk);
            g.text(to_wide(format_duration(clip.duration_s)) + L" · " + when_text(clip.created, now_unix()),
                   Rect{px + 90, ry + 28, pw - 90, 18}, TextStyle{12.5f}, kMute);
            add_hit(row, Action::Record, arg);
            ry += 58;
        }
        right_bottom = box.bottom();
    }

    if (saved_bar_) {
        // The Saved bar floats over this page too, as in the mockup.
        end_page(area, std::max(left_bottom, right_bottom) + 28 + kSavedBar + 32);
        paint_saved_bar(area);
    } else {
        end_page(area, std::max(left_bottom, right_bottom) + 28);
    }
}

}  // namespace bt::ui
