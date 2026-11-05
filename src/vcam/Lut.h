// Applies a .cube colour LUT to NV12 frames in place.
//
// The .cube file works on RGB, but camera frames are NV12 (YUV). Converting every pixel to RGB and back
// would cost a lot of CPU, so instead we precompute the whole round trip once: a table indexed by the
// pixel's Y and its block's U/V (chroma reduced to 7 bits) that gives the new Y, U and V directly.
// Applying the LUT is then one table read per pixel.
#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <thread>

// Runs fn(i) for i in [0, count) across the thread pool and waits for all of them.
template <class Fn>
void ParallelFor(int count, Fn fn)
{
    struct Ctx { Fn* fn; std::atomic<int> next; int count; } ctx{ &fn, 0, count };
    auto callback = [](PTP_CALLBACK_INSTANCE, PVOID p, PTP_WORK) {
        auto* c = static_cast<Ctx*>(p);
        for (int i; (i = c->next.fetch_add(1)) < c->count;) (*c->fn)(i);
    };
    PTP_WORK work = CreateThreadpoolWork(callback, &ctx, nullptr);
    if (!work) { for (int i = 0; i < count; i++) fn(i); return; }
    unsigned workers = std::thread::hardware_concurrency();
    if (workers > 8) workers = 8;
    if (workers < 1) workers = 1;
    for (unsigned i = 0; i < workers; i++) SubmitThreadpoolWork(work);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
}

struct CubeLut
{
    int size = 0;                 // points per axis
    float domainMin[3] = { 0, 0, 0 };
    float domainMax[3] = { 1, 1, 1 };
    std::vector<float> rgb;       // size^3 * 3, red changes fastest

    bool Load(const std::wstring& path, std::wstring* error);
    void Sample(float r, float g, float b, float out[3]) const; // trilinear
};

// Colour space of the frames, from the media type.
struct YuvFormat
{
    bool bt709 = true;
    bool fullRange = false;
    bool operator==(const YuvFormat& o) const { return bt709 == o.bt709 && fullRange == o.fullRange; }
};

// Precomputed YUV -> YUV table for one LUT, intensity and colour space.
class YuvLutTable
{
public:
    static std::shared_ptr<YuvLutTable> Build(const CubeLut& lut, float intensity, YuvFormat format);

    // Processes an NV12 image in place. uv points to the interleaved chroma plane.
    void ApplyNV12(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height) const;

    YuvFormat Format() const { return m_format; }

private:
    // Index: (u >> 1) << 15 | (v >> 1) << 8 | y. Value: y | u << 8 | v << 16.
    std::vector<uint32_t> m_table;
    YuvFormat m_format;
};

// Flips an NV12 image left-to-right in place.
void MirrorNV12(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height);

// Converts NV12 to top-down BGRA for display, taking every step-th pixel in each direction
// (step 2 halves the size). out must hold (width / step) * (height / step) * 4 bytes.
void NV12ToBGRA(const BYTE* y, LONG yPitch, const BYTE* uv, LONG uvPitch, UINT width, UINT height,
                UINT step, YuvFormat format, BYTE* out);