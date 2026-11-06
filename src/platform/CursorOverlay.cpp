#include "platform/CursorOverlay.h"

#include <d3d10.h>

#include <algorithm>
#include <array>
#include <span>

// Compiled from src/platform/shaders/*.hlsl by fxc at build time (CMakeLists.txt).
#include "shaders/CursorPS.h"
#include "shaders/CursorVS.h"

namespace bt {
namespace {

static_assert(sizeof(CursorPixel) == 2 * sizeof(std::uint32_t), "CursorPixel is uploaded as R32G32_UINT");

struct alignas(16) Placement {
    std::int32_t origin_x = 0;
    std::int32_t origin_y = 0;
    std::int32_t unused[2] = {};
};

const char* shape_name(UINT type) {
    switch (type) {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: return "monochrome";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: return "colour";
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: return "masked colour";
        default: return "unknown";
    }
}

// Several Direct3D calls that must not be interleaved with the encoder's calls on the same
// immediate context (it runs on Media Foundation's threads). The device's multithread protection
// makes each call atomic; Enter/Leave makes the whole sequence atomic.
class DeviceLock {
public:
    explicit DeviceLock(ID3D10Multithread* multithread) : multithread_(multithread) { multithread_->Enter(); }
    ~DeviceLock() { multithread_->Leave(); }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;

private:
    ID3D10Multithread* multithread_;
};

}  // namespace

CursorOverlay::CursorOverlay(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context) {
    BT_CHECK_HR(device_.As(&multithread_));
    BT_CHECK_HR(device_->CreateVertexShader(kCursorVS, sizeof(kCursorVS), nullptr, &vertex_shader_));
    BT_CHECK_HR(device_->CreatePixelShader(kCursorPS, sizeof(kCursorPS), nullptr, &pixel_shader_));
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Placement);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    BT_CHECK_HR(device_->CreateBuffer(&desc, nullptr, &placement_));
}

void CursorOverlay::set_frame(ID3D11Texture2D* frame, PixelSize desktop, Rotation rotation) {
    composite_target_.Reset();
    composite_.Reset();
    frame_ = frame;
    drawn_ = false;
    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;  // as the frame: the video processor reads it
    BT_CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &composite_));
    BT_CHECK_HR(device_->CreateRenderTargetView(composite_.Get(), nullptr, &composite_target_));
    if (rotation != rotation_) {
        shape_stale_ = true;  // the same pointer, turned the other way (or a pending drop of it)
    }
    desktop_ = desktop;
    rotation_ = rotation;
}

void CursorOverlay::update(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& info) {
    // LastMouseUpdateTime is 0 when this frame says nothing about the pointer; then the position and
    // visibility in it are not valid and the previous ones stand.
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool visible = info.PointerPosition.Visible != FALSE;
        const PixelPoint position{info.PointerPosition.Position.x, info.PointerPosition.Position.y};
        if (visible != visible_ || (visible && !(position == position_))) {
            dirty_ = true;
        }
        visible_ = visible;
        if (visible) {
            position_ = position;
        }
    }
    if (info.PointerShapeBufferSize == 0) {
        return;  // the shape did not change
    }
    if (shape_bytes_.size() < info.PointerShapeBufferSize) {
        shape_bytes_.resize(info.PointerShapeBufferSize);
    }
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    const HRESULT hr = duplication->GetFramePointerShape(static_cast<UINT>(shape_bytes_.size()), shape_bytes_.data(),
                                                         &required, &shape);
    if (FAILED(hr)) {
        // The previous shape stays. A lost duplication is noticed by ReleaseFrame right after.
        if (shape_log_.allow()) {
            log::debug("Reading the mouse pointer's shape failed: {}", hresult_text(hr));
        }
        return;
    }
    CursorShapeType type = CursorShapeType::Color;
    if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        type = CursorShapeType::Monochrome;
    } else if (shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        type = CursorShapeType::MaskedColor;
    } else if (shape.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        if (shape_log_.allow()) {
            log::debug("Mouse pointer shape of unknown type {}, not drawn", shape.Type);
        }
        image_ = CursorImage{};
        shape_stale_ = true;  // drops the previous pointer's textures, so nothing is drawn
        dirty_ = true;
        return;
    }
    const std::size_t used = std::min<std::size_t>(required != 0 ? required : info.PointerShapeBufferSize,
                                                   shape_bytes_.size());
    image_ = convert_cursor_shape(CursorShape{type, static_cast<int>(shape.Width), static_cast<int>(shape.Height),
                                              static_cast<int>(shape.Pitch), std::span(shape_bytes_.data(), used)});
    if (shape_log_.allow()) {
        log::debug("Mouse pointer shape: {} {}x{}, pitch {}, hot spot {},{}{}", shape_name(shape.Type), shape.Width,
                   shape.Height, shape.Pitch, shape.HotSpot.x, shape.HotSpot.y,
                   image_.empty() ? " (does not fit its data, not drawn)" : "");
    }
    shape_stale_ = true;
    dirty_ = true;
}

