#include "core/ExportPlan.h"

#include "Check.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

using namespace bt;

namespace {

ExportSource clip_1080p60(double seconds = 30.0) {
    return ExportSource{1920, 1080, 60.0, seconds, true, 20000};
}

ExportOptions discord(double in = 0.0, double out = 0.0) {
    ExportOptions o;
    o.quality = ExportQuality::FitsDiscord;
    o.in_seconds = in;
    o.out_seconds = out;
    return o;
}

void sizes_keep_the_aspect_and_are_even() {
    CHECK_EQ(scaled_size(1920, 1080, 0).width, 1920);
    CHECK_EQ(scaled_size(1920, 1080, 720).width, 1280);
    CHECK_EQ(scaled_size(1920, 1080, 720).height, 720);
    CHECK_EQ(scaled_size(1920, 1080, 480).width, 854);  // 853.33 rounded to even
    CHECK_EQ(scaled_size(2560, 1440, 1080).width, 1920);
    // Never upscaled.
    CHECK_EQ(scaled_size(1280, 720, 1080).height, 720);
    CHECK_EQ(scaled_size(1280, 720, 1440).width, 1280);
    // An odd original loses its last row or column: 4:2:0 needs even sides.
    CHECK_EQ(scaled_size(1365, 767, 0).width, 1364);
    CHECK_EQ(scaled_size(1365, 767, 0).height, 766);
    CHECK_EQ(scaled_size(1894, 990, 720).width, 1378);  // 1377.45
    // Portrait: the short side is the width.
    CHECK_EQ(scaled_size(1080, 1920, 720).width, 720);
    CHECK_EQ(scaled_size(1080, 1920, 720).height, 1280);
    // Ultrawide.
    CHECK_EQ(scaled_size(3440, 1440, 1080).width, 2580);
    // Nothing to scale.
    CHECK_EQ(scaled_size(0, 1080, 720).width, 0);
    CHECK_EQ(scaled_size(2, 2, 0).width, 2);
    CHECK_EQ(scaled_size(1, 1, 0).width, 2);  // never 0
}

void long_side_fits() {
    const ExportSize gif = fit_long_side(ExportSize{1920, 1080}, 640);
    CHECK_EQ(gif.width, 640);
    CHECK_EQ(gif.height, 360);
    const ExportSize portrait = fit_long_side(ExportSize{1080, 1920}, 640);
    CHECK_EQ(portrait.height, 640);
    CHECK_EQ(portrait.width, 360);
    const ExportSize small = fit_long_side(ExportSize{480, 270}, 640);
    CHECK_EQ(small.width, 480);
    CHECK_EQ(fit_long_side(ExportSize{1894, 990}, 640).height % 2, 0);
}

void quality_rates() {
    const ExportSize p1080{1920, 1080};
    CHECK_EQ(quality_video_kbps(p1080, 60, ExportQuality::High), 16000);
    CHECK_EQ(quality_video_kbps(p1080, 60, ExportQuality::Balanced), 8000);
    CHECK_EQ(quality_video_kbps(p1080, 60, ExportQuality::Small), 3520);
    CHECK_EQ(quality_video_kbps(p1080, 60, ExportQuality::FitsDiscord), 0);
    // Lower for fewer pixels and frames, higher for more, in a sensible range.
    const int p720 = quality_video_kbps(ExportSize{1280, 720}, 60, ExportQuality::High);
    CHECK(p720 > 7000 && p720 < 10000);
    const int p1080_30 = quality_video_kbps(p1080, 30, ExportQuality::High);
    CHECK(p1080_30 > 9000 && p1080_30 < 12000);
    const int p1440 = quality_video_kbps(ExportSize{2560, 1440}, 60, ExportQuality::High);
    CHECK(p1440 > 22000 && p1440 < 28000);
    const int p144 = quality_video_kbps(p1080, 144, ExportQuality::High);
    CHECK(p144 > 16000 && p144 < 30000);
    // Clamped at both ends.
    CHECK_EQ(quality_video_kbps(ExportSize{2, 2}, 1, ExportQuality::Small), 150);
    CHECK_EQ(quality_video_kbps(ExportSize{16384, 16384}, 1000, ExportQuality::High), 100000);
    CHECK_EQ(quality_video_kbps(p1080, std::nan(""), ExportQuality::High), 16000);  // unknown rate: 60
    CHECK_EQ(quality_video_kbps(ExportSize{0, 0}, 60, ExportQuality::High), 0);
}

void mp4_plan_and_estimate() {
    ExportOptions o;
    o.quality = ExportQuality::Balanced;
    o.height = 720;
    const ExportPlan plan = plan_export(o, clip_1080p60());
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.width, 1280);
    CHECK_EQ(plan.height, 720);
    CHECK_EQ(plan.fps, 60.0);
    CHECK(plan.every_frame);
    CHECK(!plan.constant_bitrate);
    CHECK_EQ(plan.audio_kbps, 160);
    CHECK_EQ(plan.start_s, 0.0);
    CHECK_EQ(plan.end_s, 30.0);
    // (video + audio) * 30 s / 8, plus the index.
    const auto payload = static_cast<std::uint64_t>((plan.video_kbps + 160) * 125.0 * 30.0);
    CHECK_EQ(plan.estimated_bytes, payload + mp4_overhead_bytes(30.0, 60.0, true));
    CHECK_EQ(estimate_bytes(o, clip_1080p60()), plan.estimated_bytes);

