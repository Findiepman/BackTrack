#pragma once

// Plays one clip on the Linux clip page: FFmpeg decodes on a thread of its own, the sound goes
// out through the PulseAudio API (pipewire-pulse on KDE and Arch), and the window asks for the
// picture that is due on every tick (take_frame), so the controls are ordinary widgets.
//
// The clock is the wall clock, not the sound card: over a clip of a few minutes the two drift
// apart by a few milliseconds at most, and the sound thread drops what is late or waits for what
// is early. That keeps playing without sound (no sound track, no sound server, another speed)
// the same code path. At a speed other than 1x the sound is left out (no pitch correction).
//
// Every method is for the GUI thread. Times are seconds from the start of the video track.

#include <QImage>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt::linux_app {

class Player {
public:
    // Opens the file and starts decoding at once; the first picture arrives a moment later.
    explicit Player(const std::string& path);
    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Empty while all is well; one line for the user when the clip cannot be played.
    std::string error() const;
    bool ready() const;  // opened, the length is known
    double duration() const;

    void play();
    void pause();
    bool playing() const;
    bool ended() const;
    // Frame exact: the picture shown is the first one at or after `seconds`.
    void seek(double seconds);
    double position() const;
    void set_volume(double volume);  // 0..1
    void set_muted(bool muted);
    void set_rate(double rate);  // 0.25..4

    // The size the pictures are made in (the player area on screen, keeping the video's shape).
    void set_view_size(int width, int height);
    // The video's own picture size; 0 before it is known.
    int video_width() const;
    int video_height() const;

    // When a picture is due that has not been taken yet: moves it into `frame` and returns true.
    bool take_frame(QImage& frame);

private:
    struct VideoFrame {
        double pts = 0.0;
        std::uint64_t generation = 0;
        QImage image;
    };
    struct SoundChunk {
        double pts = 0.0;
        std::uint64_t generation = 0;
        std::vector<std::int16_t> samples;  // interleaved stereo, kSoundRate
    };
    struct Decoding;  // FFmpeg's state, only touched by the decode thread

    void decode_loop();
    void sound_loop();
    double clock_locked() const;
    void anchor_locked(double media_time);
    double sound_queued_locked() const;

    mutable std::mutex mutex_;
    std::condition_variable decode_wake_;
    std::condition_variable sound_wake_;
    std::unique_ptr<Decoding> decoding_;

    // Guarded by mutex_.
    bool stop_ = false;
    bool opened_ = false;
    std::string error_;
    double duration_ = 0.0;
    int video_width_ = 0;
    int video_height_ = 0;
    bool has_sound_ = false;
    int view_width_ = 0;
    int view_height_ = 0;
    bool playing_ = false;
    double rate_ = 1.0;
    double anchor_media_ = 0.0;  // the clock: media time at anchor_wall_
    std::int64_t anchor_wall_ = 0;  // steady clock, nanoseconds
    double volume_ = 1.0;
    bool muted_ = false;
    std::uint64_t generation_ = 0;  // +1 on every seek: frames of an older one are thrown away
    bool seek_pending_ = false;
    double seek_target_ = 0.0;
    bool want_frame_ = true;  // after a seek or at the start: show the first picture even while paused
    bool decoder_done_ = false;  // the end of the file was reached for this generation
    double last_shown_ = -1.0;
    std::deque<VideoFrame> frames_;
    std::deque<SoundChunk> sound_;

    std::thread decode_thread_;
    std::thread sound_thread_;
};

}  // namespace bt::linux_app
