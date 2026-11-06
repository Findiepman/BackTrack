#pragma once

// Uploads a share copy of a clip to catbox.moe or Litterbox (core/ShareUpload.h) over HTTPS with
// WinHTTP, which ships with Windows, and returns the link.
//
// Blocking: call it on a background thread. It streams the file in small pieces, so memory stays
// small for any size, the progress moves smoothly and a cancel is noticed within a piece.

#include "core/ShareUpload.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace bt {

struct UploadResult {
    bool ok = false;
    bool cancelled = false;
    std::string url;  // ok: the direct link to the MP4
    std::string message;  // one plain sentence for the user when not ok
};

using UploadProgress = std::function<void(double fraction)>;  // 0..1, on the calling thread

// Never throws.
UploadResult upload_clip(const std::filesystem::path& file, ShareHost host, const UploadProgress& progress,
                         const std::atomic<bool>& cancel);

}  // namespace bt
