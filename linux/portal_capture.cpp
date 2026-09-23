#include "portal_capture.h"
#include "source_ring.h"
#include "capture_formats.h"
#include "drm_timeline.h"
#include "capture_scale.h"
#include "synthetic_scene.h"
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/iter.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace vrx {
namespace {
// Capture scaling threads: a 4K frame takes about 2.7 ms with four on the
// reference Ryzen 7 5700X; more gave little further gain.
constexpr int kScaleThreads = 4;
}
struct PortalCapture::Impl {
    GMainLoop* portal_loop = nullptr;
    XdpPortal* portal = nullptr;
    XdpSession* session = nullptr;
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    pw_node* source = nullptr;
    pw_stream* stream = nullptr;
    spa_hook node_listener{}, stream_listener{};
    pw_node_events node_events{};
    pw_stream_events stream_events{};
    spa_video_info_raw format{};
    SourceRing<3> ring;
    std::array<std::vector<uint32_t>, 3> colorSlots;
    ResampleTables tables;
    // DMA-BUF: modifiers the renderer can import; the negotiated one.
    std::vector<uint64_t> modifiers;
    XdpOutputType outputs = static_cast<XdpOutputType>(XDP_OUTPUT_WINDOW | XDP_OUTPUT_MONITOR);
    std::atomic<bool> dmabuf{false};
    uint64_t modifier = 0;
    // DMA-BUF buffers held for the renderer: the newest not yet taken, those
    // in use by frames in flight (by sequence), and those to give back on the
    // PipeWire thread.
    std::mutex gpuMutex;
    pw_buffer* latestGpu = nullptr;
    GpuFrame latestInfo;
    std::map<uint64_t, pw_buffer*> inUseGpu;
    std::vector<pw_buffer*> toRelease;
    uint64_t nextBufferId = 0, bufferGeneration = 0;
    // Explicit sync (SPA_META_SyncTimeline): each buffer's acquire and release
    // timelines and its current points. Offered only when a DRM render node
    // can signal the release points; otherwise the source would wait forever.
    DrmTimelines timelines;
    struct BufferSync {
        uint32_t acquire = 0, release = 0;     // syncobj handles
        uint64_t acquirePoint = 0, releasePoint = 0;
        bool active = false;                   // the current frame uses explicit sync
    };
    std::map<pw_buffer*, BufferSync> sync;
    bool syncLogged = false;
    // Fixed by the first negotiated size; later sizes are letterboxed into it.
    std::atomic<uint32_t> colorWidth{0}, colorHeight{0};
    std::thread worker;
    std::atomic<bool> stop{false}, failed{false};
    std::atomic<uint64_t> captured{0}, dropped{0};
    uint64_t layout = 0;
    std::string serial;
    bool pw_initialized = false;

    static void Created(GObject* object, GAsyncResult* result, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        GError* error = nullptr;
        self.session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(object), result, &error);
        if (!self.session) {
            std::fprintf(stderr, "ScreenCast session: %s\n", error ? error->message : "unknown error");
            if (error) g_error_free(error);
            self.failed = true;
            g_main_loop_quit(self.portal_loop);
            return;
        }
        xdp_session_start(self.session, nullptr, nullptr, Started, &self);
    }
    static void Started(GObject* object, GAsyncResult* result, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        GError* error = nullptr;
        if (!xdp_session_start_finish(XDP_SESSION(object), result, &error)) {
            std::fprintf(stderr, "ScreenCast start: %s\n", error ? error->message : "unknown error");
            if (error) g_error_free(error);
            self.failed = true;
        }
        g_main_loop_quit(self.portal_loop);
    }
    static void NodeInfo(void* data, const pw_node_info* info) {
        auto& self = *static_cast<Impl*>(data);
        const char* value = info && info->props ? spa_dict_lookup(info->props, PW_KEY_OBJECT_SERIAL) : nullptr;
        if (value) self.serial = value;
    }
    static void State(void* data, pw_stream_state, pw_stream_state state, const char* error) {
        auto& self = *static_cast<Impl*>(data);
        std::printf("Capture stream state: %s%s%s\n", pw_stream_state_as_string(state),
                    error ? ": " : "", error ? error : "");
        std::fflush(stdout);
        if (state == PW_STREAM_STATE_ERROR ||
            (state == PW_STREAM_STATE_UNCONNECTED && !self.stop)) {
            std::fprintf(stderr, "Capture stream: %s%s%s\n", pw_stream_state_as_string(state),
                         error ? ": " : "", error ? error : "");
            self.failed = true;
        }
    }
    static void Format(void* data, uint32_t id, const spa_pod* param) {
        if (id != SPA_PARAM_Format || !param) return;
        auto& self = *static_cast<Impl*>(data);
        uint8_t storage[4096];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_pod_prop* offeredModifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (offeredModifier && (offeredModifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
            // The source offers several modifiers: fix the first we can import
            // and renegotiate, keeping shared memory as the fallback.
            uint32_t count = 0, choice = 0;
            const spa_pod* values = spa_pod_get_values(&offeredModifier->value, &count, &choice);
            const auto* offered = static_cast<const uint64_t*>(SPA_POD_BODY(values));
            std::vector<uint64_t> fixed;
            for (uint32_t i = choice == SPA_CHOICE_None ? 0 : 1; i < count && fixed.empty(); ++i)
                if (std::find(self.modifiers.begin(), self.modifiers.end(), offered[i]) != self.modifiers.end())
                    fixed.push_back(offered[i]);
            auto formats = BuildCaptureFormats(&builder, fixed);
            pw_stream_update_params(self.stream, formats.data(), uint32_t(formats.size()));
            return;
        }
        spa_video_info_raw next{};
        if (spa_format_video_raw_parse(param, &next) < 0 ||
            (next.format != SPA_VIDEO_FORMAT_BGRA && next.format != SPA_VIDEO_FORMAT_BGRx &&
             next.format != SPA_VIDEO_FORMAT_RGBA) || !next.size.width || !next.size.height) {
            std::fputs("Capture negotiated an unsupported video format\n", stderr);
            self.failed = true;
            return;
        }
        const bool dmabuf = (next.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
        if (dmabuf && next.format == SPA_VIDEO_FORMAT_RGBA) {
            std::fputs("Capture negotiated an RGBA DMA-BUF, which is not imported\n", stderr);
            self.failed = true;
            return;
        }
        if (self.format.size.width != next.size.width || self.format.size.height != next.size.height ||
            self.format.format != next.format || dmabuf != self.dmabuf) ++self.layout;
        self.format = next;
        self.dmabuf = dmabuf;
        self.modifier = dmabuf ? next.modifier : 0;
        const int types = dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        std::vector<const spa_pod*> params;
        if (dmabuf && self.timelines.Available()) {
            // Preferred: the image plane plus acquire and release syncobjs.
            params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_sync_timeline)))));
            params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(3),
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types),
                SPA_PARAM_BUFFERS_metaType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_META_SyncTimeline))));
        }
        params.push_back(static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types))));
        pw_stream_update_params(self.stream, params.data(), uint32_t(params.size()));
        if (!self.colorWidth) {
            int width = 0, height = 0;
            ColorSizeFor(int(next.size.width), int(next.size.height), width, height);
            self.colorHeight = uint32_t(height);
            self.colorWidth = uint32_t(width);
            std::printf("Capture colour %dx%d; depth %dx%d\n", width, height,
                        kSyntheticWidth, kSyntheticHeight);
        }
        std::printf("Capture format %ux%u, SPA %u, layout %llu, %s\n", next.size.width,
                    next.size.height, next.format, static_cast<unsigned long long>(self.layout),
                    dmabuf ? "DMA-BUF" : "shared memory");
        if (dmabuf) std::printf("Capture DMA-BUF modifier 0x%016llx\n",
                                static_cast<unsigned long long>(self.modifier));
    }
    static void Process(void* data);
    static void AddBuffer(void* data, pw_buffer* buffer) {
        auto& self = *static_cast<Impl*>(data);
        std::lock_guard<std::mutex> lock(self.gpuMutex);
        buffer->user_data = reinterpret_cast<void*>(uintptr_t(++self.nextBufferId));
    }
    static void RemoveBuffer(void* data, pw_buffer* buffer) {
        auto& self = *static_cast<Impl*>(data);
        std::lock_guard<std::mutex> lock(self.gpuMutex);
        // The renderer's imported copy keeps its own reference to the memory;
        // a new generation tells it to import the new buffers.
        if (self.latestGpu == buffer) self.latestGpu = nullptr;
        for (auto it = self.inUseGpu.begin(); it != self.inUseGpu.end();)
            it = it->second == buffer ? self.inUseGpu.erase(it) : std::next(it);
        self.toRelease.erase(std::remove(self.toRelease.begin(), self.toRelease.end(), buffer),
                             self.toRelease.end());
        ++self.bufferGeneration;
        auto found = self.sync.find(buffer);
        if (found != self.sync.end()) {
            self.timelines.Destroy(found->second.acquire);
            self.timelines.Destroy(found->second.release);
            self.sync.erase(found);
        }
    }
    // Signal a buffer's release point (explicit sync), then return it to the
    // source. On the PipeWire thread.
    void GiveBack(pw_buffer* buffer) {
        uint32_t release = 0;
        uint64_t point = 0;
        {
            std::lock_guard<std::mutex> lock(gpuMutex);
            auto found = sync.find(buffer);
            if (found != sync.end() && found->second.active) {
                release = found->second.release;
                point = found->second.releasePoint;
                found->second.active = false;
            }
        }
        if (release && !timelines.Signal(release, point))
            std::fputs("Capture: signalling a DMA-BUF release point failed\n", stderr);
        pw_stream_queue_buffer(stream, buffer);
    }
    // On the PipeWire thread: give released DMA-BUFs back to the source.
    void QueueReleased() {
        std::vector<pw_buffer*> release;
        {
            std::lock_guard<std::mutex> lock(gpuMutex);
            release.swap(toRelease);
        }
        for (pw_buffer* buffer : release) GiveBack(buffer);
    }
    static int InvokeRelease(spa_loop*, bool, uint32_t, const void*, size_t, void* data) {
        static_cast<Impl*>(data)->QueueReleased();
        return 0;
    }
    void WakeRelease() {
        if (loop) pw_loop_invoke(pw_main_loop_get_loop(loop), InvokeRelease, 0, nullptr, 0, false, this);
    }
    bool Open();
    bool Connect(int fd, uint32_t node_id);
    void RunPipeWire(int fd, uint32_t node_id);
    void ClosePipeWire();
    ~Impl();
};
void PortalCapture::Impl::Process(void* data) {
    auto& self = *static_cast<Impl*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(self.stream);
    if (!buffer) return;
    self.QueueReleased();
    const spa_buffer* source = buffer->buffer;
    const auto width = self.format.size.width, height = self.format.size.height;
    if (self.dmabuf && source->n_datas >= 1 && source->datas[0].type == SPA_DATA_DmaBuf) {
        const spa_data& plane = source->datas[0];
        GpuFrame frame;
        frame.fd = int(plane.fd);
        frame.offset = plane.chunk ? plane.chunk->offset : 0;
        frame.stride = plane.chunk ? uint32_t(plane.chunk->stride) : 0;
        frame.modifier = self.modifier;
        frame.width = width;
        frame.height = height;
        frame.layout = self.layout;
        frame.sequence = self.captured.load(std::memory_order_relaxed) + 1;
        frame.arrival = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Explicit sync: the plane is followed by acquire and release syncobjs.
        auto* timeline = static_cast<spa_meta_sync_timeline*>(
            spa_buffer_find_meta_data(source, SPA_META_SyncTimeline, sizeof(spa_meta_sync_timeline)));
        const bool explicitSync = timeline && source->n_datas == 3 &&
            source->datas[1].type == SPA_DATA_SyncObj && source->datas[2].type == SPA_DATA_SyncObj;
        if (!self.syncLogged) {
            std::printf("Capture DMA-BUF explicit sync: %s\n", explicitSync ? "on" : "off (implicit)");
            self.syncLogged = true;
        }
        bool syncReady = false;
        if (explicitSync) {
            std::lock_guard<std::mutex> lock(self.gpuMutex);
            auto& state = self.sync[buffer];
            if (!state.acquire) state.acquire = self.timelines.Import(int(source->datas[1].fd));
            if (!state.release) state.release = self.timelines.Import(int(source->datas[2].fd));
            state.acquirePoint = timeline->acquire_point;
            state.releasePoint = timeline->release_point;
            state.active = state.acquire && state.release;
            syncReady = state.active;
            // We promise to signal the release point.
            if (state.active) timeline->flags &= ~uint32_t(SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE);
        }
        if (source->n_datas != (explicitSync ? 3u : 1u) || frame.fd < 0 || frame.stride < width * 4 ||
            !width || !height || (explicitSync && !syncReady)) {
            if (explicitSync) std::fputs("Capture: cannot use an explicitly synchronised DMA-BUF\n", stderr);
            ++self.dropped;
            self.GiveBack(buffer);
            return;
        }
        frame.explicitSync = explicitSync;
        pw_buffer* superseded = nullptr;
        {
            std::lock_guard<std::mutex> lock(self.gpuMutex);
            frame.bufferId = uint64_t(reinterpret_cast<uintptr_t>(buffer->user_data));
            frame.bufferGeneration = self.bufferGeneration;
            superseded = self.latestGpu;
            self.latestGpu = buffer;
            self.latestInfo = frame;
        }
        // A frame the renderer never took is replaced, not counted as dropped.
        if (superseded) self.GiveBack(superseded);
        self.captured.store(frame.sequence, std::memory_order_release);
        return;
    }
    if (source->n_datas != 1 || !width || !height) {
        ++self.dropped;
        pw_stream_queue_buffer(self.stream, buffer);
        return;
    }
    const spa_data& plane = source->datas[0];
    const int64_t stride = plane.chunk ? plane.chunk->stride : 0;
    const uint64_t offset = plane.chunk ? plane.chunk->offset : 0;
    const uint64_t size = plane.chunk ? plane.chunk->size : 0;
    if (!plane.data || stride < static_cast<int64_t>(width) * 4 ||
        offset > plane.maxsize || size > plane.maxsize - offset ||
        static_cast<uint64_t>(stride) * (height - 1) + static_cast<uint64_t>(width) * 4 > size) {
        ++self.dropped;
        pw_stream_queue_buffer(self.stream, buffer);
        return;
    }
    const uint64_t completed = self.captured.load(std::memory_order_relaxed);
    auto frame = self.ring.Reserve(completed, completed, completed);
    if (!frame) {
        ++self.dropped;
        pw_stream_queue_buffer(self.stream, buffer);
        return;
    }
    const auto* pixels = static_cast<const unsigned char*>(plane.data) + offset;
    const bool rgba = self.format.format == SPA_VIDEO_FORMAT_RGBA;
    const int colorWidth = int(self.colorWidth), colorHeight = int(self.colorHeight);
    if (self.tables.sourceWidth != int(width) || self.tables.sourceHeight != int(height))
        self.tables = BuildResampleTables(int(width), int(height), colorWidth, colorHeight,
                                          FitCapture(int(width), int(height), colorWidth, colorHeight));
    ScaleCaptureColor(pixels, size_t(stride), rgba, self.tables, self.colorSlots[frame->index], kScaleThreads);
    frame->layout = self.layout;
    const double arrival = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    self.ring.Publish(frame, completed + 1, arrival);
    self.captured.store(completed + 1, std::memory_order_relaxed);
    pw_stream_queue_buffer(self.stream, buffer);
}
bool PortalCapture::Impl::Open() {
    portal_loop = g_main_loop_new(nullptr, FALSE);
    portal = xdp_portal_new();
    xdp_portal_create_screencast_session(portal, outputs,
        XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE,
        nullptr, nullptr, Created, this);
    std::puts(outputs == XDP_OUTPUT_WINDOW ? "Select a window in the ScreenCast chooser." :
              outputs == XDP_OUTPUT_MONITOR ? "Select a screen in the ScreenCast chooser." :
              "Select a window or monitor in the ScreenCast chooser.");
    g_main_loop_run(portal_loop);
    if (failed || !session) return false;
    GVariant* streams = xdp_session_get_streams(session);
    if (!streams || !g_variant_n_children(streams)) return false;
    GVariant* first = g_variant_get_child_value(streams, 0);
    GVariant* properties = nullptr;
    guint32 node_id = 0;
    g_variant_get(first, "(u@a{sv})", &node_id, &properties);
    guint64 portal_serial = 0;
    if (g_variant_lookup(properties, "pipewire-serial", "t", &portal_serial))
        serial = std::to_string(portal_serial);
    g_variant_unref(properties);
    g_variant_unref(first);
    const int fd = xdp_session_open_pipewire_remote(session);
    if (fd < 0) return false;

    worker = std::thread([this, fd, node_id] { RunPipeWire(fd, node_id); });
    return true;
}
bool PortalCapture::Impl::Connect(int fd, uint32_t node_id) {
    pw_init(nullptr, nullptr);
    pw_initialized = true;
    loop = pw_main_loop_new(nullptr);
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    core = pw_context_connect_fd(context, fd, nullptr, 0);
    if (!core) { close(fd); return false; }
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    source = static_cast<pw_node*>(pw_registry_bind(registry, node_id,
        PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (source) {
        node_events.version = PW_VERSION_NODE_EVENTS;
        node_events.info = NodeInfo;
        pw_node_add_listener(source, &node_listener, &node_events, this);
        for (int i = 0; i < 20 && serial.empty(); ++i)
            pw_loop_iterate(pw_main_loop_get_loop(loop), 50);
    }
    const std::string target = serial.empty() ? std::to_string(node_id) : serial;
    std::printf("Capture node %u, target %s%s\n", node_id, target.c_str(),
                serial.empty() ? " (ID fallback)" : " (serial)");
    stream = pw_stream_new(core, "VRX live capture", pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, target.c_str(), nullptr));
    if (!stream) return false;
    stream_events.version = PW_VERSION_STREAM_EVENTS;
    stream_events.state_changed = State;
    stream_events.param_changed = Format;
    stream_events.process = Process;
    stream_events.add_buffer = AddBuffer;
    stream_events.remove_buffer = RemoveBuffer;
    pw_stream_add_listener(stream, &stream_listener, &stream_events, this);
    uint8_t pod_storage[4096];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    auto offers = BuildCaptureFormats(&builder, modifiers);
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            offers.data(), uint32_t(offers.size())) < 0) return false;
    return true;
}
void PortalCapture::Impl::RunPipeWire(int fd, uint32_t node_id) {
    if (!Connect(fd, node_id)) failed = true;
    while (!stop && !failed)
        pw_loop_iterate(pw_main_loop_get_loop(loop), 20);
    stop = true;
    ClosePipeWire();
}
void PortalCapture::Impl::ClosePipeWire() {
    if (stream) { pw_stream_destroy(stream); stream = nullptr; }
    if (source) { pw_proxy_destroy(reinterpret_cast<pw_proxy*>(source)); source = nullptr; }
    if (registry) { pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry)); registry = nullptr; }
    if (core) { pw_core_disconnect(core); core = nullptr; }
    if (context) { pw_context_destroy(context); context = nullptr; }
    if (loop) { pw_main_loop_destroy(loop); loop = nullptr; }
    if (pw_initialized) { pw_deinit(); pw_initialized = false; }
}
PortalCapture::Impl::~Impl() {
    stop = true;
    if (worker.joinable()) worker.join();
    if (session) { xdp_session_close(session); g_object_unref(session); }
    if (portal) g_object_unref(portal);
    if (portal_loop) g_main_loop_unref(portal_loop);
}
PortalCapture::PortalCapture() : impl_(std::make_unique<Impl>()) {}
PortalCapture::~PortalCapture() = default;
bool PortalCapture::Open(const std::vector<uint64_t>& modifiers, Source source) {
    impl_->modifiers = modifiers;
    impl_->outputs = source == Source::Window ? XDP_OUTPUT_WINDOW :
                     source == Source::Screen ? XDP_OUTPUT_MONITOR :
                     static_cast<XdpOutputType>(XDP_OUTPUT_WINDOW | XDP_OUTPUT_MONITOR);
    return impl_->Open();
}
bool PortalCapture::ColorSize(uint32_t& width, uint32_t& height) const {
    width = impl_->colorWidth;
    height = impl_->colorHeight;
    return width && height;
}
bool PortalCapture::DmaBuf() const { return impl_->dmabuf; }
bool PortalCapture::AcquireGpuFrame(GpuFrame& output) {
    uint32_t acquire = 0;
    uint64_t point = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->gpuMutex);
        if (!impl_->latestGpu) return false;
        output = impl_->latestInfo;
        impl_->inUseGpu[output.sequence] = impl_->latestGpu;
        auto found = impl_->sync.find(impl_->latestGpu);
        if (output.explicitSync && found != impl_->sync.end()) {
            acquire = found->second.acquire;
            point = found->second.acquirePoint;
        }
        impl_->latestGpu = nullptr;
    }
    // Explicit sync: the source's rendering into the buffer must be complete.
    // It normally is; a frame that is not ready soon is skipped.
    constexpr int64_t kAcquireTimeoutNs = 20000000;
    if (acquire && !impl_->timelines.Wait(acquire, point, kAcquireTimeoutNs)) {
        std::fputs("Capture: DMA-BUF not ready within 20 ms; skipping it\n", stderr);
        ReleaseGpuFrame(output.sequence);
        return false;
    }
    return true;
}
void PortalCapture::ReleaseGpuFrame(uint64_t sequence) {
    {
        std::lock_guard<std::mutex> lock(impl_->gpuMutex);
        auto found = impl_->inUseGpu.find(sequence);
        if (found == impl_->inUseGpu.end()) return;   // its buffer was removed meanwhile
        impl_->toRelease.push_back(found->second);
        impl_->inUseGpu.erase(found);
    }
    impl_->WakeRelease();
}
void PortalCapture::ReleaseGpuFrames() {
    {
        std::lock_guard<std::mutex> lock(impl_->gpuMutex);
        for (const auto& held : impl_->inUseGpu) impl_->toRelease.push_back(held.second);
        impl_->inUseGpu.clear();
    }
    impl_->WakeRelease();
}
bool PortalCapture::Latest(Frame& output) const {
    auto frame = impl_->ring.Latest();
    if (!frame) return false;
    output.color = impl_->colorSlots[frame->index];
    output.colorWidth = impl_->colorWidth;
    output.colorHeight = impl_->colorHeight;
    output.sequence = frame->seq;
    output.layout = frame->layout;
    output.arrival = frame->time;
    return true;
}
bool PortalCapture::Healthy() const {
    for (int i = 0; i < 8 && g_main_context_iteration(nullptr, FALSE); ++i) {}
    return !impl_->failed && (!impl_->session ||
        xdp_session_get_session_state(impl_->session) != XDP_SESSION_CLOSED);
}
uint64_t PortalCapture::Captured() const { return impl_->captured; }
uint64_t PortalCapture::Dropped() const { return impl_->dropped; }
} // namespace vrx
