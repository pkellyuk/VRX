// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Bounded Windows reference transport. A named mutex protects the whole pair;
// consumers never observe one eye from a different publication.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace xg {
constexpr uint32_t PairMagic = 0x50534758; // XGSP
constexpr uint32_t PairVersion = 1;
constexpr uint32_t PairHeaderBytes = 256;
constexpr uint32_t MaxEyeSize = 2048;
constexpr size_t MaxPairBytes = size_t(MaxEyeSize) * MaxEyeSize * 8;
constexpr size_t PairMappingBytes = PairHeaderBytes + MaxPairBytes;
constexpr uint64_t PairMaxAgeMs = 1000;

struct alignas(8) PairHeader {
    uint32_t magic, version, headerBytes, eyeWidth, eyeHeight, stride, pixelBytes, active;
    uint64_t sequence, leftFrame, rightFrame, publishedMs;
    uint32_t producerPid, reserved;
};
static_assert(sizeof(PairHeader) == 72, "shared pair ABI");
struct PairFrame {
    PairHeader header{};
    std::vector<uint8_t> rgba; // full-width SBS: left eye, then right eye on each row
};
enum class PairRead { NewPair, Unchanged, Busy, Empty, Stale, Invalid, Closed };

inline bool ValidSession(const std::wstring& name)
{
    if (name.empty() || name.size() > 64) return false;
    for (wchar_t c : name)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')) return false;
    return true;
}
inline bool ValidPairShape(uint32_t width, uint32_t height, size_t bytes)
{
    return width >= 2 && height >= 2 && width <= MaxEyeSize && height <= MaxEyeSize &&
        !(width & 1) && !(height & 1) && bytes == size_t(width) * height * 8;
}
inline bool ValidPairHeader(const PairHeader& h)
{
    return h.magic == PairMagic && h.version == PairVersion && h.headerBytes == PairHeaderBytes &&
        ValidPairShape(h.eyeWidth, h.eyeHeight, h.pixelBytes) && h.stride == h.eyeWidth * 8 &&
        h.sequence && h.leftFrame && h.rightFrame >= h.leftFrame && h.rightFrame - h.leftFrame <= 1 &&
        h.producerPid && h.active == 1 && h.reserved == 0;
}

class PairLock {
    HANDLE mutex_;
    DWORD status_;
public:
    explicit PairLock(HANDLE mutex, DWORD timeout = 0) : mutex_(mutex),
        status_(mutex ? WaitForSingleObject(mutex, timeout) : WAIT_FAILED) {}
    ~PairLock() { if (status_ == WAIT_OBJECT_0 || status_ == WAIT_ABANDONED) ReleaseMutex(mutex_); }
    bool Ready() const { return status_ == WAIT_OBJECT_0; }
    bool Abandoned() const { return status_ == WAIT_ABANDONED; }
    bool Busy() const { return status_ == WAIT_TIMEOUT; }
    PairLock(const PairLock&) = delete;
    PairLock& operator=(const PairLock&) = delete;
};

class PairMapping {
protected:
    HANDLE mutex_ = nullptr, mapping_ = nullptr;
    uint8_t* view_ = nullptr;
    static std::wstring Name(const std::wstring& session, const wchar_t* suffix)
    { return L"Local\\VRX.xgstereo." + session + suffix; }
public:
    PairMapping() = default;
    PairMapping(const PairMapping&) = delete;
    PairMapping& operator=(const PairMapping&) = delete;
    ~PairMapping() { Reset(); }
    void Reset()
    {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
        view_ = nullptr; mapping_ = mutex_ = nullptr;
    }
};

