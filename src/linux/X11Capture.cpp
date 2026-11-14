#include "linux/X11Capture.h"

#include "linux/Log.h"
#include "linux/X11Errors.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <algorithm>
#include <atomic>
#include <format>

namespace bt::linux_app {

struct X11Capture::Shm {
    XShmSegmentInfo info{};
    bool attached = false;
};

namespace {

// Grabs that fail in a row (about a second at 60 fps) before the screen's size and monitors are
// read again: a resolution or monitor change makes every XShmGetImage of the old rectangle fail.
constexpr int kFailuresBeforeReopen = 60;

std::uint8_t blend(std::uint8_t dst, std::uint32_t src, std::uint32_t alpha) {
    // The cursor image is premultiplied ARGB.
    return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, src + dst * (255 - alpha) / 255));
}

}  // namespace

X11Capture::X11Capture(int monitor, bool cursor) : monitor_(monitor), cursor_(cursor) {}

X11Capture::~X11Capture() {
    stop();
}

bool X11Capture::start(std::string& error) {
    stop();
    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
        error = error_ = "Cannot open the X11 display (is DISPLAY set?)";
        return false;
    }
    const int screen = DefaultScreen(display_);
    const Window root = RootWindow(display_, screen);
    x_ = 0;
    y_ = 0;
    width_ = DisplayWidth(display_, screen);
    height_ = DisplayHeight(display_, screen);
    std::string description = std::format("Screen {}x{} (X11)", width_, height_);

    int event_base = 0;
    int error_base = 0;
    if (XRRQueryExtension(display_, &event_base, &error_base) != 0) {
        int count = 0;
        XRRMonitorInfo* monitors = XRRGetMonitors(display_, root, True, &count);
        if (monitors != nullptr) {
            if (monitor_ >= 0 && monitor_ < count) {
                const XRRMonitorInfo& m = monitors[monitor_];
                x_ = m.x;
                y_ = m.y;
                width_ = m.width;
                height_ = m.height;
                description = std::format("Display {} (X11, {}x{})", monitor_ + 1, width_, height_);
            } else if (count > 0) {
                log_warning(std::format("Display {} does not exist ({} found): recording the whole screen",
                                        monitor_ + 1, count));
            }
            XRRFreeMonitors(monitors);
        }
    }
    // The encoders want even sizes (4:2:0 chroma); an odd last column or row is left out.
    width_ &= ~1;
    height_ &= ~1;
    {
        std::lock_guard lock(description_mutex_);
        description_ = description;
    }
    if (width_ < 2 || height_ < 2) {
        error = error_ = "The X11 screen has no size";
        stop();
        return false;
    }

    Visual* visual = DefaultVisual(display_, screen);
    const int depth = DefaultDepth(display_, screen);
    if (XShmQueryExtension(display_) != 0) {
        auto shm = std::make_unique<Shm>();
        image_ = XShmCreateImage(display_, visual, static_cast<unsigned>(depth), ZPixmap, nullptr, &shm->info,
                                 static_cast<unsigned>(width_), static_cast<unsigned>(height_));
        if (image_ != nullptr) {
            shm->info.shmid = shmget(IPC_PRIVATE, static_cast<std::size_t>(image_->bytes_per_line) * image_->height,
                                     IPC_CREAT | 0600);
            if (shm->info.shmid >= 0) {
                shm->info.shmaddr = image_->data = static_cast<char*>(shmat(shm->info.shmid, nullptr, 0));
                if (shm->info.shmaddr != reinterpret_cast<char*>(-1)) {
                    shm->info.readOnly = False;
                    const int errors_before = x11_error_count();
                    shm->attached = XShmAttach(display_, &shm->info) != 0;
                    XSync(display_, False);
                    shm->attached = shm->attached && x11_error_count() == errors_before;
                }
                // Marked for removal now: the segment goes when both sides detach, even after a crash.
                shmctl(shm->info.shmid, IPC_RMID, nullptr);
            }
            if (shm->attached) {
                shm_ = std::move(shm);
            } else {
                if (shm->info.shmaddr != nullptr && shm->info.shmaddr != reinterpret_cast<char*>(-1)) {
                    shmdt(shm->info.shmaddr);
                }
                image_->data = nullptr;
                XDestroyImage(image_);
                image_ = nullptr;
                log_warning("MIT-SHM did not work: capturing with XGetImage (slower)");
            }
        }
    }

    int fixes_event = 0;
    int fixes_error = 0;
    xfixes_ = cursor_ && XFixesQueryExtension(display_, &fixes_event, &fixes_error) != 0;
    log_info(std::format("Capture: {}, {}", description, shm_ ? "MIT-SHM" : "XGetImage"));
    return true;
}

