#pragma once
#include <cwctype>
#include <string>
#include <vector>

// Which GPU runs the depth model. Pure logic, no D3D: unit-tested in playback_test.
//
// GPUs are chosen by name, never by DXGI adapter index: enumeration order can
// change after a driver update, a reboot or a new device, so a saved index could
// silently point at a different card. Identical cards are told apart by their
// order among cards with the same name ("the 2nd RTX 3060").

struct GpuEntry
{
    std::wstring name;
    unsigned long long memoryMB = 0;
    bool software = false;              // e.g. Microsoft Basic Render Driver
    bool headset = false;               // the GPU the VR runtime renders on
};

struct GpuRequest
{
    enum class Kind { Same, Auto, Index, Named };
    Kind kind = Kind::Same;
    int index = -1;                     // Kind::Index: DXGI adapter index (diagnostics)
    std::wstring name;                  // Kind::Named
    int nth = 0;                        // Kind::Named: 0 = first card with this name
};

inline std::wstring LowerGpuName(std::wstring s)
{
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t start = 0;
    while (start < s.size() && iswspace(s[start])) ++start;
    s = s.substr(start);
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

// "same" | "auto" | "<adapter index>" | "name:<GPU name>#<n>"
inline bool ParseGpuRequest(const std::wstring& text, GpuRequest& out)
{
    GpuRequest r;
    if (text == L"same") { out = r; return true; }
    if (text == L"auto") { r.kind = GpuRequest::Kind::Auto; out = r; return true; }
    if (text.rfind(L"name:", 0) == 0)
    {
        const std::wstring body = text.substr(5);
        const size_t hash = body.rfind(L'#');
        if (hash == std::wstring::npos || hash == 0 || hash + 1 >= body.size()) return false;
        int nth = 0;
        for (size_t i = hash + 1; i < body.size(); ++i)
        {
            if (!iswdigit(body[i]) || nth > 99) return false;
            nth = nth * 10 + int(body[i] - L'0');
        }
        r.kind = GpuRequest::Kind::Named;
        r.name = body.substr(0, hash);
        r.nth = nth;
        if (LowerGpuName(r.name).empty()) return false;
        out = r;
        return true;
    }
    if (text.empty() || text.size() > 2) return false;
    int index = 0;
    for (wchar_t c : text) { if (!iswdigit(c)) return false; index = index * 10 + int(c - L'0'); }
    r.kind = GpuRequest::Kind::Index;
    r.index = index;
    out = r;
    return true;
}

// Position of gpus[i] among cards with the same name, in enumeration order.
inline int NthOfName(const std::vector<GpuEntry>& gpus, size_t i)
{
    if (i >= gpus.size()) return -1;
    const std::wstring name = LowerGpuName(gpus[i].name);
    int n = 0;
    for (size_t k = 0; k < i; ++k)
        if (LowerGpuName(gpus[k].name) == name) ++n;
    return n;
}

// Returns the adapter index to run depth on, or -1 to use the headset GPU;
// `reason` says why when -1 (or which card was picked).
inline int ChooseDepthAdapter(const std::vector<GpuEntry>& gpus, const GpuRequest& request, std::wstring& reason)
{
    switch (request.kind)
    {
    case GpuRequest::Kind::Same:
        reason = L"same GPU as the headset requested";
        return -1;
    case GpuRequest::Kind::Auto:
    {
        int best = -1;
        for (size_t i = 0; i < gpus.size(); ++i)
            if (!gpus[i].software && !gpus[i].headset && (best < 0 || gpus[i].memoryMB > gpus[size_t(best)].memoryMB))
                best = int(i);
        reason = best < 0 ? L"no other hardware GPU found" : L"automatic: the largest other GPU";
        return best;
    }
    case GpuRequest::Kind::Index:
        if (request.index < 0 || size_t(request.index) >= gpus.size()) { reason = L"no GPU with that index"; return -1; }
        if (gpus[size_t(request.index)].software) { reason = L"that is a software adapter"; return -1; }
        if (gpus[size_t(request.index)].headset) { reason = L"that is the headset GPU; nothing to offload"; return -1; }
        reason = L"requested by index";
        return request.index;
    case GpuRequest::Kind::Named:
    {
        const std::wstring want = LowerGpuName(request.name);
        int seen = 0;
        for (size_t i = 0; i < gpus.size(); ++i)
        {
            if (LowerGpuName(gpus[i].name) != want) continue;
            if (seen++ != request.nth) continue;
            if (gpus[i].software) { reason = L"that is a software adapter"; return -1; }
            if (gpus[i].headset) { reason = L"that is the headset GPU; nothing to offload"; return -1; }
            reason = L"requested by name";
            return int(i);
        }
        reason = L"that GPU is not in this PC (removed, renamed by a driver, or disabled)";
        return -1;
    }
    }
    reason = L"unknown request";
    return -1;
}