class PairWriter : public PairMapping {
public:
    ~PairWriter() { Close(); }
    bool Create(const std::wstring& session)
    {
        Close(); Reset();
        if (!ValidSession(session)) return false;
        mutex_ = CreateMutexW(nullptr, FALSE, Name(session, L".lock").c_str());
        if (!mutex_) return false;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(PairMappingBytes), Name(session, L".frames").c_str());
        if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) { Reset(); return false; }
        view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, PairMappingBytes));
        if (!view_) { Reset(); return false; }
        bool ready = false;
        {
            PairLock lock(mutex_, 1000);
            ready = lock.Ready() || lock.Abandoned();
            if (ready)
            {
                std::memset(view_, 0, PairHeaderBytes);
                PairHeader initial{};
                initial.magic = PairMagic; initial.version = PairVersion; initial.headerBytes = PairHeaderBytes;
                initial.active = 1; initial.producerPid = GetCurrentProcessId();
                std::memcpy(view_, &initial, sizeof(initial));
            }
        }
        if (!ready) { Reset(); return false; }
        return true;
    }
    bool Publish(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
        uint64_t leftFrame, uint64_t rightFrame)
    {
        if (!view_ || !ValidPairShape(width, height, rgba.size()) || !leftFrame ||
            rightFrame < leftFrame || rightFrame - leftFrame > 1) return false;
        PairLock lock(mutex_);
        // A timeout drops the entire pair. Abandonment means an interrupted
        // consumer; this writer can restore a complete frame while owning it.
        if (!lock.Ready() && !lock.Abandoned()) return false;
        PairHeader old{}; std::memcpy(&old, view_, sizeof(old));
        if (old.sequence == UINT64_MAX) return false;
        PairHeader next{PairMagic, PairVersion, PairHeaderBytes, width, height, width * 8,
            static_cast<uint32_t>(rgba.size()), 1, old.sequence + 1, leftFrame, rightFrame,
            GetTickCount64(), GetCurrentProcessId(), 0};
        std::memcpy(view_ + PairHeaderBytes, rgba.data(), rgba.size());
        std::memcpy(view_, &next, sizeof(next));
        return true;
    }
    void Close()
    {
        if (!view_) return;
        PairLock lock(mutex_, 1000);
        if (!lock.Ready() && !lock.Abandoned()) return;
        auto* header = reinterpret_cast<PairHeader*>(view_);
        header->active = 0;
    }
};

class PairReader : public PairMapping {
    bool tainted_ = false;
public:
    bool Open(const std::wstring& session)
    {
        Reset(); tainted_ = false;
        if (!ValidSession(session)) return false;
        mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, Name(session, L".lock").c_str());
        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, Name(session, L".frames").c_str());
        if (!mutex_ || !mapping_) { Reset(); return false; }
        view_ = static_cast<uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, PairMappingBytes));
        if (!view_) { Reset(); return false; }
        return true;
    }
    PairRead Read(PairFrame& out, uint64_t previous = 0, uint64_t now = GetTickCount64())
    {
        if (!view_) return PairRead::Closed;
        PairLock lock(mutex_);
        if (lock.Abandoned()) tainted_ = true;
        if (tainted_) return PairRead::Invalid;
        if (!lock.Ready()) return PairRead::Busy;
        PairHeader h{}; std::memcpy(&h, view_, sizeof(h));
        if (h.magic == PairMagic && h.version == PairVersion && !h.active) return PairRead::Closed;
        if (!h.sequence) return PairRead::Empty;
        if (!h.active) return PairRead::Closed;
        if (!ValidPairHeader(h)) return PairRead::Invalid;
        if (h.publishedMs > now || now - h.publishedMs > PairMaxAgeMs) return PairRead::Stale;
        if (h.sequence == previous) return PairRead::Unchanged;
        // Copy to a temporary first: allocation failure cannot leave a half-pair
        // in the caller's last usable frame.
        PairFrame frame;
        frame.header = h;
        frame.rgba.assign(view_ + PairHeaderBytes, view_ + PairHeaderBytes + h.pixelBytes);
        out = std::move(frame);
        return PairRead::NewPair;
    }
};
} // namespace xg
