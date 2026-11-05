// OpenCamHub settings window: camera controls, framing (zoom/pan/tilt), mirror, LUT, and a live
// preview of the virtual camera. Drawn with Direct2D; follows the Windows light/dark setting.
// Everything stays on this PC; the app makes no network calls.
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mfapi.h>
#include <wrl.h>
#include <cmath>
#include <string>
#include <vector>
#include "Camera.h"
#include "Preview.h"
#include "../common/OpenCamHub.h"
#include "../vcam/Lut.h"

using Microsoft::WRL::ComPtr;

namespace
{
    // ------------------------------------------------------------------ constants

    const UINT WM_PREVIEW = WM_APP + 1;
    const UINT_PTR TIMER_REFRESH = 1, TIMER_LUT_SAVE = 2, TIMER_STATUS = 3;
    const float CLIENT_W = 1196, CLIENT_H = 892;   // in DIPs (1/96 inch)
    const float M = 20, GAP = 14, ROW = 36, HEAD = 42;
    const float LEFT_W = 720, RIGHT_X = M + LEFT_W + 16, RIGHT_W = 420;
    const float PREVIEW_H = 405;

    // ------------------------------------------------------------------ theme

    D2D1_COLOR_F Hex(UINT32 rgb, float a = 1.f)
    {
        return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, a);
    }

    struct Theme
    {
        bool dark;
        D2D1_COLOR_F bg, card, border, text, dim, track, control, controlHover, accent, accentHover, onAccent, preview;
        COLORREF caption;
    };

    Theme MakeTheme(bool dark)
    {
        if (dark)
            return { true, Hex(0x141417), Hex(0x1F1F24), Hex(0x2C2C33), Hex(0xECECF1), Hex(0x9A9AA6), Hex(0x3A3A43),
                     Hex(0x2A2A31), Hex(0x34343C), Hex(0x5B8CFF), Hex(0x76A0FF), Hex(0xFFFFFF), Hex(0x0B0B0D), RGB(0x14, 0x14, 0x17) };
        return { false, Hex(0xF1F2F5), Hex(0xFFFFFF), Hex(0xE1E3E8), Hex(0x1A1B1F), Hex(0x6A6D78), Hex(0xD5D8DF),
                 Hex(0xEEF0F3), Hex(0xE3E6EB), Hex(0x2F6BEA), Hex(0x4A80F0), Hex(0xFFFFFF), Hex(0x16171A), RGB(0xF1, 0xF2, 0xF5) };
    }

    int g_forceTheme = -1;   // --theme=dark / --theme=light, otherwise follow Windows
    bool SystemUsesDark()
    {
        if (g_forceTheme >= 0) return g_forceTheme == 1;
        DWORD v = 1, size = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
        return v == 0;
    }

    // ------------------------------------------------------------------ model

    enum class Kind { Slider, Segment, Toggle };
    struct Option { long value; const wchar_t* text; };
    struct Item
    {
        Ctl ctl;
        Kind kind;
        const wchar_t* label;
        std::vector<Option> options;
        CtlRange range;
        bool available = false;
        long value = 0;
    };

    std::vector<Item> g_items;
    Camera g_cam;
    Preview g_view;
    DWORD g_lastWrite = 0;

    struct LutUi
    {
        bool on = false;
        std::vector<std::wstring> files;
        int sel = -1;
        long intensity = 100;
    } g_lut;
    struct BgUi { bool on = false; long strength = 60; } g_bg;
    bool g_mirror = false, g_previewOn = true, g_minimized = false;
    std::wstring g_status;
    bool g_statusIsError = false;

    Item* Find(Ctl c)
    {
        for (auto& it : g_items) if (it.ctl == c) return &it;
        return nullptr;
    }

    void BuildItems()
    {
        g_items = {
            { Ctl::Zoom, Kind::Slider, L"Zoom" },
            { Ctl::Pan, Kind::Slider, L"Pan" },
            { Ctl::Tilt, Kind::Slider, L"Tilt" },
            { Ctl::Contrast, Kind::Slider, L"Contrast" },
            { Ctl::Saturation, Kind::Slider, L"Saturation" },
            { Ctl::Sharpness, Kind::Slider, L"Sharpness" },
            { Ctl::Vibrancy, Kind::Slider, L"Vibrancy" },
            { Ctl::AutoExposure, Kind::Toggle, L"Auto" },
            { Ctl::Metering, Kind::Segment, L"Metering", { { 0, L"Center" }, { 1, L"Spot" }, { 2, L"Average" } } },
            { Ctl::Shutter, Kind::Slider, L"Shutter" },
            { Ctl::Brightness, Kind::Slider, L"Brightness" },
            { Ctl::DynamicRange, Kind::Segment, L"Dynamic range", { { 0, L"Standard" }, { 1, L"High" } } },
            { Ctl::AutoWhiteBalance, Kind::Toggle, L"Auto" },
            { Ctl::Temperature, Kind::Slider, L"Temperature" },
            { Ctl::Tint, Kind::Slider, L"Tint" },
            { Ctl::NoiseReduction, Kind::Segment, L"Noise reduction", { { 0, L"Off" }, { 1, L"Low" }, { 2, L"Medium" }, { 3, L"High" } } },
            { Ctl::AntiFlicker, Kind::Segment, L"Anti-flicker", { { 1, L"50 Hz" }, { 2, L"60 Hz" } } },
        };
    }

    std::wstring FormatValue(const Item& it, long v)
    {
        wchar_t buf[64];
        const CtlRange& r = it.range;
        switch (it.ctl)
        {
        case Ctl::Zoom: swprintf_s(buf, L"%.2f\x00D7", v / 100.0); break;
        case Ctl::Shutter:
        {
            // UVC exposure is log2 seconds; show the usual rounded camera labels.
            static const wchar_t* names[] = { L"1 s", L"1/2 s", L"1/4 s", L"1/8 s", L"1/15 s", L"1/30 s", L"1/60 s",
                L"1/125 s", L"1/250 s", L"1/500 s", L"1/1000 s", L"1/2000 s", L"1/4000 s", L"1/8000 s" };
            if (v <= 0 && -v < (long)_countof(names)) return names[-v];
            swprintf_s(buf, L"2^%ld s", v);
            break;
        }
        case Ctl::Temperature: swprintf_s(buf, L"%ld K", v); break;
        case Ctl::Pan: case Ctl::Tilt: case Ctl::Tint: swprintf_s(buf, L"%ld", v); break;
        default:
            swprintf_s(buf, L"%ld%%", r.max > r.min ? (long)std::lround((v - r.min) * 100.0 / (r.max - r.min)) : v);
        }
        return buf;
    }

    // ------------------------------------------------------------------ widgets

    enum class W { Slider, Segment, Toggle, Button, TextButton, Chip };
    enum class Act { None, Item, BgOn, BgStrength, LutIntensity, LutOn, LutChip, LutImport, LutFolder, Mirror, PreviewOn, ResetFraming, ResetPicture, Save };

    struct Widget
    {
        W type;
        Act act;
        D2D1_RECT_F rect;        // hit area / control area
        int item = -1;           // index into g_items for Act::Item
        int chip = -1;           // LUT file index for Act::LutChip
        std::wstring text;
        bool primary = false;
    };

    struct Card { D2D1_RECT_F rect; std::wstring title; };
    struct Label { D2D1_RECT_F rect; std::wstring text; int item = -1; Act act = Act::None; bool value = false; };

    std::vector<Widget> g_widgets;
    std::vector<Card> g_cards;
    std::vector<Label> g_labels;
    int g_hover = -1, g_pressed = -1, g_focus = -1, g_hoverSeg = -1;
    bool g_dragging = false;

    // ------------------------------------------------------------------ graphics

    HINSTANCE g_inst;
    HWND g_wnd, g_previewWnd;
    UINT g_dpi = 96;
    Theme g_theme;
    ComPtr<ID2D1Factory> g_d2d;
    ComPtr<IDWriteFactory> g_dw;
    ComPtr<ID2D1HwndRenderTarget> g_rt, g_prt;
    ComPtr<ID2D1SolidColorBrush> g_brush, g_pbrush;
    ComPtr<ID2D1Bitmap> g_frame;
    uint64_t g_frameSeq = ~0ull;
    ComPtr<IDWriteTextFormat> g_fTitle, g_fBody, g_fBodyRight, g_fCenter, g_fSmall, g_fButton, g_fIcon;

    float Dip(int px) { return px * 96.f / g_dpi; }
    int Px(float dip) { return (int)std::lround(dip * g_dpi / 96.f); }

    ComPtr<IDWriteTextFormat> MakeFont(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align)
    {
        ComPtr<IDWriteTextFormat> f;
        g_dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f)
        {
            f->SetTextAlignment(align);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            ComPtr<IDWriteInlineObject> ellipsis;
            g_dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
            f->SetTrimming(&trim, ellipsis.Get());
        }
        return f;
    }

    bool FontExists(const wchar_t* family)
    {
        ComPtr<IDWriteFontCollection> fonts;
        g_dw->GetSystemFontCollection(&fonts);
        UINT32 index;
        BOOL exists = FALSE;
        return fonts && SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
    }

    void CreateFonts()
    {
        const wchar_t* text = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
        const wchar_t* display = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
        g_fTitle = MakeFont(display, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_fBody = MakeFont(text, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_fBodyRight = MakeFont(text, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
        g_fCenter = MakeFont(text, 13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_fSmall = MakeFont(text, 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_fButton = MakeFont(text, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        g_fIcon = MakeFont(FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets", 14,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    void ApplyTitleBar()
    {
        BOOL dark = g_theme.dark;
        DwmSetWindowAttribute(g_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        COLORREF caption = g_theme.caption;
        DwmSetWindowAttribute(g_wnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    }

    // ------------------------------------------------------------------ layout

    float TextWidth(IDWriteTextFormat* f, const std::wstring& s)
    {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 40, &layout))) return 0;
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    int AddWidget(Widget w) { g_widgets.push_back(w); return (int)g_widgets.size() - 1; }

    int IndexOf(Ctl c) { return (int)(Find(c) - g_items.data()); }

    // A labelled row inside a card: label on the left, control in the middle, value on the right.
    void Row(Ctl c, float x, float w, float y)
    {
        int i = IndexOf(c);
        Item& it = g_items[i];
        const float labelW = 112, valueW = 72;
        g_labels.push_back({ D2D1::RectF(x + 18, y, x + 18 + labelW, y + ROW), it.label, i });
        if (it.kind == Kind::Slider)
        {
            AddWidget({ W::Slider, Act::Item, D2D1::RectF(x + 18 + labelW, y, x + w - 18 - valueW, y + ROW), i });
            g_labels.push_back({ D2D1::RectF(x + w - 18 - valueW, y, x + w - 18, y + ROW), L"", i, Act::None, true });
        }
        else
            AddWidget({ W::Segment, Act::Item, D2D1::RectF(x + 18 + labelW, y + 4, x + w - 18, y + ROW - 4), i });
    }

    // A toggle at the right end of a card header, with its label before it.
    void HeaderToggle(const D2D1_RECT_F& card, Act act, int item, const std::wstring& label, float rightInset = 18)
    {
        float r = card.right - rightInset, top = card.top + 11;
        float lw = TextWidth(g_fBody.Get(), label) + 10;
        AddWidget({ W::Toggle, act, D2D1::RectF(r - 40 - lw, top, r, top + 22), item, -1, label });
    }

    void HeaderTextButton(const D2D1_RECT_F& card, Act act, const std::wstring& text, float right)
    {
        float w = TextWidth(g_fBody.Get(), text) + 20;
        AddWidget({ W::TextButton, act, D2D1::RectF(right - w, card.top + 8, right, card.top + 34), -1, -1, text });
    }

    float LutChipsHeight(float width);

    void Layout()
    {
        int focusAct = -1, focusItem = -1, focusChip = -1;
        if (g_focus >= 0 && g_focus < (int)g_widgets.size())
        {
            focusAct = (int)g_widgets[g_focus].act; focusItem = g_widgets[g_focus].item; focusChip = g_widgets[g_focus].chip;
        }
        g_widgets.clear();
        g_cards.clear();
        g_labels.clear();

        // Left column: preview, toolbar, framing, LUT.
        float y = M + PREVIEW_H + 10;
        AddWidget({ W::Toggle, Act::PreviewOn, D2D1::RectF(M, y + 5, M + 120, y + 27), -1, -1, L"Preview" });
        AddWidget({ W::Toggle, Act::Mirror, D2D1::RectF(M + 136, y + 5, M + 250, y + 27), -1, -1, L"Mirror" });
        g_labels.push_back({ D2D1::RectF(M + 270, y, M + LEFT_W, y + 32), L"", -1, Act::PreviewOn });

        y += 32 + GAP;
        D2D1_RECT_F framing = D2D1::RectF(M, y, M + LEFT_W, y + HEAD + 3 * ROW + 10);
        g_cards.push_back({ framing, L"Framing" });
        HeaderTextButton(framing, Act::ResetFraming, L"Reset", framing.right - 10);
        Row(Ctl::Zoom, M, LEFT_W, y + HEAD);
        Row(Ctl::Pan, M, LEFT_W, y + HEAD + ROW);
        Row(Ctl::Tilt, M, LEFT_W, y + HEAD + 2 * ROW);

        y = framing.bottom + GAP;
        D2D1_RECT_F bgCard = D2D1::RectF(M, y, M + LEFT_W, y + HEAD + ROW + 10);
        g_cards.push_back({ bgCard, L"Background" });
        HeaderToggle(bgCard, Act::BgOn, -1, L"Blur");
        {
            const float labelW = 112, valueW = 72;
            float ry = y + HEAD;
            g_labels.push_back({ D2D1::RectF(M + 18, ry, M + 18 + labelW, ry + ROW), L"Strength", -1, Act::BgStrength });
            AddWidget({ W::Slider, Act::BgStrength, D2D1::RectF(M + 18 + labelW, ry, M + LEFT_W - 18 - valueW, ry + ROW) });
            g_labels.push_back({ D2D1::RectF(M + LEFT_W - 18 - valueW, ry, M + LEFT_W - 18, ry + ROW), L"", -1, Act::BgStrength, true });
        }

        y = bgCard.bottom + GAP;
        float chipsH = LutChipsHeight(LEFT_W - 36);
        D2D1_RECT_F lut = D2D1::RectF(M, y, M + LEFT_W, y + HEAD + chipsH + 8 + ROW + 8);
        g_cards.push_back({ lut, L"Color filter (LUT)" });
        HeaderToggle(lut, Act::LutOn, -1, L"Apply");
        HeaderTextButton(lut, Act::LutFolder, L"Open folder", g_widgets.back().rect.left - 12);
        {
            float cx = M + 18, cy = y + HEAD, maxX = M + LEFT_W - 18;
            auto place = [&](const std::wstring& text, Act act, int chip) {
                float w = TextWidth(g_fBody.Get(), text) + 30;
                if (cx + w > maxX && cx > M + 18) { cx = M + 18; cy += 38; }
                AddWidget({ W::Chip, act, D2D1::RectF(cx, cy, cx + w, cy + 30), -1, chip, text });
                cx += w + 8;
            };
            for (size_t i = 0; i < g_lut.files.size(); i++)
            {
                std::wstring name = g_lut.files[i].substr(0, g_lut.files[i].size() - 5);
                for (auto& ch : name) if (ch == L'_') ch = L' ';
                place(name, Act::LutChip, (int)i);
            }
            place(L"+  Import", Act::LutImport, -1);
        }
        {
            float ry = y + HEAD + chipsH + 8;
            const float labelW = 112, valueW = 72;
            g_labels.push_back({ D2D1::RectF(M + 18, ry, M + 18 + labelW, ry + ROW), L"Intensity", -1, Act::LutIntensity });
            AddWidget({ W::Slider, Act::LutIntensity, D2D1::RectF(M + 18 + labelW, ry, M + LEFT_W - 18 - valueW, ry + ROW) });
            g_labels.push_back({ D2D1::RectF(M + LEFT_W - 18 - valueW, ry, M + LEFT_W - 18, ry + ROW), L"", -1, Act::LutIntensity, true });
        }

        // Right column: picture, exposure, white balance, processing.
        const float X = RIGHT_X, Wd = RIGHT_W;
        y = M;
        D2D1_RECT_F pic = D2D1::RectF(X, y, X + Wd, y + HEAD + 4 * ROW + 10);
        g_cards.push_back({ pic, L"Picture" });
        HeaderTextButton(pic, Act::ResetPicture, L"Reset", pic.right - 10);
        Row(Ctl::Contrast, X, Wd, y + HEAD);
        Row(Ctl::Saturation, X, Wd, y + HEAD + ROW);
        Row(Ctl::Sharpness, X, Wd, y + HEAD + 2 * ROW);
        Row(Ctl::Vibrancy, X, Wd, y + HEAD + 3 * ROW);

        y = pic.bottom + GAP;
        D2D1_RECT_F exp = D2D1::RectF(X, y, X + Wd, y + HEAD + 4 * ROW + 10);
        g_cards.push_back({ exp, L"Exposure" });
        HeaderToggle(exp, Act::Item, IndexOf(Ctl::AutoExposure), L"Auto");
        Row(Ctl::Metering, X, Wd, y + HEAD);
        Row(Ctl::Shutter, X, Wd, y + HEAD + ROW);
        Row(Ctl::Brightness, X, Wd, y + HEAD + 2 * ROW);
        Row(Ctl::DynamicRange, X, Wd, y + HEAD + 3 * ROW);

        y = exp.bottom + GAP;
        D2D1_RECT_F wb = D2D1::RectF(X, y, X + Wd, y + HEAD + 2 * ROW + 10);
        g_cards.push_back({ wb, L"White balance" });
        HeaderToggle(wb, Act::Item, IndexOf(Ctl::AutoWhiteBalance), L"Auto");
        Row(Ctl::Temperature, X, Wd, y + HEAD);
        Row(Ctl::Tint, X, Wd, y + HEAD + ROW);

        y = wb.bottom + GAP;
        D2D1_RECT_F proc = D2D1::RectF(X, y, X + Wd, y + HEAD + 2 * ROW + 10);
        g_cards.push_back({ proc, L"Processing" });
        Row(Ctl::NoiseReduction, X, Wd, y + HEAD);
        Row(Ctl::AntiFlicker, X, Wd, y + HEAD + ROW);

        y = proc.bottom + GAP;
        AddWidget({ W::Button, Act::Save, D2D1::RectF(X + Wd - 156, y, X + Wd, y + 36), -1, -1, L"Save to camera", true });
        g_labels.push_back({ D2D1::RectF(X, y, X + Wd - 168, y + 36), L"", -1, Act::Save });

        // Keep keyboard focus on the same control.
        g_focus = -1;
        for (size_t i = 0; i < g_widgets.size(); i++)
            if ((int)g_widgets[i].act == focusAct && g_widgets[i].item == focusItem && g_widgets[i].chip == focusChip) g_focus = (int)i;
        g_hover = g_pressed = -1;
    }

    float LutChipsHeight(float width)
    {
        float cx = 0, rows = 1;
        auto add = [&](const std::wstring& t) {
            float w = TextWidth(g_fBody.Get(), t) + 30;
            if (cx + w > width && cx > 0) { cx = 0; rows++; }
            cx += w + 8;
        };
        for (auto& f : g_lut.files)
        {
            std::wstring name = f.substr(0, f.size() - 5);
            add(name);
        }
        add(L"+  Import");
        return rows * 38 - 8;
    }

    // ------------------------------------------------------------------ widget state

    bool CamItemEnabled(const Item& it)
    {
        if (!it.available) return false;
        if (it.ctl == Ctl::Shutter) { Item* a = Find(Ctl::AutoExposure); return !(a->available && a->value); }
        if (it.ctl == Ctl::Temperature) { Item* a = Find(Ctl::AutoWhiteBalance); return !(a->available && a->value); }
        return true;
    }

    bool Enabled(const Widget& w)
    {
        switch (w.act)
        {
        case Act::Item: return CamItemEnabled(g_items[w.item]);
        case Act::ResetFraming: case Act::ResetPicture: case Act::Save: return g_cam.IsOpen();
        case Act::LutIntensity: return g_lut.on;
        case Act::BgStrength: return g_bg.on;
        default: return true;
        }
    }

    bool ToggleValue(const Widget& w)
    {
        switch (w.act)
        {
        case Act::Item: return g_items[w.item].value != 0;
        case Act::LutOn: return g_lut.on;
        case Act::BgOn: return g_bg.on;
        case Act::Mirror: return g_mirror;
        case Act::PreviewOn: return g_previewOn;
        default: return false;
        }
    }

    struct SliderModel { long min = 0, max = 100, step = 1, def = 0, value = 0; };
    SliderModel GetSlider(const Widget& w)
    {
        if (w.act == Act::LutIntensity) return { 0, 100, 1, 100, g_lut.intensity };
        if (w.act == Act::BgStrength) return { 0, 100, 1, 60, g_bg.strength };
        const Item& it = g_items[w.item];
        long v = it.value < it.range.min ? it.range.min : (it.value > it.range.max ? it.range.max : it.value);
        return { it.range.min, it.range.max > it.range.min ? it.range.max : it.range.min + 1, it.range.step, it.range.def, v };
    }

    // ------------------------------------------------------------------ actions

    void SetStatus(const std::wstring& s, bool error = false)
    {
        g_status = s;
        g_statusIsError = error;
        if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
        if (g_wnd && !s.empty() && !error) SetTimer(g_wnd, TIMER_STATUS, 6000, nullptr);
    }

    void Invalidate() { InvalidateRect(g_wnd, nullptr, FALSE); }

    void LoadRanges()
    {
        for (auto& it : g_items) it.available = g_cam.Range(it.ctl, it.range);
    }

    // Reads every setting from the camera. Skipped for a moment after a change, because the
    // camera reports garbage for Elgato's own controls for about half a second after a write.
    void Refresh(bool force)
    {
        if (!g_cam.IsOpen())
        {
            if (!g_cam.Open())
            {
                if (g_status.empty() || g_statusIsError) SetStatus(L"Facecam MK.2 not found. Is it plugged in?", true);
                return;
            }
            force = true;
            if (g_statusIsError) SetStatus(L"");
            LoadRanges();
        }
        if (!force && (GetTickCount() - g_lastWrite < 1500 || g_dragging)) return;
        bool lost = true, changed = false;
        for (auto& it : g_items)
        {
            if (!it.available) continue;
            long v;
            if (!g_cam.Get(it.ctl, v)) continue;
            lost = false;
            if (it.kind != Kind::Toggle && (v < it.range.min || v > it.range.max)) continue;   // mid-update read
            if (v != it.value) { it.value = v; changed = true; }
        }
        if (lost)
        {
            g_cam.Close();
            for (auto& it : g_items) it.available = false;
            SetStatus(L"Facecam MK.2 disconnected.", true);
        }
        if (changed || force || lost) Invalidate();
    }

    void WriteItem(Item& it, long v)
    {
        if (!it.available) return;
        if (it.kind == Kind::Slider)
        {
            if (v < it.range.min) v = it.range.min;
            if (v > it.range.max) v = it.range.max;
            if (it.range.step > 1)
                v = it.range.min + (long)std::lround((double)(v - it.range.min) / it.range.step) * it.range.step;
        }
        if (v == it.value && it.kind != Kind::Toggle) return;
        if (!g_cam.Set(it.ctl, v)) { SetStatus(L"The camera did not accept that change.", true); return; }
        g_lastWrite = GetTickCount();
        it.value = v;
        // A manual shutter or temperature turns that automatic mode off.
        if (it.ctl == Ctl::Shutter) Find(Ctl::AutoExposure)->value = 0;
        if (it.ctl == Ctl::Temperature) Find(Ctl::AutoWhiteBalance)->value = 0;
        Invalidate();
    }

    void ResetTo(Ctl c)
    {
        Item* it = Find(c);
        if (it->available) WriteItem(*it, it->range.def);
    }

    std::vector<std::wstring> ListLuts()
    {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((OpenCamHubLutDir() + L"\\*.cube").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return files;
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
        return files;
    }

    void LoadSettings()
    {
        LutSettings s = ReadLutSettings();
        auto files = ListLuts();
        bool relayout = files != g_lut.files;
        g_lut.files = files;
        g_lut.on = s.enabled;
        g_lut.intensity = s.intensity;
        g_lut.sel = -1;
        for (size_t i = 0; i < files.size(); i++) if (_wcsicmp(files[i].c_str(), s.file.c_str()) == 0) g_lut.sel = (int)i;
        g_mirror = ReadMirrorSetting();
        BackgroundSettings bg = ReadBackgroundSettings();
        g_bg.on = bg.mode == 1;
        g_bg.strength = bg.strength;
        if (relayout) Layout();
        Invalidate();
    }

    void SaveBackground()
    {
        BackgroundSettings s;
        s.mode = g_bg.on ? 1 : 0;
        s.strength = (int)g_bg.strength;
        if (!WriteBackgroundSettings(s)) SetStatus(L"Could not save the background setting.", true);
    }

    void SaveLut()
    {
        LutSettings s;
        s.enabled = g_lut.on;
        if (g_lut.sel >= 0 && g_lut.sel < (int)g_lut.files.size()) s.file = g_lut.files[g_lut.sel];
        s.intensity = (int)g_lut.intensity;
        WriteLutSettings(s);
    }

    void ImportLut()
    {
        wchar_t path[MAX_PATH] = {};
        OPENFILENAMEW ofn = { sizeof(ofn) };
        ofn.hwndOwner = g_wnd;
        ofn.lpstrFilter = L"3D LUT (*.cube)\0*.cube\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;

        CubeLut lut;
        std::wstring error;
        if (!lut.Load(path, &error))
        {
            MessageBoxW(g_wnd, (L"That file can't be used as a LUT:\n" + error).c_str(), L"OpenCamHub", MB_ICONWARNING);
            return;
        }
        std::wstring name = path + ofn.nFileOffset;
        std::wstring dest = OpenCamHubLutDir() + L"\\" + name;
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES &&
            MessageBoxW(g_wnd, (name + L" is already in the LUT folder. Replace it?").c_str(), L"OpenCamHub", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (!CopyFileW(path, dest.c_str(), FALSE))
        {
            MessageBoxW(g_wnd, L"Could not copy the file into the LUT folder.", L"OpenCamHub", MB_ICONWARNING);
            return;
        }
        LutSettings s = ReadLutSettings();
        s.file = name;
        s.enabled = true;
        WriteLutSettings(s);
        LoadSettings();
        Layout();
        SetStatus(L"Imported " + name);
    }

    void UpdatePreviewRunning()
    {
        bool want = !g_minimized && g_previewOn;
        RECT r;
        GetClientRect(g_previewWnd, &r);
        if (want && !g_view.Running()) g_view.Start(g_previewWnd, WM_PREVIEW, (UINT32)r.right);
        if (!want && g_view.Running()) { g_view.Stop(); g_frame.Reset(); g_frameSeq = ~0ull; }
        InvalidateRect(g_previewWnd, nullptr, FALSE);
        Invalidate();
    }

    void Activate(int index)
    {
        Widget& w = g_widgets[index];
        if (!Enabled(w)) return;
        switch (w.act)
        {
        case Act::Item:
        {
            Item& it = g_items[w.item];
            if (it.kind == Kind::Toggle)
            {
                long v = it.value ? 0 : 1;
                if (g_cam.Set(it.ctl, v)) { it.value = v; g_lastWrite = GetTickCount(); }
                else SetStatus(L"The camera did not accept that change.", true);
            }
            break;
        }
        case Act::LutOn: g_lut.on = !g_lut.on; SaveLut(); break;
        case Act::BgOn: g_bg.on = !g_bg.on; SaveBackground(); break;
        case Act::LutChip:
            g_lut.sel = w.chip;
            g_lut.on = true;
            SaveLut();
            break;
        case Act::LutImport: ImportLut(); return;
        case Act::LutFolder: ShellExecuteW(g_wnd, L"open", OpenCamHubLutDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Act::Mirror:
            g_mirror = !g_mirror;
            if (!WriteMirrorSetting(g_mirror)) SetStatus(L"Could not save the mirror setting.", true);
            break;
        case Act::PreviewOn: g_previewOn = !g_previewOn; UpdatePreviewRunning(); break;
        case Act::ResetFraming: ResetTo(Ctl::Zoom); ResetTo(Ctl::Pan); ResetTo(Ctl::Tilt); break;
        case Act::ResetPicture:
            ResetTo(Ctl::Contrast); ResetTo(Ctl::Saturation); ResetTo(Ctl::Sharpness); ResetTo(Ctl::Vibrancy);
            break;
        case Act::Save:
            if (g_cam.SaveToCamera()) SetStatus(L"Saved. The camera will start up with these settings.");
            else SetStatus(L"Saving to the camera failed.", true);
            g_lastWrite = GetTickCount();
            break;
        default: break;
        }
        Invalidate();
    }

    void SetSliderValue(const Widget& w, long v)
    {
        if (w.act == Act::BgStrength)
        {
            v = v < 0 ? 0 : (v > 100 ? 100 : v);
            if (v == g_bg.strength) return;
            g_bg.strength = v;
            SetTimer(g_wnd, TIMER_LUT_SAVE, 200, nullptr);
            Invalidate();
            return;
        }
        if (w.act == Act::LutIntensity)
        {
            v = v < 0 ? 0 : (v > 100 ? 100 : v);
            if (v == g_lut.intensity) return;
            g_lut.intensity = v;
            SetTimer(g_wnd, TIMER_LUT_SAVE, 200, nullptr);
            Invalidate();
            return;
        }
        WriteItem(g_items[w.item], v);
    }

    long SliderStep(const SliderModel& s)
    {
        long fine = (s.max - s.min) / 100;
        return fine > s.step ? fine : s.step;
    }

    // Track area inside a slider widget.
    void TrackBounds(const Widget& w, float& x0, float& x1) { x0 = w.rect.left + 10; x1 = w.rect.right - 10; }

    void SliderFromX(const Widget& w, float x)
    {
        SliderModel s = GetSlider(w);
        float x0, x1;
        TrackBounds(w, x0, x1);
        float t = (x - x0) / (x1 - x0);
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        SetSliderValue(w, s.min + (long)std::lround(t * (s.max - s.min)));
    }

    int SegmentAt(const Widget& w, float x)
    {
        const Item& it = g_items[w.item];
        int n = (int)it.options.size();
        int i = (int)((x - w.rect.left) / ((w.rect.right - w.rect.left) / n));
        return i < 0 ? 0 : (i >= n ? n - 1 : i);
    }

    int HitTest(float x, float y)
    {
        for (size_t i = 0; i < g_widgets.size(); i++)
        {
            const D2D1_RECT_F& r = g_widgets[i].rect;
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i;
        }
        return -1;
    }

    // ------------------------------------------------------------------ drawing

    void Fill(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c)
    {
        g_brush->SetColor(c);
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), g_brush.Get());
    }

    void Stroke(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c, float width = 1)
    {
        g_brush->SetColor(c);
        D2D1_RECT_F h = D2D1::RectF(r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(h, radius, radius), g_brush.Get(), width);
    }

    void Text(ID2D1RenderTarget* rt, const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c)
    {
        g_brush->SetColor(c);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    D2D1_COLOR_F Fade(D2D1_COLOR_F c, bool enabled) { if (!enabled) c.a *= 0.38f; return c; }

    D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t)
    {
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    void DrawToggle(ID2D1RenderTarget* rt, const Widget& w, bool hover, bool en)
    {
        const Theme& t = g_theme;
        bool on = ToggleValue(w);
        float pw = 40, ph = 22;
        D2D1_RECT_F pill = D2D1::RectF(w.rect.right - pw, w.rect.top, w.rect.right, w.rect.top + ph);
        if (w.act == Act::PreviewOn || w.act == Act::Mirror)   // toolbar toggles: switch first, then label
            pill = D2D1::RectF(w.rect.left, w.rect.top, w.rect.left + pw, w.rect.top + ph);
        D2D1_COLOR_F fill = on ? (hover ? t.accentHover : t.accent) : (hover ? t.controlHover : t.control);
        Fill(rt, pill, ph / 2, Fade(fill, en));
        if (!on) Stroke(rt, pill, ph / 2, Fade(t.track, en));
        float kr = on ? 7.f : 6.f;
        float kx = on ? pill.right - 11 : pill.left + 11;
        g_brush->SetColor(Fade(on ? t.onAccent : t.dim, en));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, pill.top + ph / 2), kr, kr), g_brush.Get());
        D2D1_RECT_F label = (w.act == Act::PreviewOn || w.act == Act::Mirror)
            ? D2D1::RectF(pill.right + 10, w.rect.top - 4, w.rect.right + 40, w.rect.bottom + 4)
            : D2D1::RectF(w.rect.left, w.rect.top - 4, pill.left - 10, w.rect.bottom + 4);
        Text(rt, w.text, (w.act == Act::PreviewOn || w.act == Act::Mirror) ? g_fBody.Get() : g_fBodyRight.Get(), label, Fade(t.text, en));
    }

    void DrawSlider(ID2D1RenderTarget* rt, const Widget& w, bool hover, bool en)
    {
        const Theme& t = g_theme;
        SliderModel s = GetSlider(w);
        float x0, x1;
        TrackBounds(w, x0, x1);
        float cy = (w.rect.top + w.rect.bottom) / 2;
        float pos = x0 + (x1 - x0) * (float)(s.value - s.min) / (float)(s.max - s.min);
        Fill(rt, D2D1::RectF(x0, cy - 2, x1, cy + 2), 2, Fade(t.track, en));
        // Fill from the centre for ranges that go negative (pan, tilt, tint), else from the left.
        float from = (s.min < 0 && s.max > 0) ? x0 + (x1 - x0) * (float)(0 - s.min) / (float)(s.max - s.min) : x0;
        D2D1_RECT_F filled = D2D1::RectF(from < pos ? from : pos, cy - 2, from < pos ? pos : from, cy + 2);
        Fill(rt, filled, 2, Fade(t.accent, en));
        if (s.min < 0 && s.max > 0)
        {
            g_brush->SetColor(Fade(t.dim, en));
            rt->FillRectangle(D2D1::RectF(from - 0.75f, cy - 6, from + 0.75f, cy + 6), g_brush.Get());
        }
        bool active = en && (hover || (g_dragging && &w == &g_widgets[g_pressed]));
        float r = active ? 9.f : 8.f;
        g_brush->SetColor(Fade(t.dark ? Hex(0xFFFFFF) : Hex(0xFFFFFF), en));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pos, cy), r, r), g_brush.Get());
        g_brush->SetColor(Fade(t.accent, en));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(pos, cy), r - 1, r - 1), g_brush.Get(), 2);
        g_brush->SetColor(Fade(t.accent, en));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pos, cy), active ? 4.f : 3.f, active ? 4.f : 3.f), g_brush.Get());
    }

    void DrawSegment(ID2D1RenderTarget* rt, const Widget& w, int index, bool en)
    {
        const Theme& t = g_theme;
        const Item& it = g_items[w.item];
        Fill(rt, w.rect, 7, Fade(t.control, en));
        int n = (int)it.options.size();
        float sw = (w.rect.right - w.rect.left) / n;
        for (int i = 0; i < n; i++)
        {
            D2D1_RECT_F seg = D2D1::RectF(w.rect.left + i * sw + 2, w.rect.top + 2, w.rect.left + (i + 1) * sw - 2, w.rect.bottom - 2);
            bool sel = it.options[i].value == it.value && it.available;
            bool hov = en && g_hover == index && g_hoverSeg == i && !sel;
            if (sel) Fill(rt, seg, 5, Fade(t.accent, en));
            else if (hov) Fill(rt, seg, 5, t.controlHover);
            Text(rt, it.options[i].text, sel ? g_fButton.Get() : g_fCenter.Get(), seg, Fade(sel ? t.onAccent : t.text, en));
        }
    }

    void DrawChip(ID2D1RenderTarget* rt, const Widget& w, bool hover)
    {
        const Theme& t = g_theme;
        bool sel = w.act == Act::LutChip && w.chip == g_lut.sel;
        bool dimmed = sel && !g_lut.on;
        float r = (w.rect.bottom - w.rect.top) / 2;
        if (sel)
        {
            Fill(rt, w.rect, r, dimmed ? t.controlHover : (hover ? t.accentHover : t.accent));
            Text(rt, w.text, g_fButton.Get(), w.rect, dimmed ? t.text : t.onAccent);
            if (dimmed) Stroke(rt, w.rect, r, t.accent, 1.5f);
        }
        else if (w.act == Act::LutImport)
        {
            if (hover) Fill(rt, w.rect, r, t.control);
            Stroke(rt, w.rect, r, t.track);
            Text(rt, w.text, g_fCenter.Get(), w.rect, t.dim);
        }
        else
        {
            Fill(rt, w.rect, r, hover ? t.controlHover : t.control);
            Text(rt, w.text, g_fCenter.Get(), w.rect, t.text);
        }
    }

    void DrawButton(ID2D1RenderTarget* rt, const Widget& w, bool hover, bool pressed, bool en)
    {
        const Theme& t = g_theme;
        if (w.type == W::TextButton)
        {
            if (hover && en) Fill(rt, w.rect, 6, t.control);
            Text(rt, w.text, g_fCenter.Get(), w.rect, Fade(hover ? t.text : t.dim, en));
            return;
        }
        D2D1_COLOR_F c = pressed ? Mix(t.accent, Hex(0x000000), 0.15f) : (hover ? t.accentHover : t.accent);
        Fill(rt, w.rect, 8, Fade(c, en));
        Text(rt, w.text, g_fButton.Get(), w.rect, Fade(t.onAccent, en));
    }

    std::wstring LabelText(const Label& l)
    {
        if (l.item >= 0)
        {
            const Item& it = g_items[l.item];
            if (!l.value) return l.text;
            return it.available ? FormatValue(it, it.value < it.range.min ? it.range.min : it.value) : L"\x2013";
        }
        if (l.act == Act::LutIntensity) return l.value ? std::to_wstring(g_lut.intensity) + L"%" : l.text;
        if (l.act == Act::BgStrength) return l.value ? std::to_wstring(g_bg.strength) + L"%" : l.text;
        if (l.act == Act::PreviewOn)
        {
            if (!g_previewOn) return L"Preview is off";
            return g_view.Status();
        }
        if (l.act == Act::Save) return g_status;
        return l.text;
    }

    void Paint()
    {
        if (!g_rt) return;
        const Theme& t = g_theme;
        ID2D1HwndRenderTarget* rt = g_rt.Get();
        rt->BeginDraw();
        rt->Clear(t.bg);

        for (auto& c : g_cards)
        {
            Fill(rt, c.rect, 12, t.card);
            Stroke(rt, c.rect, 12, t.border);
            Text(rt, c.title, g_fTitle.Get(), D2D1::RectF(c.rect.left + 18, c.rect.top + 6, c.rect.right - 18, c.rect.top + HEAD - 4), t.text);
        }
        for (auto& l : g_labels)
        {
            bool en = true;
            if (l.item >= 0) en = CamItemEnabled(g_items[l.item]);
            if (l.act == Act::LutIntensity) en = g_lut.on;
            if (l.act == Act::BgStrength) en = g_bg.on;
            D2D1_COLOR_F color = l.value ? t.text : t.dim;
            IDWriteTextFormat* f = l.value ? g_fBodyRight.Get() : g_fBody.Get();
            if (l.act == Act::PreviewOn) { f = g_fBodyRight.Get(); color = g_view.GetMode() == Preview::Mode::Direct ? Hex(0xE8A33D) : t.dim; }
            if (l.act == Act::Save) { f = g_fBody.Get(); color = g_statusIsError ? Hex(0xE5534B) : t.dim; }
            Text(rt, LabelText(l), f, l.rect, Fade(color, en));
        }
        for (size_t i = 0; i < g_widgets.size(); i++)
        {
            const Widget& w = g_widgets[i];
            bool hover = g_hover == (int)i, en = Enabled(w);
            switch (w.type)
            {
            case W::Slider: DrawSlider(rt, w, hover, en); break;
            case W::Segment: DrawSegment(rt, w, (int)i, en); break;
            case W::Toggle: DrawToggle(rt, w, hover, en); break;
            case W::Chip: DrawChip(rt, w, hover); break;
            default: DrawButton(rt, w, hover, g_pressed == (int)i, en); break;
            }
            if (g_focus == (int)i && GetFocus() == g_wnd)
            {
                D2D1_RECT_F f = w.rect;
                f.left -= 3; f.top -= 3; f.right += 3; f.bottom += 3;
                Stroke(rt, f, 8, Mix(t.accent, t.bg, 0.2f), 2);
            }
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { g_rt.Reset(); g_brush.Reset(); }
    }

    void CreateTargets()
    {
        if (!g_rt)
        {
            RECT r;
            GetClientRect(g_wnd, &r);
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
            props.dpiX = props.dpiY = (float)g_dpi;
            g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(r.right, r.bottom)), &g_rt);
            if (g_rt) g_rt->CreateSolidColorBrush(D2D1::ColorF(0), &g_brush);
        }
        if (!g_prt)
        {
            RECT r;
            GetClientRect(g_previewWnd, &r);
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
            props.dpiX = props.dpiY = (float)g_dpi;
            g_d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_previewWnd, D2D1::SizeU(r.right, r.bottom),
                D2D1_PRESENT_OPTIONS_IMMEDIATELY), &g_prt);
            if (g_prt) g_prt->CreateSolidColorBrush(D2D1::ColorF(0), &g_pbrush);
            g_frame.Reset();
            g_frameSeq = ~0ull;
        }
    }

    void PaintPreview()
    {
        CreateTargets();
        if (!g_prt) return;
        const Theme& t = g_theme;
        ID2D1HwndRenderTarget* rt = g_prt.Get();
        D2D1_SIZE_F size = rt->GetSize();
        D2D1_RECT_F area = D2D1::RectF(0, 0, size.width, size.height);
        rt->BeginDraw();
        rt->Clear(t.bg);
        g_pbrush->SetColor(t.preview);
        rt->FillRoundedRectangle(D2D1::RoundedRect(area, 12, 12), g_pbrush.Get());

        bool drew = false;
        g_view.ReadFrame([&](const BYTE* data, UINT32 w, UINT32 h, uint64_t seq) {
            if (!data || !w || !h) { g_frame.Reset(); return; }
            if (seq != g_frameSeq || !g_frame)
            {
                D2D1_SIZE_U have = g_frame ? g_frame->GetPixelSize() : D2D1::SizeU(0, 0);
                if (have.width != w || have.height != h)
                {
                    g_frame.Reset();
                    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                    rt->CreateBitmap(D2D1::SizeU(w, h), data, w * 4, bp, &g_frame);
                }
                else
                    g_frame->CopyFromMemory(nullptr, data, w * 4);
                g_frameSeq = seq;
            }
        });
        g_view.FrameDrawn();
        if (g_frame && g_view.Running())
        {
            // Fit the frame inside the rounded area, keeping its shape.
            D2D1_SIZE_U px = g_frame->GetPixelSize();
            float scale = size.width / px.width;
            if (px.height * scale > size.height) scale = size.height / px.height;
            float dw = px.width * scale, dh = px.height * scale;
            D2D1_RECT_F dest = D2D1::RectF((size.width - dw) / 2, (size.height - dh) / 2, (size.width + dw) / 2, (size.height + dh) / 2);
            ComPtr<ID2D1BitmapBrush> bb;
            rt->CreateBitmapBrush(g_frame.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &bb);
            if (bb)
            {
                // The bitmap is in pixels at the render target's DPI; map it onto dest.
                D2D1_SIZE_F bs = g_frame->GetSize();
                bb->SetTransform(D2D1::Matrix3x2F::Scale(dw / bs.width, dh / bs.height) * D2D1::Matrix3x2F::Translation(dest.left, dest.top));
                rt->FillRoundedRectangle(D2D1::RoundedRect(dest, 12, 12), bb.Get());
                drew = true;
            }
        }
        if (!drew)
        {
            std::wstring s = g_previewOn ? (g_view.GetMode() == Preview::Mode::Unavailable ? g_view.Status() : L"Waiting for video\x2026") : L"Preview is off";
            g_pbrush->SetColor(Hex(0x9A9AA6));
            g_prt->DrawText(L"\xE714", 1, g_fIcon.Get(), D2D1::RectF(0, size.height / 2 - 34, size.width, size.height / 2 - 6), g_pbrush.Get());
            g_prt->DrawText(s.c_str(), (UINT32)s.size(), g_fCenter.Get(), D2D1::RectF(20, size.height / 2, size.width - 20, size.height / 2 + 24), g_pbrush.Get());
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { g_prt.Reset(); g_pbrush.Reset(); g_frame.Reset(); }
    }

    void PlaceChildren()
    {
        SetWindowPos(g_previewWnd, nullptr, Px(M), Px(M), Px(LEFT_W), Px(PREVIEW_H), SWP_NOZORDER | SWP_NOACTIVATE);
        RECT r;
        GetClientRect(g_wnd, &r);
        if (g_rt) { g_rt->Resize(D2D1::SizeU(r.right, r.bottom)); g_rt->SetDpi((float)g_dpi, (float)g_dpi); }
        if (g_prt) { g_prt->Resize(D2D1::SizeU(Px(LEFT_W), Px(PREVIEW_H))); g_prt->SetDpi((float)g_dpi, (float)g_dpi); }
        g_view.SetTargetWidth((UINT32)Px(LEFT_W));
    }

    // ------------------------------------------------------------------ keyboard

    bool Focusable(int i) { return i >= 0 && i < (int)g_widgets.size() && Enabled(g_widgets[i]); }

    void MoveFocus(int dir)
    {
        int n = (int)g_widgets.size();
        if (!n) return;
        int i = g_focus;
        for (int k = 0; k < n; k++)
        {
            i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (Focusable(i)) { g_focus = i; Invalidate(); return; }
        }
    }

    void KeyOnFocus(UINT key)
    {
        if (!Focusable(g_focus)) return;
        Widget& w = g_widgets[g_focus];
        if (w.type == W::Slider)
        {
            SliderModel s = GetSlider(w);
            long step = SliderStep(s), page = (s.max - s.min) / 10;
            if (key == VK_LEFT || key == VK_DOWN) SetSliderValue(w, s.value - step);
            else if (key == VK_RIGHT || key == VK_UP) SetSliderValue(w, s.value + step);
            else if (key == VK_PRIOR) SetSliderValue(w, s.value + page);
            else if (key == VK_NEXT) SetSliderValue(w, s.value - page);
            else if (key == VK_HOME) SetSliderValue(w, s.min);
            else if (key == VK_END) SetSliderValue(w, s.max);
        }
        else if (w.type == W::Segment)
        {
            Item& it = g_items[w.item];
            int n = (int)it.options.size(), cur = 0;
            for (int i = 0; i < n; i++) if (it.options[i].value == it.value) cur = i;
            if (key == VK_LEFT && cur > 0) WriteItem(it, it.options[cur - 1].value);
            if (key == VK_RIGHT && cur < n - 1) WriteItem(it, it.options[cur + 1].value);
        }
        else if (key == VK_SPACE || key == VK_RETURN)
            Activate(g_focus);
    }

    // ------------------------------------------------------------------ window procedures

    LRESULT CALLBACK PreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            PaintPreview();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PREVIEW:
            InvalidateRect(h, nullptr, FALSE);
            InvalidateRect(g_wnd, nullptr, FALSE);   // status line
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONDOWN: SetFocus(g_wnd); return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            g_wnd = h;
            g_dpi = GetDpiForWindow(h);
            g_theme = MakeTheme(SystemUsesDark());
            ApplyTitleBar();
            g_previewWnd = CreateWindowExW(0, L"OpenCamHubPreview", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, h, nullptr, g_inst, nullptr);
            PlaceChildren();
            LoadSettings();
            Layout();
            Refresh(true);
            SetTimer(h, TIMER_REFRESH, 1000, nullptr);
            UpdatePreviewRunning();
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            CreateTargets();
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;

        case WM_DPICHANGED:
        {
            g_dpi = HIWORD(wp);
            RECT* r = (RECT*)lp;
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            PlaceChildren();
            Invalidate();
            return 0;
        }

        case WM_SIZE:
            g_minimized = wp == SIZE_MINIMIZED;
            if (!g_minimized) PlaceChildren();
            UpdatePreviewRunning();
            return 0;

        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0)
            {
                g_theme = MakeTheme(SystemUsesDark());
                ApplyTitleBar();
                Invalidate();
                InvalidateRect(g_previewWnd, nullptr, FALSE);
            }
            return 0;

        case WM_TIMER:
            if (wp == TIMER_REFRESH) Refresh(false);
            if (wp == TIMER_LUT_SAVE) { KillTimer(h, TIMER_LUT_SAVE); SaveLut(); SaveBackground(); }
            if (wp == TIMER_STATUS) { KillTimer(h, TIMER_STATUS); if (!g_statusIsError) SetStatus(L""); }
            return 0;

        case WM_MOUSEMOVE:
        {
            float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
            if (g_dragging && g_pressed >= 0) { SliderFromX(g_widgets[g_pressed], x); return 0; }
            int hit = HitTest(x, y);
            int seg = (hit >= 0 && g_widgets[hit].type == W::Segment) ? SegmentAt(g_widgets[hit], x) : -1;
            if (hit != g_hover || seg != g_hoverSeg)
            {
                g_hover = hit;
                g_hoverSeg = seg;
                Invalidate();
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hover != -1) { g_hover = -1; Invalidate(); }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT)
            {
                bool hand = g_hover >= 0 && Enabled(g_widgets[g_hover]);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        {
            SetFocus(h);
            float x = Dip(GET_X_LPARAM(lp)), y = Dip(GET_Y_LPARAM(lp));
            int hit = HitTest(x, y);
            if (hit < 0 || !Enabled(g_widgets[hit])) return 0;
            g_focus = hit;
            Widget& w = g_widgets[hit];
            if (w.type == W::Slider)
            {
                if (msg == WM_LBUTTONDBLCLK) { SetSliderValue(w, GetSlider(w).def); Invalidate(); return 0; }   // double-click resets
                g_pressed = hit;
                g_dragging = true;
                SetCapture(h);
                SliderFromX(w, x);
            }
            else if (w.type == W::Segment)
            {
                Item& it = g_items[w.item];
                WriteItem(it, it.options[SegmentAt(w, x)].value);
            }
            else
            {
                g_pressed = hit;
                SetCapture(h);
            }
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP:
        {
            int pressed = g_pressed;
            bool wasDrag = g_dragging;
            g_pressed = -1;
            g_dragging = false;
            if (GetCapture() == h) ReleaseCapture();
            if (pressed >= 0 && !wasDrag && HitTest(Dip(GET_X_LPARAM(lp)), Dip(GET_Y_LPARAM(lp))) == pressed) Activate(pressed);
            if (wasDrag && pressed >= 0 && (g_widgets[pressed].act == Act::LutIntensity || g_widgets[pressed].act == Act::BgStrength)) { KillTimer(h, TIMER_LUT_SAVE); SaveLut(); SaveBackground(); }
            Invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_dragging = false;
            g_pressed = -1;
            return 0;

        case WM_MOUSEWHEEL:
        {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &p);
            int hit = HitTest(Dip(p.x), Dip(p.y));
            if (hit >= 0 && g_widgets[hit].type == W::Slider && Enabled(g_widgets[hit]))
            {
                SliderModel s = GetSlider(g_widgets[hit]);
                int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                SetSliderValue(g_widgets[hit], s.value + notches * SliderStep(s));
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wp == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
            else KeyOnFocus((UINT)wp);
            Invalidate();
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            Invalidate();
            return 0;

        case WM_ACTIVATE:
            // Pick up LUT files or settings changed elsewhere while the window was in the background.
            if (LOWORD(wp) != WA_INACTIVE)
            {
                if (KillTimer(h, TIMER_LUT_SAVE)) { SaveLut(); SaveBackground(); }
                LoadSettings();
                SetFocus(h);
            }
            return 0;

        case WM_DESTROY:
            KillTimer(h, TIMER_REFRESH);
            if (KillTimer(h, TIMER_LUT_SAVE)) { SaveLut(); SaveBackground(); }
            g_view.Stop();
            g_cam.Close();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show)
{
    g_inst = inst;
    // Only one settings window at a time.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenCamHubSettings");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND other = FindWindowW(L"OpenCamHubMain", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    // --no-preview starts with the preview switched off.
    if (cmdLine && wcsstr(cmdLine, L"--no-preview")) g_previewOn = false;
    if (cmdLine && wcsstr(cmdLine, L"--theme=dark")) g_forceTheme = 1;
    if (cmdLine && wcsstr(cmdLine, L"--theme=light")) g_forceTheme = 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dw.GetAddressOf());
    if (!g_d2d || !g_dw) return 1;
    CreateFonts();
    BuildItems();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"OpenCamHubMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.lpszClassName = L"OpenCamHubPreview";
    RegisterClassExW(&pc);

    // Size the window for the monitor it will open on.
    POINT origin = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
        if (auto fn = (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r = { 0, 0, MulDiv((int)CLIENT_W, dpiX, 96), MulDiv((int)CLIENT_H, dpiX, 96) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpiX);
    HWND wnd = CreateWindowExW(0, L"OpenCamHubMain", L"OpenCamHub", style, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!wnd) return 1;
    ShowWindow(wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0))
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_frame.Reset(); g_pbrush.Reset(); g_prt.Reset(); g_brush.Reset(); g_rt.Reset();
    MFShutdown();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
