#include "portal_capture.h"
#include "source_ring.h"
#include "capture_scale.h"
#include "synthetic_scene.h"
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/raw-utils.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

namespace vrx {
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
    std::array<std::vector<unsigned char>, 3> slots;
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
        spa_video_info_raw next{};
        if (spa_format_video_raw_parse(param, &next) < 0 ||
            (next.format != SPA_VIDEO_FORMAT_BGRA && next.format != SPA_VIDEO_FORMAT_BGRx &&
             next.format != SPA_VIDEO_FORMAT_RGBA) || !next.size.width || !next.size.height) {
            std::fputs("Capture negotiated an unsupported video format\n", stderr);
            self.failed = true;
            return;
        }
        if (self.format.size.width != next.size.width || self.format.size.height != next.size.height ||
            self.format.format != next.format) ++self.layout;
        self.format = next;
        if (!self.colorWidth) {
            int width = 0, height = 0;
            ColorSizeFor(int(next.size.width), int(next.size.height), width, height);
            self.colorHeight = uint32_t(height);
            self.colorWidth = uint32_t(width);
            std::printf("Capture colour %dx%d; depth %dx%d\n", width, height,
                        kSyntheticWidth, kSyntheticHeight);
        }
        std::printf("Capture format %ux%u, SPA %u, layout %llu\n", next.size.width,
                    next.size.height, next.format, static_cast<unsigned long long>(self.layout));
    }
    static void Process(void* data);
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
    const spa_buffer* source = buffer->buffer;
    const auto width = self.format.size.width, height = self.format.size.height;
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
    auto& color = self.colorSlots[frame->index];
    ScaleCaptureColor(pixels, size_t(stride), int(width), int(height), rgba,
                      colorWidth, colorHeight, color);
    DepthGridFromColor(color, colorWidth, colorHeight, self.slots[frame->index]);
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
    xdp_portal_create_screencast_session(portal,
        static_cast<XdpOutputType>(XDP_OUTPUT_WINDOW | XDP_OUTPUT_MONITOR),
        XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE,
        nullptr, nullptr, Created, this);
    std::puts("Select a window or monitor in the ScreenCast chooser.");
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
    pw_stream_add_listener(stream, &stream_listener, &stream_events, this);
    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    const spa_rectangle default_size = {1920, 1080}, min_size = {1, 1}, max_size = {8192, 8192};
    const spa_fraction default_rate = {60, 1}, min_rate = {0, 1}, max_rate = {240, 1};
    const spa_pod* offer = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&default_rate, &min_rate, &max_rate)));
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            &offer, 1) < 0) return false;
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
bool PortalCapture::Open() { return impl_->Open(); }
bool PortalCapture::Latest(Frame& output, bool color, bool rgb) const {
    auto frame = impl_->ring.Latest();
    if (!frame) return false;
    if (color) output.color = impl_->colorSlots[frame->index];
    if (rgb) output.rgb = impl_->slots[frame->index];
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
