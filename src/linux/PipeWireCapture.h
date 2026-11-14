#pragma once

#include "linux/FrameSource.h"

#include <cstdint>
#include <memory>

namespace bt::linux_app {

// Screen capture on Wayland: the PipeWire stream the ScreenCast portal opened (Portal.h hands over
// the remote's file descriptor and the stream's node id). PipeWire runs its own thread and copies
// each new image into `latest_`; grab() takes the newest one. KWin only sends an image when the
// screen changed, so grab() hands out the last image again in between (a constant frame rate).
//
// Only memory buffers (MemPtr/MemFd) are accepted, no DMA-BUF: no format modifier is offered, so
// the compositor falls back to shared memory. Simpler and driver independent; costs one copy.
class PipeWireCapture final : public FrameSource {
public:
    // Takes ownership of `pipewire_fd`.
    PipeWireCapture(int pipewire_fd, std::uint32_t node, int fps, std::string description);
    ~PipeWireCapture() override;
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    bool start(std::string& error) override;
    GrabResult grab(FrameView& frame) override;
    void stop() override;
    std::string error() const override;
    std::string description() const override { return description_; }

    struct Impl;  // the PipeWire objects and callbacks, in the .cpp

private:
    std::unique_ptr<Impl> impl_;
    const std::string description_;
};

}  // namespace bt::linux_app
