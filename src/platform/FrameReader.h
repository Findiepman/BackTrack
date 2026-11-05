#pragma once

// Decoding single frames of a clip into small BGRA pictures, shared by the library thumbnails
// (MediaProbe) and the trim bar's filmstrip. Worker threads only: decoding is slow.

#include "platform/Com.h"

#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace bt {

// The visible part of a decoded frame: H.264 1080p is coded as 1088 lines and the extra 8 are
// padding, named by the minimum display aperture.
struct VisibleArea {
    UINT32 x = 0, y = 0, width = 0, height = 0;
};

VisibleArea visible_area(IMFMediaType* type);

// Selects only the first video stream and asks the reader to decode it to RGB32. The reader must
// have been created with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING. False when it cannot.
bool select_rgb_video(IMFSourceReader* reader);

// Reads the next decoded frame and scales it to width x height BGRA (top-down rows, stride
// width * 4, alpha 255), cropping to keep the aspect ratio (cover). Fine text is averaged, not
// sampled, so it does not shimmer. Empty when there is no frame (end of file, error); `name` is
// only for the log. `time` receives the frame's timestamp when not null.
std::vector<std::uint8_t> read_scaled_frame(IMFSourceReader* reader, int width, int height, std::string_view name,
                                            LONGLONG* time = nullptr);

}  // namespace bt
