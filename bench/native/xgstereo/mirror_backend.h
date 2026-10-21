// App-unaware resource and command mirroring, adapted from vrdual.
// See MIRROR_ORIGIN.md for provenance and differences.
#pragma once
#include <shared_mutex>
#include <unordered_set>
#include <deque>
#include <chrono>
#include "push_pointers.h"
#include "mirror_addresses.h"
#include "gpu_export.h"
#include <cstdarg>
// Diagnostic switches (VRX_XG_MIRROR_TRACE, VRX_XG_MIRROR_FAIL_AT,
// VRX_XG_SHADOW, VRX_XG_TRANSIENT_MAPS) exist only in Debug builds or with the
// CMake option VRX_XG_DIAGNOSTICS; a release layer ignores those variables.
#ifndef VRX_XG_DIAGNOSTICS
#define VRX_XG_DIAGNOSTICS 0
#endif
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
// Patches one module for an eye. `stages` limits the execution models that
// may be rewritten (xg::PatchStage bits); `stage` receives the rewritten one.
static bool spv_patch_for(EyeBuf& ep, const VkShaderModuleCreateInfo* in, std::vector<uint32_t>& code,
    uint32_t stages = xg::PatchStageVertex, uint32_t* stage = nullptr) {
    if (!in || !in->pCode || in->codeSize % 4) return false;
    code.assign(in->pCode, in->pCode + in->codeSize / 4);
    const auto result = xg::PatchClipX(code, 0, ep.address, true, stages);
    if (result.changed && ep.patches) ++*ep.patches;
    if (result.changed && stage) *stage = result.stage;
    return result.changed;
}
static VkResult create_patched_module(EyeBuf& ep, PFN_vkCreateShaderModule fn, VkDevice dev, const VkShaderModuleCreateInfo* ci,
    const VkAllocationCallbacks* alloc, VkShaderModule* out, uint32_t stages = xg::PatchStageVertex, uint32_t* stage = nullptr) {
    std::vector<uint32_t> code; auto changed = *ci;
    if (spv_patch_for(ep, ci, code, stages, stage)) { changed.pCode = code.data(); changed.codeSize = code.size() * 4; }
    return fn(dev, &changed, alloc, out);
}
// Inline module of a pipeline stage (VkShaderModuleCreateInfo in pNext), if any.
// Only a module at the head of the stage chain is patched (as before).
static const VkShaderModuleCreateInfo* inline_module(const VkPipelineShaderStageCreateInfo& stage) {
    const auto* sm = (const VkShaderModuleCreateInfo*)stage.pNext;
    return !stage.module && sm && sm->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO ? sm : nullptr;
}
static uint32_t patch_stage_bit(VkShaderStageFlagBits stage) {
    if (stage == VK_SHADER_STAGE_VERTEX_BIT) return xg::PatchStageVertex;
    if (stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) return xg::PatchStageTessellationEvaluation;
    if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) return xg::PatchStageGeometry;
    return 0;
}
// Which stage of a graphics pipeline carries the eye shift. Only the LAST
// pre-rasterisation stage is shifted; earlier patched modules are replaced by
// unpatched variants for this pipeline so Position is never shifted twice.
struct StagePlan {
    int patchInline = -1;           // inline stage (index) to patch, or -1
    uint32_t inlineStage = 0;       // its xg::PatchStage bit
    std::vector<uint32_t> unpatched;// module stages that must use the original code
    bool flat = false;              // no shifted stage: renders identically in both eyes
    const char* note = nullptr;     // reason, logged (rate limited) by the caller
};
struct InlinePatch {
    std::vector<VkGraphicsPipelineCreateInfo> cis;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages;
    std::deque<std::vector<uint32_t>> codes;
    std::deque<VkShaderModuleCreateInfo> smcis;
    void Own(uint32_t n, const VkGraphicsPipelineCreateInfo* in, uint32_t i) {
        if (cis.empty()) { cis.assign(in, in + n); stages.resize(n); }
        if (stages[i].empty()) { stages[i].assign(in[i].pStages, in[i].pStages + in[i].stageCount); cis[i].pStages = stages[i].data(); }
    }
};

