// App-unaware resource and command mirroring, adapted from vrdual.
// See MIRROR_ORIGIN.md for provenance and differences.
#pragma once
#include <shared_mutex>
#include <unordered_set>
#include <deque>
#include <chrono>
#include "push_pointers.h"
#include <cstdarg>
namespace mirror {
#include "mirror_dispatch.hpp"
struct EyeBuf { VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceAddress address{}; std::atomic<uint64_t>* patches = nullptr; };
static void logf(const char* format, ...) {
    char line[1024]; va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args); Log(line);
}
static const char* env(const char* name, const char* fallback) {
    // Diagnostic flags only. Avoid borrowing the CRT environment across the game ABI.
    static thread_local char value[128]; const DWORD n = GetEnvironmentVariableA(name, value, 128);
    return n && n < 128 ? value : fallback;
}
template<typename T> static void* key(T h) { return Key(h); }
static uint64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool spv_patch_for(EyeBuf& ep, const VkShaderModuleCreateInfo* in, std::vector<uint32_t>& code) {
    if (!in->pCode || in->codeSize % 4) return false;
    code.assign(in->pCode, in->pCode + in->codeSize / 4);
    const bool changed = xg::PatchClipX(code, 0, ep.address, true).changed;
    if (changed && ep.patches) ++*ep.patches;
    return changed;
}
static VkResult create_patched_module(EyeBuf& ep, PFN_vkCreateShaderModule fn, VkDevice dev, const VkShaderModuleCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    std::vector<uint32_t> code; auto changed = *ci;
    if (spv_patch_for(ep, ci, code)) { changed.pCode = code.data(); changed.codeSize = code.size() * 4; }
    return fn(dev, &changed, alloc, out);
}
struct InlinePatch {
    std::vector<VkGraphicsPipelineCreateInfo> cis;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages;
    std::deque<std::vector<uint32_t>> codes;
    std::deque<VkShaderModuleCreateInfo> smcis;
};

static const VkGraphicsPipelineCreateInfo* patch_inline(EyeBuf& ep, uint32_t n, const VkGraphicsPipelineCreateInfo* cis, InlinePatch& p) {
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t s = 0; s < cis[i].stageCount && cis[i].pStages; ++s) {
            auto* sm = (const VkShaderModuleCreateInfo*)cis[i].pStages[s].pNext;
            if (cis[i].pStages[s].module || !sm || sm->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            std::vector<uint32_t> code;
            if (!spv_patch_for(ep, sm, code)) continue;
            if (p.cis.empty()) { p.cis.assign(cis, cis + n); p.stages.resize(n); }
            if (p.stages[i].empty()) p.stages[i].assign(cis[i].pStages, cis[i].pStages + cis[i].stageCount);
            p.codes.push_back(std::move(code));
            VkShaderModuleCreateInfo m = *sm;
            m.codeSize = p.codes.back().size() * 4;
            m.pCode = p.codes.back().data();
            p.smcis.push_back(m);
            p.stages[i][s].pNext = &p.smcis.back();
            p.cis[i].pStages = p.stages[i].data();
        }
    return p.cis.empty() ? cis : p.cis.data();
}

}
namespace mirror {

template <typename T> static uint64_t h2u(T h) {
    if constexpr (std::is_pointer_v<T>) return (uint64_t)(uintptr_t)h;
    else return (uint64_t)h;
}
template <typename T> static T u2h(uint64_t u) {
    if constexpr (std::is_pointer_v<T>) return (T)(uintptr_t)u;
    else return (T)u;
}
// ---------------------------------------------------------------- device ----
// Host-visible allocation. Non-cached types (write-combined/BAR, written by
// the app, never read) are mapped through a CPU shadow: the app writes the
// shadow, submit copies changed words to A and B. Cached types are read back
// by the app, so it gets A's real mapping and only the changes go to B.
struct Mem {
    uint64_t bmem = 0;
    VkDeviceSize size = 0;
    bool cached = false, coherent = true;
    bool mapped = false;
    char* amap = nullptr;    // null while A is mapped only during pushes (transient)
    char* bmap = nullptr;    // same for B
    bool a_released = false; // transient: A unmapped right after the app's vkMapMemory
    VkDeviceSize off = 0, len = 0;
    char* shadow = nullptr;  // non-cached only
    char* last = nullptr;    // what A and B hold, as last pushed
    size_t cap = 0;          // VirtualAlloc bytes for shadow/last
    std::vector<void*> dirtyPages;
    VkDeviceSize cap_off = 0;
};

struct Template {
    std::vector<VkDescriptorUpdateTemplateEntry> entries;
    size_t size = 0;  // bytes of pData read by an update
};

struct MDev {
    Dispatch a, b;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkSetDeviceLoaderData loader = nullptr;
    EyeBuf aep;
    std::unordered_map<uint64_t, uint64_t> handles;
    VkDevice bdev = VK_NULL_HANDLE;
    VkInstance binst = VK_NULL_HANDLE;
    VkPhysicalDevice bphys = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr icd_gipa = nullptr;
    VkPhysicalDeviceMemoryProperties amp{}, bmp{};
    std::string bname, bpci;
    std::atomic<bool> live{true};
    bool replay = false;  // device addresses: captured on A, replayed on B (same values)
    EyeBuf bep;
    bool stoppedLogged = false;

    VkQueue bqueue_any = VK_NULL_HANDLE;
    std::mutex hlock;
    PFN_vkGetDeviceProcAddr next = nullptr;
    uint32_t family = 0;
    LUID adapter{};
    std::wstring session;
    std::unique_ptr<xg::GpuExporter> gpu;
    struct Swap {
        VkSwapchainKHR handle{}; VkExtent2D extent{};
        std::vector<VkImage> images, twins;
        std::vector<VkDeviceMemory> memory;
        std::vector<VkSemaphore> ready;
    } swap;
    uint64_t frame = 0, publicationDrops = 0;
    struct Address { VkDeviceAddress a = 0, b = 0; VkDeviceSize size = 0; };
    std::unordered_map<uint64_t, Address> addresses;
    std::unordered_map<uint64_t, VkDeviceSize> bufferSizes;
    std::unordered_map<uint64_t, xg::PushPointers> shaderPointers;
    struct Field { VkShaderStageFlags stages; uint32_t offset; };
    struct Pipeline { VkPipelineLayout layout{}; std::vector<Field> fields; };
    std::unordered_map<uint64_t, Pipeline> pipelines;
    struct Bound { VkPipeline graphics{}, compute{}; };
    std::unordered_map<uint64_t, Bound> bound;
    std::mutex addressLock;
    uint64_t translated = 0;
    std::atomic<uint64_t> patches{0};
    std::atomic<uint64_t> submits{0};
    std::mutex qlock;  // our own extra submits on B vs the app's
    std::mutex lock;   // tables below
    std::unordered_map<uint64_t, Mem> mems;
    std::unordered_map<uint64_t, Template> templates;
    std::unordered_set<VkCommandBuffer> secondary;
    std::unordered_set<uint64_t> pending;  // fences with a B submit not yet waited for
    uint64_t pushed_bytes = 0, push_ns = 0, pushes = 0;
};

template <typename T> static T xl_handle(MDev* md, T a) {
    if (!a) return a;
    std::lock_guard<std::mutex> l(md->hlock);
    auto it = md->handles.find(h2u(a));
    if (it != md->handles.end()) return u2h<T>(it->second);
    md->live = false;
    logf("mirror STOPPED: missing twin 0x%llx (%s)", (unsigned long long)h2u(a), __FUNCSIG__);
    if (md->gpu) md->gpu->Close();
    return T{};
}
template <typename T> static void record_handle(MDev* md, T a, T b) {
    if (!a) return;
    std::lock_guard<std::mutex> l(md->hlock); md->handles[h2u(a)] = h2u(b);
}
template <typename T> static void forget_handle(MDev* md, T a) {
    std::lock_guard<std::mutex> l(md->hlock); md->handles.erase(h2u(a));
}
static MDev* mdev_of(void* k) {
    auto& data = Data(); std::lock_guard<std::mutex> l(data.mutex);
    auto it = data.devices.find(k);
    return it == data.devices.end() ? nullptr : it->second.mirror.get();
}
static void mirror_fail(MDev* md, const char* why) {
    if (md->live.exchange(false)) { logf("mirror STOPPED: %s; device=0x%llx", why, (unsigned long long)h2u(md->device)); if (md->gpu) md->gpu->Close(); }
}
// Per-call scratch: arena for translated structs + translation status.
struct Ctx {
    MDev* md = nullptr;
    bool bad = false;
    std::vector<std::pair<char*, size_t>> blocks;
    size_t used = 0;

