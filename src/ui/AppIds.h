#pragma once

// Window messages and timers shared by the App source files.

#include <windows.h>

namespace bt::ui::ids {

inline constexpr UINT kMsgSaved = WM_APP + 2;
inline constexpr UINT kMsgProbe = WM_APP + 3;
inline constexpr UINT kMsgTray = WM_APP + 4;
inline constexpr UINT kMsgPlayer = WM_APP + 5;  // wparam: MF_MEDIA_ENGINE_EVENT
inline constexpr UINT kMsgWork = WM_APP + 6;  // the background worker finished something
inline constexpr UINT_PTR kTickTimer = 1;
inline constexpr UINT kTickMs = 1000;
inline constexpr UINT_PTR kPlayerTimer = 2;
inline constexpr UINT kPlayerMs = 15;  // the clip page pulls video frames this often (about 60 per second)

}  // namespace bt::ui::ids