// Left-eye create infos: patch the planned inline stage with the left eye's
// buffer and substitute unpatched module variants. App structures stay intact.
static const VkGraphicsPipelineCreateInfo* patch_inline(EyeBuf& ep, uint32_t n, const VkGraphicsPipelineCreateInfo* cis, InlinePatch& p,
    const std::vector<StagePlan>& plans, const std::unordered_map<uint64_t, VkShaderModule>& variants) {
    for (uint32_t i = 0; i < n && i < plans.size(); ++i) {
        const auto& plan = plans[i];
        for (const uint32_t s : plan.unpatched) {
            const auto variant = variants.find((uint64_t)cis[i].pStages[s].module);
            if (variant == variants.end()) continue;
            p.Own(n, cis, i); p.stages[i][s].module = variant->second;
        }
        if (plan.patchInline < 0) continue;
        const auto* sm = inline_module(cis[i].pStages[plan.patchInline]);
        std::vector<uint32_t> code;
        if (!spv_patch_for(ep, sm, code, plan.inlineStage)) continue;
        p.Own(n, cis, i);
        p.codes.push_back(std::move(code));
        VkShaderModuleCreateInfo m = *sm;
        m.codeSize = p.codes.back().size() * 4;
        m.pCode = p.codes.back().data();
        p.smcis.push_back(m);
        p.stages[i][plan.patchInline].pNext = &p.smcis.back();
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
    std::wstring tuningPath;
    float tuningShift = 0, tuningConvergence = 0, queuedShift = 0, queuedConvergence = 0;
    uint64_t tuningProbe = 0, tuningRevision = 0;
    VkCommandPool tuningPool{};
    VkCommandBuffer tuningCommand{};
    VkFence tuningFence{};
    bool tuningPending = false;
    bool tuningStatusDirty = false;
    bool stoppedLogged = false;
    std::mutex tuningLock;               // tuning command buffer/fence: present and submit threads
    std::atomic<bool> zeroPending{false};// mirror stopped: offsets must be zeroed on the GPU
    std::atomic<bool> releasePending{false};
    std::atomic<bool> drained{false};
    bool drainOk = false;
    std::mutex drainLock;

    VkQueue bqueue_any = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, uint32_t> queueFamilies; // A queue -> family (hlock)
    std::mutex hlock;
    PFN_vkGetDeviceProcAddr next = nullptr;
    uint32_t family = 0;
    LUID adapter{};
    std::wstring session;
    std::unique_ptr<xg::GpuExporter> gpu;
    // Right-eye twins of swapchain images become free when the export copy
    // that read them completes: that copy signals `timeline` with `value`.
    struct TwinFree { VkSemaphore timeline{}; uint64_t value = 0; };
    struct Swap {
        VkSwapchainKHR handle{}; VkExtent2D extent{}; VkFormat format = VK_FORMAT_UNDEFINED;
        std::vector<VkImage> images, twins;
        std::vector<VkDeviceMemory> memory;
        std::vector<VkSemaphore> ready;
        std::vector<TwinFree> free;
    } swap;
    std::unordered_set<uint64_t> retired;      // swapchains replaced via oldSwapchain (lock)
    std::unordered_map<uint64_t, TwinFree> acquired; // acquire semaphore -> twin availability (lock)
    // One timeline per queue, signalled by every right-eye submission and
    // every layer copy on that queue (values ordered by qlock). Waiting on
    // them replaces vkDeviceWaitIdle, which raced the app's own queue use.
    std::unordered_map<uint64_t, TwinFree> timelines; // A queue -> timeline (qlock)
    uint64_t frame = 0, publicationDrops = 0, exporterRebuilds = 0;
    std::mutex bufferLock;
    std::unordered_map<uint64_t, VkDeviceSize> bufferSizes;  // bufferLock
    xg::AddressMap addresses;                                // own shared lock
    std::mutex shaderLock;
    std::unordered_map<uint64_t, xg::PushPointers> shaderPointers;  // shaderLock
    struct ModuleRecord { std::vector<uint32_t> original; uint32_t stage = 0; };
    std::unordered_map<uint64_t, ModuleRecord> modules;  // patched modules (shaderLock)
    struct Pipeline { VkPipelineLayout layout{}; std::vector<uint32_t> fields; bool flat = false; };
    std::shared_mutex pipelineLock;
    std::unordered_map<uint64_t, Pipeline> pipelines;                    // pipelineLock
    std::unordered_map<uint64_t, std::vector<xg::PushRange>> layouts;    // pipelineLock
    // Push constants are recorded per command buffer and translated when a
    // draw/dispatch uses them, against the pipeline bound at that moment.
    struct CommandState {
        VkPipeline bound[2]{};          // graphics, compute
        VkPipelineLayout layout{};      // layout of the most recent push
        std::vector<uint8_t> bytes, written;
        bool dirty = false;             // pushed since the last right-eye flush
        VkPipeline flushedFor{};        // pipeline whose fields the last flush used
        bool flushedTranslated = false; // last flush rewrote any pointer
    };
    std::shared_mutex commandLock;
    std::unordered_map<uint64_t, CommandState> commands;  // commandLock
    std::atomic<uint64_t> commandEpoch{0};                // bumped on erase
    static inline std::atomic<uint64_t> instances{0};
    const uint64_t id = ++instances;                      // never reused (thread-local caches)
    std::atomic<uint64_t> translated{0};
    std::atomic<uint64_t> stageNotes{0}, rightPipelines{0}, rightPipelineNs{0}, rightPipelineMaxNs{0};
    std::unordered_map<uint64_t, std::pair<uint64_t, VkDeviceSize>> bmemories; // A memory -> (B memory, size) (lock)
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore = nullptr;
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
static void release_after_stop(MDev* md);

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
static void publish_tuning_state(MDev* md, bool active);
// Stops stereo for the device. The left-eye shaders stay patched, so the next
// submission on the graphics queue zeroes both eye offsets on the GPU (mono
// again), and right-eye resources are released once their GPU work drained.
static void mirror_fail(MDev* md, const char* why) {
    if (!md || !md->live.exchange(false)) return;
    logf("mirror STOPPED: %s; device=0x%llx; zeroing eye offsets and releasing right-eye resources at the next submission",
        why ? why : "(no reason)", (unsigned long long)h2u(md->device));
    if (md->gpu) md->gpu->Close();
    md->zeroPending = true; md->releasePending = true;
    publish_tuning_state(md, false);
}
// Twin of a left-eye handle without stopping the mirror; the mapping is removed.
template <typename T> static T take_twin(MDev* md, T a) {
    if (!a) return T{};
    std::lock_guard<std::mutex> l(md->hlock);
    const auto it = md->handles.find(h2u(a));
    if (it == md->handles.end()) return T{};
    const auto twin = u2h<T>(it->second);
    md->handles.erase(it);
    return twin;
}
// Per-call scratch: arena for translated structs + translation status.
struct Ctx {
    MDev* md = nullptr;
    bool bad = false;
    // Inline modules seen during translation: patched only when allowed (the
    // planned last pre-rasterisation stage of a graphics pipeline).
    bool inlineAllowed = true;
    uint32_t inlineStages = xg::PatchStageVertex;
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

// VRX_XG_MIRROR_TRACE=N: log the first N mirrored calls (diagnostic builds).
#if VRX_XG_DIAGNOSTICS
static long g_trace = atol(env("VRX_XG_MIRROR_TRACE", "0"));
#else
static constexpr long g_trace = 0;
#endif
static void trace_call(const char* name) {
    static std::atomic<long> n{0};
    if (n++ < g_trace) logf("call %s", name);
}

static thread_local Ctx t_ctx;
static Ctx& ctx_begin(MDev* md) {
    t_ctx.md = md;
    t_ctx.bad = false;
    t_ctx.inlineAllowed = true;
    t_ctx.inlineStages = xg::PatchStageVertex;
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
// Hooks called by the generated wrappers (defined below).
static bool push_flush(MDev* md, VkCommandBuffer cb, VkPipelineBindPoint point);
static bool drain_after_stop(MDev* md);
static void forget_buffer_address(MDev* md, VkBuffer buffer);
static void forget_shader_module(MDev* md, VkShaderModule module);
static void forget_pipeline(MDev* md, VkPipeline pipeline);
static void forget_pipeline_layout(MDev* md, VkPipelineLayout layout);
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
    if (c.inlineAllowed && spv_patch_for(c.md->bep, in, code, c.inlineStages)) {
        uint32_t* w = c.alloc<uint32_t>(code.size());
        memcpy(w, code.data(), code.size() * 4);
        out->pCode = w;
        out->codeSize = code.size() * 4;
    }
}

static void tr_gpci(Ctx& c, const VkGraphicsPipelineCreateInfo* in, VkGraphicsPipelineCreateInfo* out, const StagePlan* plan = nullptr,
    const std::unordered_map<uint64_t, VkShaderModule>* variants = nullptr) {
    *out = *in;
    // A may have hit its pipeline cache; B has to compile instead of failing.
    out->flags &= ~(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    out->pNext = tr_chain(c, in->pNext);
    if (in->pStages && in->stageCount) {
        auto* t = c.alloc<VkPipelineShaderStageCreateInfo>(in->stageCount);
        for (uint32_t i = 0; i < in->stageCount; ++i) {
            // Right-eye inline code is patched for the planned stage only.
            c.inlineAllowed = plan && plan->patchInline == int(i);
            c.inlineStages = plan ? plan->inlineStage : xg::PatchStageVertex;
            tr_VkPipelineShaderStageCreateInfo(c, &in->pStages[i], &t[i]);
        }
        c.inlineAllowed = true; c.inlineStages = xg::PatchStageVertex;
        // Earlier pre-rasterisation modules use the shared unpatched variant.
        if (plan && variants)
            for (const uint32_t s : plan->unpatched) {
                const auto variant = variants->find((uint64_t)in->pStages[s].module);
                if (variant != variants->end()) t[s].module = variant->second;
            }
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
#if VRX_XG_DIAGNOSTICS
    static const bool t = sizeof(void*) == 4 || !strcmp(env("VRX_XG_TRANSIENT_MAPS", "0"), "1");
#else
    static const bool t = sizeof(void*) == 4;
#endif
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
#if VRX_XG_DIAGNOSTICS
    static const uint64_t fail_at = strtoull(env("VRX_XG_MIRROR_FAIL_AT", "0"), nullptr, 10);  // selftest: stop on purpose
    if (fail_at && md->pushes == fail_at && md->live) mirror_fail(md, "VRX_XG_MIRROR_FAIL_AT reached");
#endif
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
    if (rb < 0) { logf("mirror allocation failed: result=%d bytes=%llu type=%u translated=%d",int(rb),(unsigned long long)ai->allocationSize,ai->memoryTypeIndex,c.ok()?1:0); mirror_fail(md, "vkAllocateMemory failed on B"); return r; }
    record_handle(md, *out, bm);
    VkMemoryPropertyFlags f = md->amp.memoryTypes[ai->memoryTypeIndex].propertyFlags;
    std::lock_guard<std::mutex> l(md->lock);
    md->bmemories[h2u(*out)] = {h2u(bm), ai->allocationSize};  // released in bulk after a stop
    if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        Mem m;
        m.bmem = h2u(bm);
        m.size = ai->allocationSize;
#if VRX_XG_DIAGNOSTICS
        m.cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || !strcmp(env("VRX_XG_SHADOW", "1"), "0");
#else
        m.cached = (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
#endif
        m.coherent = f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
        md->addresses.Erase(h2u(*out));
        std::lock_guard<std::mutex> lock(md->bufferLock);
        md->bufferSizes[h2u(*out)] = ci->size;
    }
    return r;
}

// The app sees A addresses. Track the B range for typed push-pointer translation.
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL m_GetBufferDeviceAddress(VkDevice dev, const VkBufferDeviceAddressInfo* info) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(state.next(state.handle, "vkGetBufferDeviceAddress")); return fn(dev, info); }
    VkDeviceAddress a = md->a.GetBufferDeviceAddress(dev, info);
    if (!a || !md->live) return a;
    VkBufferDeviceAddressInfo bi = *info; bi.buffer = xl_handle(md, info->buffer);
    const auto b = bi.buffer ? md->a.GetBufferDeviceAddress(md->device, &bi) : 0;
    VkDeviceSize size = 0;
    {
        std::lock_guard<std::mutex> lock(md->bufferLock);
        const auto it = md->bufferSizes.find(h2u(info->buffer));
        if (it != md->bufferSizes.end()) size = it->second;
    }
    if (!b || !size || !md->addresses.Insert(h2u(info->buffer), a, b, size)) mirror_fail(md, "unknown device-address buffer");
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
    if (!mem) return;
    {
        std::lock_guard<std::mutex> l(md->lock);
        md->bmemories.erase(h2u(mem));
    }
    // After a stop the twin is still freed, once right-eye GPU work drained.
    const auto twin = take_twin(md, mem);
    if (twin && (md->live || drain_after_stop(md))) md->b.FreeMemory(md->bdev, twin, nullptr);
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
            // After a stop, new maps return A's pointer directly: the shadow
            // and its comparison copy are no longer needed.
            if (!md->live) { free_shadow(m); md->mems.erase(it); }
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

// Reads go to A's mapping. A non-cached (write-combined) mapping is shadowed:
// the app's pointer is the CPU shadow, which only holds what the app wrote.
// An explicit invalidate is the app's request to read GPU results, so the
// shadow (and its comparison copy) is refreshed from A for those ranges.
// Coherent memory read without an invalidate still observes only the app's
// own writes; upload heaps (D3D12 via vkd3d) and DXVK staging never read
// GPU-written non-cached memory, readbacks use cached memory types.
static VKAPI_ATTR VkResult VKAPI_CALL m_InvalidateMappedMemoryRanges(VkDevice dev, uint32_t n, const VkMappedMemoryRange* r) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkInvalidateMappedMemoryRanges>(state.next(state.handle, "vkInvalidateMappedMemoryRanges")); return fn(dev, n, r); }
    const VkResult res = md->a.InvalidateMappedMemoryRanges(dev, n, r);
    if (res != VK_SUCCESS || !r) return res;
    std::lock_guard<std::mutex> l(md->lock);
    for (uint32_t i = 0; i < n; ++i) {
        auto it = md->mems.find(h2u(r[i].memory));
        if (it == md->mems.end()) continue;
        Mem& m = it->second;
        if (!m.mapped || m.cached || !m.shadow || !m.last || r[i].offset < m.off || r[i].offset - m.off >= m.len) continue;
        push_one(md, it->first, m);  // pending app writes reach A (and B) first
        const size_t begin = size_t(r[i].offset - m.off);
        const size_t end = r[i].size == VK_WHOLE_SIZE ? size_t(m.len) : std::min<size_t>(size_t(m.len), begin + size_t(r[i].size));
        char* a = m.amap;
        const bool temporary = !a;
        if (temporary && md->a.MapMemory(dev, r[i].memory, m.off, m.len, 0, (void**)&a) < 0) {
            logf("mirror: cannot map app memory to refresh a shadow after invalidate (%llu bytes)", (unsigned long long)(end - begin));
            continue;
        }
        std::memcpy(m.shadow + begin, a + begin, end - begin);
        std::memcpy(m.last + begin, a + begin, end - begin);
        if (temporary) md->a.UnmapMemory(dev, r[i].memory);
        // The refresh is not an app write: clear its write-watch marks.
        const size_t page = begin & ~size_t(4095);
        ResetWriteWatch(m.shadow + page, ((end + 4095) & ~size_t(4095)) - page);
    }
    return res;
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
    { std::lock_guard<std::mutex> l(md->hlock); md->queueFamilies[h2u(*q)] = fam; }
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
    { std::lock_guard<std::mutex> l(md->hlock); md->queueFamilies[h2u(*q)] = info->queueFamilyIndex; }
    if (!md->bqueue_any) md->bqueue_any = bq;
}

// qlock held. The queue's layer timeline, created on first use.
static MDev::TwinFree* queue_timeline(MDev* md, VkQueue queue) {
    auto& t = md->timelines[h2u(queue)];
    if (t.timeline) return &t;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; ci.pNext = &type;
    const auto r = md->a.CreateSemaphore(md->device, &ci, nullptr, &t.timeline);
    if (r != VK_SUCCESS) { logf("mirror: layer timeline creation failed (%d) queue=0x%llx", int(r), (unsigned long long)h2u(queue)); t = {}; return nullptr; }
    logf("mirror: layer timeline created for queue 0x%llx", (unsigned long long)h2u(queue));
    return &t;
}
// Waits until every layer-tracked GPU operation submitted so far (right-eye
// work and layer copies, on every queue) has completed. Only the layer's own
// timelines are waited on: no vkDeviceWaitIdle/vkQueueWaitIdle, which would
// need the app's queue synchronisation.
static bool wait_timelines(MDev* md, const char* why) {
    std::vector<VkSemaphore> semaphores; std::vector<uint64_t> values;
    {
        std::lock_guard<std::mutex> l(md->qlock);
        for (const auto& kv : md->timelines)
            if (kv.second.timeline && kv.second.value) { semaphores.push_back(kv.second.timeline); values.push_back(kv.second.value); }
    }
    if (semaphores.empty()) return true;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = uint32_t(semaphores.size()); wi.pSemaphores = semaphores.data(); wi.pValues = values.data();
    const auto t0 = now_ns();
    const auto r = md->a.WaitSemaphores(md->device, &wi, kBWaitNs);
    logf("mirror: waited %.2f ms for %zu layer timeline(s) (%s): result %d", (now_ns() - t0) / 1e6, semaphores.size(), why, int(r));
    return r == VK_SUCCESS;
}
// Right-eye submissions never wait for swapchain acquisition: their targets
// are the offscreen twins. A wait on an acquire semaphore is replaced by a
// wait for the export copy that last read that twin; the twin semaphore of
// an acquire semaphore is never signalled. Every right-eye submission also
// signals the queue's layer timeline. qlock held; returns the signalled value.
template <typename AppWait> static bool acquired_wait(MDev* md, AppWait semaphore, MDev::TwinFree& out) {
    std::lock_guard<std::mutex> l(md->lock);
    const auto it = md->acquired.find(h2u(semaphore));
    if (it == md->acquired.end()) return false;
    out = it->second; md->acquired.erase(it);
    return true;
}
static void fix_submit(Ctx& c, MDev* md, MDev::TwinFree& timeline, uint32_t n, const VkSubmitInfo* si, VkSubmitInfo* bsi) {
    for (uint32_t i = 0; i < n; ++i) {
        const bool signal = i + 1 == n;
        bool replaced = false;
        std::vector<uint32_t> swap; std::vector<MDev::TwinFree> waits;
        for (uint32_t j = 0; j < si[i].waitSemaphoreCount && si[i].pWaitSemaphores; ++j) {
            MDev::TwinFree free;
            if (!acquired_wait(md, si[i].pWaitSemaphores[j], free)) continue;
            swap.push_back(j); waits.push_back(free); replaced = true;
        }
        if (!replaced && !signal) continue;
        // Existing timeline values (copied by tr_chain) or none.
        VkTimelineSemaphoreSubmitInfo* tl = nullptr;
        for (auto* node = (VkBaseOutStructure*)bsi[i].pNext; node; node = node->pNext)
            if (node->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) tl = (VkTimelineSemaphoreSubmitInfo*)node;
        if (!tl) { tl = c.alloc<VkTimelineSemaphoreSubmitInfo>(1); tl->sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO; tl->pNext = bsi[i].pNext; bsi[i].pNext = tl; }
        if (replaced) {
            const uint32_t count = bsi[i].waitSemaphoreCount;
            auto* sems = c.alloc<VkSemaphore>(count); auto* values = c.alloc<uint64_t>(count);
            for (uint32_t j = 0; j < count; ++j) {
                sems[j] = bsi[i].pWaitSemaphores[j];
                values[j] = tl->pWaitSemaphoreValues && j < tl->waitSemaphoreValueCount ? tl->pWaitSemaphoreValues[j] : 0;
            }
            for (size_t k = 0; k < swap.size(); ++k) {
                // A never-exported twin has no copy to wait for: value 0 is
                // satisfied immediately by the queue's own timeline.
                sems[swap[k]] = waits[k].timeline ? waits[k].timeline : timeline.timeline;
                values[swap[k]] = waits[k].timeline ? waits[k].value : 0;
            }
            bsi[i].pWaitSemaphores = sems; tl->waitSemaphoreValueCount = count; tl->pWaitSemaphoreValues = values;
        }
        if (signal) {
            const uint32_t count = bsi[i].signalSemaphoreCount;
            auto* sems = c.alloc<VkSemaphore>(count + 1); auto* values = c.alloc<uint64_t>(count + 1);
            for (uint32_t j = 0; j < count; ++j) {
                sems[j] = bsi[i].pSignalSemaphores[j];
                values[j] = tl->pSignalSemaphoreValues && j < tl->signalSemaphoreValueCount ? tl->pSignalSemaphoreValues[j] : 0;
            }
            sems[count] = timeline.timeline; values[count] = timeline.value;
            bsi[i].signalSemaphoreCount = count + 1; bsi[i].pSignalSemaphores = sems;
            tl->signalSemaphoreValueCount = count + 1; tl->pSignalSemaphoreValues = values;
        }
    }
}
static void fix_submit2(Ctx& c, MDev* md, MDev::TwinFree& timeline, uint32_t n, const VkSubmitInfo2* si, VkSubmitInfo2* bsi) {
    for (uint32_t i = 0; i < n; ++i) {
        auto* waits = c.alloc<VkSemaphoreSubmitInfo>(si[i].waitSemaphoreInfoCount);
        for (uint32_t j = 0; j < si[i].waitSemaphoreInfoCount && si[i].pWaitSemaphoreInfos; ++j) {
            waits[j] = bsi[i].pWaitSemaphoreInfos[j];
            MDev::TwinFree free;
            if (!acquired_wait(md, si[i].pWaitSemaphoreInfos[j].semaphore, free)) continue;
            waits[j].semaphore = free.timeline ? free.timeline : timeline.timeline;
            waits[j].value = free.timeline ? free.value : 0;
        }
        if (si[i].waitSemaphoreInfoCount) bsi[i].pWaitSemaphoreInfos = waits;
        if (i + 1 != n) continue;
        auto* signals = c.alloc<VkSemaphoreSubmitInfo>(bsi[i].signalSemaphoreInfoCount + 1);
        for (uint32_t j = 0; j < bsi[i].signalSemaphoreInfoCount; ++j) signals[j] = bsi[i].pSignalSemaphoreInfos[j];
        auto& own = signals[bsi[i].signalSemaphoreInfoCount];
        own.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO; own.semaphore = timeline.timeline; own.value = timeline.value;
        own.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        bsi[i].pSignalSemaphoreInfos = signals; ++bsi[i].signalSemaphoreInfoCount;
    }
}
static void zero_offsets(MDev* md, VkQueue queue);
// Work that must happen inside the app's own queue call (the app holds the
// queue's external synchronisation there) once stereo has stopped.
static void after_stop(MDev* md, VkQueue queue) {
    if (md->zeroPending) zero_offsets(md, queue);
    if (md->releasePending) release_after_stop(md);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f) {
    MDev* md = mdev_of(key(q));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(q)); auto fn = reinterpret_cast<PFN_vkQueueSubmit>(state.next(state.handle, "vkQueueSubmit")); return fn(q, n, si, f); }
    push_memory(md);  // after a stop too: shadows must keep reaching A
    if (!md->live) after_stop(md, q);
    VkResult r = md->a.QueueSubmit(q, n, si, f);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    const uint32_t bn = n ? n : 1;  // an empty submit still signals the layer timeline
    auto* bsi = c.alloc<VkSubmitInfo>(bn);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo(c, &si[i], &bsi[i]);
    if (!n) bsi[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        auto* timeline = queue_timeline(md, q);
        const auto bq = c.xl(q); const auto bf = c.xl(f);
        if (timeline && md->live && c.ok()) {
            ++timeline->value;
            if (n) fix_submit(c, md, *timeline, n, si, bsi);
            else { VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO}; fix_submit(c, md, *timeline, 1, &empty, bsi); }
            rb = md->b.QueueSubmit(bq, bn, bsi, bf);
            if (rb < 0) --timeline->value;  // never signalled: keep waits satisfiable
        }
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
    if (!md->live) after_stop(md, q);
    VkResult r = md->a.QueueSubmit2(q, n, si, f);
    if (r < 0 || !md->live) return r;
    Ctx& c = ctx_begin(md);
    const uint32_t bn = n ? n : 1;
    auto* bsi = c.alloc<VkSubmitInfo2>(bn);
    for (uint32_t i = 0; i < n; ++i) tr_VkSubmitInfo2(c, &si[i], &bsi[i]);
    if (!n) bsi[0].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    VkResult rb = VK_ERROR_UNKNOWN;
    if (c.ok()) {
        std::lock_guard<std::mutex> l(md->qlock);
        auto* timeline = queue_timeline(md, q);
        const auto bq = c.xl(q); const auto bf = c.xl(f);
        if (timeline && md->live && c.ok()) {
            ++timeline->value;
            if (n) fix_submit2(c, md, *timeline, n, si, bsi);
            else { VkSubmitInfo2 empty{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; fix_submit2(c, md, *timeline, 1, &empty, bsi); }
            rb = md->b.QueueSubmit2(bq, bn, bsi, bf);
            if (rb < 0) --timeline->value;
        }
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
    if (!cbs) return;
    {
        std::unique_lock<std::shared_mutex> l(md->commandLock);
        for (uint32_t i = 0; i < n; ++i) if (cbs[i]) md->commands.erase(h2u(cbs[i]));
        ++md->commandEpoch;
    }
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (uint32_t i = 0; i < n; ++i) md->secondary.erase(cbs[i]);
    }
    // After a stop the right-eye buffers are freed with their (twin) pool.
    std::vector<VkCommandBuffer> b;
    for (uint32_t i = 0; i < n; ++i)
        if (const auto twin = take_twin(md, cbs[i])) b.push_back(twin);
    if (!md->live || b.empty()) return;
    const auto twinPool = xl_handle(md, pool);
    if (twinPool && md->live) md->b.FreeCommandBuffers(md->bdev, twinPool, uint32_t(b.size()), b.data());
}
// Push-constant state of a command buffer (app-synchronised per buffer).
// A thread-local cache avoids the shared lock for repeated lookups of the
// buffer a thread is recording; it is invalidated whenever states are erased.
static MDev::CommandState* command_state(MDev* md, VkCommandBuffer cb, bool create) {
    struct Cache { uint64_t device = 0, cb = 0, epoch = UINT64_MAX; MDev::CommandState* state = nullptr; };
    static thread_local Cache cache;
    const uint64_t epoch = md->commandEpoch.load(std::memory_order_acquire);
    if (cache.device == md->id && cache.cb == h2u(cb) && cache.epoch == epoch && cache.state) return cache.state;
    MDev::CommandState* state = nullptr;
    {
        std::shared_lock<std::shared_mutex> l(md->commandLock);
        const auto it = md->commands.find(h2u(cb));
        if (it != md->commands.end()) state = &it->second;
    }
    if (!state && create) { std::unique_lock<std::shared_mutex> l(md->commandLock); state = &md->commands[h2u(cb)]; }
    if (state) cache = {md->id, h2u(cb), epoch, state};
    return state;
}

// pInheritanceInfo is ignored (may be garbage) for primary command buffers.
static VKAPI_ATTR VkResult VKAPI_CALL m_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo* bi) {
    MDev* md = mdev_of(key(cb));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(cb)); auto fn = reinterpret_cast<PFN_vkBeginCommandBuffer>(state.next(state.handle, "vkBeginCommandBuffer")); return fn(cb, bi); }
    if (auto* state = command_state(md, cb, false)) *state = {};  // recording restarts: no binds, no pushes
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
static void forget_shader_module(MDev* md, VkShaderModule module) {
    if (!md || !module) return;
    std::lock_guard<std::mutex> l(md->shaderLock);
    md->shaderPointers.erase(h2u(module));
    md->modules.erase(h2u(module));
}
static void forget_pipeline(MDev* md, VkPipeline pipeline) {
    if (!md || !pipeline) return;
    std::unique_lock<std::shared_mutex> l(md->pipelineLock);
    md->pipelines.erase(h2u(pipeline));
}
static void forget_pipeline_layout(MDev* md, VkPipelineLayout layout) {
    if (!md || !layout) return;
    std::unique_lock<std::shared_mutex> l(md->pipelineLock);
    md->layouts.erase(h2u(layout));
}
static void forget_buffer_address(MDev* md, VkBuffer buffer) {
    if (!md || !buffer) return;
    md->addresses.Erase(h2u(buffer));
    std::lock_guard<std::mutex> l(md->bufferLock);
    md->bufferSizes.erase(h2u(buffer));
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* alloc, VkShaderModule* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateShaderModule>(state.next(state.handle, "vkCreateShaderModule")); return fn(dev, ci, alloc, out); }
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
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
    // Vertex, tessellation-evaluation and geometry modules are all patched;
    // each pipeline then keeps only its last pre-rasterisation stage shifted.
    uint32_t stage = 0;
    VkResult r = create_patched_module(md->aep, md->a.CreateShaderModule, dev, ci, alloc, out, xg::PatchStagePreRasterization, &stage);
    if (r < 0) return r;
    if (stage && ci->pCode) {
        std::lock_guard<std::mutex> l(md->shaderLock);
        md->modules[h2u(*out)] = {std::vector<uint32_t>(ci->pCode, ci->pCode + ci->codeSize / 4), stage};
    }
    if (!md->live) return r;
    Ctx& c = ctx_begin(md);
    VkShaderModuleCreateInfo bci = *ci;
    bci.pNext = tr_chain(c, ci->pNext);
    VkShaderModule bm = VK_NULL_HANDLE;
    VkResult rb = create_patched_module(md->bep, md->b.CreateShaderModule, md->bdev, &bci, nullptr, &bm, xg::PatchStagePreRasterization);
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkCreateShaderModule failed on B");
    else { record_handle(md, *out, bm); std::lock_guard<std::mutex> l(md->shaderLock); md->shaderPointers[h2u(*out)] = reflection; }
    return r;
}

