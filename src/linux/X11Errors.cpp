#include "linux/X11Errors.h"

#include "linux/Log.h"

#include <X11/Xlib.h>

#include <atomic>
#include <format>

namespace bt::linux_app {

namespace {

std::atomic<int> g_errors{0};
std::atomic<int> g_bad_access{0};

int count_x_error(Display*, XErrorEvent* event) {
    g_errors.fetch_add(1);
    if (event->error_code == BadAccess) {
        g_bad_access.fetch_add(1);
    }
    log_warning(std::format("X11 error {} (request {}.{})", static_cast<int>(event->error_code),
                            static_cast<int>(event->request_code), static_cast<int>(event->minor_code)));
    return 0;
}

}  // namespace

void install_x11_error_handler() {
    XSetErrorHandler(count_x_error);
}

int x11_error_count() {
    return g_errors.load();
}

int x11_bad_access_count() {
    return g_bad_access.load();
}

}  // namespace bt::linux_app
