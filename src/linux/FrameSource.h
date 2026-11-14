#pragma once

#include <cstdint>
#include <string>

namespace bt::linux_app {

// Byte order in memory of a 32-bit pixel; the fourth byte is ignored.
enum class PixelLayout { Bgrx, Rgbx };

// One screen image. Points into the source's memory and stays valid until the next grab() or
// stop() of the same source.
struct FrameView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row
    PixelLayout layout = PixelLayout::Bgrx;
};

enum class GrabResult {
    Frame,  // `frame` holds the newest screen image
    NotYet,  // nothing arrived yet (the portal stream is still starting)
    Failed,  // the source is gone: error() says why
};

// Where the screen images come from: X11 (XShm) or the Wayland portal (PipeWire). grab() is
// called by the recorder's video thread at the frame rate and returns the newest image; a screen
// that did not change gives the same image again, so the video keeps a constant frame rate.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual bool start(std::string& error) = 0;
    virtual GrabResult grab(FrameView& frame) = 0;
    virtual void stop() = 0;
    virtual std::string error() const = 0;
    virtual std::string description() const = 0;  // "Display 1 (X11)", for the window and the log
};

}  // namespace bt::linux_app
