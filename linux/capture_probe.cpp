#include "source_ring.h"
#include "capture_formats.h"
#include "vulkan_dmabuf.h"
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/debug/pod.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
// A Vulkan device able to import DMA-BUFs, for --dmabuf.
struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Gpu() {
        if (device) vkDeviceWaitIdle(device);
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

std::string Hex(uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(16) << std::setfill('0') << value;
    return text.str();
}

struct Capture {
    GMainLoop* portal_loop = nullptr;
    XdpPortal* portal = nullptr;
    XdpSession* session = nullptr;
    pw_stream* stream = nullptr;
    pw_node* source_node = nullptr;
    spa_hook node_listener{};
    pw_node_events node_events{};
    spa_hook stream_listener{};
    spa_video_info_raw format{};
    uint64_t frames = 0;
    uint64_t copied = 0;
    uint64_t unsupported = 0;
    uint64_t generation = 0;
    uint64_t last_sequence = 0;
    std::string source_serial;
    int64_t last_pts = -1;
    SourceRing<3> ring;
    std::array<std::vector<uint8_t>, 3> slot_bgra;
    uint64_t dropped = 0;
    bool failed = false;
    bool stopping = false;
    // --dmabuf
    Gpu* gpu = nullptr;
    std::vector<uint64_t> modifiers;      // importable by Vulkan, offered to the source
    uint64_t chosen_modifier = 0;
    bool dmabuf_format = false;           // the negotiated format carries a modifier
    uint64_t dmabuf_frames = 0, imported = 0, import_failures = 0;
    std::map<pw_buffer*, std::unique_ptr<vrx::DmabufImage>> images;
    std::map<pw_buffer*, bool> import_failed;
    bool readback_done = false;
    std::string dump_path;                // --dump: the read-back frame, quarter size
};

void ReadBack(Capture& capture, const vrx::DmabufImage& image) {
    Gpu& gpu = *capture.gpu;
    const VkDeviceSize bytes = VkDeviceSize(image.Width()) * image.Height() * 4;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    vkCreateBuffer(gpu.device, &bufferInfo, nullptr, &buffer);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(gpu.device, buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gpu.physical, &properties);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & host) == host) {
            allocation.memoryTypeIndex = i; break;
        }
    vkAllocateMemory(gpu.device, &allocation, nullptr, &memory);
    vkBindBufferMemory(gpu.device, buffer, memory, 0);
    vkResetCommandBuffer(gpu.command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(gpu.command, &begin);
    // Acquire the compositor's image from the foreign queue family, copy it,
    // and hand it back.
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.dstQueueFamilyIndex = gpu.family;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.image = image.Image();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.Width(), image.Height(), 1};
    vkCmdCopyImageToBuffer(gpu.command, image.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    barrier.srcQueueFamilyIndex = gpu.family;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostBarrier.srcQueueFamilyIndex = hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostBarrier.buffer = buffer;
    hostBarrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(gpu.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, nullptr, 1, &hostBarrier, 0, nullptr);
    vkEndCommandBuffer(gpu.command);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &gpu.command;
    const auto start = std::chrono::steady_clock::now();
    const VkResult submitted = vkQueueSubmit(gpu.queue, 1, &submit, gpu.fence);
    if (submitted == VK_SUCCESS) vkWaitForFences(gpu.device, 1, &gpu.fence, VK_TRUE, UINT64_MAX);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    vkResetFences(gpu.device, 1, &gpu.fence);
    void* mapped = nullptr;
    vkMapMemory(gpu.device, memory, 0, bytes, 0, &mapped);
    const auto* pixels = static_cast<const uint8_t*>(mapped);
    double sums[3]{};
    size_t samples = 0, nonblack = 0;
    for (size_t i = 0; submitted == VK_SUCCESS && i < size_t(image.Width()) * image.Height(); i += 97, ++samples) {
        const uint8_t* p = pixels + i * 4;   // B, G, R, A
        sums[0] += p[2]; sums[1] += p[1]; sums[2] += p[0];
        if (p[0] | p[1] | p[2]) ++nonblack;
    }
    std::cout << "DMA-BUF readback: " << (submitted == VK_SUCCESS ? "ok" : "submit failed")
              << " in " << std::fixed << std::setprecision(2) << ms << " ms; mean RGB "
              << std::setprecision(1) << sums[0] / std::max<size_t>(samples, 1) << ' '
              << sums[1] / std::max<size_t>(samples, 1) << ' ' << sums[2] / std::max<size_t>(samples, 1)
              << ", " << (100.0 * nonblack / std::max<size_t>(samples, 1)) << "% of samples non-black"
              << std::defaultfloat << std::endl;
    if (submitted == VK_SUCCESS && !capture.dump_path.empty()) {
        std::ofstream out(capture.dump_path, std::ios::binary);
        const uint32_t w = image.Width() / 4, h = image.Height() / 4;
        out << "P6\n" << w << ' ' << h << "\n255\n";
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const uint8_t* p = pixels + (size_t(y) * 4 * image.Width() + size_t(x) * 4) * 4;
                const char rgb[3] = {char(p[2]), char(p[1]), char(p[0])};
                out.write(rgb, 3);
            }
        std::cout << "DMA-BUF frame written to " << capture.dump_path << (out ? "" : " (failed)") << std::endl;
    }
    vkUnmapMemory(gpu.device, memory);
    vkDestroyBuffer(gpu.device, buffer, nullptr);
    vkFreeMemory(gpu.device, memory, nullptr);
}

