#include "Lut.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
    float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    uint8_t ToByte(float v) { int i = (int)std::lround(v); return (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i)); }

}

bool CubeLut::Load(const std::wstring& path, std::wstring* error)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
    {
        if (error) *error = L"cannot open " + path;
        return false;
    }
    size = 0;
    rgb.clear();
    char line[512];
    size_t expected = 0;
    while (fgets(line, sizeof(line), f))
    {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == 0) continue;
        if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.')
        {
            float r, g, b;
            if (sscanf_s(p, "%f %f %f", &r, &g, &b) == 3 && rgb.size() < expected * 3)
            {
                rgb.push_back(r); rgb.push_back(g); rgb.push_back(b);
            }
            continue;
        }
        if (strncmp(p, "LUT_3D_SIZE", 11) == 0)
        {
            size = atoi(p + 11);
            if (size < 2 || size > 256) { fclose(f); if (error) *error = L"unsupported LUT size"; return false; }
            expected = (size_t)size * size * size;
            rgb.reserve(expected * 3);
        }
        else if (strncmp(p, "DOMAIN_MIN", 10) == 0)
            sscanf_s(p + 10, "%f %f %f", &domainMin[0], &domainMin[1], &domainMin[2]);
        else if (strncmp(p, "DOMAIN_MAX", 10) == 0)
            sscanf_s(p + 10, "%f %f %f", &domainMax[0], &domainMax[1], &domainMax[2]);
        else if (strncmp(p, "LUT_1D_SIZE", 11) == 0)
        {
            fclose(f);
            if (error) *error = L"1D LUTs are not supported";
            return false;
        }
        // TITLE and other keywords are ignored.
    }
    fclose(f);
    if (size == 0 || rgb.size() != expected * 3)
    {
        if (error) *error = L"not a valid 3D .cube file";
        return false;
    }
    return true;
}

void CubeLut::Sample(float r, float g, float b, float out[3]) const
{
    float in[3] = { r, g, b };
    float pos[3];
    int i0[3], i1[3];
    float t[3];
    for (int c = 0; c < 3; c++)
    {
        float span = domainMax[c] - domainMin[c];
        float n = span > 0 ? (in[c] - domainMin[c]) / span : in[c];
        pos[c] = Clamp01(n) * (size - 1);
        i0[c] = (int)pos[c];
        if (i0[c] > size - 2) i0[c] = size - 2;
        i1[c] = i0[c] + 1;
        t[c] = pos[c] - i0[c];
    }
    auto at = [&](int ri, int gi, int bi) { return &rgb[(((size_t)bi * size + gi) * size + ri) * 3]; };
    for (int c = 0; c < 3; c++)
    {
        float c000 = at(i0[0], i0[1], i0[2])[c], c100 = at(i1[0], i0[1], i0[2])[c];
        float c010 = at(i0[0], i1[1], i0[2])[c], c110 = at(i1[0], i1[1], i0[2])[c];
        float c001 = at(i0[0], i0[1], i1[2])[c], c101 = at(i1[0], i0[1], i1[2])[c];
        float c011 = at(i0[0], i1[1], i1[2])[c], c111 = at(i1[0], i1[1], i1[2])[c];
        float c00 = c000 + (c100 - c000) * t[0], c10 = c010 + (c110 - c010) * t[0];
        float c01 = c001 + (c101 - c001) * t[0], c11 = c011 + (c111 - c011) * t[0];
        float c0 = c00 + (c10 - c00) * t[1], c1 = c01 + (c11 - c01) * t[1];
        out[c] = c0 + (c1 - c0) * t[2];
    }
}

std::shared_ptr<YuvLutTable> YuvLutTable::Build(const CubeLut& lut, float intensity, YuvFormat format)
{
    auto table = std::make_shared<YuvLutTable>();
    table->m_format = format;
    table->m_table.resize((size_t)128 * 128 * 256);

    // Y'CbCr <-> R'G'B' coefficients.
    const float kr = format.bt709 ? 0.2126f : 0.299f;
    const float kb = format.bt709 ? 0.0722f : 0.114f;
    const float kg = 1.f - kr - kb;
    const float yOff = format.fullRange ? 0.f : 16.f, yScale = format.fullRange ? 255.f : 219.f;
    const float cScale = format.fullRange ? 255.f : 224.f;
    const float k = Clamp01(intensity);

    ParallelFor(128, [&](int ui) {
        float cb = ((ui * 2 + 1) - 128.f) / cScale;
        for (int vi = 0; vi < 128; vi++)
        {
            float cr = ((vi * 2 + 1) - 128.f) / cScale;
            uint32_t* row = &table->m_table[((size_t)ui << 15) | ((size_t)vi << 8)];
            for (int y = 0; y < 256; y++)
            {
                float yy = (y - yOff) / yScale;
                float r = Clamp01(yy + 2.f * (1.f - kr) * cr);
                float b = Clamp01(yy + 2.f * (1.f - kb) * cb);
                float g = Clamp01((yy - kr * r - kb * b) / kg);
                float o[3];
                lut.Sample(r, g, b, o);
                r += (Clamp01(o[0]) - r) * k;
                g += (Clamp01(o[1]) - g) * k;
                b += (Clamp01(o[2]) - b) * k;
                float ny = kr * r + kg * g + kb * b;
                float ncb = (b - ny) / (2.f * (1.f - kb));
                float ncr = (r - ny) / (2.f * (1.f - kr));
                uint8_t oy = ToByte(ny * yScale + yOff);
                uint8_t ou = ToByte(ncb * cScale + 128.f);
                uint8_t ov = ToByte(ncr * cScale + 128.f);
                row[y] = oy | (uint32_t)ou << 8 | (uint32_t)ov << 16;
            }
        }
    });
    return table;
}

