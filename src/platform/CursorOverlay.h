#pragma once

// Draws the mouse pointer into the recording. Desktop Duplication leaves it out of the frame and
// reports it separately (position, visibility and shape with each frame), so it is put back here,
// on the GPU, the way Windows draws it: alpha-blended colour pointers as well as the ones that
// invert the screen (the I-beam over text).
//
// The pointer goes onto a copy of the desktop texture, never onto the texture itself: the capture
// re-sends the last desktop image when nothing new arrives, and a pointer drawn into it would stay
// behind on screen after it moved.
//
// Capture thread only (ScreenCapture owns it). Per frame it costs nothing while the pointer and the
// desktop stay still; otherwise one GPU copy of the frame and a pointer-sized draw. Nothing is read
// back to the CPU and nothing is allocated per frame (only when the pointer's shape changes).

#include "core/CaptureMath.h"
#include "platform/Com.h"
#include "platform/Log.h"

#include <d3d10_1.h>  // ID3D10Multithread; d3d10_1.h rather than d3d10.h, which must not come before it
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <vector>

namespace bt {

class CursorOverlay {
public:
    // device: the capture's device, multithread protected (the encoder shares it).
    CursorOverlay(ID3D11Device* device, ID3D11DeviceContext* context);
    CursorOverlay(const CursorOverlay&) = delete;
    CursorOverlay& operator=(const CursorOverlay&) = delete;

    // A new desktop texture (the pipeline was rebuilt: first frame, mode change, rotation). The
    // copy the pointer is drawn on is made in its size. `desktop` is its size the right way up.
    void set_frame(ID3D11Texture2D* frame, PixelSize desktop, Rotation rotation);
    // The texture with the pointer in it, for the video processor's input view.
    ID3D11Texture2D* composite() const { return composite_.Get(); }

    // With every frame AcquireNextFrame returns, before ReleaseFrame (the shape can only be read
    // until then). A frame may carry only a pointer update, or nothing about the pointer.
    void update(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& info);

    // Once per delivered frame, before it is converted. True: composite() holds the frame with the
    // pointer. False: there is no pointer to draw (hidden by the game, on another monitor, shape not
    // known yet); convert the desktop texture as it is. frame_changed: the desktop texture got new
    // content since the last call. Throws on a Direct3D failure.
    bool compose(bool frame_changed);

private:
    void upload_shape();
    void draw(const PixelRect& rect, const PixelRect& visible);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D10Multithread> multithread_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> placement_;  // the pixel shader's constant buffer

    ComPtr<ID3D11Texture2D> frame_;  // the clean desktop image (ScreenCapture's)
    ComPtr<ID3D11Texture2D> composite_;
    ComPtr<ID3D11RenderTargetView> composite_target_;
    PixelSize desktop_;
    Rotation rotation_ = Rotation::None;

    // The pointer as DXGI last reported it, in desktop orientation.
    bool visible_ = false;
    PixelPoint position_;  // top-left corner of the shape, relative to the monitor
    std::vector<std::uint8_t> shape_bytes_;  // grows to the largest shape seen, never shrinks
    CursorImage image_;
    bool shape_stale_ = false;  // image_ changed (or the rotation did) since it was uploaded

    // The pointer on the GPU, turned for the texture.
    ComPtr<ID3D11ShaderResourceView> pointer_view_;
    ComPtr<ID3D11Texture2D> under_;  // the desktop under the pointer, copied out before each draw
    ComPtr<ID3D11ShaderResourceView> under_view_;
    PixelSize uploaded_size_;  // of the uploaded pointer in desktop orientation, as cursor_rect takes it
    PixelPoint placement_origin_;  // what the constant buffer holds, once placement_set_
    bool placement_set_ = false;

    bool drawn_ = false;  // composite_ holds the current frame with the pointer where it is now
    bool dirty_ = false;  // the pointer moved, appeared, disappeared or changed shape
    log::RateLimit shape_log_{std::chrono::seconds(10)};
};

}  // namespace bt