    template <typename T> T* alloc(size_t n) {
        size_t sz = (sizeof(T) * (n ? n : 1) + 15) & ~size_t(15);
        if (blocks.empty() || used + sz > blocks.back().second) {
            size_t cap = std::max<size_t>(sz, 256u << 10);
            blocks.push_back({(char*)malloc(cap), cap});
            used = 0;
        }
        T* p = (T*)(blocks.back().first + used);
        used += sz;
        memset((void*)p, 0, sz);
        return p;
    }
    template <typename T> T xl(T a) { return xl_handle(md, a); }
    template <typename T> void record(T a, T b) { record_handle(md, a, b); }
    bool ok() const { return !bad && md->live; }
    void fail(const char* why) {
        bad = true;
        logf("mirror: %s", why);
    }
    void unknown_stype(VkStructureType st) {
        char msg[96]; std::snprintf(msg, sizeof(msg), "unknown pNext structure %d", int(st)); fail(msg);

    }
};

// VRDUAL_MIRROR_TRACE=N: log the first N mirrored calls (debugging aid).
static long g_trace = atol(env("VRX_XG_MIRROR_TRACE", "0"));
static void trace_call(const char* name) {
    static std::atomic<long> n{0};
    if (n++ < g_trace) logf("call %s", name);
}

static thread_local Ctx t_ctx;
static Ctx& ctx_begin(MDev* md) {
    t_ctx.md = md;
    t_ctx.bad = false;
    return t_ctx;
}
static void ctx_end(Ctx& c) {
    while (c.blocks.size() > 1) {
        free(c.blocks.front().first);
        c.blocks.erase(c.blocks.begin());
    }
    c.used = 0;
}

#pragma warning(push)
#pragma warning(disable: 4100 4189 4505 4244 4267)

static VkImageLayout eye_layout(VkImageLayout layout) { return layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? VK_IMAGE_LAYOUT_GENERAL : layout; }
#include "mirror_gen.hpp"
#pragma warning(pop)

// ------------------------------------------------ hand-written translators --
static bool is_image_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
static bool is_buffer_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static bool is_texel_desc(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}
static bool has_sampler(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }

static VkDescriptorImageInfo xl_image_info(Ctx& c, VkDescriptorType t, VkDescriptorImageInfo ii) {
    ii.sampler = has_sampler(t) ? c.xl(ii.sampler) : VK_NULL_HANDLE;
    ii.imageView = t != VK_DESCRIPTOR_TYPE_SAMPLER ? c.xl(ii.imageView) : VK_NULL_HANDLE;
    return ii;
}

// Only the array matching descriptorType is valid; the others may be garbage.
static void tr_VkWriteDescriptorSet(Ctx& c, const VkWriteDescriptorSet* in, VkWriteDescriptorSet* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    out->dstSet = c.xl(in->dstSet);
    out->pImageInfo = nullptr;
    out->pBufferInfo = nullptr;
    out->pTexelBufferView = nullptr;
    uint32_t n = in->descriptorCount;
    if (is_image_desc(in->descriptorType) && in->pImageInfo) {
        auto* t = c.alloc<VkDescriptorImageInfo>(n);
        for (uint32_t i = 0; i < n; ++i) t[i] = xl_image_info(c, in->descriptorType, in->pImageInfo[i]);
        out->pImageInfo = t;
    } else if (is_buffer_desc(in->descriptorType) && in->pBufferInfo) {
        auto* t = c.alloc<VkDescriptorBufferInfo>(n);
        for (uint32_t i = 0; i < n; ++i) { t[i] = in->pBufferInfo[i]; t[i].buffer = c.xl(t[i].buffer); }
        out->pBufferInfo = t;
    } else if (is_texel_desc(in->descriptorType) && in->pTexelBufferView) {
        auto* t = c.alloc<VkBufferView>(n);
        for (uint32_t i = 0; i < n; ++i) t[i] = c.xl(in->pTexelBufferView[i]);
        out->pTexelBufferView = t;
    }
}

static void tr_VkFramebufferCreateInfo(Ctx& c, const VkFramebufferCreateInfo* in, VkFramebufferCreateInfo* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    out->renderPass = c.xl(in->renderPass);
    if (!(in->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && in->pAttachments) {
        auto* t = c.alloc<VkImageView>(in->attachmentCount);
        for (uint32_t i = 0; i < in->attachmentCount; ++i) t[i] = c.xl(in->pAttachments[i]);
        out->pAttachments = t;
    }
}

static void tr_VkDescriptorSetLayoutBinding(Ctx& c, const VkDescriptorSetLayoutBinding* in, VkDescriptorSetLayoutBinding* out) {
    *out = *in;
    if (has_sampler(in->descriptorType) && in->pImmutableSamplers) {
        auto* t = c.alloc<VkSampler>(in->descriptorCount);
        for (uint32_t i = 0; i < in->descriptorCount; ++i) t[i] = c.xl(in->pImmutableSamplers[i]);
        out->pImmutableSamplers = t;
    } else {
        out->pImmutableSamplers = nullptr;
    }
}

// Inline module (pipeline stage pNext): patched for the right eye on B.
static void tr_VkShaderModuleCreateInfo(Ctx& c, const VkShaderModuleCreateInfo* in, VkShaderModuleCreateInfo* out) {
    *out = *in;
    out->pNext = tr_chain(c, in->pNext);
    std::vector<uint32_t> code;
    if (spv_patch_for(c.md->bep, in, code)) {
        uint32_t* w = c.alloc<uint32_t>(code.size());
        memcpy(w, code.data(), code.size() * 4);
        out->pCode = w;
        out->codeSize = code.size() * 4;
    }
}

static void tr_gpci(Ctx& c, const VkGraphicsPipelineCreateInfo* in, VkGraphicsPipelineCreateInfo* out) {
    *out = *in;
    // A may have hit its pipeline cache; B has to compile instead of failing.
    out->flags &= ~(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    out->pNext = tr_chain(c, in->pNext);
    if (in->pStages && in->stageCount) {
        auto* t = c.alloc<VkPipelineShaderStageCreateInfo>(in->stageCount);
        for (uint32_t i = 0; i < in->stageCount; ++i) tr_VkPipelineShaderStageCreateInfo(c, &in->pStages[i], &t[i]);
        out->pStages = t;
    }
    out->layout = c.xl(in->layout);
    out->renderPass = c.xl(in->renderPass);
    out->basePipelineHandle = c.xl(in->basePipelineHandle);
}

// ------------------------------------------------------------- memory -------
static bool word_diff_copy(const char* src, char* last, size_t len, char* d0, char* d1) {
    bool any = false;
    for (size_t p = 0; p < len; p += 4096) {
        size_t n = std::min<size_t>(4096, len - p);
        if (!memcmp(src + p, last + p, n)) continue;
        any = true;
        size_t i = p, end = p + n;
        for (; i + 8 <= end; i += 8) {
            uint64_t v, l;
            memcpy(&v, src + i, 8);  // read once: app threads may be writing
            memcpy(&l, last + i, 8);
            if (v == l) continue;
            memcpy(last + i, &v, 8);
            if (d0) memcpy(d0 + i, &v, 8);
            if (d1) memcpy(d1 + i, &v, 8);
        }
        for (; i < end; ++i) {
            char v = src[i];
            if (v == last[i]) continue;
            last[i] = v;
            if (d0) d0[i] = v;
            if (d1) d1[i] = v;
        }
    }
    return any;
}

// Offset of the first 4 KB page that differs, or SIZE_MAX.
static size_t first_change(const char* src, const char* last, size_t len) {
    for (size_t p = 0; p < len; p += 4096)
        if (memcmp(src + p, last + p, std::min<size_t>(4096, len - p))) return p;
    return SIZE_MAX;
}

// 32-bit processes run out of address space with four mappings per mapped
// allocation (A, B, shadow, last): there A (under a shadow) and B are mapped
// only while a push copies changed pages.
static bool transient_maps() {
    static const bool t = sizeof(void*) == 4 || !strcmp(env("VRX_XG_TRANSIENT_MAPS", "0"), "1");
    return t;
}

// md->lock held. Copies what the app changed since the last push to A (when
// the app writes a shadow; also after the mirror stopped, or the game would
// render stale data) and to B (while the mirror is live).
static void push_one(MDev* md, uint64_t amem, Mem& m) {
    bool to_a = !m.cached, to_b = md->live;
    const char* src = m.cached ? m.amap : m.shadow;
    if (!m.mapped || !src || (!to_a && !to_b)) return;
    // WRITE_WATCH avoids scanning hundreds of megabytes of unchanged upload
    // allocations at each DXVK submit. Reset precedes copying, so writes by
    // another app thread during the copy remain dirty for the next submit.
    bool watched = false;
    size_t first = 0;
    ULONG_PTR dirtyCount = m.dirtyPages.size(); DWORD granularity = 0;
    if (!m.cached) {
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, m.shadow, m.cap, m.dirtyPages.data(), &dirtyCount, &granularity) != 0) {
            mirror_fail(md, "shadow write tracking failed");
            // A must still receive pending shadow writes after a mirror stop.
        } else { if (!dirtyCount) return; watched = true; }
    }
    if (!watched) { first = first_change(src, m.last, m.len); if (first == SIZE_MAX) return; }
    VkDevice adev = md->device;
    char *a = to_a ? m.amap : nullptr, *b = to_b ? m.bmap : nullptr;
    bool a_tmp = to_a && !a, b_tmp = to_b && !b;
    if (a_tmp && md->a.MapMemory(adev, u2h<VkDeviceMemory>(amem), m.off, m.len, 0, (void**)&a) < 0) {
        a = nullptr;
        logf("mirror: cannot map app memory for a shadow push (%llu MB) — data lost", (unsigned long long)(m.len >> 20));
    }
    if (b_tmp && md->b.MapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem), m.off, m.len, 0, (void**)&b) < 0) {
        b = nullptr;
        mirror_fail(md, "vkMapMemory failed on B");
    }
    if (watched) {
        for (ULONG_PTR i = 0; i < dirtyCount; ++i) {
            const auto offset = size_t((char*)m.dirtyPages[i] - m.shadow);
            if (offset < m.len) word_diff_copy(src+offset, m.last+offset, std::min<size_t>(granularity, size_t(m.len)-offset),
                a ? a+offset : nullptr, b ? b+offset : nullptr);
        }
    } else word_diff_copy(src + first, m.last + first, m.len - first, a ? a + first : nullptr, b ? b + first : nullptr);
    if (!m.coherent) {
        VkMappedMemoryRange ra{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(amem), m.off, VK_WHOLE_SIZE};
        VkMappedMemoryRange rb{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, u2h<VkDeviceMemory>(m.bmem), m.off, VK_WHOLE_SIZE};
        if (a) md->a.FlushMappedMemoryRanges(adev, 1, &ra);
        if (b) md->b.FlushMappedMemoryRanges(md->bdev, 1, &rb);
    }
    if (a_tmp && a) md->a.UnmapMemory(adev, u2h<VkDeviceMemory>(amem));
    if (b_tmp && b) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
}

