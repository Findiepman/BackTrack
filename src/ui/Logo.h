#pragma once

// The Backtrack logo ("Ring Rewind", branding/logo-options/01-ring-rewind.svg, the version for dark
// backgrounds): a dark rounded square, an amber ring that loops back on itself into an arrow, and
// a light rewind double-chevron inside. Drawn from the SVG's own coordinates (a 256 x 256 box) so
// it is sharp at every size, from the 16 px tray icon to the About page.

#include <d2d1.h>

namespace bt::ui {

// Draws the logo filling `box` (a square) on any Direct2D target.
void draw_logo(ID2D1RenderTarget* target, ID2D1Factory* factory, const D2D1_RECT_F& box);

}  // namespace bt::ui