    // Sound off, or a clip without sound: no audio bitrate.
    o.audio = false;
    CHECK_EQ(plan_export(o, clip_1080p60()).audio_kbps, 0);
    o.audio = true;
    ExportSource silent = clip_1080p60();
    silent.has_audio = false;
    CHECK_EQ(plan_export(o, silent).audio_kbps, 0);

    // Higher quality, larger file; a shorter range, smaller file.
    o.quality = ExportQuality::High;
    const std::uint64_t high = estimate_bytes(o, clip_1080p60());
    o.quality = ExportQuality::Small;
    const std::uint64_t small = estimate_bytes(o, clip_1080p60());
    CHECK(high > plan.estimated_bytes && plan.estimated_bytes > small);
    o.in_seconds = 10;
    o.out_seconds = 20;
    CHECK(estimate_bytes(o, clip_1080p60()) < small / 2);

    // Never more bits than the clip has.
    ExportSource low = clip_1080p60();
    low.video_kbps = 5000;
    o.quality = ExportQuality::High;
    o.height = 0;
    CHECK_EQ(plan_export(o, low).video_kbps, 5000);
    // 1080p60 High from the clip page's default 20 Mbit/s recording.
    CHECK_EQ(plan_export(o, clip_1080p60()).video_kbps, 16000);
    // An unknown frame rate counts as 60.
    ExportSource unknown = clip_1080p60();
    unknown.fps = 0;
    CHECK_EQ(plan_export(o, unknown).fps, 60.0);
}

void ranges() {
    ExportOptions o;
    o.in_seconds = 5;
    o.out_seconds = 0;  // to the end
    ExportPlan plan = plan_export(o, clip_1080p60(12));
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.start_s, 5.0);
    CHECK_EQ(plan.end_s, 12.0);
    o.out_seconds = 50;  // past the end
    CHECK_EQ(plan_export(o, clip_1080p60(12)).end_s, 12.0);
    o.in_seconds = -3;  // before the start
    CHECK_EQ(plan_export(o, clip_1080p60(12)).start_s, 0.0);
    o.in_seconds = std::nan("");
    CHECK_EQ(plan_export(o, clip_1080p60(12)).start_s, 0.0);
    o.in_seconds = 12;
    CHECK(bt_test::contains(plan_export(o, clip_1080p60(12)).problem, "after the end"));
    o.in_seconds = 6;
    o.out_seconds = 6;
    CHECK(bt_test::contains(plan_export(o, clip_1080p60(12)).problem, "empty"));
    o.out_seconds = 4;
    CHECK(bt_test::contains(plan_export(o, clip_1080p60(12)).problem, "empty"));
    // A clip whose length or picture is unknown cannot be planned.
    CHECK(!plan_export(ExportOptions{}, clip_1080p60(0)).problem.empty());
    CHECK(!plan_export(ExportOptions{}, ExportSource{0, 0, 60, 10, true, 0}).problem.empty());
    // A tiny piece is fine.
    o.in_seconds = 1.0;
    o.out_seconds = 1.2;
    plan = plan_export(o, clip_1080p60(12));
    CHECK(plan.problem.empty());
    CHECK(plan.estimated_bytes > 0);
}