void CursorOverlay::upload_shape() {
    pointer_view_.Reset();
    under_view_.Reset();
    under_.Reset();
    uploaded_size_ = PixelSize{};
    if (image_.empty()) {
        return;
    }
    const CursorImage turned = place_cursor(image_, PixelPoint{}, desktop_, rotation_).image;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(turned.width);
    desc.Height = static_cast<UINT>(turned.height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = turned.pixels.data();
    data.SysMemPitch = static_cast<UINT>(turned.width) * sizeof(CursorPixel);
    ComPtr<ID3D11Texture2D> pointer;
    BT_CHECK_HR(device_->CreateTexture2D(&desc, &data, &pointer));
    BT_CHECK_HR(device_->CreateShaderResourceView(pointer.Get(), nullptr, &pointer_view_));

    // Same format as the desktop texture, so the part under the pointer can be copied straight out.
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    BT_CHECK_HR(device_->CreateTexture2D(&desc, nullptr, &under_));
    BT_CHECK_HR(device_->CreateShaderResourceView(under_.Get(), nullptr, &under_view_));
    uploaded_size_ = PixelSize{image_.width, image_.height};
}

bool CursorOverlay::compose(bool frame_changed) {
    if (!frame_ || !composite_) {
        return false;
    }
    if (shape_stale_) {
        shape_stale_ = false;
        drawn_ = false;
        upload_shape();
    }
    if (!visible_ || !pointer_view_) {
        drawn_ = false;
        dirty_ = false;
        return false;
    }
    const PixelRect rect = cursor_rect(uploaded_size_, position_, desktop_, rotation_);
    const PixelSize texture = texture_size(desktop_, rotation_);
    const PixelRect visible = intersect(rect, PixelRect{0, 0, texture.width, texture.height});
    if (visible.empty()) {
        drawn_ = false;
        dirty_ = false;
        return false;
    }
    if (drawn_ && !dirty_ && !frame_changed) {
        return true;  // the composite from the last frame is still exactly right
    }
    draw(rect, visible);
    drawn_ = true;
    dirty_ = false;
    return true;
}

void CursorOverlay::draw(const PixelRect& rect, const PixelRect& visible) {
    DeviceLock lock(multithread_.Get());
    ID3D11DeviceContext* c = context_.Get();
    c->CopyResource(composite_.Get(), frame_.Get());
    const D3D11_BOX box{static_cast<UINT>(visible.left), static_cast<UINT>(visible.top), 0,
                        static_cast<UINT>(visible.right), static_cast<UINT>(visible.bottom), 1};
    c->CopySubresourceRegion(under_.Get(), 0, static_cast<UINT>(visible.left - rect.left),
                             static_cast<UINT>(visible.top - rect.top), 0, frame_.Get(), 0, &box);
    const PixelPoint origin{rect.left, rect.top};
    if (!placement_set_ || !(origin == placement_origin_)) {
        Placement placement;
        placement.origin_x = origin.x;
        placement.origin_y = origin.y;
        c->UpdateSubresource(placement_.Get(), 0, nullptr, &placement, 0, 0);
        placement_origin_ = origin;
        placement_set_ = true;
    }

    // Every stage this draw depends on is set, so whatever else ran on the context cannot change it.
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0);
    c->DSSetShader(nullptr, nullptr, 0);
    c->GSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(pixel_shader_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {under_view_.Get(), pointer_view_.Get()};
    c->PSSetShaderResources(0, static_cast<UINT>(std::size(views)), views);
    ID3D11Buffer* buffers[] = {placement_.Get()};
    c->PSSetConstantBuffers(0, 1, buffers);
    c->RSSetState(nullptr);
    c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(nullptr, 0);
    // The viewport is the visible part of the pointer: the one triangle fills exactly those pixels.
    const D3D11_VIEWPORT viewport{static_cast<float>(visible.left), static_cast<float>(visible.top),
                                  static_cast<float>(visible.width()), static_cast<float>(visible.height()), 0.0f, 1.0f};
    c->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* targets[] = {composite_target_.Get()};
    c->OMSetRenderTargets(1, targets, nullptr);
    c->Draw(3, 0);

    // Unbound again, so the composite is not still bound as a render target while the video
    // processor reads it.
    c->OMSetRenderTargets(0, nullptr, nullptr);
    std::array<ID3D11ShaderResourceView*, 2> none{};
    c->PSSetShaderResources(0, static_cast<UINT>(none.size()), none.data());
}

}  // namespace bt