bool CreateGpu(Gpu& gpu) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VRX DMA-BUF probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    if (vkCreateInstance(&instanceInfo, nullptr, &gpu.instance) != VK_SUCCESS) return false;
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu.physical = device; break; }
    }
    if (!gpu.physical && !devices.empty()) gpu.physical = devices.front();
    if (!gpu.physical) return false;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu.physical, &properties);
    std::cout << "Vulkan GPU: " << properties.deviceName << std::endl;
    if (!vrx::DmabufSupported(gpu.physical)) {
        std::cout << "Vulkan DMA-BUF import extensions: missing" << std::endl;
        return false;
    }
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &familyCount, families.data());
    gpu.family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gpu.family = i; break; }
    if (gpu.family == UINT32_MAX) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = gpu.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const auto extensions = vrx::DmabufDeviceExtensions();
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = uint32_t(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(gpu.physical, &deviceInfo, nullptr, &gpu.device) != VK_SUCCESS) return false;
    vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = gpu.family;
    if (vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &gpu.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandInfo.commandPool = gpu.pool;
    commandInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(gpu.device, &commandInfo, &gpu.command) != VK_SUCCESS) return false;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(gpu.device, &fenceInfo, nullptr, &gpu.fence) == VK_SUCCESS;
}

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
    if (state == PW_STREAM_STATE_ERROR ||
        (state == PW_STREAM_STATE_UNCONNECTED && !capture.stopping))
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
    if (capture.gpu) {
        uint8_t storage[4096];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_pod_prop* modifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (modifier && (modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
            // The source offered several modifiers: choose the first Vulkan can
            // import and renegotiate with that one fixed.
            uint32_t count = 0, choice = 0;
            const spa_pod* values = spa_pod_get_values(&modifier->value, &count, &choice);
            const auto* offered = static_cast<const uint64_t*>(SPA_POD_BODY(values));
            std::cout << "Source offers " << (choice == SPA_CHOICE_None ? count : count - 1) << " modifiers:";
            uint64_t chosen = 0;
            bool found = false;
            for (uint32_t i = choice == SPA_CHOICE_None ? 0 : 1; i < count; ++i) {
                std::cout << ' ' << Hex(offered[i]);
                if (!found && std::find(capture.modifiers.begin(), capture.modifiers.end(), offered[i]) !=
                    capture.modifiers.end()) { chosen = offered[i]; found = true; }
            }
            std::cout << std::endl;
            std::vector<uint64_t> fixed;
            if (found) fixed.push_back(chosen);
            std::cout << (found ? "Fixing modifier " + Hex(chosen) : std::string("No common modifier; shared memory only"))
                      << std::endl;
            auto formats = vrx::BuildCaptureFormats(&builder, fixed);
            pw_stream_update_params(capture.stream, formats.data(), uint32_t(formats.size()));
            return;
        }
        capture.dmabuf_format = (next.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
        if (capture.dmabuf_format) capture.chosen_modifier = next.modifier;
        std::cout << "Negotiated " << (capture.dmabuf_format ? "DMA-BUF, modifier " + Hex(next.modifier)
                                                              : std::string("shared memory")) << std::endl;
        const int types = capture.dmabuf_format ? (1 << SPA_DATA_DmaBuf) :
            ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        const spa_pod* buffers = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types)));
        pw_stream_update_params(capture.stream, &buffers, 1);
    }
    if (next.size.width != capture.format.size.width || next.size.height != capture.format.size.height ||
        next.format != capture.format.format) ++capture.generation;
    capture.format = next;
    std::cout << "Format generation " << capture.generation << ": " << next.size.width << 'x'
              << next.size.height << " SPA format " << next.format << std::endl;
}