void discord_fits_the_budget() {
    // 30 s of 1080p60: 2.3 Mbit/s is too thin for 1080p, right for 720p60.
    ExportPlan plan = plan_export(discord(), clip_1080p60(30));
    CHECK(plan.problem.empty());
    CHECK(plan.constant_bitrate);
    CHECK_EQ(plan.height, 720);
    CHECK_EQ(plan.fps, 60.0);
    CHECK_EQ(plan.audio_kbps, 128);
    CHECK(plan.estimated_bytes <= kDiscordFreeUploadBytes * 92 / 100);
    CHECK(plan.estimated_bytes > kDiscordFreeUploadBytes * 85 / 100);
    CHECK(bt_test::contains(plan.note, "1280 x 720"));
    CHECK(bt_test::contains(plan.note, "10 MB"));

    // A short piece keeps 1080p and is capped at High: no point in filling 10 MB with 3 s.
    plan = plan_export(discord(10, 13), clip_1080p60(30));
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.height, 1080);
    CHECK_EQ(plan.video_kbps, 16000);
    CHECK(plan.estimated_bytes < kDiscordFreeUploadBytes * 92 / 100);
    CHECK(plan.note.empty());

    // A millisecond: the budget's bitrate is beyond int, the plan still caps it at High.
    plan = plan_export(discord(10, 10.001), clip_1080p60(30));
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.video_kbps, 16000);

    // A minute: 480p.
    plan = plan_export(discord(0, 60), clip_1080p60(60));
    CHECK(plan.problem.empty());
    CHECK(plan.height < 720);
    CHECK(plan.estimated_bytes <= kDiscordFreeUploadBytes);

    // Five minutes: 360p30, the sound at 96 kbit/s.
    plan = plan_export(discord(), clip_1080p60(300));
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.height, 360);
    CHECK_EQ(plan.fps, 30.0);
    CHECK(!plan.every_frame);
    CHECK_EQ(plan.audio_kbps, 96);
    CHECK(plan.estimated_bytes <= kDiscordFreeUploadBytes);
    CHECK(bt_test::contains(plan.note, "30 fps"));
    CHECK(bt_test::contains(plan.note, "blocky"));
    // Two minutes still fit well enough to say nothing about blocks.
    CHECK(!bt_test::contains(plan_export(discord(), clip_1080p60(120)).note, "blocky"));

    // Twenty minutes do not fit; the user is told how much does.
    plan = plan_export(discord(), clip_1080p60(1200));
    CHECK(bt_test::contains(plan.problem, "too long"));
    CHECK(bt_test::contains(plan.problem, "10 MB"));
    CHECK(bt_test::contains(plan.problem, "sound off"));

    // Without sound everything goes to the picture.
    ExportOptions mute = discord();
    mute.audio = false;
    const ExportPlan silent = plan_export(mute, clip_1080p60(30));
    CHECK_EQ(silent.audio_kbps, 0);
    CHECK(silent.video_kbps > plan_export(discord(), clip_1080p60(30)).video_kbps);

    // A bigger budget (Nitro's 50 MB) keeps 1080p.
    ExportOptions nitro = discord();
    nitro.byte_budget = 50'000'000;
    CHECK_EQ(plan_export(nitro, clip_1080p60(30)).height, 1080);
    // A budget of 0 is refused, not divided by.
    ExportOptions zero = discord();
    zero.byte_budget = 0;
    CHECK(!plan_export(zero, clip_1080p60(30)).problem.empty());

    // 144 fps is brought down to 60, and a smaller pick in the dialog is respected.
    ExportSource fast = clip_1080p60(10);
    fast.fps = 144;
    plan = plan_export(discord(), fast);
    CHECK_EQ(plan.fps, 60.0);
    CHECK(!plan.every_frame);
    ExportOptions picked = discord();
    picked.height = 480;
    CHECK_EQ(plan_export(picked, clip_1080p60(10)).height, 480);

    // Portrait keeps its shape.
    plan = plan_export(discord(), ExportSource{1080, 1920, 60, 30, true, 0});
    CHECK_EQ(plan.width, 720);
    CHECK_EQ(plan.height, 1280);
}

void discord_retry_aims_lower() {
    const ExportSource source = clip_1080p60(30);
    const ExportOptions o = discord();
    const ExportPlan first = plan_export(o, source);
    // The encoder made 12 % more than planned: 10.3 MB.
    const auto actual = static_cast<std::uint64_t>(static_cast<double>(first.estimated_bytes) * 1.12);
    CHECK(actual > kDiscordFreeUploadBytes);
    const ExportPlan second = plan_budget_retry(o, source, first, actual);
    CHECK(second.problem.empty());
    CHECK(second.video_kbps < first.video_kbps);
    // 12 % more of the second plan still fits.
    CHECK(static_cast<double>(second.estimated_bytes) * 1.12 < static_cast<double>(kDiscordFreeUploadBytes));
    CHECK(second.height <= first.height);

    // A big overshoot steps the size down.
    const ExportPlan halved = plan_budget_retry(o, source, first, first.estimated_bytes * 2);
    CHECK(halved.problem.empty());
    CHECK(halved.height < first.height);

    // Even a tiny overshoot lowers the bitrate.
    const ExportPlan barely = plan_budget_retry(o, source, first, first.estimated_bytes);
    CHECK(barely.video_kbps < first.video_kbps);
}