static void push_memory(MDev* md) {
    uint64_t t0 = now_ns();
    std::lock_guard<std::mutex> l(md->lock);
    static const uint64_t fail_at = strtoull(env("VRX_XG_MIRROR_FAIL_AT", "0"), nullptr, 10);  // selftest: stop on purpose
    if (fail_at && md->pushes == fail_at && md->live) mirror_fail(md, "VRX_XG_MIRROR_FAIL_AT reached");
    uint64_t bytes = 0;
    for (auto& kv : md->mems)
        if (kv.second.mapped) { push_one(md, kv.first, kv.second); bytes += kv.second.len; }
    md->pushed_bytes += bytes;
    md->push_ns += now_ns() - t0;
    if (++md->pushes % 600 == 0)
        logf("mirror: memory sync %.2f ms/submit avg over %.1f MB mapped", md->push_ns / 1e6 / 600, bytes / 1048576.0), md->push_ns = 0;
}

static void free_shadow(Mem& m) {
    if (m.shadow) VirtualFree(m.shadow, 0, MEM_RELEASE);
    if (m.last) VirtualFree(m.last, 0, MEM_RELEASE);
    m.shadow = m.last = nullptr;
    m.cap = 0;
}

static char* anon(size_t n, bool watch = false) {
    return (char*)VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0), PAGE_READWRITE);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* alloc, VkDeviceMemory* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAllocateMemory>(state.next(state.handle, "vkAllocateMemory")); return fn(dev, ai, alloc, out); }
    // Independent allocations on the same device. Typed address fields are
    // translated at command recording; app-owned flags remain untouched.
    VkResult r = md->a.AllocateMemory(dev, ai, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkMemoryAllocateInfo bai;
    tr_VkMemoryAllocateInfo(c, ai, &bai);
    VkDeviceMemory bm = VK_NULL_HANDLE;
    VkResult rb = c.ok() ? md->b.AllocateMemory(md->bdev, &bai, nullptr, &bm) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) { mirror_fail(md, "vkAllocateMemory failed on B"); return r; }
    record_handle(md, *out, bm);
    VkMemoryPropertyFlags f = md->amp.memoryTypes[ai->memoryTypeIndex].propertyFlags;
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        Mem m;
        m.bmem = h2u(bm);
        m.size = ai->allocationSize;
        m.cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || !strcmp(env("VRX_XG_SHADOW", "1"), "0");
        m.coherent = f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::lock_guard<std::mutex> l(md->lock);
        md->mems[h2u(*out)] = m;
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateBuffer(VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* alloc, VkBuffer* out) {
    if (g_trace) trace_call("vkCreateBuffer");
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateBuffer>(state.next(state.handle, "vkCreateBuffer")); return fn(dev, ci, alloc, out); }
    VkResult r = md->a.CreateBuffer(dev, ci, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md); VkBufferCreateInfo bi = *ci; bi.pNext = tr_chain(c, ci->pNext);
    VkBuffer twin{};
    const auto rb = c.ok() ? md->b.CreateBuffer(md->bdev, &bi, nullptr, &twin) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb != VK_SUCCESS) mirror_fail(md, "buffer twin allocation"); else {
        record_handle(md, *out, twin);
        std::lock_guard<std::mutex> lock(md->addressLock);
        md->bufferSizes[h2u(*out)] = ci->size; md->addresses.erase(h2u(*out));
    }
    return r;
}

// The app sees A addresses. Track the B range for typed push-pointer translation.
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL m_GetBufferDeviceAddress(VkDevice dev, const VkBufferDeviceAddressInfo* info) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(state.next(state.handle, "vkGetBufferDeviceAddress")); return fn(dev, info); }
    VkDeviceAddress a = md->a.GetBufferDeviceAddress(dev, info);
    if (a && md->live) {
        VkBufferDeviceAddressInfo bi = *info; bi.buffer = xl_handle(md, info->buffer);
        const auto b = md->a.GetBufferDeviceAddress(md->device, &bi);
        std::lock_guard<std::mutex> lock(md->addressLock);
        auto it = md->bufferSizes.find(h2u(info->buffer));
        if (!b || it == md->bufferSizes.end()) mirror_fail(md, "unknown device-address buffer");
        else md->addresses[h2u(info->buffer)] = {a,b,it->second};
    }
    return a;
}

static VKAPI_ATTR void VKAPI_CALL m_FreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks* alloc) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkFreeMemory>(state.next(state.handle, "vkFreeMemory")); return fn(dev, mem, alloc); }
    if (mem) {
        std::lock_guard<std::mutex> l(md->lock);
        auto it = md->mems.find(h2u(mem));
        if (it != md->mems.end()) {
            free_shadow(it->second);
            md->mems.erase(it);
        }
    }
    md->a.FreeMemory(dev, mem, alloc);
    if (mem && md->live) md->b.FreeMemory(md->bdev, xl_handle(md, mem), nullptr);
    forget_handle(md, mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_MapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags flags, void** pp) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkMapMemory>(state.next(state.handle, "vkMapMemory")); return fn(dev, mem, off, size, flags, pp); }
    VkResult r = md->a.MapMemory(dev, mem, off, size, flags, pp);
    if (r < 0 || !md->live) return r;
    std::lock_guard<std::mutex> l(md->lock);
    auto it = md->mems.find(h2u(mem));
    if (it == md->mems.end()) return r;
    Mem& m = it->second;
    VkDeviceSize len = size == VK_WHOLE_SIZE ? m.size - off : size;
    bool tmp = transient_maps();
    void* bp = nullptr;
    if (!tmp && md->b.MapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem), off, len, 0, &bp) < 0) { mirror_fail(md, "vkMapMemory failed on B"); return r; }
    size_t cap = (size_t(len) + 4095) & ~size_t(4095);
    if (m.cap < cap || m.cap_off != off) {
        free_shadow(m);
        m.last = anon(cap);
        m.shadow = m.cached ? nullptr : anon(cap, true);
        if (!m.cached) m.dirtyPages.resize(cap/4096+1);
        if (!m.last || (!m.cached && !m.shadow)) {
            free_shadow(m);
            if (bp) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
            mirror_fail(md, "out of memory for shadow mapping");
            return r;  // the app gets A's own pointer; other shadows keep reaching A
        }
        m.cap = cap;
        m.cap_off = off;
    }
    m.amap = (char*)*pp;
    m.a_released = tmp && !m.cached;
    if (m.a_released) {  // the app writes the shadow; A is mapped per push
        md->a.UnmapMemory(dev, mem);
        m.amap = nullptr;
    }
    m.bmap = (char*)bp;
    m.off = off;
    m.len = len;
    m.mapped = true;
    if (m.cached) { memcpy(m.last, m.amap, len); if (m.bmap) memcpy(m.bmap, m.amap, len); }
    else *pp = m.shadow;
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_UnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkUnmapMemory>(state.next(state.handle, "vkUnmapMemory")); return fn(dev, mem); }
    bool a_released = false;
    {
        std::lock_guard<std::mutex> l(md->lock);  // also after a stop: the shadow's last writes must reach A
        auto it = md->mems.find(h2u(mem));
        if (it != md->mems.end() && it->second.mapped) {
            Mem& m = it->second;
            push_one(md, it->first, m);
            if (m.bmap) md->b.UnmapMemory(md->bdev, u2h<VkDeviceMemory>(m.bmem));
            a_released = m.a_released;
            m.mapped = m.a_released = false;
            m.amap = m.bmap = nullptr;
        }
    }
    if (!a_released) md->a.UnmapMemory(dev, mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_FlushMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(state.next(state.handle, "vkFlushMappedMemoryRanges")); return fn(dev, n, r); }
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) {
            auto it = md->mems.find(h2u(r[i].memory));
            if (it != md->mems.end()) push_one(md, it->first, it->second);
        }
    }
    VkResult res = md->a.FlushMappedMemoryRanges(dev, n, r);
    if (md->live) {
        Ctx& c = ctx_begin(md);
        auto* br = c.alloc<VkMappedMemoryRange>(n);
        for (uint32_t i = 0; i < n; ++i) tr_VkMappedMemoryRange(c, &r[i], &br[i]);
        md->b.FlushMappedMemoryRanges(md->bdev, n, br);
        ctx_end(c);
    }
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_InvalidateMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    return mdev_of(key(dev))->a.InvalidateMappedMemoryRanges(dev, n, r);  // reads go to A's (cached) mapping
}

