// Linux OpenXR/Vulkan diagnostic renderer for synthetic, still and live portal input.
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "stereo_warp.h"
#include "vulkan_warp.h"
#include "vulkan_prep.h"
#ifdef VRX_HAS_CAPTURE
#include "portal_capture.h"
#endif
#ifdef VRX_HAS_LIVE_DEPTH
#include "live_depth.h"
#endif
#ifdef VRX_HAS_MODEL
#include "model_depth.h"
#include "still_image.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <exception>
#include <thread>
#include <string>
#include <vector>

namespace {
struct XrFns {
    PFN_xrGetInstanceProcAddr get = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrCreateReferenceSpace createSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats formats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages images = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrAcquireSwapchainImage acquire = nullptr;
    PFN_xrWaitSwapchainImage waitImage = nullptr;
    PFN_xrReleaseSwapchainImage release = nullptr;
};

template <typename T> bool Resolve(XrFns& f, XrInstance instance, const char* name, T& out) {
    PFN_xrVoidFunction raw = nullptr;
    XrResult result = f.get(instance, name, &raw);
    if (XR_FAILED(result) || !raw) {
        std::fprintf(stderr, "%s unavailable: %d\n", name, result);
        return false;
    }
    out = reinterpret_cast<T>(raw);
    return true;
}
#define XR_RESOLVE(name, field) if (!Resolve(xr, instance, name, xr.field)) return 1
#define XR_CHECK(expr) do { XrResult r = (expr); if (XR_FAILED(r)) { \
    std::fprintf(stderr, "%s failed: %d\n", #expr, r); return 1; } } while (0)
#define VK_CHECK(expr) do { VkResult r = (expr); if (r != VK_SUCCESS) { \
    std::fprintf(stderr, "%s failed: %d\n", #expr, r); return 1; } } while (0)

int Run(double seconds, const char* loaderPath, const char* stillPath, const char* modelPath, bool cuda, bool live) {
#ifdef VRX_HAS_CAPTURE
    std::unique_ptr<vrx::PortalCapture> capture;
    if (live) {
        capture = std::make_unique<vrx::PortalCapture>();
        if (!capture->Open()) { std::fputs("Live capture source selection failed\n", stderr); return 1; }
        vrx::PortalCapture::Frame first;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->Latest(first) && capture->Healthy() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (first.rgb.empty()) { std::fputs("No live capture frames received\n", stderr); return 1; }
        std::printf("First live source frame %llu, layout %llu\n",
                    static_cast<unsigned long long>(first.sequence),
                    static_cast<unsigned long long>(first.layout));
    }
#ifdef VRX_HAS_LIVE_DEPTH
    std::unique_ptr<vrx::LiveDepth> liveDepth;
    if (live && cuda) liveDepth = std::make_unique<vrx::LiveDepth>(*capture, modelPath, true);
#endif
#else
    (void)live;
#endif
    void* library = dlopen(loaderPath, RTLD_NOW | RTLD_LOCAL);
    std::string steamLoader;
    if (!library && std::strcmp(loaderPath, "libopenxr_loader.so.1") == 0) {
        const char* home = std::getenv("HOME");
        if (home && *home) {
            steamLoader = std::string(home) +
                "/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/libopenxr_loader.so";
            library = dlopen(steamLoader.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (library) std::printf("OpenXR loader: %s\n", steamLoader.c_str());
        }
    }
    if (!library) { std::fprintf(stderr, "OpenXR loader: %s\n", dlerror()); return 1; }
    XrFns xr{};
    xr.get = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(library, "xrGetInstanceProcAddr"));
    if (!xr.get) { std::fputs("OpenXR loader has no xrGetInstanceProcAddr\n", stderr); return 1; }
    if (!Resolve(xr, XR_NULL_HANDLE, "xrCreateInstance", xr.createInstance)) return 1;
    XrInstanceCreateInfo ici{};
    ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strncpy(ici.applicationInfo.applicationName, "VRX Linux synthetic", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ici.applicationInfo.engineName, "VRX", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* extension = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = &extension;
    XrInstance instance = XR_NULL_HANDLE;
    XR_CHECK(xr.createInstance(&ici, &instance));
    XR_RESOLVE("xrDestroyInstance", destroyInstance);
    XR_RESOLVE("xrGetSystem", getSystem);
    XR_RESOLVE("xrGetVulkanGraphicsRequirements2KHR", requirements);
    XR_RESOLVE("xrCreateVulkanInstanceKHR", createVulkanInstance);
    XR_RESOLVE("xrGetVulkanGraphicsDevice2KHR", graphicsDevice);
    XR_RESOLVE("xrCreateVulkanDeviceKHR", createVulkanDevice);
    XR_RESOLVE("xrCreateSession", createSession);
    XR_RESOLVE("xrDestroySession", destroySession);
    XR_RESOLVE("xrCreateReferenceSpace", createSpace);
    XR_RESOLVE("xrDestroySpace", destroySpace);
    XR_RESOLVE("xrEnumerateSwapchainFormats", formats);
    XR_RESOLVE("xrCreateSwapchain", createSwapchain);
    XR_RESOLVE("xrDestroySwapchain", destroySwapchain);
    XR_RESOLVE("xrEnumerateSwapchainImages", images);
    XR_RESOLVE("xrPollEvent", pollEvent);
    XR_RESOLVE("xrBeginSession", beginSession);
    XR_RESOLVE("xrEndSession", endSession);
    XR_RESOLVE("xrWaitFrame", waitFrame);
    XR_RESOLVE("xrBeginFrame", beginFrame);
    XR_RESOLVE("xrEndFrame", endFrame);
    XR_RESOLVE("xrAcquireSwapchainImage", acquire);
    XR_RESOLVE("xrWaitSwapchainImage", waitImage);
    XR_RESOLVE("xrReleaseSwapchainImage", release);

    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XR_CHECK(xr.getSystem(instance, &systemInfo, &system));
    XrGraphicsRequirementsVulkan2KHR req{};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
    XR_CHECK(xr.requirements(instance, system, &req));
    std::printf("OpenXR Vulkan API range: %u.%u to %u.%u\n",
        XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
        XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VRX Linux synthetic";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo vici{};
    vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xvici{};
    xvici.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    xvici.systemId = system;
    xvici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvici.vulkanCreateInfo = &vici;
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkResult vkResult = VK_SUCCESS;
    XR_CHECK(xr.createVulkanInstance(instance, &xvici, &vkInstance, &vkResult));
    if (vkResult != VK_SUCCESS) { std::fprintf(stderr, "vkCreateInstance: %d\n", vkResult); return 1; }
    XrVulkanGraphicsDeviceGetInfoKHR gd{};
    gd.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
    gd.systemId = system;
    gd.vulkanInstance = vkInstance;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    XR_CHECK(xr.graphicsDevice(instance, &gd, &gpu));
    VkPhysicalDeviceProperties gpuProps{};
    vkGetPhysicalDeviceProperties(gpu, &gpuProps);
    std::printf("OpenXR Vulkan GPU: %s\n", gpuProps.deviceName);
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family = i; break; }
    if (family == UINT32_MAX) { std::fputs("No graphics queue\n", stderr); return 1; }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo vdci{};
    vdci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    vdci.queueCreateInfoCount = 1;
    vdci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xvdci{};
    xvdci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
    xvdci.systemId = system;
    xvdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvdci.vulkanPhysicalDevice = gpu;
    xvdci.vulkanCreateInfo = &vdci;
    VkDevice device = VK_NULL_HANDLE;
    XR_CHECK(xr.createVulkanDevice(instance, &xvdci, &device, &vkResult));
    if (vkResult != VK_SUCCESS) { std::fprintf(stderr, "vkCreateDevice: %d\n", vkResult); return 1; }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    XrGraphicsBindingVulkan2KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
    binding.instance = vkInstance;
    binding.physicalDevice = gpu;
    binding.device = device;
    binding.queueFamilyIndex = family;
    XrSessionCreateInfo sci{};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    XR_CHECK(xr.createSession(instance, &sci, &session));
    XrReferenceSpaceCreateInfo rsci{};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xr.createSpace(session, &rsci, &space));
    uint32_t formatCount = 0;
    XR_CHECK(xr.formats(session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xr.formats(session, formatCount, &formatCount, formats.data()));
    VkFormat format = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM}) {
        if (std::find(formats.begin(), formats.end(), int64_t(candidate)) != formats.end()) {
            format = candidate; break;
        }
    }
    if (format == VK_FORMAT_UNDEFINED) { std::fputs("No RGBA/BGRA swapchain format\n", stderr); return 1; }
    XrSwapchainCreateInfo swci{};
    swci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swci.format = format;
    swci.sampleCount = 1;
    swci.width = vrx::kSyntheticWidth;
    swci.height = vrx::kSyntheticHeight;
    swci.faceCount = 1;
    swci.arraySize = 2;
    swci.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    XR_CHECK(xr.createSwapchain(session, &swci, &swapchain));
    uint32_t imageCount = 0;
    XR_CHECK(xr.images(swapchain, 0, &imageCount, nullptr));
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount);
    for (auto& image : images) image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    XR_CHECK(xr.images(swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

    constexpr VkDeviceSize eyeBytes = VkDeviceSize(vrx::kSyntheticWidth) * vrx::kSyntheticHeight * 4;
    auto warp = std::make_unique<vrx::VulkanWarp>(gpu, device, format);
    auto prep = std::make_unique<vrx::VulkanPrep>(gpu, device, warp->SceneBuffer());
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbi, &command));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    std::vector<unsigned char> stillRgb;
    std::vector<float> stillDepth;
    bool prepMatched = true;
#ifdef VRX_HAS_MODEL
    if (stillPath) {
        stillRgb = vrx::LoadStillPng(stillPath);
        vrx::ModelDepth model(modelPath, cuda);
        std::vector<float> flat(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight, 0.0f);
        warp->Upload(stillRgb, flat);
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &cbbi));
        prep->Record(command);
        VK_CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fence));
        prepMatched = prep->CompareReference(stillRgb);
        if (!prepMatched) std::fputs("Still-image GPU preprocessing differs from CPU reference\n", stderr);
        stillDepth = model.Run(prep->ModelInput());
    }
