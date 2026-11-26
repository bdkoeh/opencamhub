#pragma once
#include "../vcam/Lut.h"

// A centre-cropped still, prepared once for the stream's dimensions and colour space.
class BackgroundImage
{
public:
    bool Load(const std::wstring& path, UINT width, UINT height, YuvFormat format);
    void Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch,
               const uint8_t* alpha, UINT aw, UINT ah) const;
private:
    UINT m_width = 0, m_height = 0;
    std::vector<BYTE> m_pixels;
};