void gif_plan() {
    ExportOptions o;
    o.format = ExportFormat::Gif;
    o.in_seconds = 2;
    o.out_seconds = 7;
    ExportPlan plan = plan_export(o, clip_1080p60(30));
    CHECK(plan.problem.empty());
    CHECK_EQ(plan.width, 640);
    CHECK_EQ(plan.height, 360);
    CHECK_EQ(plan.fps, 15.0);
    CHECK(!plan.every_frame);
    CHECK_EQ(plan.audio_kbps, 0);
    CHECK(plan.estimated_bytes > 1'000'000);
    CHECK(bt_test::contains(plan.note, "big"));
    // A smaller resolution picked in the dialog makes it smaller still.
    o.height = 480;
    CHECK_EQ(plan_export(o, clip_1080p60(30)).width, 640);
    o.height = 0;
    plan = plan_export(o, ExportSource{1080, 1920, 30, 10, false, 0});
    CHECK_EQ(plan.width, 360);
    CHECK_EQ(plan.height, 640);
    // A 10 fps source keeps every frame.
    plan = plan_export(o, ExportSource{800, 600, 10, 10, false, 0});
    CHECK_EQ(plan.fps, 10.0);
    CHECK(plan.every_frame);
    CHECK_EQ(plan.width, 640);
    CHECK_EQ(plan.height, 480);
}