static bool reflect_stage(MDev* md, const VkPipelineShaderStageCreateInfo& stage, MDev::Pipeline& pipeline) {
    xg::PushPointers reflection;
    if (stage.module) {
        std::lock_guard<std::mutex> l(md->shaderLock);
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
    pipeline.fields.insert(pipeline.fields.end(), reflection.offsets.begin(), reflection.offsets.end());
    std::sort(pipeline.fields.begin(), pipeline.fields.end());
    pipeline.fields.erase(std::unique(pipeline.fields.begin(), pipeline.fields.end()), pipeline.fields.end());
    return true;
}

// Chooses the shifted stage of one graphics pipeline (see StagePlan).
static StagePlan plan_pipeline(MDev* md, const VkGraphicsPipelineCreateInfo& ci) {
    StagePlan plan;
    int vs = -1, tes = -1, gs = -1;
    bool mesh = false;
    for (uint32_t s = 0; s < ci.stageCount && ci.pStages; ++s) {
        const auto stage = ci.pStages[s].stage;
        if (stage == VK_SHADER_STAGE_VERTEX_BIT) vs = int(s);
        else if (stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) tes = int(s);
        else if (stage == VK_SHADER_STAGE_GEOMETRY_BIT) gs = int(s);
        else if (stage & (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT)) mesh = true;
    }
    if (mesh) { plan.flat = true; plan.note = "mesh/task shader pipeline: Position is not shifted (renders flat in both eyes)"; return plan; }
    const int last = gs >= 0 ? gs : (tes >= 0 ? tes : vs);
    if (last < 0) return plan;  // e.g. a library without pre-rasterisation stages
    // Patched module (recorded at creation) or patchable inline code.
    auto patchable = [&](int index) {
        const auto& stage = ci.pStages[index];
        const uint32_t bit = patch_stage_bit(stage.stage);
        if (stage.module) {
            std::lock_guard<std::mutex> l(md->shaderLock);
            const auto it = md->modules.find(h2u(stage.module));
            return it != md->modules.end() && it->second.stage == bit;
        }
        const auto* sm = inline_module(stage);
        if (!sm || !sm->pCode || sm->codeSize % 4) return false;
        std::vector<uint32_t> probe(sm->pCode, sm->pCode + sm->codeSize / 4);
        return xg::PatchClipX(probe, 0, md->aep.address, true, bit).changed;
    };
    int target = patchable(last) ? last : -1;
    if (target < 0 && last != vs && vs >= 0 && patchable(vs)) {
        target = vs;
        plan.note = "tessellation/geometry stage could not be patched; the vertex-stage shift is kept (correct only if Position passes through)";
    }
    if (target < 0) { plan.flat = true; plan.note = "no patchable pre-rasterisation stage: pipeline renders flat in both eyes"; return plan; }
    for (const int index : {vs, tes, gs}) {
        if (index < 0 || index == target || !ci.pStages[index].module) continue;
        std::lock_guard<std::mutex> l(md->shaderLock);
        if (md->modules.count(h2u(ci.pStages[index].module))) plan.unpatched.push_back(uint32_t(index));
    }
    if (!ci.pStages[target].module) { plan.patchInline = target; plan.inlineStage = patch_stage_bit(ci.pStages[target].stage); }
    return plan;
}
static void log_stage_note(MDev* md, const StagePlan& plan, uint32_t index) {
    if (!plan.note) return;
    const auto n = ++md->stageNotes;
    if (n <= 8 || n % 500 == 0) logf("mirror pipeline %u: %s (%llu such pipelines so far)", index, plan.note, (unsigned long long)n);
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* cis,
                                                              const VkAllocationCallbacks* alloc, VkPipeline* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(state.next(state.handle, "vkCreateGraphicsPipelines")); return fn(dev, cache, n, cis, alloc, out); }
    if (!n || !cis || !out) return md->a.CreateGraphicsPipelines(dev, cache, n, cis, alloc, out);
    // Per pipeline: which stage is shifted, and which earlier patched modules
    // need their unpatched code (created here, destroyed after creation).
    std::vector<StagePlan> plans(n);
    std::unordered_map<uint64_t, VkShaderModule> variants;
    for (uint32_t i = 0; i < n; ++i) {
        plans[i] = plan_pipeline(md, cis[i]);
        log_stage_note(md, plans[i], i);
        for (const uint32_t s : plans[i].unpatched) {
            const auto module = cis[i].pStages[s].module;
            if (variants.count(h2u(module))) continue;
            std::vector<uint32_t> original;
            { std::lock_guard<std::mutex> l(md->shaderLock); const auto it = md->modules.find(h2u(module)); if (it != md->modules.end()) original = it->second.original; }
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = original.size() * 4; mi.pCode = original.data();
            VkShaderModule variant{};
            if (original.empty() || md->a.CreateShaderModule(dev, &mi, nullptr, &variant) != VK_SUCCESS) { logf("mirror: unpatched module variant failed for stage %u", s); continue; }
            variants[h2u(module)] = variant;
        }
    }
    struct Variants { MDev* md; VkDevice dev; std::unordered_map<uint64_t, VkShaderModule>& map;
        ~Variants() { for (auto& kv : map) md->a.DestroyShaderModule(dev, kv.second, nullptr); } } cleanup{md, dev, variants};
    InlinePatch ip;
    const VkGraphicsPipelineCreateInfo* acis = patch_inline(md->aep, n, cis, ip, plans, variants);
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
    for (uint32_t i = 0; i < n; ++i) tr_gpci(c, &cis[i], &bcis[i], &plans[i], &variants);
    auto* bp = c.alloc<VkPipeline>(n);
    // Right-eye pipelines compile synchronously inside the app's call; the
    // cost is measured and summarised (never logged per pipeline).
    const auto t0 = now_ns();
    VkResult rb = c.ok() ? md->b.CreateGraphicsPipelines(md->bdev, c.xl(cache), n, bcis, nullptr, bp) : VK_ERROR_UNKNOWN;
    const auto elapsed = now_ns() - t0;
    const auto count = md->rightPipelines.fetch_add(n) + n;
    md->rightPipelineNs += elapsed;
    if (elapsed > md->rightPipelineMaxNs) md->rightPipelineMaxNs = elapsed;
    if ((elapsed > 100000000ull && count <= 64) || count / 500 != (count - n) / 500)
        logf("mirror: right-eye pipeline compile %.1f ms for %u pipeline(s); %llu total, %.1f ms avg, %.1f ms max",
            elapsed / 1e6, n, (unsigned long long)count, md->rightPipelineNs.load() / 1e6 / double(count), md->rightPipelineMaxNs.load() / 1e6);
    if (g_trace)
        for (uint32_t i = 0; i < n; ++i) logf("  B rb=%d ok=%d pipe=0x%llx layout=0x%llx", rb, c.ok(), (unsigned long long)h2u(bp[i]), (unsigned long long)h2u(bcis[i].layout));
    if (rb < 0) mirror_fail(md, "vkCreateGraphicsPipelines failed on B");
    else
        for (uint32_t i = 0; i < n; ++i) {
            // The app's pipeline is null when its creation returned
            // VK_PIPELINE_COMPILE_REQUIRED (the right eye always compiles):
            // the right-eye pipeline has no owner and is destroyed here.
            if (!out[i]) { if (bp[i]) md->b.DestroyPipeline(md->bdev, bp[i], nullptr); continue; }
            if (!bp[i]) { mirror_fail(md, "vkCreateGraphicsPipelines: no pipeline on B"); break; }
            record_handle(md, out[i], bp[i]);
            MDev::Pipeline pipeline; pipeline.layout = cis[i].layout; pipeline.flat = plans[i].flat;
            for (uint32_t j = 0; j < cis[i].stageCount; ++j)
                if (!reflect_stage(md, cis[i].pStages[j], pipeline)) mirror_fail(md, "untranslatable graphics push pointers");
            std::unique_lock<std::shared_mutex> l(md->pipelineLock);
            md->pipelines[h2u(out[i])] = std::move(pipeline);
        }
    ctx_end(c);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_CreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* alloc, VkPipelineLayout* out) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkCreatePipelineLayout>(state.next(state.handle, "vkCreatePipelineLayout")); return fn(dev, ci, alloc, out); }
    if (!ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = md->a.CreatePipelineLayout(dev, ci, alloc, out);
    if (r < 0) return r;
    // Push ranges let deferred right-eye pushes use the exact stage flags.
    std::vector<xg::PushRange> ranges;
    for (uint32_t i = 0; i < ci->pushConstantRangeCount && ci->pPushConstantRanges; ++i)
        ranges.push_back({ci->pPushConstantRanges[i].stageFlags, ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size});
    { std::unique_lock<std::shared_mutex> l(md->pipelineLock); md->layouts[h2u(*out)] = std::move(ranges); }
    if (!md->live) return r;
    Ctx& c = ctx_begin(md);
    VkPipelineLayoutCreateInfo bci;
    tr_VkPipelineLayoutCreateInfo(c, ci, &bci);
    VkPipelineLayout twin{};
    VkResult rb = c.ok() ? md->b.CreatePipelineLayout(md->bdev, &bci, nullptr, &twin) : VK_ERROR_UNKNOWN;
    ctx_end(c);
    if (rb < 0) mirror_fail(md, "vkCreatePipelineLayout failed on B");
    else record_handle(md, *out, twin);
    return r;
}

