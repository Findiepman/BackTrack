#pragma once

// Uploads a share copy of a clip to catbox.moe or Litterbox (core/ShareUpload.h) over HTTPS with
// WinHTTP, which ships with Windows, and returns the link.
//
// Blocking: call it on a background thread. It streams the file in small pieces, so memory stays
// small for any size and the progress moves smoothly.

#include "core/ShareUpload.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace bt {

struct UploadResult {
    bool ok = false;
    bool cancelled = false;
    std::string url;  // ok: the direct link to the MP4
    std::string message;  // one plain sentence for the user when not ok
};

using UploadProgress = std::function<void(double fraction)>;  // 0..1, on the calling thread

// Stops an upload from another thread at once. A cancel flag is not enough: the upload spends
// most of its time inside blocking WinHTTP calls (connecting, a piece on a stalled line, waiting
// up to minutes for the host to store a big file). Closing the request handle from another
// thread is the documented way to make such a call return right away.
class UploadAbort {
public:
    void abort();  // any thread, any number of times
    bool aborted() const;

private:
    friend class UploadRequestGuard;
    mutable std::mutex mutex_;
    void* request_ = nullptr;  // the HINTERNET of the running request; null when none
    bool aborted_ = false;
};

// Never throws.
UploadResult upload_clip(const std::filesystem::path& file, ShareHost host, const UploadProgress& progress,
                         UploadAbort& abort);

}  // namespace bt