// Core in Vulkan 1.4 (DXVK uses it). Plain maps only: placed maps
// (VK_EXT_map_memory_placed) are hidden from the app.
static VKAPI_ATTR VkResult VKAPI_CALL m_MapMemory2(VkDevice dev, const VkMemoryMapInfo* info, void** pp) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkMapMemory2>(state.next(state.handle, "vkMapMemory2")); return fn(dev, info, pp); }
    if (info->pNext) {
        mirror_fail(md, "vkMapMemory2 with a pNext chain (placed map?) not supported");
        return md->a.MapMemory2(dev, info, pp);
    }
    return m_MapMemory(dev, info->memory, info->offset, info->size, info->flags, pp);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_UnmapMemory2(VkDevice dev, const VkMemoryUnmapInfo* info) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkUnmapMemory2>(state.next(state.handle, "vkUnmapMemory2")); return fn(dev, info); }
    if (info->pNext || info->flags) {
        mirror_fail(md, "vkUnmapMemory2 with flags/pNext not supported");
        return md->a.UnmapMemory2(dev, info);
    }
    m_UnmapMemory(dev, info->memory);
    return VK_SUCCESS;
}

// --------------------------------------------------------------- sync -------
static const uint64_t kBWaitNs = 5000000000ull;  // B that far behind = broken

static void pending_add(MDev* md, VkFence f) {
    if (!f) return;
    std::lock_guard<std::mutex> l(md->lock);
    md->pending.insert(h2u(f));
}

// Waits on B for those of the app's fences that have a B submit in flight.
static void b_wait_fences(MDev* md, uint32_t n, const VkFence* f) {
    std::vector<VkFence> bf;
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i)
            if (f[i] && md->pending.erase(h2u(f[i]))) bf.push_back(xl_handle(md, f[i]));
    }
    if (bf.empty() || !md->live) return;
    if (md->b.WaitForFences(md->bdev, uint32_t(bf.size()), bf.data(), VK_TRUE, kBWaitNs) != VK_SUCCESS)
        mirror_fail(md, "right GPU fell >5 s behind (fence)");
}

static VKAPI_ATTR VkResult VKAPI_CALL m_WaitForFences(VkDevice dev, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkWaitForFences>(state.next(state.handle, "vkWaitForFences")); return fn(dev, n, f, all, timeout); }
    VkResult r = md->a.WaitForFences(dev, n, f, all, timeout);
    if (r != VK_SUCCESS || !md->live) return r;
    if (all) {
        b_wait_fences(md, n, f);
    } else {
        for (uint32_t i = 0; i < n; ++i)
            if (md->a.GetFenceStatus(dev, f[i]) == VK_SUCCESS) b_wait_fences(md, 1, &f[i]);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_GetFenceStatus(VkDevice dev, VkFence f) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetFenceStatus>(state.next(state.handle, "vkGetFenceStatus")); return fn(dev, f); }
    VkResult r = md->a.GetFenceStatus(dev, f);
    if (r == VK_SUCCESS && md->live) b_wait_fences(md, 1, &f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_ResetFences(VkDevice dev, uint32_t n, const VkFence* f) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkResetFences>(state.next(state.handle, "vkResetFences")); return fn(dev, n, f); }
    if (md->live) {
        b_wait_fences(md, n, f);
        std::vector<VkFence> bf;
        for (uint32_t i = 0; i < n; ++i) bf.push_back(xl_handle(md, f[i]));
        md->b.ResetFences(md->bdev, n, bf.data());
    }
    return md->a.ResetFences(dev, n, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_WaitSemaphores(VkDevice dev, const VkSemaphoreWaitInfo* wi, uint64_t timeout) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkWaitSemaphores>(state.next(state.handle, "vkWaitSemaphores")); return fn(dev, wi, timeout); }
    VkResult r = md->a.WaitSemaphores(dev, wi, timeout);
    if (r != VK_SUCCESS || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkSemaphoreWaitInfo bwi;
    tr_VkSemaphoreWaitInfo(c, wi, &bwi);
    if (md->b.WaitSemaphores(md->bdev, &bwi, kBWaitNs) != VK_SUCCESS) mirror_fail(md, "right GPU fell >5 s behind (timeline)");
    ctx_end(c);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_GetSemaphoreCounterValue(VkDevice dev, VkSemaphore s, uint64_t* v) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(state.next(state.handle, "vkGetSemaphoreCounterValue")); return fn(dev, s, v); }
    VkResult r = md->a.GetSemaphoreCounterValue(dev, s, v);
    if (r != VK_SUCCESS || !md->live) return r;
    VkSemaphore bs = xl_handle(md, s);
    if (!md->live) return r;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &bs, v};
    if (md->b.WaitSemaphores(md->bdev, &wi, kBWaitNs) != VK_SUCCESS) mirror_fail(md, "right GPU fell >5 s behind (timeline)");
    return r;
}

// --------------------------------------------------------------- queues -----
static VKAPI_ATTR void VKAPI_CALL m_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue* q) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetDeviceQueue>(state.next(state.handle, "vkGetDeviceQueue")); return fn(dev, fam, idx, q); }
    md->a.GetDeviceQueue(dev, fam, idx, q);
    VkQueue bq = VK_NULL_HANDLE;
    md->b.GetDeviceQueue(md->bdev, fam, idx, &bq);
    record_handle(md, *q, bq);
    if (!md->bqueue_any) md->bqueue_any = bq;
}

static VKAPI_ATTR void VKAPI_CALL m_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* q) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetDeviceQueue2>(state.next(state.handle, "vkGetDeviceQueue2")); return fn(dev, info, q); }
    md->a.GetDeviceQueue2(dev, info, q);
    VkDeviceQueueInfo2 bi = *info;
    bi.pNext = nullptr;
    VkQueue bq = VK_NULL_HANDLE;
    md->b.GetDeviceQueue2(md->bdev, &bi, &bq);
    record_handle(md, *q, bq);
    if (!md->bqueue_any) md->bqueue_any = bq;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(q)); auto fn = reinterpret_cast<PFN_vkQueueSubmit>(state.next(state.handle, "vkQueueSubmit")); return fn(q, n, si, f); }
    push_memory(md);  // after a stop too: shadows must keep reaching A
    VkResult r = md->a.QueueSubmit(q, n, si, f);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bsi = c.alloc<VkSubmitInfo>(n);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo(c, &si[i], &bsi[i]);
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        rb = md->b.QueueSubmit(c.xl(q), n, bsi, c.xl(f));
    }
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkQueueSubmit failed on B");
    else { pending_add(md, f); ++md->submits; }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(q)); auto fn = reinterpret_cast<PFN_vkQueueSubmit2>(state.next(state.handle, "vkQueueSubmit2")); return fn(q, n, si, f); }
    push_memory(md);  // after a stop too: shadows must keep reaching A
    VkResult r = md->a.QueueSubmit2(q, n, si, f);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bsi = c.alloc<VkSubmitInfo2>(n);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo2(c, &si[i], &bsi[i]);
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        rb = md->b.QueueSubmit2(c.xl(q), n, bsi, c.xl(f));
    }
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkQueueSubmit2 failed on B");
    else { pending_add(md, f); ++md->submits; }
    return r;
}