// ------------------------------------------------------------------ WSI -----
// Acquisition is left-eye only. The right eye renders into offscreen twins,
// so nothing is submitted here (an extra vkQueueSubmit from inside
// vkAcquireNextImageKHR would race the app's own use of that queue). The
// acquire semaphore is remembered with the export copy that last read the
// acquired image's twin; right-eye waits on it are translated to that copy.
static void remember_acquire(MDev* md, VkSwapchainKHR sc, VkSemaphore s, uint32_t index) {
    if (!s) return;
    std::lock_guard<std::mutex> l(md->lock);
    MDev::TwinFree free;
    if (sc == md->swap.handle && index < md->swap.free.size()) free = md->swap.free[index];
    md->acquired[h2u(s)] = free;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore s, VkFence f, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAcquireNextImageKHR>(state.next(state.handle, "vkAcquireNextImageKHR")); return fn(dev, sc, timeout, s, f, idx); }
    VkResult r = md->a.AcquireNextImageKHR(dev, sc, timeout, s, f, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live && idx) remember_acquire(md, sc, s, *idx);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL m_AcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* idx) {
    MDev* md = mdev_of(key(dev));
    if (!md) { auto state = FindDevice(reinterpret_cast<VkDevice>(dev)); auto fn = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(state.next(state.handle, "vkAcquireNextImage2KHR")); return fn(dev, info, idx); }
    VkResult r = md->a.AcquireNextImage2KHR(dev, info, idx);
    if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && md->live && info && idx) remember_acquire(md, info->swapchain, info->semaphore, *idx);
    return r;
}
// ------------------------------------------------------- tuning and eyes ----
static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties& memory, uint32_t mask, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}
static bool read_tuning(const std::wstring& path,float& shift,float& convergence) {
    if(path.empty()) return false;
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    char buffer[128]{};DWORD size=0;
    const bool read=ReadFile(f,buffer,sizeof(buffer),&size,nullptr)!=FALSE;CloseHandle(f);
    return read && size<sizeof(buffer) && xg::ParseTuning(std::string_view(buffer,size),shift,convergence);
}
static void publish_tuning_state(MDev* md,bool active) {
    if(md->tuningPath.empty()) return;
    md->tuningStatusDirty=true;
    auto number=[](float value) {char b[64];auto r=std::to_chars(b,b+sizeof(b),value);return std::string(b,r.ptr);};
    std::string json="{\"Version\":1,\"Pid\":"+std::to_string(GetCurrentProcessId())+",\"Separation\":"+number(md->tuningShift)+
        ",\"Convergence\":"+number(md->tuningConvergence)+",\"Revision\":"+std::to_string(md->tuningRevision)+",\"Active\":"+(active?"true":"false")+"}";
    const auto path=md->tuningPath+L".live.json";
    const auto temporary=path+L"."+std::to_wstring(h2u(md->device))+L".tmp";
    HANDLE f=CreateFileW(temporary.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return;
    DWORD written=0;const bool ok=WriteFile(f,json.data(),DWORD(json.size()),&written,nullptr) && written==json.size();CloseHandle(f);
    if(!ok || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temporary.c_str());
    else md->tuningStatusDirty=false;
}
// tuningLock held. Command pool/buffer/fence for queue-ordered offset writes.
static bool ensure_tuning(MDev* md) {
    if (md->tuningPool) return true;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=md->family;
    if (md->a.CreateCommandPool(md->device,&pool,nullptr,&md->tuningPool)!=VK_SUCCESS) { md->tuningPool = VK_NULL_HANDLE; return false; }
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocation.commandPool=md->tuningPool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return md->a.AllocateCommandBuffers(md->device,&allocation,&md->tuningCommand)==VK_SUCCESS &&
        (!md->loader || md->loader(md->device,md->tuningCommand)==VK_SUCCESS) &&
        md->a.CreateFence(md->device,&fence,nullptr,&md->tuningFence)==VK_SUCCESS;
}
// tuningLock held, previous write complete. Writes both eyes' {shift,
// convergence} on the GPU, ordered after earlier and before later work on
// `queue` (never a CPU write while a draw may read the values).
static const char* submit_offsets(MDev* md, VkQueue queue, const float left[2], const float right[2]) {
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(md->a.ResetCommandBuffer(md->tuningCommand,0)!=VK_SUCCESS || md->a.ResetFences(md->device,1,&md->tuningFence)!=VK_SUCCESS ||
       md->a.BeginCommandBuffer(md->tuningCommand,&begin)!=VK_SUCCESS) return "offset recording";
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    md->a.CmdPipelineBarrier(md->tuningCommand,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    md->a.CmdUpdateBuffer(md->tuningCommand,md->aep.buffer,0,8,left);md->a.CmdUpdateBuffer(md->tuningCommand,md->bep.buffer,0,8,right);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    md->a.CmdPipelineBarrier(md->tuningCommand,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    if(md->a.EndCommandBuffer(md->tuningCommand)!=VK_SUCCESS) return "offset recording end";
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&md->tuningCommand;
    if(md->a.QueueSubmit(queue,1,&submit,md->tuningFence)!=VK_SUCCESS) return "offset submission";
    md->tuningPending=true;
    return nullptr;
}
// Called only at the end of the supported single-queue presentation path.
// Queue-ordered GPU writes update BOTH eyes between frames; values apply to
// cached command buffers too. Only while stereo is live.
static void update_tuning(MDev* md,VkQueue queue) {
    if(md->tuningPath.empty() || !md->live) return;
    std::lock_guard<std::mutex> lock(md->tuningLock);
    if(md->tuningStatusDirty) publish_tuning_state(md,true);
    if(md->tuningPending) {
        const auto ready=md->a.GetFenceStatus(md->device,md->tuningFence);
        if(ready==VK_NOT_READY) return;
        if(ready!=VK_SUCCESS) {mirror_fail(md,"live tuning completion");return;}
        md->tuningPending=false;md->tuningShift=md->queuedShift;md->tuningConvergence=md->queuedConvergence;++md->tuningRevision;
        publish_tuning_state(md,true);
        logf("live stereo tuning %llu: separation %.6f convergence %.6f (GPU ordered, both eyes)",(unsigned long long)md->tuningRevision,md->tuningShift,md->tuningConvergence);
    }
    const auto now=GetTickCount64();if(now<md->tuningProbe) return;md->tuningProbe=now+100;
    float shift=0,convergence=0;
    if(!read_tuning(md->tuningPath,shift,convergence) || (shift==md->tuningShift && convergence==md->tuningConvergence)) return;
    if(!ensure_tuning(md)) {mirror_fail(md,"live tuning command allocation");return;}
    const float left[2]={shift,convergence},right[2]={-shift,-convergence};
    if(const char* failed=submit_offsets(md,queue,left,right)) {mirror_fail(md,failed);return;}
    md->queuedShift=shift;md->queuedConvergence=convergence;
}
static uint32_t queue_family(MDev* md, VkQueue queue) {
    std::lock_guard<std::mutex> l(md->hlock);
    const auto it = md->queueFamilies.find(h2u(queue));
    return it == md->queueFamilies.end() ? UINT32_MAX : it->second;
}
// After a stop the left eye's patched shaders must render unshifted: both eye
// buffers are set to {0, 0} by a GPU write ordered on the app's queue, from
// inside the app's own submit/present call. Retried until a graphics-family
// queue is used and any in-flight tuning write has completed.
static void zero_offsets(MDev* md, VkQueue queue) {
    if (!md || !queue || !md->zeroPending) return;
    if (queue_family(md, queue) != md->family) return;
    std::lock_guard<std::mutex> lock(md->tuningLock);
    if (!md->zeroPending) return;
    logf("mirror stop: zeroing eye offsets on queue 0x%llx", (unsigned long long)h2u(queue));
    if (md->tuningPending) {
        const auto ready = md->a.GetFenceStatus(md->device, md->tuningFence);
        if (ready == VK_NOT_READY) { logf("mirror stop: tuning write in flight; zeroing at a later submission"); return; }
        md->tuningPending = false;
    }
    const float zero[2] = {0, 0};
    const char* failed = ensure_tuning(md) ? submit_offsets(md, queue, zero, zero) : "offset command allocation";
    md->zeroPending = false;
    if (failed) { logf("mirror stop: eye offsets NOT zeroed (%s); the left eye keeps its shift", failed); return; }
    md->tuningShift = md->tuningConvergence = md->queuedShift = md->queuedConvergence = 0;
    logf("mirror stop: eye offsets zeroed on the GPU; the game renders unshifted (mono)");
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

// ---------------------------------------------------------- swapchain -------
// Tears down the export of the current swapchain. Instead of
// vkDeviceWaitIdle (which needs every queue's external synchronisation, held
// by the app), it waits for the layer's own timelines: they cover the export
// copies and all right-eye work that used the twin images.
static void destroy_swap(MDev* md) {
    if (!md || !md->swap.handle) return;
    logf("mirror swapchain teardown: 0x%llx %ux%u", (unsigned long long)h2u(md->swap.handle), md->swap.extent.width, md->swap.extent.height);
    if (!wait_timelines(md, "swapchain teardown")) logf("mirror swapchain teardown: GPU work did not complete in time; destroying anyway");
    md->gpu.reset();
    auto& s = md->swap;
    for (auto image : s.images) forget_handle(md, image);
    for (auto image : s.twins) md->a.DestroyImage(md->device, image, nullptr);
    for (auto memory : s.memory) md->a.FreeMemory(md->device, memory, nullptr);
    for (auto ready : s.ready) md->a.DestroySemaphore(md->device, ready, nullptr);
    { std::lock_guard<std::mutex> l(md->lock); s = {}; md->acquired.clear(); }
    logf("mirror swapchain teardown complete");
}
// vkDestroySwapchainKHR of the exported or a retired swapchain.
static void swapchain_destroyed(MDev* md, VkSwapchainKHR swapchain) {
    if (!md || !swapchain) return;
    if (md->swap.handle == swapchain) destroy_swap(md);
    std::lock_guard<std::mutex> l(md->lock);
    md->retired.erase(h2u(swapchain));
}
// Waits once for all right-eye GPU work after a stop, so twins can be
// destroyed. False (twins leaked, never freed under the GPU) if it times out.
static bool drain_after_stop(MDev* md) {
    if (!md) return false;
    if (md->drained) return md->drainOk;
    std::lock_guard<std::mutex> lock(md->drainLock);
    if (md->drained) return md->drainOk;
    md->drainOk = wait_timelines(md, "mirror stop");
    md->drained = true;
    if (!md->drainOk) logf("mirror stop: right-eye work did not drain; its resources are kept rather than freed under the GPU");
    return md->drainOk;
}
// After a stop: right-eye memory, swapchain twins and the shared transport
// are released so VRAM returns to the game's own use. Later app destroys
// free the remaining right-eye objects (see the generated wrappers).
static void release_after_stop(MDev* md) {
    if (!md || md->live || !md->releasePending.exchange(false)) return;
    logf("mirror stop: releasing right-eye resources");
    if (!drain_after_stop(md)) return;
    std::vector<std::pair<uint64_t, VkDeviceSize>> memories;
    {
        std::lock_guard<std::mutex> l(md->lock);
        for (const auto& kv : md->bmemories) memories.push_back(kv.second);
        for (const auto& kv : md->bmemories) { std::lock_guard<std::mutex> h(md->hlock); md->handles.erase(kv.first); }
        md->bmemories.clear();
        for (auto it = md->mems.begin(); it != md->mems.end();) {
            if (it->second.mapped) { ++it; continue; } // the app still writes this shadow
            free_shadow(it->second); it = md->mems.erase(it);
        }
    }
    VkDeviceSize bytes = 0;
    for (const auto& memory : memories) { md->b.FreeMemory(md->bdev, u2h<VkDeviceMemory>(memory.first), nullptr); bytes += memory.second; }
    destroy_swap(md);
    logf("mirror stop: released %zu right-eye allocations (%.1f MB) and the swapchain twins", memories.size(), bytes / 1048576.0);
}
static void shutdown(MDev* md) {
    if (!md) return;
    // vkDestroyDevice: the app no longer uses any queue, so idling is legal here.
    md->a.DeviceWaitIdle(md->device);
    destroy_swap(md);
    if(md->tuningFence) md->a.DestroyFence(md->device,md->tuningFence,nullptr);
    if(md->tuningPool) md->a.DestroyCommandPool(md->device,md->tuningPool,nullptr);
    if(md->frame) publish_tuning_state(md,false);
    for (auto& kv : md->mems) free_shadow(kv.second);
    for (auto& kv : md->timelines) if (kv.second.timeline) md->a.DestroySemaphore(md->device, kv.second.timeline, nullptr);
    md->timelines.clear();
    for (auto* eye : {&md->aep, &md->bep}) {
        if (eye->buffer) md->a.DestroyBuffer(md->device, eye->buffer, nullptr);
        if (eye->memory) md->a.FreeMemory(md->device, eye->memory, nullptr);
        *eye = {};
    }
    logf("mirror closed: %llu replayed submissions, %llu shader patches, %llu frames, %llu translated push pointers, %llu right-eye pipelines (%.1f ms); live=%d",
        (unsigned long long)md->submits.load(), (unsigned long long)md->patches.load(),
        (unsigned long long)md->frame, (unsigned long long)md->translated.load(),
        (unsigned long long)md->rightPipelines.load(), md->rightPipelineNs.load() / 1e6, int(md->live.load()));
}
static std::shared_ptr<MDev> create(VkDevice device, PFN_vkGetDeviceProcAddr next, PFN_vkSetDeviceLoaderData loader,
    const VkPhysicalDeviceMemoryProperties& memory, uint32_t family, LUID adapter, float shift, const wchar_t* session) {
    if (!device || !next || !session) return {};
    auto md = std::make_shared<MDev>();
    md->device = md->bdev = device; md->next = next; md->loader = loader;
    md->amp = md->bmp = memory; md->family = family; md->adapter = adapter; md->session = session;
    load_dispatch(md->a, next, device); md->b = md->a;
    md->importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(next(device, "vkImportSemaphoreWin32HandleKHR"));
    float convergence = 0;
    // Initial values are uploaded before shaders exist; later changes use
    // queue-ordered transfers in update_tuning, not CPU mappings.
    wchar_t path[32768]{};
    if (GetEnvironmentVariableW(L"VRX_XG_TUNING", path, 32768)) {
        md->tuningPath=path;read_tuning(md->tuningPath,shift,convergence);
    }
    md->tuningShift=shift;md->tuningConvergence=convergence;
    if (!make_eye(md.get(), md->aep, shift, convergence) || !make_eye(md.get(), md->bep, -shift, -convergence)) {
        shutdown(md.get()); return {};
    }
    logf("same-frame mirror enabled: one device, isolated eye resources and command buffers; separation %.6f convergence %.6f", shift, convergence);
    return md;
}
static VkResult create_swap(MDev* md, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* allocator, VkSwapchainKHR* out) {
    if (!md || !ci || !out) return VK_ERROR_INITIALIZATION_FAILED;
    logf("mirror swapchain request: device=0x%llx live=%d size=%ux%u format=%d colorSpace=%d old=0x%llx", (unsigned long long)h2u(md->device), int(md->live.load()),
        ci->imageExtent.width, ci->imageExtent.height, int(ci->imageFormat), int(ci->imageColorSpace), (unsigned long long)h2u(ci->oldSwapchain));
    auto changed = *ci; changed.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkResult result = md->a.CreateSwapchainKHR(md->device, &changed, allocator, out);
    if (result != VK_SUCCESS || !md->live) return result;
    if (md->swap.handle) {
        if (!ci->oldSwapchain || ci->oldSwapchain != md->swap.handle) { mirror_fail(md, "a second swapchain while the exported one is alive is not supported"); return result; }
        // Recreation (resize, mode change): the old swapchain is retired. Its
        // export ends and a new transport generation starts at the new size;
        // the engine follows replacement generations of the same process.
        logf("mirror swapchain recreated: %ux%u -> %ux%u", md->swap.extent.width, md->swap.extent.height, ci->imageExtent.width, ci->imageExtent.height);
        { std::lock_guard<std::mutex> l(md->lock); md->retired.insert(h2u(ci->oldSwapchain)); }
        destroy_swap(md);
    }
    // The shared textures carry display-referred sRGB-encoded RGBA8 bytes.
    // Other colour spaces (HDR10/PQ, scRGB, wide gamut) would change meaning.
    if (ci->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        char reason[160]; std::snprintf(reason, sizeof(reason), "unsupported swapchain colour space %d (only SRGB_NONLINEAR is exported; HDR and wide gamut are not)", int(ci->imageColorSpace));
        mirror_fail(md, reason); return result;
    }
    xg::GpuSourcePath path{};
    if (!xg::GpuSourcePathFor(ci->imageFormat, path)) {
        char reason[128]; std::snprintf(reason, sizeof(reason), "unsupported swapchain format %d (8-bit or 10-bit UNORM, or 8-bit sRGB, only)", int(ci->imageFormat));
        mirror_fail(md, reason); return result;
    }
    auto& s = md->swap; s.handle = *out; s.extent = ci->imageExtent; s.format = ci->imageFormat;
    uint32_t n = 0; md->a.GetSwapchainImagesKHR(md->device, *out, &n, nullptr); s.images.resize(n);
    if (!n || md->a.GetSwapchainImagesKHR(md->device, *out, &n, s.images.data()) != VK_SUCCESS) {
        mirror_fail(md, "swapchain image enumeration"); return result;
    }
    const auto deviceState=FindDevice(md->device);
    VkFormatProperties sourceFormat{},destinationFormat{},stagingFormat{};
    deviceState.formatProperties(deviceState.physical,ci->imageFormat,&sourceFormat);
    deviceState.formatProperties(deviceState.physical,VK_FORMAT_R8G8B8A8_UNORM,&destinationFormat);
    deviceState.formatProperties(deviceState.physical,VK_FORMAT_B8G8R8A8_UNORM,&stagingFormat);
    // sRGB sources are copied (raw encoded bytes), never blitted: a blit into
    // UNORM would decode the gamma and darken the picture.
    const auto source = sourceFormat.optimalTilingFeatures, target = destinationFormat.optimalTilingFeatures, staging = stagingFormat.optimalTilingFeatures;
    const bool capable = path == xg::GpuSourcePath::Blit ? (source & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (target & VK_FORMAT_FEATURE_BLIT_DST_BIT) :
        path == xg::GpuSourcePath::Copy ? (source & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (target & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) :
        (source & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) && (staging & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) && (staging & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        (target & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (ci->imageArrayLayers != 1 || !xg::GpuShape(ci->imageExtent.width, ci->imageExtent.height) || !capable) {
        mirror_fail(md, "unsupported swapchain size, layers or transfer capability"); return result;
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
    { std::lock_guard<std::mutex> l(md->lock); s.free.assign(n, {}); }
    md->gpu = std::make_unique<xg::GpuExporter>();
    if (!md->gpu->Init(md->device, md->next, md->loader, md->family, md->adapter, s.extent.width, s.extent.height, md->session, xg::GpuFrameKind::SameFrame) ||
        !md->gpu->SetSourceFormat(ci->imageFormat, md->amp)) {
        mirror_fail(md, md->gpu->Error()); return result;
    }
    logf("mirror swapchain: %ux%u per eye, %u isolated targets; GPU same-frame transport (%s)", s.extent.width, s.extent.height, n,
        path == xg::GpuSourcePath::Blit ? "UNORM blit" : path == xg::GpuSourcePath::Copy ? "sRGB byte copy" : "sRGB byte copy + channel swap");
    publish_tuning_state(md,true);
    return result;
}
// Abandoned shared fences (the consumer process was terminated): wait for the
// layer's copies that used them, then publish a replacement generation so a
// restarted engine can attach without restarting the game.
static bool rebuild_exporter(MDev* md) {
    if (!md || !md->gpu) return false;
    logf("mirror: shared transport fences abandoned (consumer process terminated); starting a replacement generation (%llu so far)",
        (unsigned long long)md->exporterRebuilds);
    if (++md->exporterRebuilds > 64) { mirror_fail(md, "shared transport replaced too often"); return false; }
    wait_timelines(md, "transport replacement");
    md->gpu.reset();
    auto gpu = std::make_unique<xg::GpuExporter>();
    if (!gpu->Init(md->device, md->next, md->loader, md->family, md->adapter, md->swap.extent.width, md->swap.extent.height, md->session, xg::GpuFrameKind::SameFrame) ||
        !gpu->SetSourceFormat(md->swap.format, md->amp)) {
        mirror_fail(md, gpu->Error()); return false;
    }
    md->gpu = std::move(gpu);
    logf("mirror: replacement transport generation ready");
    return true;
}
static VkResult present(MDev* md, VkQueue queue, const VkPresentInfoKHR* pi) {
    if (!md || !pi) return VK_ERROR_INITIALIZATION_FAILED;
    if (!md->live || !md->gpu) {
        if (!md->live) after_stop(md, queue);
        return md->a.QueuePresentKHR(queue, pi);
    }
    // An image of a swapchain retired by recreation is presented without export.
    bool retired = false;
    if (pi->swapchainCount == 1 && pi->pSwapchains) { std::lock_guard<std::mutex> l(md->lock); retired = md->retired.count(h2u(pi->pSwapchains[0])) != 0; }
    if (retired) return md->a.QueuePresentKHR(queue, pi);
    if (pi->swapchainCount != 1 || pi->pSwapchains[0] != md->swap.handle || pi->pImageIndices[0] >= md->swap.images.size()) {
        mirror_fail(md, "unsupported presentation chain or swapchain"); return md->a.QueuePresentKHR(queue, pi);
    }
    if (md->gpu->Lost() && !rebuild_exporter(md)) return md->a.QueuePresentKHR(queue, pi);
    const uint32_t index = pi->pImageIndices[0];
    std::vector<VkSemaphore> waits;
    std::vector<uint64_t> values;
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; ++i) {
        waits.push_back(pi->pWaitSemaphores[i]); values.push_back(0);
        // Presenting straight after acquisition: the twin needs no wait.
        MDev::TwinFree free;
        if (acquired_wait(md, pi->pWaitSemaphores[i], free)) {
            if (free.timeline) { waits.push_back(free.timeline); values.push_back(free.value); }
            continue;
        }
        waits.push_back(xl_handle(md, pi->pWaitSemaphores[i])); values.push_back(0);
    }
    if (!md->live) return md->a.QueuePresentKHR(queue, pi);
    std::vector<VkPipelineStageFlags> stages(waits.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkCommandBuffer copies[2]{};
    const bool capture = md->patches && md->gpu->Reserve();
    if (capture) {
        copies[0] = md->gpu->Record(md->swap.images[index], md->swap.extent, 0);
        copies[1] = md->gpu->Record(md->swap.twins[index], md->swap.extent, 1, VK_IMAGE_LAYOUT_GENERAL);
        if (!copies[0] || !copies[1]) { mirror_fail(md, "capture recording"); return md->a.QueuePresentKHR(queue, pi); }
        waits.push_back(md->gpu->Released()); values.push_back(md->gpu->ReleaseValue()); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }
    VkResult queued = VK_ERROR_UNKNOWN;
    VkSemaphore signals[3] = {md->swap.ready[index]};
    uint64_t signalValues[3] = {0};
    uint32_t signalCount = 1;
    if (capture) { signals[signalCount] = md->gpu->Ready(); signalValues[signalCount++] = md->gpu->Sequence(); }
    {
        // The copy also signals the queue's layer timeline: it frees the twin
        // for the next right-eye frame and bounds swapchain teardown waits.
        std::lock_guard<std::mutex> l(md->qlock);
        auto* timeline = queue_timeline(md, queue);
        if (timeline) { signals[signalCount] = timeline->timeline; signalValues[signalCount++] = ++timeline->value; }
        VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tl.waitSemaphoreValueCount = (uint32_t)values.size(); tl.pWaitSemaphoreValues = values.data();
        tl.signalSemaphoreValueCount = signalCount; tl.pSignalSemaphoreValues = signalValues;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.pNext = &tl;
        submit.waitSemaphoreCount = (uint32_t)waits.size(); submit.pWaitSemaphores = waits.data(); submit.pWaitDstStageMask = stages.data();
        submit.commandBufferCount = capture ? 2 : 0; submit.pCommandBuffers = copies;
        submit.signalSemaphoreCount = signalCount; submit.pSignalSemaphores = signals;
        queued = md->a.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (timeline && queued != VK_SUCCESS) --timeline->value;
        if (timeline && queued == VK_SUCCESS) { std::lock_guard<std::mutex> f(md->lock); if (index < md->swap.free.size()) md->swap.free[index] = {timeline->timeline, timeline->value}; }
    }
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
            logf("mirror pair %llu: same game frame, %llu replayed submits, %llu shader patches, %llu push pointers", (unsigned long long)md->frame,
                (unsigned long long)md->submits.load(), (unsigned long long)md->patches.load(), (unsigned long long)md->translated.load());
        md->gpu->Drop();
    }
    update_tuning(md,queue);
    return result;
}

// ------------------------------------------------------- compute and push ---
static VKAPI_ATTR VkResult VKAPI_CALL m_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
    const VkComputePipelineCreateInfo* cis, const VkAllocationCallbacks* allocator, VkPipeline* out) {
    auto* md = mdev_of(key(dev));
    if (!md) { const auto state = FindDevice(dev); return ((PFN_vkCreateComputePipelines)state.next(dev,"vkCreateComputePipelines"))(dev,cache,n,cis,allocator,out); }
    const auto result = md->a.CreateComputePipelines(dev,cache,n,cis,allocator,out);
    if (result < 0 || !md->live || !n || !cis || !out) return result;
    auto& c = ctx_begin(md); auto* infos = c.alloc<VkComputePipelineCreateInfo>(n); auto* twins = c.alloc<VkPipeline>(n);
    for (uint32_t i=0;i<n;++i) tr_VkComputePipelineCreateInfo(c,&cis[i],&infos[i]);
    const auto replay = c.ok() ? md->a.CreateComputePipelines(dev,c.xl(cache),n,infos,nullptr,twins) : VK_ERROR_UNKNOWN;
    if (replay != VK_SUCCESS) mirror_fail(md,"compute pipeline replay");
    else for (uint32_t i=0;i<n;++i) {
        // Null app pipeline (VK_PIPELINE_COMPILE_REQUIRED): no owner for the twin.
        if (!out[i]) { if (twins[i]) md->a.DestroyPipeline(dev,twins[i],nullptr); continue; }
        record_handle(md,out[i],twins[i]);
        MDev::Pipeline pipeline; pipeline.layout=cis[i].layout;
        if (!reflect_stage(md,cis[i].stage,pipeline)) mirror_fail(md,"untranslatable compute push pointers");
        std::unique_lock<std::shared_mutex> l(md->pipelineLock);
        md->pipelines[h2u(out[i])] = std::move(pipeline);
    }
    ctx_end(c); return result;
}
static VKAPI_ATTR void VKAPI_CALL m_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    auto* md = mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdBindPipeline)s.next(s.handle,"vkCmdBindPipeline"))(cb,point,pipeline); }
    md->a.CmdBindPipeline(cb,point,pipeline); if (!md->live) return;
    if (point!=VK_PIPELINE_BIND_POINT_GRAPHICS && point!=VK_PIPELINE_BIND_POINT_COMPUTE) { mirror_fail(md,"unsupported pipeline bind point"); return; }
    const auto twin=xl_handle(md,pipeline); const auto twinCb=xl_handle(md,cb);
    if (!md->live) return;
    md->a.CmdBindPipeline(twinCb,point,twin);
    // Push constants are translated at the next draw/dispatch for this pipeline.
    if (auto* state = command_state(md, cb, true)) state->bound[point==VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0] = pipeline;
}
// Records app push bytes (left-eye call already made). The right eye receives
// them at the next draw/dispatch, translated for the pipeline bound then:
// push data persists across binds, so translating at push time against the
// pipeline bound at that moment could hand left-eye addresses to the right.
static bool record_push(MDev* md, VkCommandBuffer cb, VkPipelineLayout layout, uint32_t offset, uint32_t size, const void* values) {
    if (!values || !size || offset + size < offset || offset + size > 4096) return false;
    auto* state = command_state(md, cb, true);
    if (!state) return false;
    if (state->bytes.size() < offset + size) { state->bytes.resize(offset + size, 0); state->written.resize(offset + size, 0); }
    std::memcpy(state->bytes.data() + offset, values, size);
    std::memset(state->written.data() + offset, 1, size);
    state->layout = layout;
    state->dirty = true;
    return true;
}
// Draw/dispatch time: pushes the recorded bytes to the right eye with every
// pointer field of the bound pipeline translated. Cheap when nothing changed
// (no lock, no copy); otherwise O(fields * log buffers).
static bool push_flush(MDev* md, VkCommandBuffer cb, VkPipelineBindPoint point) {
    auto* state = command_state(md, cb, false);
    if (!state || !state->layout) return true;  // nothing pushed
    const VkPipeline pipeline = state->bound[point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
    if (!state->dirty && state->flushedFor == pipeline) return true;
    static thread_local std::vector<uint32_t> fields;
    static thread_local std::vector<xg::PushRange> ranges;
    fields.clear(); ranges.clear();
    {
        std::shared_lock<std::shared_mutex> l(md->pipelineLock);
        const auto it = md->pipelines.find(h2u(pipeline));
        if (it != md->pipelines.end()) fields = it->second.fields;
        const auto layout = md->layouts.find(h2u(state->layout));
        if (layout == md->layouts.end()) { l.unlock(); mirror_fail(md, "push constants with an unknown pipeline layout"); return false; }
        ranges = layout->second;
    }
    // Same untranslated bytes already reached the right eye: nothing to do.
    if (!state->dirty && fields.empty() && !state->flushedTranslated) { state->flushedFor = pipeline; return true; }
    static thread_local std::vector<uint8_t> bytes;
    bytes = state->bytes;
    uint32_t count = 0;
    const auto translation = xg::TranslatePushPointers(bytes, state->written, fields, md->addresses, count);
    if (translation != xg::PushTranslation::Translated) {
        mirror_fail(md, translation == xg::PushTranslation::PartialPointer ? "push constant pointer written only partly" :
            "push constant pointer outside every known buffer");
        return false;
    }
    const auto twinCb = xl_handle(md, cb); const auto twinLayout = xl_handle(md, state->layout);
    if (!md->live) return false;
    for (const auto& segment : xg::PushSegments(ranges)) {
        if (segment.offset >= bytes.size()) continue;
        const uint32_t size = std::min<uint32_t>(segment.size, uint32_t(bytes.size()) - segment.offset);
        bool written = false;
        for (uint32_t i = 0; i < size && !written; ++i) written = state->written[segment.offset + i] != 0;
        if (!written) continue;
        if (size != segment.size) { mirror_fail(md, "push constant segment pushed only partly"); return false; }
        md->a.CmdPushConstants(twinCb, twinLayout, segment.stages, segment.offset, segment.size, bytes.data() + segment.offset);
    }
    const auto total = md->translated.fetch_add(count) + count;
    if (count && total == count) logf("mirror: first push-constant pointer translated at draw time (%u field(s))", count);
    state->dirty = false; state->flushedFor = pipeline; state->flushedTranslated = count != 0;
    return true;
}
static VKAPI_ATTR void VKAPI_CALL m_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
    uint32_t offset, uint32_t size, const void* values) {
    auto* md=mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdPushConstants)s.next(s.handle,"vkCmdPushConstants"))(cb,layout,stages,offset,size,values); }
    md->a.CmdPushConstants(cb,layout,stages,offset,size,values); if (!md->live) return;
    if (!record_push(md,cb,layout,offset,size,values)) mirror_fail(md,"push constant recording");
}
static VKAPI_ATTR void VKAPI_CALL m_CmdPushConstants2(VkCommandBuffer cb, const VkPushConstantsInfo* info) {
    auto* md=mdev_of(key(cb));
    if (!md) { auto s=FindDevice((VkDevice)cb); return ((PFN_vkCmdPushConstants2)s.next(s.handle,"vkCmdPushConstants2"))(cb,info); }
    md->a.CmdPushConstants2(cb,info); if (!md->live) return;
    if (!info || info->pNext || !record_push(md,cb,info->layout,info->offset,info->size,info->pValues))
        mirror_fail(md,"push constants2 recording (pNext layouts are unsupported)");
}
// External semaphore payloads cannot be duplicated for the right eye: a
// right-eye wait on the twin would never be satisfied by the external signal
// and would stall the shared queue. Stop stereo first, then import.
static VKAPI_ATTR VkResult VKAPI_CALL m_ImportSemaphoreWin32HandleKHR(VkDevice dev, const VkImportSemaphoreWin32HandleInfoKHR* info) {
    auto* md = mdev_of(key(dev));
    auto fn = md ? md->importSemaphore : reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(FindDevice(dev).next(dev, "vkImportSemaphoreWin32HandleKHR"));
    if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (md && md->live) mirror_fail(md, "external semaphore import (vkImportSemaphoreWin32HandleKHR) cannot be mirrored");
    return fn(dev, info);
}

