// The picture changes OpenCamHub makes to each frame: mirroring, background blur and the colour LUT,
// all taken from settings.ini. Used by the virtual camera, and by the settings app when it has to
// preview the raw camera. Changes to settings.ini are picked up within about half a second.
#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include "../vcam/Lut.h"
#include "Background.h"
#include "Matting.h"

class FrameEffects
{
public:
    // Processes one NV12 frame in place.
    void Apply(BYTE* y, LONG yPitch, BYTE* uv, LONG uvPitch, UINT width, UINT height, YuvFormat format);

private:
    struct Shared
    {
        SRWLOCK lock = SRWLOCK_INIT;
        std::shared_ptr<YuvLutTable> table;   // null = LUT off
        bool mirror = false;
        std::shared_ptr<Matting> matting;     // null = background effect off
        int blurStrength = 60;
        FILETIME builtTime = {};
        YuvFormat builtFormat;
        bool built = false;
        std::atomic<bool> building{ false };
    };
    void CheckSettings(YuvFormat format);
    std::shared_ptr<Shared> m_s = std::make_shared<Shared>();
    unsigned m_frame = 0;
    BackgroundBlur m_blur;
    Matting* m_lastMatting = nullptr;
    UINT m_lastW = 0, m_lastH = 0;
};