void frames_in_range() {
    constexpr Time100ns f = kTicksPerSecond / 60;
    const Time100ns start = 2 * kTicksPerSecond;
    // The frame on screen at the start is the first one in.
    CHECK(frame_in_range(start - f / 2, f, start, start + kTicksPerSecond));
    CHECK(!frame_in_range(start - f, f, start, start + kTicksPerSecond));  // ends exactly at the start
    CHECK(frame_in_range(start, f, start, start + kTicksPerSecond));
    CHECK(!frame_in_range(start + kTicksPerSecond, f, start, start + kTicksPerSecond));  // the end is not in
    CHECK(frame_in_range(start + kTicksPerSecond - 10'000, f, start, start + kTicksPerSecond));
    // A tick of rounding either way does not pull in the neighbour.
    CHECK(!frame_in_range(start - f + 1, f, start, start + kTicksPerSecond));
    CHECK(!frame_in_range(start + kTicksPerSecond - 1, f, start, start + kTicksPerSecond));
    // 30 fps frames rounded to the tick: 15 s from 60 s are exactly 450 frames.
    int count = 0;
    for (int i = 0; i < 30 * 120; ++i) {
        const Time100ns pts = (static_cast<Time100ns>(i) * kTicksPerSecond + 15) / 30;  // rounded
        if (frame_in_range(pts, kTicksPerSecond / 30, 60 * kTicksPerSecond, 75 * kTicksPerSecond)) ++count;
    }
    CHECK_EQ(count, 450);
}

void decimation() {
    // 60 fps to 30: every other frame.
    FrameDecimator half(30);
    std::vector<int> kept;
    for (int i = 0; i < 12; ++i) {
        if (half.keep(frame_offset(i, 60))) kept.push_back(i);
    }
    CHECK_EQ(kept.size(), std::size_t{6});
    CHECK_EQ(kept[1], 2);
    CHECK_EQ(kept[5], 10);

    // 60 fps to 15: one in four, over a long run too (no drift).
    FrameDecimator gif(15);
    int count = 0;
    for (int i = 0; i < 60 * 600; ++i) {
        if (gif.keep(frame_offset(i, 60))) ++count;
    }
    CHECK_EQ(count, 15 * 600);

    // 144 to 60: uneven steps, the right count.
    FrameDecimator odd(60);
    count = 0;
    for (int i = 0; i < 144 * 10; ++i) {
        if (odd.keep(frame_offset(i, 144))) ++count;
    }
    CHECK_EQ(count, 600);

    // A timestamp a tick early still counts for its slot.
    FrameDecimator early(30);
    CHECK(early.keep(0));
    CHECK(!early.keep(kTicksPerSecond / 60));
    CHECK(early.keep(kTicksPerSecond / 30 - 1));

    // A lower source rate than asked keeps everything.
    FrameDecimator all(15);
    count = 0;
    for (int i = 0; i < 100; ++i) {
        if (all.keep(frame_offset(i, 10))) ++count;
    }
    CHECK_EQ(count, 100);
    // 0 fps: keep everything.
    FrameDecimator none(0);
    CHECK(none.keep(0));
    CHECK(none.keep(0));
}

void gif_delays() {
    // 15 fps: 7 6 7 7 6 7 ..., and 15 frames make exactly a second.
    int total = 0;
    std::vector<int> delays;
    for (int i = 0; i < 15; ++i) {
        const int d = gif_delay_cs(frame_offset(i, 15), frame_offset(i + 1, 15));
        delays.push_back(d);
        total += d;
    }
    CHECK_EQ(total, 100);
    CHECK_EQ(delays[0], 7);
    CHECK_EQ(delays[1], 6);
    // Over ten minutes the running total stays exact.
    total = 0;
    for (int i = 0; i < 15 * 600; ++i) {
        total += gif_delay_cs(frame_offset(i, 15), frame_offset(i + 1, 15));
    }
    CHECK_EQ(total, 60000);
    // Never below 2, never past the field's range.
    CHECK_EQ(gif_delay_cs(0, 1), 2);
    CHECK_EQ(gif_delay_cs(0, 0), 2);
    CHECK_EQ(gif_delay_cs(0, 1000 * kTicksPerSecond), 65535);
    CHECK_EQ(gif_delay_cs(-5, kTicksPerSecond), 100);
}

void pcm_placement() {
    constexpr std::int64_t tol = 48;
    // Back to back: all of it.
    PcmStep s = place_pcm(0, 0, 1024, 48000, tol);
    CHECK_EQ(s.silence, 0);
    CHECK_EQ(s.skip, 0);
    CHECK_EQ(s.take, 1024);
    // Starts before the export: the part before is skipped.
    s = place_pcm(0, -300, 1024, 48000, tol);
    CHECK_EQ(s.skip, 300);
    CHECK_EQ(s.take, 724);
    // Entirely before: nothing.
    s = place_pcm(0, -2000, 1024, 48000, tol);
    CHECK_EQ(s.take, 0);
    // A gap: silence first, so the sound stays in place.
    s = place_pcm(1024, 1524, 1024, 48000, tol);
    CHECK_EQ(s.silence, 500);
    CHECK_EQ(s.take, 1024);
    // Rounding jitter is not a gap or an overlap.
    s = place_pcm(1024, 1025, 1024, 48000, tol);
    CHECK_EQ(s.silence, 0);
    CHECK_EQ(s.take, 1024);
    s = place_pcm(1024, 1000, 1024, 48000, tol);
    CHECK_EQ(s.skip, 0);
    CHECK_EQ(s.take, 1024);
    // A real overlap is skipped.
    s = place_pcm(2048, 1024, 2048, 48000, tol);
    CHECK_EQ(s.skip, 1024);
    CHECK_EQ(s.take, 1024);
    // The end cuts the piece.
    s = place_pcm(47500, 47500, 1024, 48000, tol);
    CHECK_EQ(s.take, 500);
    s = place_pcm(48000, 48000, 1024, 48000, tol);
    CHECK_EQ(s.take, 0);
    // A gap reaching past the end is silence to the end and nothing more.
    s = place_pcm(47000, 49000, 1024, 48000, tol);
    CHECK_EQ(s.silence, 1000);
    CHECK_EQ(s.take, 0);
    // Empty pieces.
    s = place_pcm(0, 0, 0, 48000, tol);
    CHECK_EQ(s.take, 0);
    CHECK_EQ(s.silence, 0);
}

void file_names() {
    CHECK(export_file_name(L"clip_2026-11-06", 1, ExportFormat::Mp4) == L"clip_2026-11-06 (export).mp4");
    CHECK(export_file_name(L"clip", 2, ExportFormat::Mp4) == L"clip (export 2).mp4");
    CHECK(export_file_name(L"clip", 12, ExportFormat::Gif) == L"clip (export 12).gif");
    CHECK(export_file_name(L"", 0, ExportFormat::Gif) == L" (export).gif");
}

std::vector<std::uint8_t> box(const char* type, const std::vector<std::uint8_t>& body) {
    const auto size = static_cast<std::uint32_t>(8 + body.size());
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(size >> 24), static_cast<std::uint8_t>(size >> 16),
                                  static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

// A trak box as an MP4 muxer writes it, down to the handler type that says what the track is.
std::vector<std::uint8_t> trak(const char* handler) {
    std::vector<std::uint8_t> hdlr(8, 0);  // version/flags, pre_defined
    hdlr.insert(hdlr.end(), handler, handler + 4);
    hdlr.resize(hdlr.size() + 12 + 1, 0);  // reserved, empty name
    return box("trak", join({box("tkhd", std::vector<std::uint8_t>(84, 0)),
                             box("mdia", join({box("mdhd", std::vector<std::uint8_t>(24, 0)), box("hdlr", hdlr),
                                               box("minf", {})}))}));
}

void track_kinds() {
    const auto moov = box("moov", join({box("mvhd", std::vector<std::uint8_t>(100, 0)), trak("vide"), trak("soun"),
                                        trak("soun"), trak("soun"), box("udta", {1, 2, 3})}));
    const std::vector<TrackKind> kinds = mp4_track_kinds(moov);
    CHECK_EQ(kinds.size(), std::size_t{4});
    CHECK(kinds.size() == 4 && kinds[0] == TrackKind::Video && kinds[1] == TrackKind::Audio &&
          kinds[3] == TrackKind::Audio);
    // A subtitle track is neither.
    const auto with_text = box("moov", join({trak("soun"), trak("text")}));
    const auto text_kinds = mp4_track_kinds(with_text);
    CHECK(text_kinds.size() == 2 && text_kinds[1] == TrackKind::Other);
    // Damaged or foreign boxes give nothing.
    CHECK(mp4_track_kinds(std::span(moov).first(moov.size() - 1)).empty());
    auto broken = moov;
    broken[8] = 0x7F;  // mvhd claims more bytes than there are
    CHECK(mp4_track_kinds(broken).empty());
    CHECK(mp4_track_kinds(box("mdat", {0, 0, 0, 0})).empty());
    CHECK(mp4_track_kinds(std::vector<std::uint8_t>{0, 0, 0}).empty());
    CHECK(mp4_track_kinds(box("moov", {})).empty());
}

void reader_streams() {
    using enum TrackKind;
    const std::vector<TrackKind> file{Video, Audio, Audio, Audio};
    // Media Foundation lists the tracks last to first: the mix (track 2) is its stream 2.
    const std::vector<TrackKind> reversed{Audio, Audio, Audio, Video};
    CHECK(reader_stream_of_first_track(file, reversed, Audio) == std::optional<std::size_t>(2));
    CHECK(reader_stream_of_first_track(file, reversed, Video) == std::optional<std::size_t>(3));
    // A reader that keeps the file's order.
    CHECK(reader_stream_of_first_track(file, file, Audio) == std::optional<std::size_t>(1));
    CHECK(reader_stream_of_first_track(file, file, Video) == std::optional<std::size_t>(0));
    // One sound track: either way.
    const std::vector<TrackKind> one{Video, Audio};
    const std::vector<TrackKind> one_reversed{Audio, Video};
    CHECK(reader_stream_of_first_track(one, one_reversed, Audio) == std::optional<std::size_t>(0));
    // No sound, different counts, kinds that match neither way: not known.
    CHECK(!reader_stream_of_first_track(std::vector<TrackKind>{Video}, std::vector<TrackKind>{Video}, Audio));
    CHECK(!reader_stream_of_first_track(file, one, Audio));
    CHECK(!reader_stream_of_first_track(one, std::vector<TrackKind>{Video, Video}, Audio));
    CHECK(!reader_stream_of_first_track(std::vector<TrackKind>{}, std::vector<TrackKind>{}, Audio));
    // A mirrored layout: the sound's position is ambiguous, the single video is not.
    const std::vector<TrackKind> mirrored{Audio, Video, Audio};
    CHECK(!reader_stream_of_first_track(mirrored, mirrored, Audio));
    CHECK(reader_stream_of_first_track(mirrored, mirrored, Video) == std::optional<std::size_t>(1));
}

}  // namespace

int main() {
    track_kinds();
    reader_streams();
    sizes_keep_the_aspect_and_are_even();
    long_side_fits();
    quality_rates();
    mp4_plan_and_estimate();
    ranges();
    discord_fits_the_budget();
    discord_retry_aims_lower();
    gif_plan();
    frames_in_range();
    decimation();
    gif_delays();
    pcm_placement();
    file_names();
    return bt_test::finish("ExportPlanTests");
}