// --------------------------------------------------------- device support ---
// Device extensions the mirror is known to handle: every non-platform,
// non-provisional device extension of the pinned registry (generated), plus
// reviewed Windows extensions. Any other enabled extension may add commands
// or structures the generated mirror would pass to the left eye only, giving
// a silently wrong right eye; such a device keeps ordinary rendering.
static const char* unsupported_device_feature(const VkDeviceCreateInfo* info, uint32_t apiVersion) {
    if (!info) return "missing device create info";
    if (VK_API_VERSION_MINOR(apiVersion) > kMirrorRegistryMinor) return "Vulkan core version newer than the mirror's registry";
    static const char* const platform[] = {
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,    // imports keep caller handles (tr_chain); queries are left-eye only
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, // imports stop the mirror (m_ImportSemaphoreWin32HandleKHR)
        VK_KHR_EXTERNAL_FENCE_WIN32_EXTENSION_NAME,     // fences are CPU-waited; the twin is signalled by the right-eye submit
        VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,        // submit structures are untranslatable: stop at first use (logged)
        VK_NV_WIN32_KEYED_MUTEX_EXTENSION_NAME,
        VK_NV_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME,    // swapchain-only: the presented (left) image
    };
    for (uint32_t i = 0; i < info->enabledExtensionCount && info->ppEnabledExtensionNames; ++i) {
        const char* name = info->ppEnabledExtensionNames[i];
        if (!name) continue;
        if (mirror_extension_known(name)) continue;
        bool reviewed = false;
        for (const char* p : platform) reviewed = reviewed || !strcmp(name, p);
        if (!reviewed) return name;
    }
    return nullptr;
}

static PFN_vkVoidFunction lookup(const char* name) {
    if (!name) return nullptr;
    if (!strcmp(name, "vkImportSemaphoreWin32HandleKHR")) return (PFN_vkVoidFunction)m_ImportSemaphoreWin32HandleKHR;
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
        M(CreatePipelineLayout);
#undef M
        return nullptr;
    }
    return gen_lookup(name);
}
} // namespace mirror
