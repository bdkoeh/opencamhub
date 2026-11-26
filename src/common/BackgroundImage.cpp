#include "BackgroundImage.h"
#include <algorithm>
#include <wincodec.h>
#include <wrl/client.h>
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

bool BackgroundImage::Load(const std::wstring& path, UINT w, UINT h, YuvFormat format)
{
    if (!w || !h || (w | h) & 1 || (uint64_t)w * h > UINT_MAX / 3) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT sw, sh;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&sw, &sh)) ||
        !sw || !sh || sw > INT_MAX || sh > INT_MAX) return false;

    UINT cw = sw, ch = sh;
    if ((uint64_t)sw * h > (uint64_t)sh * w)
        cw = std::max(1u, (UINT)((uint64_t)sh * w / h));
    else
        ch = std::max(1u, (UINT)((uint64_t)sw * h / w));
    WICRect crop{ (INT)((sw - cw) / 2), (INT)((sh - ch) / 2), (INT)cw, (INT)ch };
    ComPtr<IWICBitmapClipper> clip;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> rgb;
    if (FAILED(factory->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(frame.Get(), &crop)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clip.Get(), w, h, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&rgb)) ||
        FAILED(rgb->Initialize(scaler.Get(), GUID_WICPixelFormat24bppRGB, WICBitmapDitherTypeNone,
                               nullptr, 0, WICBitmapPaletteTypeCustom))) return false;

    std::vector<BYTE> pixels((size_t)w * h * 3);
    if (FAILED(rgb->CopyPixels(nullptr, w * 3, (UINT)pixels.size(), pixels.data()))) return false;
    m_width = w;
    m_height = h;
    m_pixels.resize((size_t)w * h * 3 / 2);
    const float kr = format.bt709 ? .2126f : .299f, kb = format.bt709 ? .0722f : .114f;
    const float ys = format.fullRange ? 1.f : 219.f / 255, cs = format.fullRange ? 1.f : 224.f / 255;
    auto byte = [](float v) { return (BYTE)std::clamp((int)(v + .5f), 0, 255); };
    for (UINT y = 0; y < h; y += 2)
        for (UINT x = 0; x < w; x += 2)
        {
            float cb = 0, cr = 0;
            for (UINT j = 0; j < 2; j++)
                for (UINT i = 0; i < 2; i++)
                {
                    size_t k = (size_t)(y + j) * w + x + i;
                    const BYTE* p = &pixels[3 * k];
                    float luma = kr * p[0] + (1 - kr - kb) * p[1] + kb * p[2];
                    m_pixels[k] = byte((format.fullRange ? 0 : 16) + ys * luma);
                    cb += p[2] - luma;
                    cr += p[0] - luma;
                }
            size_t k = (size_t)w * h + (size_t)(y / 2) * w + x;
            m_pixels[k] = byte(128 + cs * cb / (8 * (1 - kb)));
            m_pixels[k + 1] = byte(128 + cs * cr / (8 * (1 - kr)));
        }
    return true;
}

void BackgroundImage::Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch,
                            const uint8_t* alpha, UINT aw, UINT ah) const
{
    if (!alpha || !aw || !ah || m_pixels.empty()) return;
    const float sx = (float)aw / m_width, sy = (float)ah / m_height;
    auto blend = [](BYTE src, BYTE bg, int a) { return (BYTE)((src * a + bg * (255 - a) + 127) / 255); };
    ParallelFor(m_height / 2, [&](int block) {
        BYTE* chroma = uv + (ptrdiff_t)block * uvPitch;
        const BYTE* bgChroma = m_pixels.data() + (size_t)m_width * (m_height + block);
        thread_local std::vector<int> weights;
        weights.assign(m_width / 2, 0);
        for (int j = 0; j < 2; j++)
        {
            UINT row = block * 2 + j;
            float ty = std::clamp((row + .5f) * sy - .5f, 0.f, (float)(ah - 1));
            UINT y0 = (UINT)ty;
            const BYTE* top = alpha + (size_t)y0 * aw;
            const BYTE* bottom = y0 + 1 < ah ? top + aw : top;
            BYTE* dst = y + (ptrdiff_t)row * yPitch;
            const BYTE* bg = m_pixels.data() + (size_t)row * m_width;
            for (UINT x = 0; x < m_width; x++)
            {
                float tx = std::clamp((x + .5f) * sx - .5f, 0.f, (float)(aw - 1));
                UINT left = (UINT)tx, right = std::min(left + 1, aw - 1);
                float a0 = top[left] + (top[right] - top[left]) * (tx - left);
                float a1 = bottom[left] + (bottom[right] - bottom[left]) * (tx - left);
                int a = (int)(a0 + (a1 - a0) * (ty - y0) + .5f);
                dst[x] = blend(dst[x], bg[x], a);
                weights[x / 2] += a;
            }
        }
        for (UINT x = 0; x < m_width; x += 2)
        {
            int a = (weights[x / 2] + 2) / 4;
            chroma[x] = blend(chroma[x], bgChroma[x], a);
            chroma[x + 1] = blend(chroma[x + 1], bgChroma[x + 1], a);
        }
    });
}
