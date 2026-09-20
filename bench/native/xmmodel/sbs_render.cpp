// sbs_render: turns colour frames + depth-grid near maps into full side-by-side 3D
// frames with VRX's own stereo warp, so depth variants can be compared in any VR video
// player (bench/xmmodel/xm_sbs.py drives it).
//
// The warp is a line-for-line CPU port of kWarpHlsl in openxr/xrapp5.cpp (colour-
// resolution forward warp, bilinear NearAt on the 686x392 grid, nearest source wins,
// mirror hole fill from the farther neighbour with a nearness guard), with the
// disparity calibration of the desktop (non head-locked) path:
//   invZ spans 1/12 m (far) .. 1/1.2 m (near), minus 1/screenDistance, so the screen
//   plane has zero disparity; focalPx = colourWidth * screenDistance / screenWidth;
//   each eye is offset by +-ipd/2; shift = lround(strength * focalPx * eye * invZ).
//
// stdin, per frame:  CW*CH*3 bytes RGB, then 686*392 float32 near values (0 far..1 near,
//                    already dilated as the engine does before publishing)
// stdout, per frame: (2*CW)*CH*3 bytes RGB, left eye | right eye
//
// --subpixel keeps the fractional part of each pixel's shift instead of rounding it
// to whole pixels, which removes the depth banding ("ploughed field" ridges) on
// smoothly receding surfaces; the colour is then sampled bilinearly.
//
// usage: sbs_render.exe --size=CWxCH [--screen-width=5.7] [--distance=3] [--ipd=0.063] [--strength=1] [--subpixel]
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static const int DW = 686, DH = 392;            // depth grid (xr_common.h W x H)
static const float MIRROR_TOL = 0.08f;          // xr_common.h MIRROR_TOL
static const int NONE = -1;

static void Log(const char* fmt, ...)
{
    if (!fmt) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[sbs_render] %s\n", buf);
    fflush(stderr);
}

struct Params
{
    int cw = 1920, ch = 1080;
    float screenWidth = 5.7f, distance = 3.0f, ipd = 0.063f, strength = 1.0f;
    bool subpixel = false;
};

struct Frame
{
    const unsigned char* rgb = nullptr;         // cw x ch x 3
    const float* nearGrid = nullptr;            // DW x DH
};

// kWarpHlsl NearAt: pixel-centre bilinear from colour (x, y) into the depth grid.
static float NearAt(const Frame& f, const Params& p, int x, int y)
{
    const float fx = ((float)x + 0.5f) * (float)DW / (float)p.cw - 0.5f;
    const float fy = ((float)y + 0.5f) * (float)DH / (float)p.ch - 0.5f;
    const float x0f = floorf(fx), y0f = floorf(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int x0 = std::clamp((int)x0f, 0, DW - 1), x1 = std::clamp((int)x0f + 1, 0, DW - 1);
    const int y0 = std::clamp((int)y0f, 0, DH - 1), y1 = std::clamp((int)y0f + 1, 0, DH - 1);
    const float a = f.nearGrid[y0 * DW + x0], b = f.nearGrid[y0 * DW + x1];
    const float c = f.nearGrid[y1 * DW + x0], d = f.nearGrid[y1 * DW + x1];
    const float top = a + tx * (b - a);
    const float bot = c + tx * (d - c);
    return top + ty * (bot - top);
}

// One row of one eye: kWarpHlsl main(), fillMode = mirror.
static void WarpRow(const Frame& f, const Params& p, int y, float eye, float scaleFocal, float invZNear, float invZFar,
                    std::vector<int>& src, std::vector<float>& srcNear, std::vector<float>& rowNear,
                    std::vector<float>& srcDest, unsigned char* out)
{
    const int iw = p.cw;
    for (int x = 0; x < iw; x++) rowNear[x] = NearAt(f, p, x, y);
    std::fill(src.begin(), src.end(), NONE);

    // scatter: every source pixel moves by its own disparity; nearer wins
    for (int x = 0; x < iw; x++)
    {
        const float n = rowNear[x];
        const float invZ = invZFar + n * (invZNear - invZFar);
        const float s = scaleFocal * eye * invZ;
        const int r = (s < 0.0f) ? -(int)floorf(-s + 0.5f) : (int)floorf(s + 0.5f);   // lround
        const int dx = x - r;
        if (dx < 0 || dx >= iw) continue;
        if (src[dx] == NONE || n > srcNear[dx])
        {
            src[dx] = x;
            srcNear[dx] = n;
            srcDest[dx] = (float)x - s;          // where this source pixel really lands
        }
    }

    // hole fill from the FARTHER neighbour (background side), mirror mode
    int lastValid = -1;
    float lastNear = 0;
    int x = 0;
    while (x < iw)
    {
        if (src[x] != NONE) { lastValid = src[x]; lastNear = srcNear[x]; x++; continue; }

        int r = x + 1;
        while (r < iw && src[r] == NONE) r++;
        int rightValid = -1;
        float rightNear = 0;
        if (r < iw) { rightValid = src[r]; rightNear = srcNear[r]; }

        if (lastValid < 0 && rightValid < 0)
        {
            const float xn = rowNear[x];
            for (int h = x; h < r; h++) { src[h] = x; srcNear[h] = xn; srcDest[h] = (float)h; }
        }
        else
        {
            const bool useLeft = lastValid >= 0 && !(rightValid >= 0 && rightNear < lastNear);
            const int anchor = useLeft ? lastValid : rightValid;
            const float anchorNear = useLeft ? lastNear : rightNear;
            int good = anchor;
            float goodNear = anchorNear;
            const int n = r - x;
            for (int k = 1; k <= n; k++)
            {
                const int h = useLeft ? (x - 1 + k) : (r - k);
                const int cand = std::clamp(useLeft ? anchor - k : anchor + k, 0, iw - 1);
                const float cn = rowNear[cand];
                if (cn <= anchorNear + MIRROR_TOL) { good = cand; goodNear = cn; }
                src[h] = good;
                srcNear[h] = goodNear;
                srcDest[h] = (float)h;           // filled pixels have no sub-pixel position
            }
        }
        x = r;      // lastValid/lastNear deliberately unchanged: fills are not sources
    }

    const unsigned char* in = f.rgb + (size_t)y * p.cw * 3;
    if (!p.subpixel)
    {
        for (int xo = 0; xo < iw; xo++) memcpy(out + (size_t)xo * 3, in + (size_t)src[xo] * 3, 3);
        return;
    }
    // Sub-pixel: the source pixel `s` lands at srcDest[s]; the content that lands
    // exactly on this destination is at s + (dest - srcDest[s]) (the mapping's slope
    // is ~1), sampled bilinearly.
    for (int xo = 0; xo < iw; xo++)
    {
        const int s0 = src[xo];
        const float pos = std::clamp((float)s0 + ((float)xo - srcDest[xo]), 0.0f, (float)(iw - 1));
        const int i0 = (int)pos, i1 = std::min(i0 + 1, iw - 1);
        const float fr = pos - i0;
        for (int c = 0; c < 3; c++)
            out[(size_t)xo * 3 + c] = (unsigned char)(in[(size_t)i0 * 3 + c] * (1.0f - fr) + in[(size_t)i1 * 3 + c] * fr + 0.5f);
    }
}

static void RenderFrame(const Frame& f, const Params& p, unsigned char* sbs)
{
    if (!f.rgb || !f.nearGrid || !sbs) return;
    const float focalPx = (float)p.cw * p.distance / p.screenWidth;
    const float scaleFocal = p.strength * focalPx;
    const float invZNear = 1.0f / 1.2f - 1.0f / p.distance;
    const float invZFar = 1.0f / 12.0f - 1.0f / p.distance;
    const size_t outRow = (size_t)p.cw * 2 * 3;

    const unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++)
    {
        pool.emplace_back([&, t]()
        {
            std::vector<int> src(p.cw);
            std::vector<float> srcNear(p.cw), rowNear(p.cw), srcDest(p.cw);
            for (int y = (int)t; y < p.ch; y += (int)threads)
            {
                unsigned char* row = sbs + (size_t)y * outRow;
                WarpRow(f, p, y, -0.5f * p.ipd, scaleFocal, invZNear, invZFar, src, srcNear, rowNear, srcDest, row);
                WarpRow(f, p, y, +0.5f * p.ipd, scaleFocal, invZNear, invZFar, src, srcNear, rowNear, srcDest, row + (size_t)p.cw * 3);
            }
        });
    }
    for (auto& th : pool) th.join();
}

