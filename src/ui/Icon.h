#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace bt::ui {

// The logo (ui/Logo) drawn at exactly `size_px` square: BGRA, straight (not premultiplied) alpha,
// top-down rows. Empty on failure. Needs COM on the calling thread (WIC).
std::vector<std::uint8_t> render_logo_pixels(int size_px);

// Backtrack's icon (window, taskbar, tray) at the exact pixel size asked for. The exe also carries
// it as an .ico resource (made from the same drawing at build time by tools/make_icon) for
// Explorer and the Start menu. The caller destroys it (DestroyIcon). Null on failure.
HICON make_app_icon(int size_px);

}  // namespace bt::ui