#else
    (void)modelPath;
    (void)cuda;
#endif

    XrCompositionLayerQuad quads[2]{};
    const XrCompositionLayerBaseHeader* layers[2]{};
    for (int eye = 0; eye < 2; ++eye) {
        quads[eye].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quads[eye].space = space;
        quads[eye].eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
        quads[eye].subImage.swapchain = swapchain;
        quads[eye].subImage.imageRect.extent = {vrx::kSyntheticWidth, vrx::kSyntheticHeight};
        quads[eye].subImage.imageArrayIndex = eye;
        quads[eye].pose.orientation.w = 1.0f;
        quads[eye].pose.position.z = -2.0f;
        quads[eye].size = {2.0f, 2.0f * vrx::kSyntheticHeight / vrx::kSyntheticWidth};
        layers[eye] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[eye]);
    }
    const auto beginTime = std::chrono::steady_clock::now();
    bool running = false;
    int frames = 0;
    int renderedFrames = 0;
    int skippedFrames = 0;
    bool referenceMatched = prepMatched;
    bool captureLost = false;
    bool depthFailed = false;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - beginTime).count() < seconds) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult eventResult = xr.pollEvent(instance, &event);
        while (eventResult == XR_SUCCESS) {
            auto* header = reinterpret_cast<XrEventDataBaseHeader*>(&event);
            if (header->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                std::printf("OpenXR session state: %d\n", state->state);
                if (state->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{};
                    bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xr.beginSession(session, &bi));
                    running = true;
                } else if (state->state == XR_SESSION_STATE_STOPPING) {
                    XR_CHECK(xr.endSession(session));
                    running = false;
                } else if (state->state == XR_SESSION_STATE_EXITING || state->state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = false;
                    break;
                }
            }
            event = {};
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
            eventResult = xr.pollEvent(instance, &event);
        }
        if (XR_FAILED(eventResult)) { std::fprintf(stderr, "xrPollEvent: %d\n", eventResult); break; }
