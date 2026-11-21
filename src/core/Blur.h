#pragma once

// The background picture's blur (Settings > Appearance > Customise). Three box blurs in a row
// look like a Gaussian blur and cost the same whatever the radius, so a 1920 px picture blurs in
// a few tens of milliseconds on the background worker.

#include <cstdint>
#include <vector>

namespace bt {

// Blurs a BGRA picture (4 bytes a pixel, rows of width * 4 bytes) in place. radius 0 leaves it
// as it is. Edges repeat the outermost pixel, so the picture does not fade to black at its sides.
// Does nothing when the buffer is smaller than width * height * 4.
void box_blur_bgra(std::vector<std::uint8_t>& pixels, int width, int height, int radius);

// The box radius of each of the three passes for a Gaussian of about `sigma` pixels.
int box_radius_for(double sigma);

}  // namespace bt
