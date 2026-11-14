#pragma once

namespace bt::linux_app {

// Xlib's default error handler ends the process, and the handler is one for the whole process.
// Installed once in main(): it counts errors instead, so the capture (a failed grab after a screen
// change) and the hotkey (a key another program holds) can each see their own by comparing counts.
void install_x11_error_handler();

int x11_error_count();
int x11_bad_access_count();  // BadAccess: XGrabKey on a key another program already grabbed

}  // namespace bt::linux_app
