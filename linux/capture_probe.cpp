#include "source_ring.h"
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw-utils.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Capture {
    GMainLoop* portal_loop = nullptr;
    XdpPortal* portal = nullptr;
    XdpSession* session = nullptr;
    pw_stream* stream = nullptr;
    spa_hook stream_listener{};
    spa_video_info_raw format{};
    uint64_t frames = 0;
    uint64_t copied = 0;
    uint64_t unsupported = 0;
    uint64_t generation = 0;
    uint64_t last_sequence = 0;
    int64_t last_pts = -1;
    SourceRing<3> ring;
    std::array<std::vector<uint8_t>, 3> slot_bgra;
    uint64_t dropped = 0;
    bool failed = false;
};

void show_error(const char* stage, GError* error) {
    std::cerr << stage << ": " << (error ? error->message : "unknown error") << '\n';
    if (error) g_error_free(error);
}

void on_started(GObject* object, GAsyncResult* result, gpointer data) {
    auto& capture = *static_cast<Capture*>(data);
    GError* error = nullptr;
    if (!xdp_session_start_finish(XDP_SESSION(object), result, &error)) {
        show_error("ScreenCast start", error);
        capture.failed = true;
    }
    g_main_loop_quit(capture.portal_loop);
}

void on_created(GObject* object, GAsyncResult* result, gpointer data) {
    auto& capture = *static_cast<Capture*>(data);
    GError* error = nullptr;
    capture.session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(object), result, &error);
    if (!capture.session) {
        show_error("ScreenCast session", error);
        capture.failed = true;
        g_main_loop_quit(capture.portal_loop);
        return;
    }
    xdp_session_start(capture.session, nullptr, nullptr, on_started, &capture);
}

void on_state(void* data, pw_stream_state, pw_stream_state state, const char* error) {
    auto& capture = *static_cast<Capture*>(data);
    std::cout << "PipeWire state: " << pw_stream_state_as_string(state);
    if (error) std::cout << " (" << error << ')';
    std::cout << std::endl;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
        capture.failed = true;
}

void on_format(void* data, uint32_t id, const spa_pod* param) {
    std::cout << "PipeWire parameter " << id << (param ? " set" : " cleared") << std::endl;
    if (id != SPA_PARAM_Format && id != SPA_PARAM_EnumFormat) return;
    if (!param) return;
    auto& capture = *static_cast<Capture*>(data);
    spa_video_info_raw next{};
    if (spa_format_video_raw_parse(param, &next) < 0) {
        std::cerr << "Could not parse PipeWire video format parameter " << id << std::endl;
        if (id == SPA_PARAM_Format) capture.failed = true;
        return;
    }
    std::cout << "Video format candidate: " << next.size.width << 'x' << next.size.height
              << " SPA format " << next.format << " modifier " << next.modifier << std::endl;
    if (id != SPA_PARAM_Format) return;
    if (next.size.width != capture.format.size.width || next.size.height != capture.format.size.height ||
        next.format != capture.format.format) ++capture.generation;
    capture.format = next;
    std::cout << "Format generation " << capture.generation << ": " << next.size.width << 'x'
              << next.size.height << " SPA format " << next.format << std::endl;
}

