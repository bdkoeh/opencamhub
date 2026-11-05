#include "Background.h"
#include "../vcam/Lut.h"

namespace
{
    const int SHRINK = 8;

    // Bilinear sample of a float plane at (x, y) in plane coordinates.
    inline float Sample(const float* p, UINT w, UINT h, float x, float y)
    {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > w - 1.001f) x = w - 1.001f;
        if (y > h - 1.001f) y = h - 1.001f;
        UINT x0 = (UINT)x, y0 = (UINT)y;
        float fx = x - x0, fy = y - y0;
        const float* r0 = p + (size_t)y0 * w + x0;
        const float* r1 = r0 + w;
        return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
    }

    inline int AlphaAt(const uint8_t* a, UINT aw, UINT ah, float x, float y)
    {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > aw - 1.001f) x = aw - 1.001f;
        if (y > ah - 1.001f) y = ah - 1.001f;
        UINT x0 = (UINT)x, y0 = (UINT)y;
        int fx = (int)((x - x0) * 256), fy = (int)((y - y0) * 256);
        const uint8_t* r0 = a + (size_t)y0 * aw + x0;
        const uint8_t* r1 = r0 + aw;
        int top = r0[0] * (256 - fx) + r0[1] * fx, bottom = r1[0] * (256 - fx) + r1[1] * fx;
        return (top * (256 - fy) + bottom * fy + (1 << 15)) >> 16;
    }
}

// Three box-blur passes in each direction approximate a Gaussian.
void BackgroundBlur::Blur(std::vector<float>& p, std::vector<float>& tmp, int r)
{
    const int w = (int)m_sw, h = (int)m_sh;
    tmp.resize(p.size());
    const float norm = 1.f / (2 * r + 1);
    for (int pass = 0; pass < 3; pass++)
    {
        for (int y = 0; y < h; y++)   // horizontal: p -> tmp
        {
            const float* src = &p[(size_t)y * w];
            float* dst = &tmp[(size_t)y * w];
            float sum = 0;
            for (int i = -r; i <= r; i++) sum += src[i < 0 ? 0 : (i >= w ? w - 1 : i)];
            for (int x = 0; x < w; x++)
            {
                dst[x] = sum * norm;
                int add = x + r + 1, sub = x - r;
                sum += src[add >= w ? w - 1 : add] - src[sub < 0 ? 0 : sub];
            }
        }
        for (int x = 0; x < w; x++)   // vertical: tmp -> p
        {
            float sum = 0;
            for (int i = -r; i <= r; i++) sum += tmp[(size_t)(i < 0 ? 0 : (i >= h ? h - 1 : i)) * w + x];
            for (int y = 0; y < h; y++)
            {
                p[(size_t)y * w + x] = sum * norm;
                int add = y + r + 1, sub = y - r;
                sum += tmp[(size_t)(add >= h ? h - 1 : add) * w + x] - tmp[(size_t)(sub < 0 ? 0 : sub) * w + x];
            }
        }
    }
}

