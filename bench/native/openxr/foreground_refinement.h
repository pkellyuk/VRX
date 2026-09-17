#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// A crop in normalized source coordinates. Equal normalized width/height keeps
// the same aspect ratio as the full-frame model input.
struct DepthCrop { float x = 0, y = 0, size = 1; };

inline float SampleDepth(const std::vector<float>& values, int w, int h, float u, float v)
{
    const float x = std::clamp(u * w - .5f, 0.f, float(w - 1));
    const float y = std::clamp(v * h - .5f, 0.f, float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float a = x - x0, b = y - y0;
    return (values[y0*w+x0]*(1-a) + values[y0*w+x1]*a)*(1-b) +
        (values[y1*w+x0]*(1-a) + values[y1*w+x1]*a)*b;
}

// Depth-only region tracking, not semantic character recognition. Reject small
// specks and almost-full-screen foreground, and reacquire after cuts/disappearance.
class ForegroundTracker
{
    static constexpr int GW = 49, GH = 28;
    std::vector<float> previous;
    bool have = false;
    float cx = 0, cy = 0;
    double since = 0, last = 0;
    int observations = 0;
public:
    void Reset() { previous.clear(); have = false; observations = 0; }
    bool Update(const std::vector<float>& depth, int w, int h, double time, DepthCrop& crop)
    {
        if (w < 1 || h < 1 || depth.size() != size_t(w)*h) { Reset(); return false; }
        std::vector<float> grid(GW*GH);
        double change = 0;
        for (int y=0; y<GH; ++y) for (int x=0; x<GW; ++x)
        {
            const int i=y*GW+x;
            grid[i] = SampleDepth(depth, w, h, (x+.5f)/GW, (y+.5f)/GH);
            if (!std::isfinite(grid[i])) { Reset(); return false; }
            if (previous.size()==grid.size()) change += std::abs(grid[i]-previous[i]);
        }
        if (change/grid.size() > .20 || (have && (time <= last || time-last > .4)))
        { have=false; observations=0; }
        previous=grid;
        std::vector<bool> seen(grid.size());
        int best=0, bx0=0, by0=0, bx1=0, by1=0;
        for (int i=0; i<GW*GH; ++i)
        {
            if (seen[i] || grid[i] < .65f) continue;
            std::vector<int> queue{ i }; seen[i]=true;
            int x0=GW, y0=GH, x1=0, y1=0;
            for (size_t j=0; j<queue.size(); ++j)
            {
                const int p=queue[j], x=p%GW, y=p/GW;
                x0=std::min(x0,x); y0=std::min(y0,y); x1=std::max(x1,x); y1=std::max(y1,y);
                for (int k=0; k<4; ++k)
                {
                    const int nx=x+(k==0)-(k==1), ny=y+(k==2)-(k==3);
                    if (nx<0 || nx>=GW || ny<0 || ny>=GH) continue;
                    const int n=ny*GW+nx;
                    if (!seen[n] && grid[n]>=.65f) { seen[n]=true; queue.push_back(n); }
                }
            }
            if (int(queue.size())>best) { best=int(queue.size()); bx0=x0; by0=y0; bx1=x1; by1=y1; }
        }
        if (best<8 || best>GW*GH*.60f) { have=false; observations=0; return false; }
        const float nx=(bx0+bx1+1.f)/(2*GW), ny=(by0+by1+1.f)/(2*GH);
        if (!have || std::hypot(nx-cx,ny-cy)>.18f) { since=time; observations=0; }
        have=true; last=time; cx=nx; cy=ny; ++observations;
        const float extent=std::max(float(bx1-bx0+1)/GW, float(by1-by0+1)/GH)+.14f;
        if (extent>.85f) return false; // little useful magnification
        crop.size=std::clamp(extent,.35f,.85f);
        crop.x=std::clamp(cx-crop.size*.5f,0.f,1.f-crop.size);
        crop.y=std::clamp(cy-crop.size*.5f,0.f,1.f-crop.size);
        return observations>=3 && time-since>=.25;
    }
};

// No more than five extra passes/s, and estimated crop work at most 20% of wall
// time. Avoid a pass when it would likely push source age past the stereo limit.
struct ForegroundBudget
{
    double next = 0, estimateMs = 0;
    bool CanRun(double now, double sourceAgeMs, double baseMs) const
    { return now>=next && sourceAgeMs+std::max(baseMs,estimateMs)<180; }
    void Complete(double now, double costMs)
    { estimateMs=costMs; next=now+std::max(.2, costMs*.004); }
};

// Fit local relative depth to the same frame's global estimate. Reject poorly
// agreeing crops rather than independently normalizing them and changing scale.
// Blend only near objects and their immediate boundary; leave far scenery alone.
inline bool FuseForeground(std::vector<float>& base, const std::vector<float>& local,
    int w, int h, const DepthCrop& crop)
{
    if (w<1 || h<1 || base.size()!=size_t(w)*h || local.size()!=base.size() ||
        !std::isfinite(crop.x) || !std::isfinite(crop.y) || !std::isfinite(crop.size) ||
        crop.size<=0 || crop.x<0 || crop.y<0 || crop.x+crop.size>1.00001f || crop.y+crop.size>1.00001f) return false;
    for (float v:local) if (!std::isfinite(v)) return false;
    for (float v:base) if (!std::isfinite(v)) return false;
    struct Pair { double x,y; };
    std::vector<Pair> points;
    for (int y=2; y<30; ++y) for (int x=2; x<46; ++x)
    {
        const float u=(x+.5f)/48, v=(y+.5f)/32;
        points.push_back({SampleDepth(local,w,h,u,v), SampleDepth(base,w,h,crop.x+u*crop.size,crop.y+v*crop.size)});
    }
    double scale=0, shift=0;
    for (int pass=0; pass<2; ++pass)
    {
        double sx=0,sy=0,sxx=0,syy=0,sxy=0,n=0;
        for (const auto& p:points)
        {
            if (pass && std::abs(scale*p.x+shift-p.y)>.15) continue;
            sx+=p.x; sy+=p.y; sxx+=p.x*p.x; syy+=p.y*p.y; sxy+=p.x*p.y; ++n;
        }
        if (n<points.size()*.6) return false;
        const double vx=sxx-sx*sx/n, vy=syy-sy*sy/n, cov=sxy-sx*sy/n;
        if (vx<1e-8 || vy/n<.002 || cov<=0 || cov*cov<.45*vx*vy) return false;
        scale=cov/vx; shift=(sy-scale*sx)/n;
        if (!std::isfinite(scale) || !std::isfinite(shift)) return false;
    }
    double error=0;
    for (const auto& p:points) error+=std::min(.04,std::pow(scale*p.x+shift-p.y,2));
    if (std::sqrt(error/points.size())>.12) return false;
    for (int y=0; y<h; ++y) for (int x=0; x<w; ++x)
    {
        const float u=((x+.5f)/w-crop.x)/crop.size, v=((y+.5f)/h-crop.y)/crop.size;
        if (u<=0 || u>=1 || v<=0 || v>=1) continue;
        float& original=base[y*w+x];
        const float refined=std::clamp(float(scale*SampleDepth(local,w,h,u,v)+shift),0.f,1.f);
        const float foreground=std::clamp((std::max(original,refined)-.45f)/.20f,0.f,1.f);
        const float edge=std::clamp(std::min({u,v,1-u,1-v})/.12f,0.f,1.f);
        original+=.65f*foreground*edge*std::clamp(refined-original,-.15f,.15f);
    }
    return true;
}
