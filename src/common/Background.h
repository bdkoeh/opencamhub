// Blurs the background of an NV12 frame, keeping the person (given by an alpha matte) sharp.
//
// The blur is done on a copy shrunk 8x, which makes even a strong blur cheap. Only background
// pixels feed it (a "masked" blur), so the person's colours don't leak into the background as a
// halo. The result is blended back at full resolution with the soft matte, so hair stays natural.
#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

class BackgroundBlur
{
public:
    // strength 0-100. alpha is aw x ah, 255 = person.
    void Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height,
               const uint8_t* alpha, UINT aw, UINT ah, int strength);

private:
    void Blur(std::vector<float>& plane, std::vector<float>& tmp, int radius);
    UINT m_sw = 0, m_sh = 0;
    std::vector<float> m_y, m_u, m_v, m_w, m_tmp;
    std::vector<uint16_t> m_yq, m_uq, m_vq;
    struct Tap { UINT i0; int f; };   // bilinear: first sample index and 8-bit weight of the next
    std::vector<Tap> m_axY, m_bxY, m_ayY, m_byY, m_axC, m_bxC, m_ayC, m_byC;
};
