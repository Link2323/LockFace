// Exercise the real Windows JPEG codec without camera or WebView2 input.
#include "../../enrollment_app/preview_frame_encoder.h"

#include <windows.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using facelogin::FrameImage;
using facelogin::RgbPixel;

RgbPixel Pattern(int x, int y, int width, int height) {
    if (y < height / 2)
        return x < width / 2 ? RgbPixel(240, 10, 10) : RgbPixel(10, 240, 10);
    return x < width / 2 ? RgbPixel(10, 10, 240) : RgbPixel(240, 240, 240);
}

bool RoundTrip(facelogin::PreviewFrameEncoder& encoder, IWICImagingFactory& factory,
               int width, int height) {
    FrameImage source(height, width);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            source(y, x) = Pattern(x, y, width, height);
    const FrameImage original = source;
    std::string uri;
    if (!encoder.Encode(source, uri)) return false;
    if (std::memcmp(original.raw(), source.raw(), source.size() * sizeof(RgbPixel)) != 0)
        return false;
    const std::string prefix = "data:image/jpeg;base64,";
    if (uri.compare(0, prefix.size(), prefix) != 0) return false;
    const char* base64 = uri.c_str() + prefix.size();
    DWORD size = 0;
    if (!CryptStringToBinaryA(base64, 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr))
        return false;
    std::vector<BYTE> bytes(size);
    if (!CryptStringToBinaryA(base64, 0, CRYPT_STRING_BASE64, bytes.data(), &size, nullptr, nullptr))
        return false;
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), size));
    if (!stream) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> decoded;
    ComPtr<IWICFormatConverter> rgb;
    if (FAILED(factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                               decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, decoded.GetAddressOf())) ||
        FAILED(factory.CreateFormatConverter(rgb.GetAddressOf())) ||
        FAILED(rgb->Initialize(decoded.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT decodedWidth = 0, decodedHeight = 0;
    if (FAILED(rgb->GetSize(&decodedWidth, &decodedHeight)) ||
        decodedWidth != static_cast<UINT>(width) || decodedHeight != static_cast<UINT>(height))
        return false;
    FrameImage output(height, width);
    if (FAILED(rgb->CopyPixels(nullptr, width * 3, width * height * 3, output.raw()))) return false;
    // Asymmetric colors away from JPEG chroma boundaries catch RGB/BGR swaps,
    // odd row strides, and accidental vertical reversal.
    for (const int y : {height / 4, height * 3 / 4}) {
        for (const int x : {width / 4, width * 3 / 4}) {
            const auto expected = Pattern(x, y, width, height);
            const auto actual = output(y, x);
            const auto close = [](int a, int b) { return a - b <= 20 && b - a <= 20; };
            if (!close(expected.red, actual.red) || !close(expected.green, actual.green) ||
                !close(expected.blue, actual.blue)) return false;
        }
    }
    return true;
}
} // namespace

int RunPreviewFrameEncoderTests() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    int failures = 0;
    {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory)))) {
            failures++;
        } else {
            facelogin::PreviewFrameEncoder encoder;
            for (const auto dimensions : {std::pair{1, 1}, std::pair{97, 49},
                                          std::pair{640, 360}, std::pair{360, 640}}) {
                if (!RoundTrip(encoder, *factory.Get(), dimensions.first, dimensions.second)) {
                    std::fprintf(stderr, "FAIL: preview JPEG RGB/dimensions %dx%d\n",
                                 dimensions.first, dimensions.second);
                    failures++;
                }
            }
            std::string output = "stale image";
            if (encoder.Encode(FrameImage{}, output) || !output.empty()) {
                std::fprintf(stderr, "FAIL: empty preview JPEG clears output\n");
                failures++;
            }
        }
    }
    CoUninitialize();
    return failures;
}