void on_process(void* data) {
    auto& capture = *static_cast<Capture*>(data);
    pw_buffer* buffer = pw_stream_dequeue_buffer(capture.stream);
    if (!buffer) return;
    spa_buffer* spa = buffer->buffer;
    ++capture.frames;
    auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(spa, SPA_META_Header, sizeof(spa_meta_header)));
    if (header) {
        capture.last_sequence = header->seq;
        capture.last_pts = header->pts;
    }
    if (spa->n_datas == 1 && (capture.format.format == SPA_VIDEO_FORMAT_BGRA ||
        capture.format.format == SPA_VIDEO_FORMAT_BGRx || capture.format.format == SPA_VIDEO_FORMAT_RGBA)) {
        const spa_data& pixels = spa->datas[0];
        const uint32_t width = capture.format.size.width;
        const uint32_t height = capture.format.size.height;
        const int64_t stride = pixels.chunk ? pixels.chunk->stride : 0;
        const uint64_t offset = pixels.chunk ? pixels.chunk->offset : 0;
        const uint64_t bytes = pixels.chunk ? pixels.chunk->size : 0;
        if (pixels.data && stride >= static_cast<int64_t>(width) * 4 && height &&
            offset <= pixels.maxsize && bytes <= pixels.maxsize - offset &&
            static_cast<uint64_t>(stride) * (height - 1) + static_cast<uint64_t>(width) * 4 <= bytes) {
            auto frame = capture.ring.Reserve(capture.copied, capture.copied, capture.copied);
            if (!frame) {
                ++capture.dropped;
            } else {
                frame->layout = capture.generation;
                auto& destination = capture.slot_bgra[frame->index];
                destination.resize(static_cast<size_t>(width) * height * 4);
                const auto* source = static_cast<const uint8_t*>(pixels.data) + offset;
                for (uint32_t y = 0; y < height; ++y) {
                    auto* row = destination.data() + static_cast<size_t>(y) * width * 4;
                    std::memcpy(row, source + static_cast<size_t>(y) * stride,
                                static_cast<size_t>(width) * 4);
                    if (capture.format.format == SPA_VIDEO_FORMAT_BGRx)
                        for (uint32_t x = 0; x < width; ++x) row[x * 4 + 3] = 255;
                    if (capture.format.format == SPA_VIDEO_FORMAT_RGBA)
                        for (uint32_t x = 0; x < width; ++x)
                            std::swap(row[x * 4], row[x * 4 + 2]);
                }
                const double arrival = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                capture.ring.Publish(frame, ++capture.copied, arrival);
            }
        } else ++capture.unsupported;
    } else ++capture.unsupported;
    if (capture.frames == 1 || capture.frames % 120 == 0)
        std::cout << "Frames " << capture.frames << ", copied " << capture.copied
                  << ", unsupported " << capture.unsupported << ", dropped " << capture.dropped << ", seq " << capture.last_sequence
                  << ", pts " << capture.last_pts << std::endl;
    pw_stream_queue_buffer(capture.stream, buffer);
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: vrx-capture-probe [seconds=10]\n"
                     "Select a window or monitor in the portal chooser; count copied BGRA frames.\n";
        return 0;
    }
    int duration = 10;
    if (argc > 1) {
        try { duration = std::stoi(argv[1]); } catch (...) { return 2; }
        if (duration < 1 || duration > 300) return 2;
    }
    Capture capture;
    capture.portal_loop = g_main_loop_new(nullptr, FALSE);
    capture.portal = xdp_portal_new();
    xdp_portal_create_screencast_session(capture.portal,
        static_cast<XdpOutputType>(XDP_OUTPUT_WINDOW | XDP_OUTPUT_MONITOR),
        XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE,
        nullptr, nullptr, on_created, &capture);
    std::cout << "Select one window or monitor in the ScreenCast chooser." << std::endl;
    g_main_loop_run(capture.portal_loop);
    if (capture.failed || !capture.session) return 1;

    GVariant* streams = xdp_session_get_streams(capture.session);
    if (!streams || g_variant_n_children(streams) == 0) {
        std::cerr << "Portal returned no streams" << std::endl;
        return 1;
    }
    GVariant* first = g_variant_get_child_value(streams, 0);
    guint32 node_id = 0;
    GVariant* properties = nullptr;
    g_variant_get(first, "(u@a{sv})", &node_id, &properties);
    std::cout << "Portal selected node " << node_id << std::endl;
    g_variant_unref(properties);
    g_variant_unref(first);
    const int remote_fd = xdp_session_open_pipewire_remote(capture.session);
    if (remote_fd < 0) {
        std::cerr << "Could not open portal PipeWire remote" << std::endl;
        return 1;
    }

    pw_init(&argc, &argv);
    pw_main_loop* pw_loop = pw_main_loop_new(nullptr);
    pw_context* context = pw_context_new(pw_main_loop_get_loop(pw_loop), nullptr, 0);
    pw_core* core = pw_context_connect_fd(context, remote_fd, nullptr, 0);
    if (!core) {
        std::cerr << "Could not connect to portal PipeWire remote" << std::endl;
        return 1;
    }
    const std::string target = std::to_string(node_id);
    capture.stream = pw_stream_new(core, "VRX capture probe", pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, target.c_str(), nullptr));
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_state;
    events.param_changed = on_format;
    events.process = on_process;
    pw_stream_add_listener(capture.stream, &capture.stream_listener, &events, &capture);
    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    const spa_pod* formats[] = {
        static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
                SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA)))
    };
    if (pw_stream_connect(capture.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), formats, 1) < 0) {
        std::cerr << "Could not connect selected PipeWire stream" << std::endl;
        return 1;
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    while (!capture.failed && std::chrono::steady_clock::now() < until) {
        pw_loop_iterate(pw_main_loop_get_loop(pw_loop), 50);
        while (g_main_context_iteration(nullptr, FALSE)) {}
    }
    const auto latest = capture.ring.Latest();
    std::cout << "Result: frames=" << capture.frames << " copied=" << capture.copied
              << " unsupported=" << capture.unsupported << " dropped=" << capture.dropped
              << " generation=" << capture.generation << " latest_bytes="
              << (latest ? capture.slot_bgra[latest->index].size() : 0) << std::endl;
    pw_stream_destroy(capture.stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(pw_loop);
    xdp_session_close(capture.session);
    g_object_unref(capture.session);
    g_object_unref(capture.portal);
    g_main_loop_unref(capture.portal_loop);
    pw_deinit();
    return !capture.failed && capture.copied > 0 ? 0 : 1;
}