GrabResult X11Capture::grab(FrameView& frame) {
    if (display_ == nullptr) {
        return GrabResult::Failed;
    }
    const GrabResult result = grab_once(frame);
    if (result != GrabResult::NotYet) {
        failures_ = 0;
        return result;
    }
    if (++failures_ < kFailuresBeforeReopen) {
        return result;
    }
    failures_ = 0;
    log_info("Screen capture keeps failing (screen size or monitors changed?): reading the screen again");
    std::string error;
    if (!start(error)) {
        error_ = error;
        return GrabResult::Failed;
    }
    return GrabResult::NotYet;
}

GrabResult X11Capture::grab_once(FrameView& frame) {
    const Window root = DefaultRootWindow(display_);
    const int errors_before = x11_error_count();
    if (shm_) {
        if (XShmGetImage(display_, root, image_, x_, y_, AllPlanes) == 0) {
            return GrabResult::NotYet;  // a screen change mid-grab: try the next frame
        }
    } else {
        if (image_ != nullptr) {
            XDestroyImage(image_);
        }
        image_ = XGetImage(display_, root, x_, y_, static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                           AllPlanes, ZPixmap);
        if (image_ == nullptr) {
            return GrabResult::NotYet;
        }
    }
    if (x11_error_count() != errors_before) {
        return GrabResult::NotYet;
    }
    if (image_->bits_per_pixel != 32 || image_->byte_order != LSBFirst) {
        error_ = std::format("Unsupported X11 pixel format ({} bits per pixel)", image_->bits_per_pixel);
        return GrabResult::Failed;
    }
    if (xfixes_) {
        draw_cursor();
    }
    frame.data = reinterpret_cast<const std::uint8_t*>(image_->data);
    frame.width = width_;
    frame.height = height_;
    frame.stride = image_->bytes_per_line;
    frame.layout = image_->red_mask == 0xFF ? PixelLayout::Rgbx : PixelLayout::Bgrx;
    return GrabResult::Frame;
}

void X11Capture::draw_cursor() {
    XFixesCursorImage* cursor = XFixesGetCursorImage(display_);
    if (cursor == nullptr) {
        return;
    }
    const bool rgb = image_->red_mask == 0xFF;
    const int left = cursor->x - cursor->xhot - x_;
    const int top = cursor->y - cursor->yhot - y_;
    for (int cy = 0; cy < cursor->height; ++cy) {
        const int y = top + cy;
        if (y < 0 || y >= height_) {
            continue;
        }
        auto* row = reinterpret_cast<std::uint8_t*>(image_->data) + static_cast<std::ptrdiff_t>(y) * image_->bytes_per_line;
        for (int cx = 0; cx < cursor->width; ++cx) {
            const int x = left + cx;
            if (x < 0 || x >= width_) {
                continue;
            }
            // XFixes hands out unsigned long per pixel (64 bits on x86_64) with ARGB in the low 32.
            const auto argb = static_cast<std::uint32_t>(cursor->pixels[cy * cursor->width + cx]);
            const std::uint32_t a = argb >> 24;
            if (a == 0) {
                continue;
            }
            const std::uint32_t r = (argb >> 16) & 0xFF;
            const std::uint32_t g = (argb >> 8) & 0xFF;
            const std::uint32_t b = argb & 0xFF;
            std::uint8_t* p = row + static_cast<std::ptrdiff_t>(x) * 4;
            p[0] = blend(p[0], rgb ? r : b, a);
            p[1] = blend(p[1], g, a);
            p[2] = blend(p[2], rgb ? b : r, a);
        }
    }
    XFree(cursor);
}

void X11Capture::stop() {
    if (display_ != nullptr && shm_ && shm_->attached) {
        XShmDetach(display_, &shm_->info);
        XSync(display_, False);
    }
    if (shm_) {
        shmdt(shm_->info.shmaddr);
        if (image_ != nullptr) {
            image_->data = nullptr;  // not malloc'ed: XDestroyImage must not free it
        }
        shm_.reset();
    }
    if (image_ != nullptr) {
        XDestroyImage(image_);
        image_ = nullptr;
    }
    if (display_ != nullptr) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

}  // namespace bt::linux_app