#ifdef VRX_HAS_CAPTURE
        if (live && !capture->Healthy()) {
            std::fputs("Live capture stopped; select a source again\n", stderr);
            captureLost = true;
            break;
        }
#endif
#ifdef VRX_HAS_LIVE_DEPTH
        if (liveDepth && !liveDepth->Healthy()) {
            std::fprintf(stderr, "Live depth failed: %s\n", liveDepth->Error().c_str());
            depthFailed = true;
            break;
        }
#endif
        if (!running) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        XrFrameWaitInfo wi{}; wi.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState state{}; state.type = XR_TYPE_FRAME_STATE;
        XR_CHECK(xr.waitFrame(session, &wi, &state));
        XrFrameBeginInfo fbi{}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
        XR_CHECK(xr.beginFrame(session, &fbi));
        uint32_t index = 0;
        if (state.shouldRender) {
            XrSwapchainImageAcquireInfo ai{}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            XR_CHECK(xr.acquire(swapchain, &ai, &index));
            XrSwapchainImageWaitInfo swi{};
            swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            swi.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xr.waitImage(swapchain, &swi));
            std::vector<unsigned char> rgb;
            std::vector<float> truth;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - beginTime).count();
#ifdef VRX_HAS_CAPTURE
            if (live) {
                vrx::PortalCapture::Frame latest;
                if (capture->Latest(latest)) rgb = std::move(latest.rgb);
#ifdef VRX_HAS_LIVE_DEPTH
                const auto depth = liveDepth ? liveDepth->Latest() : nullptr;
                if (depth) truth = depth->near;
                else
#endif
                    truth.assign(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight, 0.0f);
            } else