void on_node_info(void* data, const pw_node_info* info) {
    auto& capture = *static_cast<Capture*>(data);
    const char* serial = info && info->props ? spa_dict_lookup(info->props, PW_KEY_OBJECT_SERIAL) : nullptr;
    if (serial && capture.source_serial != serial) {
        capture.source_serial = serial;
        std::cout << "Source object serial " << serial << std::endl;
    }
}

void on_node_param(void*, int, uint32_t id, uint32_t index, uint32_t,
                   const spa_pod* param) {
    if (id != SPA_PARAM_EnumFormat || !param) return;
    std::cout << "Source EnumFormat " << index << ':' << std::endl;
    spa_debug_pod(2, nullptr, param);
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
    if (capture.gpu && spa->n_datas >= 1 && spa->datas[0].type == SPA_DATA_DmaBuf) {
        const spa_data& plane = spa->datas[0];
        ++capture.dmabuf_frames;
        auto& image = capture.images[buffer];
        if (!image && !capture.import_failed[buffer]) {
            try {
                const auto start = std::chrono::steady_clock::now();
                image = std::make_unique<vrx::DmabufImage>(capture.gpu->physical, capture.gpu->device,
                    int(plane.fd), capture.format.size.width, capture.format.size.height,
                    VK_FORMAT_B8G8R8A8_UNORM, capture.chosen_modifier,
                    plane.chunk ? plane.chunk->offset : 0, plane.chunk ? uint32_t(plane.chunk->stride) : 0);
                ++capture.imported;
                std::cout << "Imported DMA-BUF " << capture.imported << ": fd " << plane.fd << ", planes "
                          << spa->n_datas << ", offset " << (plane.chunk ? plane.chunk->offset : 0)
                          << ", stride " << (plane.chunk ? plane.chunk->stride : 0) << ", "
                          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
                          << " ms" << std::endl;
            } catch (const std::exception& error) {
                ++capture.import_failures;
                capture.import_failed[buffer] = true;
                std::cout << "DMA-BUF import failed: " << error.what() << std::endl;
            }
        }
        if (image && !capture.readback_done) {
            ReadBack(capture, *image);
            capture.readback_done = true;
        }
        if (capture.dmabuf_frames == 1 || capture.dmabuf_frames % 120 == 0)
            std::cout << "DMA-BUF frames " << capture.dmabuf_frames << ", imported buffers "
                      << capture.imported << std::endl;
        pw_stream_queue_buffer(capture.stream, buffer);
        return;
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
void on_remove_buffer(void* data, pw_buffer* buffer) {
    auto& capture = *static_cast<Capture*>(data);
    if (capture.gpu) vkDeviceWaitIdle(capture.gpu->device);
    capture.images.erase(buffer);
    capture.import_failed.erase(buffer);
}
} // namespace

int main(int argc, char** argv) {
    Capture capture;
    int duration = 10;
    bool dmabuf = false, durationSeen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: vrx-capture-probe [seconds=10] [--dmabuf [--dump=frame.ppm]]\n"
                         "Select a window or monitor in the portal chooser; count copied BGRA frames.\n"
                         "--dmabuf offers the DRM modifiers Vulkan can import, imports DMA-BUF frames\n"
                         "and reads one back; shared memory remains the fallback.\n";
            return 0;
        }
        if (arg == "--dmabuf") { dmabuf = true; continue; }
        if (arg.rfind("--dump=", 0) == 0 && arg.size() > 7) { capture.dump_path = arg.substr(7); continue; }
        if (durationSeen) return 2;
        try { duration = std::stoi(arg); } catch (...) { return 2; }
        if (duration < 1 || duration > 300) return 2;
        durationSeen = true;
    }
    if (!capture.dump_path.empty() && !dmabuf) return 2;
    Gpu gpu;
    if (dmabuf) {
        if (!CreateGpu(gpu)) { std::cerr << "Cannot create a Vulkan device for DMA-BUF import" << std::endl; return 1; }
        capture.gpu = &gpu;
        capture.modifiers = vrx::DmabufModifiers(gpu.physical, VK_FORMAT_B8G8R8A8_UNORM);
        std::cout << "Vulkan importable B8G8R8A8 modifiers (" << capture.modifiers.size() << "):";
        for (uint64_t modifier : capture.modifiers) std::cout << ' ' << Hex(modifier);
        std::cout << std::endl;
    }
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
    gchar* printed_properties = g_variant_print(properties, TRUE);
    std::cout << "Portal selected node " << node_id << " properties "
              << printed_properties << std::endl;
    g_free(printed_properties);
    guint64 serial = 0;
    const bool has_serial = g_variant_lookup(properties, "pipewire-serial", "t", &serial);
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
    pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    capture.source_node = static_cast<pw_node*>(pw_registry_bind(
        registry, node_id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (capture.source_node) {
        capture.node_events.version = PW_VERSION_NODE_EVENTS;
        capture.node_events.info = on_node_info;
        capture.node_events.param = on_node_param;
        pw_node_add_listener(capture.source_node, &capture.node_listener, &capture.node_events, &capture);
        pw_node_enum_params(capture.source_node, 0, SPA_PARAM_EnumFormat, 0, UINT32_MAX, nullptr);
        for (int i = 0; i < 20; ++i) pw_loop_iterate(pw_main_loop_get_loop(pw_loop), 50);
    }
    const std::string target = has_serial ? std::to_string(serial) :
        (!capture.source_serial.empty() ? capture.source_serial : std::to_string(node_id));
    std::cout << "Target object " << target
              << ((has_serial || !capture.source_serial.empty()) ? " (serial)" : " (node ID fallback)")
              << std::endl;
    capture.stream = pw_stream_new(core, "VRX capture probe", pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_TARGET_OBJECT, target.c_str(), nullptr));
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = on_state;
    events.param_changed = on_format;
    events.process = on_process;
    events.remove_buffer = on_remove_buffer;
    pw_stream_add_listener(capture.stream, &capture.stream_listener, &events, &capture);
    uint8_t pod_storage[4096];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    auto formats = vrx::BuildCaptureFormats(&builder, capture.modifiers);
    if (pw_stream_connect(capture.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), formats.data(), uint32_t(formats.size())) < 0) {
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
    if (dmabuf)
        std::cout << "DMA-BUF result: format " << (capture.dmabuf_format ? "dmabuf" : "shared memory")
                  << " modifier " << Hex(capture.chosen_modifier) << " frames=" << capture.dmabuf_frames
                  << " imported=" << capture.imported << " import_failures=" << capture.import_failures
                  << " readback=" << (capture.readback_done ? "yes" : "no") << std::endl;
    capture.stopping = true;
    pw_stream_destroy(capture.stream);
    if (capture.source_node) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(capture.source_node));
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(pw_loop);
    xdp_session_close(capture.session);
    g_object_unref(capture.session);
    g_object_unref(capture.portal);
    g_main_loop_unref(capture.portal_loop);
    pw_deinit();
    capture.images.clear();
    return !capture.failed && (capture.copied > 0 || capture.imported > 0) ? 0 : 1;
}