void YuvLutTable::ApplyNV12(BYTE* yPlane, LONG yPitch, BYTE* uvPlane, LONG uvPitch, UINT width, UINT height) const
{
    const uint32_t* table = m_table.data();
    int blockRows = (int)(height / 2);
    const int rowsPerChunk = 16;
    int chunks = (blockRows + rowsPerChunk - 1) / rowsPerChunk;
    ParallelFor(chunks, [&](int chunk) {
        int first = chunk * rowsPerChunk, last = first + rowsPerChunk;
        if (last > blockRows) last = blockRows;
        for (int br = first; br < last; br++)
        {
            BYTE* y0 = yPlane + (size_t)(br * 2) * yPitch;
            BYTE* y1 = y0 + yPitch;
            BYTE* uv = uvPlane + (size_t)br * uvPitch;
            for (UINT x = 0; x + 1 < width; x += 2)
            {
                const uint32_t* slice = table + (((size_t)(uv[x] >> 1)) << 15 | ((size_t)(uv[x + 1] >> 1)) << 8);
                uint32_t a = slice[y0[x]], b = slice[y0[x + 1]], c = slice[y1[x]], d = slice[y1[x + 1]];
                y0[x] = (BYTE)a; y0[x + 1] = (BYTE)b; y1[x] = (BYTE)c; y1[x + 1] = (BYTE)d;
                uv[x] = (BYTE)((((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((c >> 8) & 0xFF) + ((d >> 8) & 0xFF) + 2) >> 2);
                uv[x + 1] = (BYTE)((((a >> 16) & 0xFF) + ((b >> 16) & 0xFF) + ((c >> 16) & 0xFF) + ((d >> 16) & 0xFF) + 2) >> 2);
            }
        }
    });
}

void MirrorNV12(BYTE* yPlane, LONG yPitch, BYTE* uvPlane, LONG uvPitch, UINT width, UINT height)
{
    int blockRows = (int)(height / 2);
    const int rowsPerChunk = 16;
    int chunks = (blockRows + rowsPerChunk - 1) / rowsPerChunk;
    UINT w = width & ~1u;
    ParallelFor(chunks, [&](int chunk) {
        int first = chunk * rowsPerChunk, last = first + rowsPerChunk;
        if (last > blockRows) last = blockRows;
        for (int br = first; br < last; br++)
        {
            for (int r = 0; r < 2; r++)
            {
                BYTE* row = yPlane + (size_t)(br * 2 + r) * yPitch;
                for (UINT a = 0, b = w - 1; a < b; a++, b--) { BYTE t = row[a]; row[a] = row[b]; row[b] = t; }
            }
            // Chroma is stored as U,V pairs, one pair per 2 pixels: swap whole pairs.
            uint16_t* uv = (uint16_t*)(uvPlane + (size_t)br * uvPitch);
            for (UINT a = 0, b = w / 2 - 1; a < b; a++, b--) { uint16_t t = uv[a]; uv[a] = uv[b]; uv[b] = t; }
        }
    });
}

void NV12ToBGRA(const BYTE* yPlane, LONG yPitch, const BYTE* uvPlane, LONG uvPitch, UINT width, UINT height,
                UINT step, YuvFormat format, BYTE* out)
{
    if (step < 1) step = 1;
    UINT ow = width / step, oh = height / step;
    // Fixed-point (x 1024) YUV -> RGB coefficients.
    int ys, rv, gu, gv, bu, yOff;
    if (format.fullRange) { ys = 1024; yOff = 0; } else { ys = 1192; yOff = 16; }
    if (format.bt709)
    {
        if (format.fullRange) { rv = 1613; gu = -192; gv = -479; bu = 1900; }
        else { rv = 1836; gu = -218; gv = -546; bu = 2163; }
    }
    else
    {
        if (format.fullRange) { rv = 1436; gu = -352; gv = -731; bu = 1815; }
        else { rv = 1634; gu = -401; gv = -832; bu = 2066; }
    }
    auto clamp = [](int v) { return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    const int rowsPerChunk = 32;
    int chunks = (int)((oh + rowsPerChunk - 1) / rowsPerChunk);
    ParallelFor(chunks, [&](int chunk) {
        UINT first = chunk * rowsPerChunk, last = first + rowsPerChunk;
        if (last > oh) last = oh;
        for (UINT oy = first; oy < last; oy++)
        {
            UINT sy = oy * step;
            const BYTE* yr = yPlane + (size_t)sy * yPitch;
            const BYTE* uvr = uvPlane + (size_t)(sy / 2) * uvPitch;
            BYTE* o = out + (size_t)oy * ow * 4;
            for (UINT ox = 0; ox < ow; ox++, o += 4)
            {
                UINT sx = ox * step;
                int c = ((int)yr[sx] - yOff) * ys;
                int u = (int)uvr[sx & ~1u] - 128, v = (int)uvr[(sx & ~1u) + 1] - 128;
                o[0] = clamp((c + bu * u + 512) >> 10);
                o[1] = clamp((c + gu * u + gv * v + 512) >> 10);
                o[2] = clamp((c + rv * v + 512) >> 10);
                o[3] = 255;
            }
        }
    });
}