// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Left-to-right device-address translation for mirrored push constants.
// Written for VRX; independent of the adapted vrdual mirror sources.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xg {
// Interval map from left-eye buffer address ranges to the right-eye twin.
// Lookups take a shared lock (draw-time translation from many recording
// threads); inserts and erases are exclusive. Buffers bound to the same memory
// may overlap; all of them translate with the same twin delta, so any range
// containing an address gives the correct result.
class AddressMap {
    struct Range { uint64_t b = 0, size = 0; };
    std::map<std::pair<uint64_t, uint64_t>, Range> ranges_; // (A start, buffer) -> twin
    std::unordered_map<uint64_t, uint64_t> starts_;         // buffer -> A start
    uint64_t maxSize_ = 0; // never shrinks: bounds the backwards search
    mutable std::shared_mutex lock_;
public:
    // Records (or replaces) the ranges of one buffer. Returns false for an
    // empty or overflowing range, which is then not recorded.
    bool Insert(uint64_t buffer, uint64_t a, uint64_t b, uint64_t size)
    {
        if (!buffer || !a || !b || !size || a + size < a || b + size < b) return false;
        std::unique_lock<std::shared_mutex> lock(lock_);
        const auto old = starts_.find(buffer);
        if (old != starts_.end()) ranges_.erase({old->second, buffer});
        ranges_[{a, buffer}] = {b, size};
        starts_[buffer] = a;
        maxSize_ = std::max(maxSize_, size);
        return true;
    }
    void Erase(uint64_t buffer)
    {
        std::unique_lock<std::shared_mutex> lock(lock_);
        const auto old = starts_.find(buffer);
        if (old == starts_.end()) return;
        ranges_.erase({old->second, buffer});
        starts_.erase(old);
    }
    // O(log n + k): k = ranges starting within maxSize_ below the address.
    bool Translate(uint64_t address, uint64_t& out) const
    {
        if (!address) return false;
        std::shared_lock<std::shared_mutex> lock(lock_);
        auto it = ranges_.upper_bound({address, UINT64_MAX});
        while (it != ranges_.begin())
        {
            --it;
            const uint64_t start = it->first.first;
            if (address - start >= maxSize_) break; // no earlier range can reach
            if (address - start < it->second.size) { out = it->second.b + (address - start); return true; }
        }
        return false;
    }
    size_t Size() const { std::shared_lock<std::shared_mutex> lock(lock_); return ranges_.size(); }
};

// One VkPushConstantRange (stage flags as plain bits).
struct PushRange { uint32_t stages = 0, offset = 0, size = 0; };

// Splits a layout's (possibly overlapping) push ranges into disjoint segments,
// each carrying the union of the stages of every range covering it. Pushing a
// segment with those stages satisfies both vkCmdPushConstants rules: every
// listed stage has a range containing the bytes, and every range overlapping
// them has all of its stages listed.
inline std::vector<PushRange> PushSegments(const std::vector<PushRange>& ranges)
{
    std::vector<uint32_t> cuts;
    for (const auto& r : ranges)
    {
        if (!r.size || r.offset + r.size < r.offset) continue;
        cuts.push_back(r.offset); cuts.push_back(r.offset + r.size);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<PushRange> segments;
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
    {
        PushRange segment{0, cuts[i], cuts[i + 1] - cuts[i]};
        for (const auto& r : ranges)
            if (r.size && r.offset <= segment.offset && segment.offset + segment.size <= r.offset + r.size) segment.stages |= r.stages;
        if (!segment.stages) continue; // gap between ranges
        if (!segments.empty() && segments.back().stages == segment.stages && segments.back().offset + segments.back().size == segment.offset)
            segments.back().size += segment.size;
        else segments.push_back(segment);
    }
    return segments;
}

enum class PushTranslation { Translated, PartialPointer, UnknownAddress };
// Rewrites each 8-byte pointer field of `bytes` (offsets reflected from the
// pipeline actually bound at the draw) to the twin address. Bytes never
// written are ignored; a field written only partly or holding an address that
// belongs to no known buffer is refused rather than left pointing at the
// other eye. Zero (null) pointers stay zero. `count` receives the number of
// fields rewritten.
inline PushTranslation TranslatePushPointers(std::vector<uint8_t>& bytes, const std::vector<uint8_t>& written,
    const std::vector<uint32_t>& fields, const AddressMap& map, uint32_t& count)
{
    count = 0;
    for (const uint32_t field : fields)
    {
        if (size_t(field) + 8 > bytes.size() || size_t(field) + 8 > written.size())
        {
            // A field beyond the pushed data was never written.
            bool any = false;
            for (size_t i = field; i < written.size() && i < size_t(field) + 8; ++i) any = any || written[i];
            if (any) return PushTranslation::PartialPointer;
            continue;
        }
        uint32_t set = 0;
        for (uint32_t i = 0; i < 8; ++i) set += written[field + i] ? 1u : 0u;
        if (!set) continue;
        if (set != 8) return PushTranslation::PartialPointer;
        uint64_t address = 0; std::memcpy(&address, bytes.data() + field, 8);
        if (!address) continue;
        uint64_t twin = 0;
        if (!map.Translate(address, twin)) return PushTranslation::UnknownAddress;
        std::memcpy(bytes.data() + field, &twin, 8);
        ++count;
    }
    return PushTranslation::Translated;
}
} // namespace xg
