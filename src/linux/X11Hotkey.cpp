#include "linux/X11Hotkey.h"

#include "core/LinuxDesktop.h"
#include "linux/Log.h"
#include "linux/X11Errors.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>

namespace bt::linux_app {

namespace {

unsigned x11_modifiers(std::uint32_t modifiers) {
    unsigned mask = 0;
    if ((modifiers & hotkey_mod::kCtrl) != 0) mask |= ControlMask;
    if ((modifiers & hotkey_mod::kShift) != 0) mask |= ShiftMask;
    if ((modifiers & hotkey_mod::kAlt) != 0) mask |= Mod1Mask;
    if ((modifiers & hotkey_mod::kWin) != 0) mask |= Mod4Mask;
    return mask;
}

// CapsLock (LockMask) and NumLock (Mod2Mask on nearly every layout) are part of the state X11
// compares, so each combination needs its own grab.
constexpr unsigned kIgnoredMasks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

}  // namespace

X11Hotkey::~X11Hotkey() {
    stop();
}

bool X11Hotkey::start(const Hotkey& hotkey, std::function<void()> pressed, std::string& error) {
    stop();
    const auto keysym = x11_keysym_for_vk(hotkey.virtual_key);
    if (!keysym) {
        error = "This key has no X11 equivalent";
        return false;
    }
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        error = "No X11 display";
        return false;
    }
    const KeyCode code = XKeysymToKeycode(display, static_cast<KeySym>(*keysym));
    if (code == 0) {
        XCloseDisplay(display);
        error = "This key is not on the keyboard layout";
        return false;
    }
    const Window root = DefaultRootWindow(display);
    const unsigned modifiers = x11_modifiers(hotkey.modifiers);
    // BadAccess: another program holds this key (main() installed the counting error handler).
    const int bad_access_before = x11_bad_access_count();
    for (const unsigned extra : kIgnoredMasks) {
        XGrabKey(display, code, modifiers | extra, root, False, GrabModeAsync, GrabModeAsync);
    }
    XSync(display, False);
    if (x11_bad_access_count() != bad_access_before) {
        XCloseDisplay(display);  // releases whatever was grabbed
        error = "Another program already uses " + hotkey_to_string(hotkey);
        return false;
    }
    // Holding the key: X11 repeats KeyPress, without a KeyRelease in between once detectable
    // auto-repeat is on, so one press = one clip.
    XkbSetDetectableAutoRepeat(display, True, nullptr);
    XSelectInput(display, root, KeyPressMask | KeyReleaseMask);
    if (pipe2(wake_, O_CLOEXEC) != 0) {
        XCloseDisplay(display);
        error = "Cannot start the hotkey thread";
        return false;
    }
    display_ = display;
    pressed_ = std::move(pressed);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    log_info("Hotkey " + hotkey_to_string(hotkey) + " registered with X11");
    return true;
}

void X11Hotkey::loop() {
    auto* display = static_cast<Display*>(display_);
    pollfd fds[2] = {{ConnectionNumber(display), POLLIN, 0}, {wake_[0], POLLIN, 0}};
    bool down = false;
    while (running_) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == KeyPress && !down) {
                down = true;
                if (pressed_) {
                    pressed_();
                }
            } else if (event.type == KeyRelease) {
                down = false;
            }
        }
        if (poll(fds, 2, -1) < 0 && errno != EINTR) {
            break;
        }
        if ((fds[0].revents & (POLLERR | POLLHUP)) != 0) {
            log_warning("X11 connection lost: the hotkey stops");
            break;
        }
    }
}

void X11Hotkey::stop() {
    if (thread_.joinable()) {
        running_ = false;
        const char byte = 1;
        [[maybe_unused]] const auto written = write(wake_[1], &byte, 1);
        thread_.join();
    }
    if (display_ != nullptr) {
        XCloseDisplay(static_cast<Display*>(display_));
        display_ = nullptr;
    }
    for (int& fd : wake_) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
}

}  // namespace bt::linux_app
