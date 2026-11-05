// Live preview of what OpenCamHub sends to apps.
//
// The camera is opened in Windows' shared (read-only) mode, so the preview works while another
// app is using the camera and never stops another app from opening it. If an app is holding the
// Facecam itself (not OpenCamHub), OpenCamHub can't run, so the preview reads the Facecam directly and
// applies the mirror and LUT here so it still looks the same.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

class Preview
{
public:
    enum class Mode { Off, Starting, OpenCamHub, Direct, Unavailable };

    ~Preview() { Stop(); }
    // Posts msg to notify when a new frame or status is ready. targetWidth is the drawn width in
    // pixels, used to skip converting more pixels than will be shown.
    void Start(HWND notify, UINT msg, UINT32 targetWidth);
    void Stop();
    bool Running() const { return m_thread.joinable(); }
    void SetTargetWidth(UINT32 w) { m_target = w; }

    Mode GetMode() const { return m_mode; }
    std::wstring Status();               // short text for the status line
    void FrameDrawn() { m_pending = false; }

    // Calls f(bgra, width, height, sequence) with the latest frame under a lock.
    template <class F> void ReadFrame(F f)
    {
        AcquireSRWLockShared(&m_lock);
        f(m_frame.empty() ? nullptr : m_frame.data(), m_w, m_h, m_seq);
        ReleaseSRWLockShared(&m_lock);
    }

private:
    void Run();
    enum class Result { Stopped, Failed, Busy, Ended };
    Result Stream(bool direct);
    void SetState(Mode m, const std::wstring& status);
    void Wait(int ms);

    HWND m_notify = nullptr;
    UINT m_msg = 0;
    std::atomic<UINT32> m_target{ 1280 };
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_pending{ false };
    std::atomic<Mode> m_mode{ Mode::Off };
    SRWLOCK m_lock = SRWLOCK_INIT;
    std::vector<BYTE> m_frame;   // top-down BGRA
    UINT32 m_w = 0, m_h = 0;
    uint64_t m_seq = 0;
    std::wstring m_status;
};
