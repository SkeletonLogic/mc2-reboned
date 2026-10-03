#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>

#include "../renderer_image_decode.h"

namespace mc2gfx {

bool DecodeJpeg(const unsigned char* bytes, size_t size, JpegImage& image)
{
    image = JpegImage{};
    if (!bytes || !size || size > static_cast<size_t>(UINT_MAX)) return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = FAILED(comResult) && comResult != RPC_E_CHANGED_MODE
        ? comResult
        : CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(
        const_cast<BYTE*>(bytes), static_cast<DWORD>(size));
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(
        stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(
        // PORT: canonical RGBA8, matching renderer_tga.h and the RGBA8Unorm
        // textures the renderer creates. Requesting BGRA here exchanged red and
        // blue in every JPEG-sourced texture -- see renderer_image_decode.h.
        // PORT: BGRA to match the decoder convention -- see renderer_tga.h.
        frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);

    const size_t stride = static_cast<size_t>(width) * 4u;
    size_t byteCount = 0;
    if (SUCCEEDED(hr) && (!width || !height || stride > UINT_MAX ||
                         static_cast<size_t>(height) > UINT_MAX / stride))
        hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) byteCount = stride * static_cast<size_t>(height);
    if (SUCCEEDED(hr)) image.rgba.resize(byteCount);
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(
        nullptr, static_cast<UINT>(stride), static_cast<UINT>(byteCount),
        image.rgba.data());

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(comResult)) CoUninitialize();

    if (FAILED(hr))
    {
        image = JpegImage{};
        return false;
    }
    image.width = width;
    image.height = height;
    return true;
}

} // namespace mc2gfx
