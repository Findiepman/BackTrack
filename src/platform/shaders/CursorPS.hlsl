// Draws the mouse pointer over the desktop exactly like core/CaptureMath composite_pixel(), in
// integers. XOR pointers (the I-beam) need the desktop under them, and a render target cannot be
// read while it is drawn to, so the desktop under the pointer comes in as a copy (t0).

Texture2D<float4> desktop : register(t0);  // the desktop under the pointer, pointer-sized
Texture2D<uint2> pointer : register(t1);  // x = premultiplied colour, y = XOR mask, packed B | G << 8 | R << 16 | A << 24

cbuffer Placement : register(b0) {
    int2 origin;  // the pointer's top-left corner in the frame, may be negative
    int2 unused;
};

uint3 unpack_rgb(uint packed) {
    return uint3((packed >> 16) & 255, (packed >> 8) & 255, packed & 255);
}

float4 main(float4 position : SV_Position) : SV_Target {
    const int3 at = int3(int2(position.xy) - origin, 0);
    const uint2 p = pointer.Load(at);
    const float4 under = desktop.Load(at);
    const uint3 screen = uint3(round(saturate(under.rgb) * 255.0));
    const uint alpha = p.x >> 24;
    const uint3 blended = min(unpack_rgb(p.x) + (screen * (255 - alpha) + 127) / 255, 255);
    return float4(float3(blended ^ unpack_rgb(p.y)) / 255.0, under.a);
}