// ------------------------------------------------------ command buffers -----
static VKAPI_ATTR VkResult VKAPI_CALL m_AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* cbs) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAllocateCommandBuffers>(state.next(state.handle, "vkAllocateCommandBuffers")); return fn(dev, ai, cbs); }
    VkResult r = md->a.AllocateCommandBuffers(dev, ai, cbs);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkCommandBufferAllocateInfo bai;
    tr_VkCommandBufferAllocateInfo(c, ai, &bai);
    auto* b = c.alloc<VkCommandBuffer>(ai->commandBufferCount);
    VkResult rb = c.ok() ? md->b.AllocateCommandBuffers(md->bdev, &bai, b) : VK_ERROR_UNKNOWN;
    if (rb < 0) {
        mirror_fail(md, "vkAllocateCommandBuffers failed on B");
    } else {
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i) {
            if (md->loader(md->device, b[i]) != VK_SUCCESS) mirror_fail(md, "twin command loader data");
            record_handle(md, cbs[i], b[i]);
        }
        if (ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
            std::lock_guard<std::mutex> l(md->lock);
            for (uint32_t i = 0; i < ai->commandBufferCount; ++i) md->secondary.insert(cbs[i]);
        }
    }
    ctx_end(c);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer* cbs) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkFreeCommandBuffers>(state.next(state.handle, "vkFreeCommandBuffers")); return fn(dev, pool, n, cbs); }
    md->a.FreeCommandBuffers(dev, pool, n, cbs);
    if (!md->live) return;
    std::vector<VkCommandBuffer> b;
    for (uint32_t i = 0; i < n; ++i)
        if (cbs[i]) b.push_back(xl_handle(md, cbs[i]));
    if (!b.empty()) md->b.FreeCommandBuffers(md->bdev, xl_handle(md, pool), uint32_t(b.size()), b.data());
    std::lock_guard<std::mutex> l(md->lock);
    for (uint32_t i = 0; i < n; ++i) md->secondary.erase(cbs[i]);
}

// pInheritanceInfo is ignored (may be garbage) for primary command buffers.
static VKAPI_ATTR VkResult VKAPI_CALL m_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    MDev* md = mdev_of(key(cb));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(cb)); auto fn = reinterpret_cast<PFN_vkBeginCommandBuffer>(state.next(state.handle, "vkBeginCommandBuffer")); return fn(cb, bi); }
    { std::lock_guard<std::mutex> lock(md->addressLock); md->bound.erase(h2u(cb)); }
    VkResult r = md->a.BeginCommandBuffer(cb, bi);
    if (r < 0 || !md->live) return r;
    bool sec;
    {
        std::lock_guard<std::mutex> l(md->lock);
        sec = md->secondary.count(cb) != 0;
    }
    Ctx& c = ctx_begin(md);
    VkCommandBufferBeginInfo b = *bi;
    b.pNext = tr_chain(c, bi->pNext);
    b.pInheritanceInfo = nullptr;
    if (sec && bi->pInheritanceInfo) {
        auto* t = c.alloc<VkCommandBufferInheritanceInfo>(1);
        tr_VkCommandBufferInheritanceInfo(c, bi->pInheritanceInfo, t);
        b.pInheritanceInfo = t;
    }
    VkResult rb = c.ok() ? md->b.BeginCommandBuffer(c.xl(cb), &b) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkBeginCommandBuffer failed on B");
    return r;
}

// ------------------------------------------------- descriptor templates -----
static size_t desc_elem_size(VkDescriptorType t) {
    if (is_image_desc(t)) return sizeof(VkDescriptorImageInfo);
    if (is_buffer_desc(t)) return sizeof(VkDescriptorBufferInfo);
    if (is_texel_desc(t)) return sizeof(VkBufferView);
    if (t == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) return sizeof(VkAccelerationStructureKHR);
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateDescriptorUpdateTemplate(VkDevice dev, const VkDescriptorUpdateTemplateCreateInfo* ci,
                                                                     const VkAllocationCallbacks* alloc, VkDescriptorUpdateTemplate* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateDescriptorUpdateTemplate>(state.next(state.handle, "vkCreateDescriptorUpdateTemplate")); return fn(dev, ci, alloc, out); }
    VkResult r = md->a.CreateDescriptorUpdateTemplate(dev, ci, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkDescriptorUpdateTemplateCreateInfo bci;
    tr_VkDescriptorUpdateTemplateCreateInfo(c, ci, &bci);
    VkDescriptorUpdateTemplate bt = VK_NULL_HANDLE;
    VkResult rb = c.ok() ? md->b.CreateDescriptorUpdateTemplate(md->bdev, &bci, nullptr, &bt) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) { mirror_fail(md, "vkCreateDescriptorUpdateTemplate failed on B"); return r; }
    record_handle(md, *out, bt);
    Template t;
    t.entries.assign(ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount);
    for (auto& e : t.entries) {
        if (!e.descriptorCount) continue;
        size_t end = e.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK
                         ? e.offset + e.descriptorCount
                         : e.offset + (e.descriptorCount - 1) * e.stride + desc_elem_size(e.descriptorType);
        t.size = std::max(t.size, end);
    }
    std::lock_guard<std::mutex> l(md->lock);
    md->templates[h2u(*out)] = std::move(t);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL m_DestroyDescriptorUpdateTemplate(VkDevice dev, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks* alloc) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkDestroyDescriptorUpdateTemplate>(state.next(state.handle, "vkDestroyDescriptorUpdateTemplate")); return fn(dev, t, alloc); }
    md->a.DestroyDescriptorUpdateTemplate(dev, t, alloc);
    if (md->live && t) md->b.DestroyDescriptorUpdateTemplate(md->bdev, xl_handle(md, t), nullptr);
    std::lock_guard<std::mutex> l(md->lock);
    md->templates.erase(h2u(t));
    forget_handle(md, t);
}

// Copy of a template update's raw data with every handle in it translated.
static const void* tr_template_data(Ctx& c, VkDescriptorUpdateTemplate tmpl, const void* data) {
    Template t;
    {
        std::lock_guard<std::mutex> l(c.md->lock);
        auto it = c.md->templates.find(h2u(tmpl));
        if (it == c.md->templates.end()) { c.fail("unknown descriptor update template"); return data; }
        t = it->second;
    }
    char* blob = c.alloc<char>(t.size);
    memcpy(blob, data, t.size);
    for (auto& e : t.entries) {
        for (uint32_t i = 0; i < e.descriptorCount && e.descriptorType != VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK; ++i) {
            char* p = blob + e.offset + i * e.stride;
            if (is_image_desc(e.descriptorType)) {
                VkDescriptorImageInfo ii;
                memcpy(&ii, p, sizeof ii);
                ii = xl_image_info(c, e.descriptorType, ii);
                memcpy(p, &ii, sizeof ii);
            } else if (is_buffer_desc(e.descriptorType)) {
                VkDescriptorBufferInfo bi;
                memcpy(&bi, p, sizeof bi);
                bi.buffer = c.xl(bi.buffer);
                memcpy(p, &bi, sizeof bi);
            } else if (is_texel_desc(e.descriptorType)) {
                VkBufferView v;
                memcpy(&v, p, sizeof v);
                v = c.xl(v);
                memcpy(p, &v, sizeof v);
            } else if (e.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                VkAccelerationStructureKHR v;
                memcpy(&v, p, sizeof v);
                v = c.xl(v);
                memcpy(p, &v, sizeof v);
            }
        }
    }
    return blob;
}

static VKAPI_ATTR void VKAPI_CALL m_UpdateDescriptorSetWithTemplate(VkDevice dev, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void* data) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkUpdateDescriptorSetWithTemplate>(state.next(state.handle, "vkUpdateDescriptorSetWithTemplate")); return fn(dev, set, t, data); }
    md->a.UpdateDescriptorSetWithTemplate(dev, set, t, data);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    const void* bd = tr_template_data(c, t, data);
    if (c.ok()) md->b.UpdateDescriptorSetWithTemplate(md->bdev, c.xl(set), c.xl(t), bd);
    else mirror_fail(md, "vkUpdateDescriptorSetWithTemplate: translation failed");
    ctx_end(c);
}

static VKAPI_ATTR void VKAPI_CALL m_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate t, VkPipelineLayout layout,
                                                                    uint32_t set, const void* data) {
    MDev* md = mdev_of(key(cb));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(cb)); auto fn = reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplate>(state.next(state.handle, "vkCmdPushDescriptorSetWithTemplate")); return fn(cb, t, layout, set, data); }
    md->a.CmdPushDescriptorSetWithTemplate(cb, t, layout, set, data);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    const void* bd = tr_template_data(c, t, data);
    if (c.ok()) md->b.CmdPushDescriptorSetWithTemplate(c.xl(cb), c.xl(t), c.xl(layout), set, bd);
    else mirror_fail(md, "vkCmdPushDescriptorSetWithTemplate: translation failed");
    ctx_end(c);
}

