// One triangle that covers the whole viewport; the viewport is the part of the mouse pointer that
// is on the monitor, so no vertex buffer or coordinates are needed.
float4 main(uint id : SV_VertexID) : SV_Position {
    const float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
