#pragma once

#include "linux/FrameSource.h"

#include <memory>
#include <mutex>
#include <string>

// Xlib types as opaque pointers, so X11's macros (None, Status, Bool...) stay out of the rest of
// the app, which includes Qt.
typedef struct _XDisplay Display;
typedef struct _XImage XImage;

namespace bt::linux_app {

// Screen capture on an X11 session (and Xvfb in CI): XShmGetImage of one monitor's rectangle into
// shared memory, the mouse pointer drawn in from XFixes. One copy per frame, no compositor needed.
class X11Capture final : public FrameSource {
public:
    // monitor: 0-based index into XRandR's monitor list (the `monitor` setting); out of range or no
    // XRandR: the whole screen.
    X11Capture(int monitor, bool cursor);
    ~X11Capture() override;
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    bool start(std::string& error) override;
    GrabResult grab(FrameView& frame) override;
    void stop() override;
    std::string error() const override { return error_; }
    std::string description() const override {
        std::lock_guard lock(description_mutex_);
        return description_;
    }

private:
    GrabResult grab_once(FrameView& frame);
    void draw_cursor();

    const int monitor_;
    const bool cursor_;
    Display* display_ = nullptr;
    XImage* image_ = nullptr;
    struct Shm;  // XShmSegmentInfo, defined in the .cpp
    std::unique_ptr<Shm> shm_;  // null: plain XGetImage (no MIT-SHM, e.g. a remote display)
    bool xfixes_ = false;
    int x_ = 0;
    int y_ = 0;
    int width_ = 0;
    int height_ = 0;
    int failures_ = 0;  // grabs in a row that got no image
    std::string error_;
    mutable std::mutex description_mutex_;  // the window reads it while the video thread may reopen
    std::string description_;
};

}  // namespace bt::linux_app