static VKAPI_ATTR void VKAPI_CALL m_CmdPushDescriptorSetWithTemplate2(VkCommandBuffer cb, const VkPushDescriptorSetWithTemplateInfo* info) {
    MDev* md = mdev_of(key(cb));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(cb)); auto fn = reinterpret_cast<PFN_vkCmdPushDescriptorSetWithTemplate2>(state.next(state.handle, "vkCmdPushDescriptorSetWithTemplate2")); return fn(cb, info); }
    md->a.CmdPushDescriptorSetWithTemplate2(cb, info);
    if (!md->live) return;
    Ctx& c = ctx_begin(md);
    VkPushDescriptorSetWithTemplateInfo b = *info;
    b.pNext = tr_chain(c, info->pNext);
    b.descriptorUpdateTemplate = c.xl(info->descriptorUpdateTemplate);
    b.layout = c.xl(info->layout);
    b.pData = tr_template_data(c, info->descriptorUpdateTemplate, info->pData);
    if (c.ok()) md->b.CmdPushDescriptorSetWithTemplate2(c.xl(cb), &b);
    else mirror_fail(md, "vkCmdPushDescriptorSetWithTemplate2: translation failed");
    ctx_end(c);
}

// --------------------------------------------------------------- shaders ----
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateShaderModule>(state.next(state.handle, "vkCreateShaderModule")); return fn(dev, ci, alloc, out); }
    const auto reflection = xg::ReflectPushPointers(ci->pCode, ci->codeSize/4);
    if (!reflection.supported) {
        uint32_t conversions = 0;
        for (size_t i = 5; i < ci->codeSize/4;) {
            const auto n = ci->pCode[i] >> 16;
            if (!n || n > ci->codeSize/4-i) break;
            if ((ci->pCode[i] & 65535) == spv::OpConvertUToPtr) ++conversions;
            i += n;
        }
        logf("mirror rejected shader: device=0x%llx bytes=%llu integerToPointer=%u", (unsigned long long)h2u(dev), (unsigned long long)ci->codeSize, conversions);
        mirror_fail(md, "unsupported physical-pointer shader");
    }
    VkResult r = create_patched_module(md->aep, md->a.CreateShaderModule, dev, ci, alloc, out);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    VkShaderModuleCreateInfo bci = *ci;
    bci.pNext = tr_chain(c, ci->pNext);
    VkShaderModule bm = VK_NULL_HANDLE;
    VkResult rb = create_patched_module(md->bep, md->b.CreateShaderModule, md->bdev, &bci, nullptr, &bm);
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkCreateShaderModule failed on B");
    else { record_handle(md, *out, bm); std::lock_guard<std::mutex> l(md->addressLock); md->shaderPointers[h2u(*out)] = reflection; }
    return r;
}

static bool reflect_stage(MDev* md, const VkPipelineShaderStageCreateInfo& stage, MDev::Pipeline& pipeline) {
    xg::PushPointers reflection;
    if (stage.module) {
        auto it = md->shaderPointers.find(h2u(stage.module)); if (it == md->shaderPointers.end()) {logf("missing shader reflection: module=0x%llx stage=%u",(unsigned long long)h2u(stage.module),stage.stage);return false;}
        reflection = it->second;
    } else {
        const VkShaderModuleCreateInfo* module = nullptr;
        for (auto* n = (const VkBaseInStructure*)stage.pNext; n; n=n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) module = (const VkShaderModuleCreateInfo*)n;
        if (!module) {logf("pipeline stage has no shader bytecode: stage=%u pNext=%d",stage.stage,stage.pNext?int(((const VkBaseInStructure*)stage.pNext)->sType):0);return false;}
        reflection = xg::ReflectPushPointers(module->pCode,module->codeSize/4);
    }
    if (!reflection.supported) return false;
    for (auto offset : reflection.offsets) pipeline.fields.push_back({VkShaderStageFlags(stage.stage),offset});
    return true;
}
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* cis,
                                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(state.next(state.handle, "vkCreateGraphicsPipelines")); return fn(dev, cache, n, cis, alloc, out); }
    // Inline modules need the left-eye patch on A as well (B's: in the pNext translation).
    InlinePatch ip;
    const VkGraphicsPipelineCreateInfo* acis = patch_inline(md->aep, n, cis, ip);
    VkResult r = md->a.CreateGraphicsPipelines(dev, cache, n, acis, alloc, out);
    if (g_trace) {
        trace_call("vkCreateGraphicsPipelines (manual)");
        for (uint32_t i = 0; i < n; ++i)
            logf("  r=%d inline=%d flags=0x%llx stages=%u pipe=0x%llx", r, acis != cis, (unsigned long long)cis[i].flags, cis[i].stageCount,
                 (unsigned long long)h2u(out[i]));
    }
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    auto* bcis = c.alloc<VkGraphicsPipelineCreateInfo>(n);
    for (uint32_t i = 0; i < n; ++i) tr_gpci(c, &cis[i], &bcis[i]);
    auto* bp = c.alloc<VkPipeline>(n);
    VkResult rb = c.ok() ? md->b.CreateGraphicsPipelines(md->bdev, c.xl(cache), n, bcis, nullptr, bp) : VK_ERROR_UNKNOWN;
    if (g_trace)
        for (uint32_t i = 0; i < n; ++i) logf("  B rb=%d ok=%d pipe=0x%llx layout=0x%llx", rb, c.ok(), (unsigned long long)h2u(bp[i]), (unsigned long long)h2u(bcis[i].layout));
    if (rb < 0) mirror_fail(md, "vkCreateGraphicsPipelines failed on B");
    else
        for (uint32_t i = 0; i < n; ++i) {
            if (out[i] && !bp[i]) { mirror_fail(md, "vkCreateGraphicsPipelines: no pipeline on B"); break; }
            record_handle(md, out[i], bp[i]);
            std::lock_guard<std::mutex> l(md->addressLock);
            MDev::Pipeline pipeline; pipeline.layout = cis[i].layout;
            for (uint32_t j = 0; j < cis[i].stageCount; ++j)
                if (!reflect_stage(md, cis[i].pStages[j], pipeline)) mirror_fail(md, "untranslatable graphics push pointers");
            md->pipelines[h2u(out[i])] = std::move(pipeline);
        }
    ctx_end(c);
    return r;
}

// ------------------------------------------------------------------ WSI -----
// Signals B's twins of what an acquire on A signalled.
static void b_signal(MDev* md, VkSemaphore s, VkFence f) {
    if (!s && !f) return;
    VkSemaphore bs = xl_handle(md, s);
    VkFence bf = xl_handle(md, f);
    if (!md->live) return;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.signalSemaphoreCount = bs ? 1 : 0;
    si.pSignalSemaphores = &bs;
    if (!md->bqueue_any) { mirror_fail(md, "acquire before any queue was fetched"); return; }
    VkResult r;
    {
        std::lock_guard<std::mutex> l(md->qlock);
        r = md->b.QueueSubmit(md->bqueue_any, 1, &si, bf);
    }
    if (r < 0) mirror_fail(md, "acquire signal failed on B");
    else pending_add(md, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore s, VkFence f, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAcquireNextImageKHR>(state.next(state.handle, "vkAcquireNextImageKHR")); return fn(dev, sc, timeout, s, f, idx); }
    VkResult r = md->a.AcquireNextImageKHR(dev, sc, timeout, s, f, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live) b_signal(md, s, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(state.next(state.handle, "vkAcquireNextImage2KHR")); return fn(dev, info, idx); }
    VkResult r = md->a.AcquireNextImage2KHR(dev, info, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live) b_signal(md, info->semaphore, info->fence);
    return r;
}