#endif
            if (stillPath) {
                rgb = stillRgb;
                truth = stillDepth;
            } else {
                vrx::MakeScene(rgb, elapsed);
                vrx::MakeTruth(truth, elapsed);
            }
            warp->Upload(rgb, truth);
            VK_CHECK(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo cbbi{}; cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(command, &cbbi));
            if (!stillPath && !live) prep->Record(command);
            warp->Record(command);
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index].image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 2;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkBufferImageCopy copies[2]{};
            for (int eye = 0; eye < 2; ++eye) {
                copies[eye].bufferOffset = eye * eyeBytes;
                copies[eye].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copies[eye].imageSubresource.layerCount = 1;
                copies[eye].imageSubresource.baseArrayLayer = eye;
                copies[eye].imageExtent = {vrx::kSyntheticWidth, vrx::kSyntheticHeight, 1};
            }
            vkCmdCopyBufferToImage(command, warp->ColorBuffer(), images[index].image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VK_CHECK(vkEndCommandBuffer(command));
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
            VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
            VK_CHECK(vkResetFences(device, 1, &fence));
            if (renderedFrames == 0)
                referenceMatched = referenceMatched && warp->CompareReference(rgb, truth) &&
                    (stillPath || live || prep->CompareReference(rgb));
            XrSwapchainImageReleaseInfo ri{}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            XR_CHECK(xr.release(swapchain, &ri));
            ++renderedFrames;
        }
        if (!state.shouldRender) ++skippedFrames;
        XrFrameEndInfo fe{};
        fe.type = XR_TYPE_FRAME_END_INFO;
        fe.displayTime = state.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = state.shouldRender ? 2u : 0u;
        fe.layers = state.shouldRender ? layers : nullptr;
        XR_CHECK(xr.endFrame(session, &fe));
        ++frames;
    }
    std::printf("Submitted %d OpenXR frames (%d with an image, %d skipped by runtime)\n",
                frames, renderedFrames, skippedFrames);
