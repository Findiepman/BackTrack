#include "platform/Picture.h"

#include "platform/Com.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>

namespace bt {
namespace {

constexpr std::uint64_t kMaxPixels = 100'000'000;

// EXIF tag 274; JPEG keeps it under APP1, TIFF and others in their own IFD. Unknown: upright.
WICBitmapTransformOptions orientation(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return WICBitmapTransformRotate0;
    for (const wchar_t* query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
            const int tag = value.vt == VT_UI2 ? value.uiVal : 1;
            PropVariantClear(&value);
            switch (tag) {
                case 2: return WICBitmapTransformFlipHorizontal;
                case 3: return WICBitmapTransformRotate180;
                case 4: return WICBitmapTransformFlipVertical;
                case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
                case 6: return WICBitmapTransformRotate90;
                case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
                case 8: return WICBitmapTransformRotate270;
                default: return WICBitmapTransformRotate0;
            }
        }
        PropVariantClear(&value);
    }
    return WICBitmapTransformRotate0;
}

}  // namespace

std::optional<Picture> load_picture(const std::filesystem::path& file, int max_side, std::string& error) {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        error = "Windows could not start its picture reader.";
        return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder))) {
        error = "That file is not a picture Windows can open. Choose a JPEG, PNG, BMP or GIF.";
        return std::nullopt;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
        error = "That picture could not be read. It may be damaged.";
        return std::nullopt;
    }
    // A small file can claim an enormous picture; decoding it would hold the worker (and Quit) up.
    if (static_cast<std::uint64_t>(width) * height > kMaxPixels) {
        error = "That picture is too big (more than 100 megapixels). Choose a smaller one.";
        return std::nullopt;
    }
    ComPtr<IWICBitmapSource> source = frame;
    // Phone photos are stored sideways with a note saying which way is up (EXIF orientation).
    if (const WICBitmapTransformOptions turn = orientation(frame.Get()); turn != WICBitmapTransformRotate0) {
        ComPtr<IWICBitmapFlipRotator> rotator;
        if (SUCCEEDED(factory->CreateBitmapFlipRotator(&rotator)) &&
            SUCCEEDED(rotator->Initialize(source.Get(), turn)) && SUCCEEDED(rotator->GetSize(&width, &height))) {
            source = rotator;
        } else {
            frame->GetSize(&width, &height);  // shown as stored rather than not at all
        }
    }
    const double scale = std::min(1.0, static_cast<double>(max_side) / static_cast<double>(std::max(width, height)));
    const UINT out_w = std::max(1u, static_cast<UINT>(std::lround(width * scale)));
    const UINT out_h = std::max(1u, static_cast<UINT>(std::lround(height * scale)));
    if (out_w != width || out_h != height) {
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(source.Get(), out_w, out_h, WICBitmapInterpolationModeFant))) {
            error = "That picture could not be resized.";
            return std::nullopt;
        }
        source = scaler;
    }
    // Premultiplied: a transparent pixel becomes black, which is what an opaque bitmap shows.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        error = "That picture's colours could not be read.";
        return std::nullopt;
    }
    Picture picture;
    picture.width = static_cast<int>(out_w);
    picture.height = static_cast<int>(out_h);
    picture.bgra.resize(static_cast<std::size_t>(out_w) * out_h * 4);
    if (FAILED(converter->CopyPixels(nullptr, out_w * 4, static_cast<UINT>(picture.bgra.size()), picture.bgra.data()))) {
        error = "That picture could not be read. It may be damaged.";
        return std::nullopt;
    }
    for (std::size_t i = 3; i < picture.bgra.size(); i += 4) picture.bgra[i] = 255;
    return picture;
}

}  // namespace bt