static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t mask, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}
static bool make_eye(MDev* md, EyeBuf& eye, float shift, float convergence) {
    eye.patches = &md->patches;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = 8;
    ci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (md->a.CreateBuffer(md->device, &ci, nullptr, &eye.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements needs{}; md->a.GetBufferMemoryRequirements(md->device, eye.buffer, &needs);
    const auto type = memory_type(md->amp, needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.pNext = &flags; ai.allocationSize = needs.size; ai.memoryTypeIndex = type;
    if (md->a.AllocateMemory(md->device, &ai, nullptr, &eye.memory) != VK_SUCCESS ||
        md->a.BindBufferMemory(md->device, eye.buffer, eye.memory, 0) != VK_SUCCESS) return false;
    void* mapped{};
    if (md->a.MapMemory(md->device, eye.memory, 0, 8, 0, &mapped) != VK_SUCCESS) return false;
    const float values[2] = {shift, convergence}; memcpy(mapped, values, 8); md->a.UnmapMemory(md->device, eye.memory);
    VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; address.buffer = eye.buffer;
    eye.address = md->a.GetBufferDeviceAddress(md->device, &address); return eye.address != 0;
}
static void destroy_swap(MDev* md) {
    if (!md->swap.handle) return;
    md->a.DeviceWaitIdle(md->device);
    md->gpu.reset();
    auto& s = md->swap;
    for (auto image : s.images) forget_handle(md, image);
    for (auto image : s.twins) md->a.DestroyImage(md->device, image, nullptr);
    for (auto memory : s.memory) md->a.FreeMemory(md->device, memory, nullptr);
    for (auto ready : s.ready) md->a.DestroySemaphore(md->device, ready, nullptr);
    s = {};
}
static void shutdown(MDev* md) {
    md->a.DeviceWaitIdle(md->device);
    destroy_swap(md);
    for (auto& kv : md->mems) free_shadow(kv.second);
    for (auto* eye : {&md->aep, &md->bep}) {
        if (eye->buffer) md->a.DestroyBuffer(md->device, eye->buffer, nullptr);
        if (eye->memory) md->a.FreeMemory(md->device, eye->memory, nullptr);
        *eye = {};
    }
    logf("mirror closed: %llu replayed submissions, %llu shader patches, %llu frames; live=%d",
        (unsigned long long)md->submits.load(), (unsigned long long)md->patches.load(),
        (unsigned long long)md->frame, int(md->live.load()));
}
static std::shared_ptr<MDev> create(VkDevice device, PFN_vkGetDeviceProcAddr next, PFN_vkSetDeviceLoaderData loader,
    const VkPhysicalDeviceMemoryProperties& memory, uint32_t family, LUID adapter, float shift, const wchar_t* session) {
    auto md = std::make_shared<MDev>();
    md->device = md->bdev = device; md->next = next; md->loader = loader;
    md->amp = md->bmp = memory; md->family = family; md->adapter = adapter; md->session = session;
    load_dispatch(md->a, next, device); md->b = md->a;
    float convergence = 0;
    // Read initial tuning atomically before any app shaders exist. Runtime tuning
    // requires ordered GPU updates and is deliberately separate from CPU mappings.
    wchar_t path[32768]{};
    if (GetEnvironmentVariableW(L"VRX_XG_TUNING", path, 32768)) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[128]{}; DWORD n = 0;
            if (ReadFile(f, buf, 127, &n, nullptr)) { float a, b; if (xg::ParseTuning(std::string(buf, n), a, b)) { shift = a; convergence = b; } }
            CloseHandle(f);
        }
    }
    if (!make_eye(md.get(), md->aep, shift, convergence) || !make_eye(md.get(), md->bep, -shift, -convergence)) {
        shutdown(md.get()); return {};
    }
    logf("same-frame mirror enabled: one device, isolated eye resources and command buffers; separation %.6f convergence %.6f", shift, convergence);
    return md;
}
static VkResult create_swap(MDev* md, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
    logf("mirror swapchain request: device=0x%llx live=%d size=%ux%u format=%d", (unsigned long long)h2u(md->device), int(md->live.load()), ci->imageExtent.width, ci->imageExtent.height, int(ci->imageFormat));
    auto changed = *ci; changed.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkResult result = md->a.CreateSwapchainKHR(md->device, &changed, allocator, out);
    if (result != VK_SUCCESS || !md->live) return result;
    if (md->swap.handle) { destroy_swap(md); mirror_fail(md, "swapchain replaced: restart mirror session"); return result; }
    auto& s = md->swap; s.handle = *out; s.extent = ci->imageExtent;
    uint32_t n = 0; md->a.GetSwapchainImagesKHR(md->device, *out, &n, nullptr); s.images.resize(n);
    if (!n || md->a.GetSwapchainImagesKHR(md->device, *out, &n, s.images.data()) != VK_SUCCESS) {
        mirror_fail(md, "swapchain image enumeration"); return result;
    }
    const bool normalized=ci->imageFormat==VK_FORMAT_B8G8R8A8_UNORM || ci->imageFormat==VK_FORMAT_R8G8B8A8_UNORM ||
        ci->imageFormat==VK_FORMAT_B8G8R8A8_SRGB || ci->imageFormat==VK_FORMAT_R8G8B8A8_SRGB ||
        ci->imageFormat==VK_FORMAT_A2B10G10R10_UNORM_PACK32 || ci->imageFormat==VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    const auto deviceState=FindDevice(md->device);
    VkFormatProperties sourceFormat{},destinationFormat{};
    deviceState.formatProperties(deviceState.physical,ci->imageFormat,&sourceFormat);
    deviceState.formatProperties(deviceState.physical,VK_FORMAT_R8G8B8A8_UNORM,&destinationFormat);
    if (ci->imageArrayLayers != 1 || !xg::GpuShape(ci->imageExtent.width, ci->imageExtent.height) || !normalized ||
        !(sourceFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(destinationFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        mirror_fail(md, "unsupported swapchain format or size"); return result;
    }
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    for (auto* node = (const VkBaseInStructure*)ci->pNext; node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) { fl = *(const VkImageFormatListCreateInfo*)node; fl.pNext = nullptr; }
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D; image.format = ci->imageFormat; image.extent = {s.extent.width, s.extent.height, 1};
    image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = changed.imageUsage; image.sharingMode = ci->imageSharingMode;
    image.queueFamilyIndexCount = ci->queueFamilyIndexCount; image.pQueueFamilyIndices = ci->pQueueFamilyIndices;
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; if (fl.viewFormatCount) image.pNext = &fl;
    }
    for (auto original : s.images) {
        VkImage twin{}; VkDeviceMemory mem{}; VkSemaphore ready{};
        if (md->a.CreateImage(md->device, &image, nullptr, &twin) != VK_SUCCESS) { mirror_fail(md, "twin swapchain image"); return result; }
        s.twins.push_back(twin);
        VkMemoryRequirements needs{}; md->a.GetImageMemoryRequirements(md->device, twin, &needs);
        const auto type = memory_type(md->amp, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = needs.size; ai.memoryTypeIndex = type;
        if (type == UINT32_MAX || md->a.AllocateMemory(md->device, &ai, nullptr, &mem) != VK_SUCCESS) { mirror_fail(md, "twin swapchain memory"); return result; }
        s.memory.push_back(mem);
        if (md->a.BindImageMemory(md->device, twin, mem, 0) != VK_SUCCESS) { mirror_fail(md, "twin swapchain bind"); return result; }
        record_handle(md, original, twin);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (md->a.CreateSemaphore(md->device, &sci, nullptr, &ready) != VK_SUCCESS) { mirror_fail(md, "capture completion semaphore"); return result; }
        s.ready.push_back(ready);
    }
    md->gpu = std::make_unique<xg::GpuExporter>();
    if (!md->gpu->Init(md->device, md->next, md->loader, md->family, md->adapter, s.extent.width, s.extent.height, md->session, xg::GpuFrameKind::SameFrame)) {
        mirror_fail(md, md->gpu->Error()); return result;
    }
    logf("mirror swapchain: %ux%u per eye, %u isolated targets; GPU same-frame transport", s.extent.width, s.extent.height, n);
    return result;
}
static VkResult present(MDev* md, VkQueue queue, const VkPresentInfoKHR* pi) {
    if (!md->live || !md->gpu) return md->a.QueuePresentKHR(queue, pi);
    if (pi->swapchainCount != 1 || pi->pSwapchains[0] != md->swap.handle || pi->pImageIndices[0] >= md->swap.images.size()) {
        mirror_fail(md, "unsupported presentation chain or swapchain"); return md->a.QueuePresentKHR(queue, pi);
    }
    const uint32_t index = pi->pImageIndices[0];
    std::vector<VkSemaphore> waits;
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; ++i) { waits.push_back(pi->pWaitSemaphores[i]); waits.push_back(xl_handle(md, pi->pWaitSemaphores[i])); }
    if (!md->live) return md->a.QueuePresentKHR(queue, pi);
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    std::vector<uint64_t> values(waits.size(), 0);
    VkCommandBuffer copies[2]{};
    const bool capture = md->patches && md->gpu->Reserve();
    if (capture) {
        copies[0] = md->gpu->Record(md->swap.images[index], md->swap.extent, 0);
        copies[1] = md->gpu->Record(md->swap.twins[index], md->swap.extent, 1, VK_IMAGE_LAYOUT_GENERAL);
        if (!copies[0] || !copies[1]) { mirror_fail(md, "capture recording"); return md->a.QueuePresentKHR(queue, pi); }
        waits.push_back(md->gpu->Released()); values.push_back(md->gpu->ReleaseValue()); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    VkSemaphore signals[2] = {md->swap.ready[index], capture ? md->gpu->Ready() : VK_NULL_HANDLE};
    uint64_t signalValues[2] = {0, capture ? md->gpu->Sequence() : 0};
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = (uint32_t)values.size(); timeline.pWaitSemaphoreValues = values.data();
    timeline.signalSemaphoreValueCount = capture ? 2 : 1; timeline.pSignalSemaphoreValues = signalValues;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext = &timeline;
    submit.waitSemaphoreCount = (uint32_t)waits.size(); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = stages.data();
    submit.commandBufferCount = capture ? 2 : 0; submit.pCommandBuffers = copies;
    submit.signalSemaphoreCount = capture ? 2 : 1; submit.pSignalSemaphores = signals;
    const VkResult queued = md->a.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    if (queued != VK_SUCCESS) { mirror_fail(md, "capture submission"); return queued; }
    auto changed = *pi; changed.waitSemaphoreCount = 1; changed.pWaitSemaphores = signals;
    const auto result = md->a.QueuePresentKHR(queue, &changed);
    ++md->frame;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) mirror_fail(md, "presentation failed");
    else if (capture) {
        const auto published = md->gpu->TryPublish(md->frame, md->frame);
        if (published == xg::GpuPublish::Invalid) mirror_fail(md, "invalid pair publication");
        else if (published == xg::GpuPublish::Busy) {
            // GPU copies are already queued. The reservation remains protected
            // by its ready fence until complete, but no metadata is exposed.
            // Transient consumer contention drops this pair, not the stream.
            if (++md->publicationDrops <= 3 || md->publicationDrops % 300 == 0)
                logf("mirror: dropped pair %llu (metadata busy); stream continues", (unsigned long long)md->frame);
        } else if (md->frame <= 3 || md->frame % 300 == 0)
            logf("mirror pair %llu: same game frame, %llu replayed submits, %llu shader patches", (unsigned long long)md->frame,
                (unsigned long long)md->submits.load(), (unsigned long long)md->patches.load());
        md->gpu->Drop();
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
    const VkComputePipelineCreateInfo* cis, const VkAllocationCallbacks* allocator, VkPipeline* out) {
    auto* md = mdev_of(key(dev));
    if (!md) { const auto state = FindDevice(dev); return ((PFN_vkCreateComputePipelines)state.next(dev,"vkCreateComputePipelines"))(dev,cache,n,cis,allocator,out); }
    const auto result = md->a.CreateComputePipelines(dev,cache,n,cis,allocator,out);
    if (result < 0 || !md->live) return result;
    auto& c = ctx_begin(md); auto* infos = c.alloc<VkComputePipelineCreateInfo>(n); auto* twins = c.alloc<VkPipeline>(n);
    for (uint32_t i=0;i<n;++i) tr_VkComputePipelineCreateInfo(c,&cis[i],&infos[i]);
    const auto replay = c.ok() ? md->a.CreateComputePipelines(dev,c.xl(cache),n,infos,nullptr,twins) : VK_ERROR_UNKNOWN;
    if (replay != VK_SUCCESS) mirror_fail(md,"compute pipeline replay");
    else for (uint32_t i=0;i<n;++i) {
        record_handle(md,out[i],twins[i]);
        std::lock_guard<std::mutex> l(md->addressLock);
        MDev::Pipeline pipeline; pipeline.layout=cis[i].layout;
        if (!reflect_stage(md,cis[i].stage,pipeline)) mirror_fail(md,"untranslatable compute push pointers");
        md->pipelines[h2u(out[i])] = std::move(pipeline);
    }
    ctx_end(c); return result;
}
static VKAPI_ATTR void VKAPI_CALL m_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    auto* md = mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdBindPipeline)s.next(s.handle,"vkCmdBindPipeline"))(cb,point,pipeline); }
    md->a.CmdBindPipeline(cb,point,pipeline); if (!md->live) return;
    const auto twin=xl_handle(md,pipeline); const auto twinCb=xl_handle(md,cb);
    if (!md->live) return;
    md->a.CmdBindPipeline(twinCb,point,twin);
    std::lock_guard<std::mutex> l(md->addressLock);
    if (point==VK_PIPELINE_BIND_POINT_GRAPHICS) md->bound[h2u(cb)].graphics=pipeline;
    else if (point==VK_PIPELINE_BIND_POINT_COMPUTE) md->bound[h2u(cb)].compute=pipeline;
    else mirror_fail(md,"unsupported pipeline bind point");
}
static bool push_values(MDev* md, VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void* values, std::vector<uint8_t>& translated) {
    translated.resize(size); memcpy(translated.data(),values,size);
    std::lock_guard<std::mutex> l(md->addressLock);
    const auto bound=md->bound.find(h2u(cb)); if (bound==md->bound.end()) return false;
    std::set<uint32_t> fields;
    for (const auto point : {VK_PIPELINE_BIND_POINT_GRAPHICS,VK_PIPELINE_BIND_POINT_COMPUTE}) {
        const bool selected = point == VK_PIPELINE_BIND_POINT_COMPUTE ? (stages & VK_SHADER_STAGE_COMPUTE_BIT)!=0 : (stages & VK_SHADER_STAGE_ALL_GRAPHICS)!=0;
        if (!selected) continue;
        const auto pipeline=point==VK_PIPELINE_BIND_POINT_COMPUTE ? bound->second.compute : bound->second.graphics;
        const auto it=md->pipelines.find(h2u(pipeline)); if (it==md->pipelines.end() || it->second.layout!=layout) return false;
        for (const auto& field:it->second.fields) if (field.stages & stages) fields.insert(field.offset);
    }
    for (auto field:fields) {
        if (field >= offset+size || field+8 <= offset) continue;
        if (field < offset || field+8 > offset+size) return false;
        uint64_t address=0; memcpy(&address,translated.data()+field-offset,8); if (!address) continue;
        bool found=false;
        for (const auto& item:md->addresses) {
            const auto& a=item.second;
            if (address>=a.a && address-a.a<a.size) {
                const auto twin=a.b+(address-a.a); memcpy(translated.data()+field-offset,&twin,8); found=true; ++md->translated; break;
            }
        }
        if (!found) return false;
    }
    return true;
}
static VKAPI_ATTR void VKAPI_CALL m_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void* values) {
    auto* md=mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdPushConstants)s.next(s.handle,"vkCmdPushConstants"))(cb,layout,stages,offset,size,values); }
    md->a.CmdPushConstants(cb,layout,stages,offset,size,values); if (!md->live) return;
    std::vector<uint8_t> data;
    if (!push_values(md,cb,layout,stages,offset,size,values,data)) { mirror_fail(md,"push constant address translation"); return; }
    const auto twinCb=xl_handle(md,cb); const auto twinLayout=xl_handle(md,layout);
    if (md->live) md->a.CmdPushConstants(twinCb,twinLayout,stages,offset,size,data.data());
}
static VKAPI_ATTR void VKAPI_CALL m_CmdPushConstants2(VkCommandBuffer cb, const VkPushConstantsInfo* info) {
    auto* md=mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdPushConstants2)s.next(s.handle,"vkCmdPushConstants2"))(cb,info); }
    md->a.CmdPushConstants2(cb,info); if (!md->live) return;
    std::vector<uint8_t> data;
    if (info->pNext || !push_values(md,cb,info->layout,info->stageFlags,info->offset,info->size,info->pValues,data)) {
        mirror_fail(md,"push constants2 address translation"); return;
    }
    auto changed=*info; changed.layout=xl_handle(md,info->layout); changed.pValues=data.data();
    const auto twinCb=xl_handle(md,cb); if (md->live) md->a.CmdPushConstants2(twinCb,&changed);
}
static PFN_vkVoidFunction lookup(const char* name) {
    if (const char* m = manual_canonical(name)) {
#define M(f) if (!strcmp(m, "vk" #f)) return (PFN_vkVoidFunction)m_##f
        M(AllocateMemory); M(FreeMemory); M(MapMemory); M(UnmapMemory); M(MapMemory2); M(UnmapMemory2);
        M(FlushMappedMemoryRanges); M(InvalidateMappedMemoryRanges); M(WaitForFences); M(GetFenceStatus);
        M(ResetFences); M(WaitSemaphores); M(GetSemaphoreCounterValue); M(GetDeviceQueue); M(GetDeviceQueue2);
        M(QueueSubmit); M(QueueSubmit2); M(AllocateCommandBuffers); M(FreeCommandBuffers); M(BeginCommandBuffer);
        M(CreateDescriptorUpdateTemplate); M(DestroyDescriptorUpdateTemplate); M(UpdateDescriptorSetWithTemplate);
        M(CmdPushDescriptorSetWithTemplate); M(CmdPushDescriptorSetWithTemplate2); M(CreateShaderModule);
        M(CmdPushConstants); M(CmdPushConstants2); M(CmdBindPipeline); M(CreateComputePipelines);
        M(CreateGraphicsPipelines); M(AcquireNextImageKHR); M(AcquireNextImage2KHR); M(CreateBuffer); M(GetBufferDeviceAddress);
#undef M
        return nullptr;
    }
    return gen_lookup(name);
}
} // namespace mirror
