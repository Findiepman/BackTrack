#pragma once

#include "core/Hotkey.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace bt::linux_app {

// A global hotkey on X11: XGrabKey on the root window on its own display connection and thread,
// so it works while a game has the focus. NumLock and CapsLock do not matter (every combination
// with them is grabbed too). `pressed` runs on the hotkey thread.
class X11Hotkey {
public:
    X11Hotkey() = default;
    ~X11Hotkey();
    X11Hotkey(const X11Hotkey&) = delete;
    X11Hotkey& operator=(const X11Hotkey&) = delete;

    // False when the key cannot be grabbed (taken by another program, no X11 display).
    bool start(const Hotkey& hotkey, std::function<void()> pressed, std::string& error);
    void stop();

private:
    void loop();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::function<void()> pressed_;
    void* display_ = nullptr;  // Display*, kept opaque so Xlib's macros stay in the .cpp
    int wake_[2] = {-1, -1};  // a pipe that ends the wait in loop()
};

}  // namespace bt::linux_app