static bool ReadAll(void* dst, size_t bytes)
{
    char* p = (char*)dst;
    while (bytes > 0)
    {
        const size_t got = fread(p, 1, bytes, stdin);
        if (got == 0) return false;
        p += got;
        bytes -= got;
    }
    return true;
}

int main(int argc, char** argv)
{
    Params p;
    for (int i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        if (!a) continue;
        if (!strncmp(a, "--size=", 7)) { sscanf_s(a + 7, "%dx%d", &p.cw, &p.ch); continue; }
        if (!strncmp(a, "--screen-width=", 15)) { p.screenWidth = (float)atof(a + 15); continue; }
        if (!strncmp(a, "--distance=", 11)) { p.distance = (float)atof(a + 11); continue; }
        if (!strncmp(a, "--ipd=", 6)) { p.ipd = (float)atof(a + 6); continue; }
        if (!strncmp(a, "--strength=", 11)) { p.strength = (float)atof(a + 11); continue; }
        if (!strcmp(a, "--subpixel")) { p.subpixel = true; continue; }
        Log("unknown option %s", a);
        return 2;
    }
    if (p.cw < 16 || p.ch < 16 || p.screenWidth <= 0 || p.distance <= 0 || p.ipd <= 0 || p.strength < 0)
    {
        Log("bad parameters");
        return 2;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    const float focalPx = (float)p.cw * p.distance / p.screenWidth;
    Log("main: enter %dx%d screen %.2f m at %.2f m, ipd %.3f, strength %.2f, %s -> focal %.0f px, disparity near %+.1f px far %+.1f px per eye",
        p.cw, p.ch, p.screenWidth, p.distance, p.ipd, p.strength, p.subpixel ? "sub-pixel" : "whole pixels", focalPx,
        p.strength * focalPx * 0.5f * p.ipd * (1.0f / 1.2f - 1.0f / p.distance),
        p.strength * focalPx * 0.5f * p.ipd * (1.0f / 12.0f - 1.0f / p.distance));

    std::vector<unsigned char> rgb((size_t)p.cw * p.ch * 3), sbs((size_t)p.cw * 2 * p.ch * 3);
    std::vector<float> nearGrid((size_t)DW * DH);
    int frames = 0;
    while (ReadAll(rgb.data(), rgb.size()))
    {
        if (!ReadAll(nearGrid.data(), nearGrid.size() * sizeof(float)))
        {
            Log("main: FAIL truncated near map at frame %d", frames);
            return 1;
        }
        Frame f{ rgb.data(), nearGrid.data() };
        RenderFrame(f, p, sbs.data());
        if (fwrite(sbs.data(), 1, sbs.size(), stdout) != sbs.size())
        {
            Log("main: FAIL writing frame %d", frames);
            return 1;
        }
        fflush(stdout);
        frames++;
    }
    Log("main: exit %d frames", frames);
    return 0;
}
