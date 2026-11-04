#pragma once

#include <windows.h>

namespace bt::ui {

// Backtrack's icon (window, taskbar, tray), drawn at the exact pixel size asked for: the accent
// colour rounded square with the "rewind" arrow from the Windows icon font. Made at run time so
// the source has no binary image files. The caller destroys it (DestroyIcon). Null on failure.
HICON make_app_icon(int size_px);

}  // namespace bt::ui