void BackgroundBlur::Apply(BYTE* yPlane, LONG yPitch, BYTE* uvPlane, LONG uvPitch, UINT width, UINT height,
                           const uint8_t* alpha, UINT aw, UINT ah, int strength)
{
    if (width < SHRINK * 2 || height < SHRINK * 2 || !alpha || aw < 2 || ah < 2) return;
    m_sw = width / SHRINK;
    m_sh = height / SHRINK;
    size_t n = (size_t)m_sw * m_sh;
    m_y.resize(n); m_u.resize(n); m_v.resize(n); m_w.resize(n);
    const float ax = (float)aw / width, ay = (float)ah / height;

    // Shrink: average each 8x8 block (4x4 for chroma), weighted by how much of it is background.
    ParallelFor((int)m_sh, [&](int sy) {
        for (UINT sx = 0; sx < m_sw; sx++)
        {
            UINT x0 = sx * SHRINK, y0 = (UINT)sy * SHRINK;
            int ys = 0;
            for (int j = 0; j < SHRINK; j++)
            {
                const BYTE* r = yPlane + (size_t)(y0 + j) * yPitch + x0;
                for (int i = 0; i < SHRINK; i++) ys += r[i];
            }
            int us = 0, vs = 0;
            for (int j = 0; j < SHRINK / 2; j++)
            {
                const BYTE* r = uvPlane + (size_t)(y0 / 2 + j) * uvPitch + x0;
                for (int i = 0; i < SHRINK; i += 2) { us += r[i]; vs += r[i + 1]; }
            }
            float bg = 1.f - AlphaAt(alpha, aw, ah, (x0 + SHRINK / 2) * ax - 0.5f, (y0 + SHRINK / 2) * ay - 0.5f) / 255.f;
            size_t o = (size_t)sy * m_sw + sx;
            m_w[o] = bg;
            m_y[o] = bg * ys / (SHRINK * SHRINK);
            m_u[o] = bg * us / (SHRINK * SHRINK / 4);
            m_v[o] = bg * vs / (SHRINK * SHRINK / 4);
        }
    });

    // Strength 0-100 maps to a box radius of 1-9 on the shrunk image (up to ~70 px at full size).
    int radius = 1 + (strength < 0 ? 0 : (strength > 100 ? 100 : strength)) * 8 / 100;
    // The four planes are independent; blur them in parallel.
    std::vector<float> tmps[4];
    std::vector<float>* planes[4] = { &m_y, &m_u, &m_v, &m_w };
    ParallelFor(4, [&](int i) { Blur(*planes[i], tmps[i], radius); });

    // Normalise: blurred(value x weight) / blurred(weight) = average of the background only.
    for (size_t i = 0; i < n; i++)
    {
        float w = m_w[i];
        if (w > 0.002f) { m_y[i] /= w; m_u[i] /= w; m_v[i] /= w; }
        else { m_y[i] = 16; m_u[i] = 128; m_v[i] = 128; }   // never shown: fully covered by the person
    }

    // Blend at full resolution: person from the original frame, background from the blur.
    // Bilinear positions and weights for every column and row are worked out once per frame.
    auto taps = [](UINT count, UINT srcSize, float scale, float offset, std::vector<Tap>& out) {
        out.resize(count);
        for (UINT k = 0; k < count; k++)
        {
            float p = (k + offset) * scale - 0.5f;
            if (p < 0) p = 0;
            if (p > srcSize - 1.001f) p = srcSize - 1.001f;
            out[k].i0 = (UINT)p;
            out[k].f = (int)((p - (UINT)p) * 256);
        }
    };
    UINT cw = width / 2, ch = height / 2;
    taps(width, aw, ax, 0.5f, m_axY);  taps(width, m_sw, 1.f / SHRINK, 0.5f, m_bxY);
    taps(height, ah, ay, 0.5f, m_ayY); taps(height, m_sh, 1.f / SHRINK, 0.5f, m_byY);
    taps(cw, aw, ax * 2, 0.5f, m_axC);  taps(cw, m_sw, 2.f / SHRINK, 0.5f, m_bxC);
    taps(ch, ah, ay * 2, 0.5f, m_ayC);  taps(ch, m_sh, 2.f / SHRINK, 0.5f, m_byC);

    // Blurred planes as 8.8 fixed point.
    m_yq.resize(n); m_uq.resize(n); m_vq.resize(n);
    for (size_t i = 0; i < n; i++)
    {
        auto q = [](float v) { int x = (int)(v * 256.f + 0.5f); return (uint16_t)(x < 0 ? 0 : (x > 65535 ? 65535 : x)); };
        m_yq[i] = q(m_y[i]); m_uq[i] = q(m_u[i]); m_vq[i] = q(m_v[i]);
    }

    const UINT sw = m_sw;
    // Each output row first blends the two source rows it falls between (vertical part of the
    // bilinear filter), so the per-pixel work is only the horizontal part.
    ParallelFor((int)ch, [&](int br) {
        thread_local std::vector<uint16_t> aRow, bRow, uRow, vRow;   // 8.8 fixed point
        aRow.resize(aw); bRow.resize(sw); uRow.resize(sw); vRow.resize(sw);
        auto vlerp8 = [](const uint8_t* r0, const uint8_t* r1, int fy, uint16_t* out, UINT n) {
            for (UINT k = 0; k < n; k++) out[k] = (uint16_t)((r0[k] * (256 - fy) + r1[k] * fy));
        };
        auto vlerp16 = [](const uint16_t* r0, const uint16_t* r1, int fy, uint16_t* out, UINT n) {
            for (UINT k = 0; k < n; k++) out[k] = (uint16_t)((r0[k] * (256 - fy) + r1[k] * fy) >> 8);
        };
        auto hlerp = [](const uint16_t* row, const Tap& t) {   // 8.8 in, 0..255 out
            return (row[t.i0] * (256 - t.f) + row[t.i0 + 1] * t.f + (1 << 15)) >> 16;
        };

        for (int r = 0; r < 2; r++)
        {
            UINT y = (UINT)br * 2 + r;
            BYTE* row = yPlane + (size_t)y * yPitch;
            const Tap& ty = m_ayY[y];
            const uint8_t* a0 = alpha + (size_t)ty.i0 * aw;
            vlerp8(a0, a0 + (ty.i0 + 1 < ah ? aw : 0), ty.f, aRow.data(), aw);
            const Tap& tb = m_byY[y];
            const uint16_t* b0 = m_yq.data() + (size_t)tb.i0 * sw;
            vlerp16(b0, b0 + (tb.i0 + 1 < m_sh ? sw : 0), tb.f, bRow.data(), sw);
            for (UINT x = 0; x < width; x++)
            {
                int a = hlerp(aRow.data(), m_axY[x]);
                if (a >= 255) continue;
                int b = hlerp(bRow.data(), m_bxY[x]);
                row[x] = (BYTE)((row[x] * a + b * (255 - a) + 127) / 255);
            }
        }
        BYTE* uv = uvPlane + (size_t)br * uvPitch;
        const Tap& ty = m_ayC[br];
        const uint8_t* a0 = alpha + (size_t)ty.i0 * aw;
        vlerp8(a0, a0 + (ty.i0 + 1 < ah ? aw : 0), ty.f, aRow.data(), aw);
        const Tap& tb = m_byC[br];
        size_t o0 = (size_t)tb.i0 * sw, o1 = o0 + (tb.i0 + 1 < m_sh ? sw : 0);
        vlerp16(m_uq.data() + o0, m_uq.data() + o1, tb.f, uRow.data(), sw);
        vlerp16(m_vq.data() + o0, m_vq.data() + o1, tb.f, vRow.data(), sw);
        for (UINT cx = 0; cx < cw; cx++)
        {
            int a = hlerp(aRow.data(), m_axC[cx]);
            if (a >= 255) continue;
            int u = hlerp(uRow.data(), m_bxC[cx]), v = hlerp(vRow.data(), m_bxC[cx]);
            uv[cx * 2] = (BYTE)((uv[cx * 2] * a + u * (255 - a) + 127) / 255);
            uv[cx * 2 + 1] = (BYTE)((uv[cx * 2 + 1] * a + v * (255 - a) + 127) / 255);
        }
    });
}