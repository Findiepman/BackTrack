#include "ui/Logo.h"

#include "platform/Com.h"

namespace bt::ui {
namespace {

constexpr float kSvgSize = 256.0f;

D2D1_COLOR_F rgb(unsigned value) {
    return D2D1::ColorF(static_cast<float>((value >> 16) & 0xFF) / 255.0f, static_cast<float>((value >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(value & 0xFF) / 255.0f);
}

ComPtr<ID2D1PathGeometry> triangle(ID2D1Factory* factory, D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(b);
    sink->AddLine(c);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

}  // namespace

void draw_logo(ID2D1RenderTarget* target, ID2D1Factory* factory, const D2D1_RECT_F& box) {
    const float scale = (box.right - box.left) / kSvgSize;
    D2D1_MATRIX_3X2_F previous{};
    target->GetTransform(&previous);
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(box.left, box.top) *
                         previous);

    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<ID2D1StrokeStyle> round;
    const D2D1_STROKE_STYLE_PROPERTIES style = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    if (SUCCEEDED(target->CreateSolidColorBrush(rgb(0x0e1014), &brush)) &&
        SUCCEEDED(factory->CreateStrokeStyle(style, nullptr, 0, &round))) {
        // <rect width="256" height="256" rx="58" fill="#0e1014"/>
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, kSvgSize, kSvgSize), 58, 58), brush.Get());

        // <path d="M203.3 107.8 A78 78 0 1 1 148.2 52.4" stroke="#ffb547" stroke-width="18" round caps/>
        brush->SetColor(rgb(0xffb547));
        ComPtr<ID2D1PathGeometry> ring;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory->CreatePathGeometry(&ring)) && SUCCEEDED(ring->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(203.3f, 107.8f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(148.2f, 52.4f), D2D1::SizeF(78, 78), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (SUCCEEDED(sink->Close())) target->DrawGeometry(ring.Get(), brush.Get(), 18, round.Get());
        }
        // The arrow head: <path d="M226.5 101.6 L196.1 80.8 L180.1 114 Z" fill+stroke 8, round joins/>
        if (const auto head = triangle(factory, D2D1::Point2F(226.5f, 101.6f), D2D1::Point2F(196.1f, 80.8f),
                                       D2D1::Point2F(180.1f, 114.0f))) {
            target->FillGeometry(head.Get(), brush.Get());
            target->DrawGeometry(head.Get(), brush.Get(), 8, round.Get());
        }
        // The rewind chevrons: fill+stroke #f4efe6, stroke 6, round joins.
        brush->SetColor(rgb(0xf4efe6));
        for (const float x : {134.0f, 168.0f}) {
            if (const auto chevron = triangle(factory, D2D1::Point2F(x, 106), D2D1::Point2F(x, 150),
                                              D2D1::Point2F(x - 32, 128))) {
                target->FillGeometry(chevron.Get(), brush.Get());
                target->DrawGeometry(chevron.Get(), brush.Get(), 6, round.Get());
            }
        }
    }
    target->SetTransform(previous);
}

}  // namespace bt::ui