#ifdef VRX_HAS_CAPTURE
    if (live) std::printf("Live capture: %llu frames, %llu dropped\n",
        static_cast<unsigned long long>(capture->Captured()),
        static_cast<unsigned long long>(capture->Dropped()));
#endif
#ifdef VRX_HAS_LIVE_DEPTH
    if (liveDepth) {
        const auto depth = liveDepth->Latest();
        std::printf("Live ZipDepth: %llu updates, latest source sequence %llu\n",
            static_cast<unsigned long long>(liveDepth->Completed()),
            static_cast<unsigned long long>(depth ? depth->sourceSequence : 0));
        const auto timings = liveDepth->Timings();
        std::printf("Live ZipDepth timing (%llu samples): prep %.2f/%.2f ms, model %.2f/%.2f ms, arrival-to-depth %.2f/%.2f ms (median/p95)\n",
            static_cast<unsigned long long>(timings.samples),
            timings.prepMedianMs, timings.prepP95Ms,
            timings.modelMedianMs, timings.modelP95Ms,
            timings.arrivalToDepthMedianMs, timings.arrivalToDepthP95Ms);
    }
#endif
    vkDeviceWaitIdle(device);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    xr.destroySwapchain(swapchain);
    xr.destroySpace(space);
    xr.destroySession(session);
    prep.reset();
    warp.reset();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    xr.destroyInstance(instance);
    dlclose(library);
    return renderedFrames > 0 && referenceMatched && !captureLost && !depthFailed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    double seconds = 10.0;
    const char* stillPath = nullptr;
    const char* modelPath = "bench/models/zipdepth_faithful_fp16_672x384.onnx";
    bool cuda = false, live = false, durationSeen = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("vrx-xr-synthetic [seconds=10] [--still=picture.png] [--model=model.onnx] [--cuda] [--live]");
            std::puts("--live selects a portal source; add --cuda for asynchronous ZipDepth");
            std::puts("VRX_OPENXR_LOADER, VRX_WARP_SPV and VRX_PREP_SPV override runtime paths");
            return 0;
        }
        if (std::strncmp(argv[i], "--still=", 8) == 0) stillPath = argv[i] + 8;
        else if (std::strncmp(argv[i], "--model=", 8) == 0) modelPath = argv[i] + 8;
        else if (std::strcmp(argv[i], "--cuda") == 0) cuda = true;
        else if (std::strcmp(argv[i], "--live") == 0) live = true;
        else if (!durationSeen) {
            char* end = nullptr;
            seconds = std::strtod(argv[i], &end);
            if (!end || *end) { std::fputs("Invalid duration\n", stderr); return 2; }
            durationSeen = true;
        } else { std::fputs("Unexpected argument\n", stderr); return 2; }
    }
    if (seconds <= 0 || seconds > 120 || (cuda && !stillPath && !live) || (live && stillPath) ||
        (stillPath && !*stillPath) || (modelPath && !*modelPath)) {
        std::fputs("usage: vrx-xr-synthetic [0 < seconds <= 120] [--still=picture.png] [--model=model.onnx] [--cuda] [--live]\n", stderr);
        return 2;
    }
#ifndef VRX_HAS_CAPTURE
    if (live) { std::fputs("Live capture support was not built (requires libportal and PipeWire)\n", stderr); return 2; }
#endif
#ifndef VRX_HAS_MODEL
    if (stillPath) { std::fputs("Still/model support was not built (requires ONNX Runtime and libpng)\n", stderr); return 2; }
#endif
#ifndef VRX_HAS_LIVE_DEPTH
    if (live && cuda) { std::fputs("Live ZipDepth support was not built\n", stderr); return 2; }
#endif
    const char* path = std::getenv("VRX_OPENXR_LOADER");
    try {
        return Run(seconds, path && *path ? path : "libopenxr_loader.so.1", stillPath, modelPath, cuda, live);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VRX Linux renderer: %s\n", error.what());
        return 1;
    }
}
