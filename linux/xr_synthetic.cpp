// Linux OpenXR/Vulkan diagnostic renderer for synthetic, still and live portal input.
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "stereo_warp.h"
#include "vulkan_warp.h"
#include "live_settings.h"
#include "vulkan_prep.h"
#include "vulkan_room.h"
#include "screen_anchor.h"
#include "vulkan_capture_scale.h"
#include "vulkan_dmabuf.h"
#include "playback_policy.h"
#include "frame_timing.h"
#include "vulkan_frame_history.h"
#include "capture_scale.h"
#include "resource_path.h"
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
#include <cmath>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <limits>
#include <exception>
#include <thread>
#include <string>
#include <vector>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void OnStopSignal(int) { stopRequested = 1; }
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
    PFN_xrEnumerateReferenceSpaces enumerateSpaces = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats formats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages images = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfig = nullptr;
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

int Run(double seconds, const char* loaderPath, const char* stillPath, const char* modelPath, bool cuda, bool live, bool roomEnabled, const char* settingsPath, const char* roomDumpPath,
        uint32_t syntheticColorWidth, uint32_t syntheticColorHeight, bool useDmabuf, const char* colorDumpPath,
        int sourceKind, int curvePercent) {
    vrx::LiveSettings settings;
    if (settingsPath && !vrx::ReadLiveSettings(settingsPath, settings)) {
        std::fprintf(stderr, "Invalid or missing Linux settings: %s\n", settingsPath);
        return 2;
    }
    // Colour is presented at the source's size (live) or the depth grid's.
    uint32_t colorWidth = vrx::kSyntheticWidth, colorHeight = vrx::kSyntheticHeight;
    if (syntheticColorWidth) { colorWidth = syntheticColorWidth; colorHeight = syntheticColorHeight; }
#ifdef VRX_HAS_CAPTURE
    std::unique_ptr<vrx::PortalCapture> capture;
#endif
#ifdef VRX_HAS_LIVE_DEPTH
    std::unique_ptr<vrx::LiveDepth> liveDepth;
#endif
#ifndef VRX_HAS_CAPTURE
    (void)live;
    (void)useDmabuf;
#endif
    roomEnabled = roomEnabled || (settingsPath && settings.room > 0);
    if (curvePercent >= 0 && !settingsPath) settings.curve = curvePercent;
    // The eye layer: a projection layer drawn on the GPU (VulkanRoom) that
    // carries the room and a curved screen. With live settings it is always
    // made, so the curve can be turned on during play.
    const bool eyeLayer = roomEnabled || settingsPath || settings.curve > 0;
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
    if (eyeLayer) {
        XR_RESOLVE("xrEnumerateReferenceSpaces", enumerateSpaces);
        XR_RESOLVE("xrLocateSpace", locateSpace);
    }
    XR_RESOLVE("xrDestroySpace", destroySpace);
    XR_RESOLVE("xrEnumerateSwapchainFormats", formats);
    XR_RESOLVE("xrCreateSwapchain", createSwapchain);
    XR_RESOLVE("xrDestroySwapchain", destroySwapchain);
    XR_RESOLVE("xrEnumerateSwapchainImages", images);
    XR_RESOLVE("xrPollEvent", pollEvent);
    XR_RESOLVE("xrBeginSession", beginSession);
    XR_RESOLVE("xrEndSession", endSession);
    XR_RESOLVE("xrWaitFrame", waitFrame);
    XR_RESOLVE("xrLocateViews", locateViews);
    if (eyeLayer) XR_RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfig);
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
    // The room layer's size per eye: the runtime's recommendation, unless
    // VRX_ROOM_EYE_SCALE (0.25..1) asks for less. Windows draws it at half size,
    // but with SteamVR/Steam Link on Linux the screen quads were then shown at
    // the room layer's resolution too, visibly blurring the picture.
    uint32_t roomEyeWidth = 1322, roomEyeHeight = 1322;
    if (eyeLayer) {
        uint32_t viewCount = 0;
        XR_CHECK(xr.enumerateViewConfig(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         0, &viewCount, nullptr));
        std::vector<XrViewConfigurationView> configViews(viewCount);
        for (auto& view : configViews) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        XR_CHECK(xr.enumerateViewConfig(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         viewCount, &viewCount, configViews.data()));
        float scale = 1.0f;
        if (const char* text = std::getenv("VRX_ROOM_EYE_SCALE")) {
            const float requested = std::strtof(text, nullptr);
            if (requested >= 0.25f && requested <= 1.0f) scale = requested;
        }
        if (viewCount >= 2) {
            const auto& view = configViews[0];
            roomEyeWidth = std::max(64u, std::min(uint32_t(view.recommendedImageRectWidth * scale + 0.5f),
                                                  view.maxImageRectWidth)) & ~1u;
            roomEyeHeight = std::max(64u, std::min(uint32_t(view.recommendedImageRectHeight * scale + 0.5f),
                                                   view.maxImageRectHeight)) & ~1u;
            std::printf("OpenXR recommended eye size: %ux%u (room layer %ux%u)\n",
                view.recommendedImageRectWidth, view.recommendedImageRectHeight, roomEyeWidth, roomEyeHeight);
        }
    }
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
    // The source may hand over DMA-BUFs in any layout this GPU can import.
    std::vector<uint64_t> dmabufModifiers;
#ifdef VRX_HAS_CAPTURE
    if (live) {
        if (useDmabuf && vrx::DmabufSupported(gpu))
            dmabufModifiers = vrx::DmabufModifiers(gpu, VK_FORMAT_B8G8R8A8_UNORM);
        std::printf("Capture DMA-BUF: %s (%zu importable modifiers)\n",
            !useDmabuf ? "disabled" : dmabufModifiers.empty() ? "unavailable" : "offered", dmabufModifiers.size());
        capture = std::make_unique<vrx::PortalCapture>();
        const auto source = sourceKind == 1 ? vrx::PortalCapture::Source::Window :
                            sourceKind == 2 ? vrx::PortalCapture::Source::Screen : vrx::PortalCapture::Source::Any;
        if (!capture->Open(dmabufModifiers, source)) { std::fputs("Live capture source selection failed\n", stderr); return 1; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->Captured() && capture->Healthy() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!capture->Captured() || !capture->ColorSize(colorWidth, colorHeight)) {
            std::fputs("No live capture frames received\n", stderr);
            return 1;
        }
        std::printf("First live source frame: colour %ux%u, %s\n", colorWidth, colorHeight,
                    capture->DmaBuf() ? "DMA-BUF" : "shared memory");
#ifdef VRX_HAS_LIVE_DEPTH
        if (cuda) liveDepth = std::make_unique<vrx::LiveDepth>(modelPath, true);
#endif
    }
#endif
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
    const auto dmabufExtensions = vrx::DmabufDeviceExtensions();
    if (!dmabufModifiers.empty()) {
        vdci.enabledExtensionCount = uint32_t(dmabufExtensions.size());
        vdci.ppEnabledExtensionNames = dmabufExtensions.data();
    }
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
    XrSpace stageSpace = XR_NULL_HANDLE;
    if (eyeLayer) {
        uint32_t referenceCount = 0;
        XR_CHECK(xr.enumerateSpaces(session, 0, &referenceCount, nullptr));
        std::vector<XrReferenceSpaceType> referenceTypes(referenceCount);
        XR_CHECK(xr.enumerateSpaces(session, referenceCount, &referenceCount, referenceTypes.data()));
        if (std::find(referenceTypes.begin(), referenceTypes.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != referenceTypes.end()) {
            XrReferenceSpaceCreateInfo stageInfo = rsci;
            stageInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            XrResult stageResult = xr.createSpace(session, &stageInfo, &stageSpace);
            if (XR_SUCCEEDED(stageResult)) std::puts("Room floor: OpenXR STAGE space available");
            else std::printf("Room floor: STAGE space unavailable (%d); using seated estimate\n", stageResult);
        } else std::puts("Room floor: STAGE space unavailable; using seated estimate");
    }
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
    swci.width = colorWidth;
    swci.height = colorHeight;
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

    const VkDeviceSize eyeBytes = VkDeviceSize(colorWidth) * colorHeight * 4;
    const float screenAspect = float(colorHeight) / float(colorWidth);
    auto warp = std::make_unique<vrx::VulkanWarp>(gpu, device, format, colorWidth, colorHeight);
    // ZipDepth input from the colour buffer, as xrapp5 prepares it from the source texture.
    auto prep = std::make_unique<vrx::VulkanPrep>(gpu, device, warp->SceneBuffer(), colorWidth, colorHeight);
    std::unique_ptr<vrx::VulkanRoom> room;
    XrSwapchain roomSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> roomImages;
    if (eyeLayer) {
        XrSwapchainCreateInfo roomInfo = swci;
        roomInfo.width = roomEyeWidth;
        roomInfo.height = roomEyeHeight;
        XR_CHECK(xr.createSwapchain(session, &roomInfo, &roomSwapchain));
        uint32_t roomImageCount = 0;
        XR_CHECK(xr.images(roomSwapchain, 0, &roomImageCount, nullptr));
        roomImages.resize(roomImageCount);
        for (auto& image : roomImages) image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        XR_CHECK(xr.images(roomSwapchain, roomImageCount, &roomImageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(roomImages.data())));
        room = std::make_unique<vrx::VulkanRoom>(gpu, device, warp->SceneBuffer(), warp->ColorBuffer(), format,
                                                 colorWidth, colorHeight, roomEyeWidth, roomEyeHeight);
        std::puts(roomEnabled ? "Windows room shader enabled: lighting, glass, tiles and reflections"
                              : "Eye layer enabled for a curved screen (the room is off)");
    }
    // DMA-BUF capture: source buffers imported once each (by buffer id, until
    // the source renegotiates) and scaled into the colour buffer on the GPU.
    std::unique_ptr<vrx::VulkanCaptureScale> captureScale;
    std::map<uint64_t, std::unique_ptr<vrx::DmabufImage>> imports;
    uint64_t importGeneration = UINT64_MAX;
    if (!dmabufModifiers.empty())
        captureScale = std::make_unique<vrx::VulkanCaptureScale>(gpu, device, family, warp->SceneBuffer(),
                                                                 colorWidth, colorHeight);
    // Live frames kept on the GPU for Delayed and Matched frame timing.
    std::unique_ptr<vrx::VulkanFrameHistory> history;
    if (live)
        history = std::make_unique<vrx::VulkanFrameHistory>(gpu, device, warp->SceneBuffer(),
                                                            VkDeviceSize(colorWidth) * colorHeight * 4);
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
    // Two frames in flight: the CPU records frame N+1 while the GPU runs frame N.
    constexpr uint32_t kFrameSlots = vrx::VulkanWarp::kFrameSlots;
    cbi.commandBufferCount = kFrameSlots;
    VkCommandBuffer commands[kFrameSlots]{};
    VK_CHECK(vkAllocateCommandBuffers(device, &cbi, commands));
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fences[kFrameSlots]{};
    for (VkFence& slotFence : fences) VK_CHECK(vkCreateFence(device, &fci, nullptr, &slotFence));
    VkCommandBuffer command = commands[0];
    VkFence fence = fences[0];
    // GPU timestamps per frame in flight: start, after capture scale, depth
    // prep and warp, after the room, and after the swapchain copies. Read when
    // the frame's fence has signalled, so reading never waits.
    constexpr uint32_t kTimestamps = 4;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    if (families[family].timestampValidBits) {
        VkQueryPoolCreateInfo qpci{};
        qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = kFrameSlots * kTimestamps;
        VK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &timestamps));
    }
    const double timestampMs = gpuProps.limits.timestampPeriod / 1e6;

    std::vector<unsigned char> stillRgb;
    std::vector<float> stillDepth;
    bool prepMatched = true;
#ifdef VRX_HAS_MODEL
    if (stillPath) {
        stillRgb = vrx::LoadStillPng(stillPath);
        vrx::ModelDepth model(modelPath, cuda);
        std::vector<float> flat(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight, 0.0f);
        warp->UploadColor(vrx::PackRgb(stillRgb));
        warp->UploadNearness(flat);
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &cbbi));
        warp->RecordUpload(command);
        prep->Record(command);
        VK_CHECK(vkEndCommandBuffer(command));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(device, 1, &fence));
        prepMatched = prep->CompareReference(vrx::PackRgb(stillRgb));
        if (!prepMatched) std::fputs("Still-image GPU preprocessing differs from CPU reference\n", stderr);
        stillDepth = model.Run(prep->ModelInput());
    }
#else
    (void)modelPath;
    (void)cuda;
#endif

    warp->SetStrength(settings.strength);
    warp->SetStereoGeometry(settings.distance, settings.width, 0.064f, true);
    XrCompositionLayerQuad quads[2]{};
    XrCompositionLayerProjection projection{};
    XrCompositionLayerProjectionView projectionViews[2]{};
    projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    projection.space = space;
    projection.viewCount = 2;
    projection.views = projectionViews;
    for (int eye = 0; eye < 2; ++eye) {
        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projectionViews[eye].subImage.swapchain = roomSwapchain;
        projectionViews[eye].subImage.imageRect.extent = {
            int32_t(roomEyeWidth), int32_t(roomEyeHeight)};
        projectionViews[eye].subImage.imageArrayIndex = eye;
    }
    for (int eye = 0; eye < 2; ++eye) {
        quads[eye].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quads[eye].space = space;
        quads[eye].eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
        quads[eye].subImage.swapchain = swapchain;
        quads[eye].subImage.imageRect.extent = {int32_t(colorWidth), int32_t(colorHeight)};
        quads[eye].subImage.imageArrayIndex = eye;
        quads[eye].pose.orientation.w = 1.0f;
    }
    const auto beginTime = std::chrono::steady_clock::now();
    bool running = false;
    // The runtime ended the session: EXITING is a normal end (SteamVR or the
    // user closed the app), LOSS_PENDING an error (the session was lost).
    bool sessionExiting = false, sessionLost = false;
    int frames = 0;
    int renderedFrames = 0;
    int skippedFrames = 0;
    bool referenceMatched = prepMatched;
    bool captureLost = false;
    bool depthFailed = false;
    std::vector<double> roomPrepMs, gpuWaitMs, frameWorkMs, endFrameMs;
    // colorWidth x colorHeight packed RGBA; kept across frames to reuse memory.
    std::vector<uint32_t> color;
#ifdef VRX_HAS_CAPTURE
    vrx::PortalCapture::Frame liveFrame;
#endif
    // The live frame now in the colour buffer. Live room glow is computed from a
    // quarter-size picture of the latest frame's model input: the glow only
    // averages the picture's edges, and converting it must stay cheap.
    uint64_t liveSequence = 0, liveLayout = 0;
    double liveArrival = 0;
    constexpr size_t modelPlane = size_t(vrx::VulkanPrep::width) * vrx::VulkanPrep::height;
    constexpr uint32_t ambientWidth = vrx::VulkanPrep::width / 4, ambientHeight = vrx::VulkanPrep::height / 4;
    std::vector<uint32_t> ambient(size_t(ambientWidth) * ambientHeight, 0xff000000u);
    bool laterDumped = false;
    // The screen, as xrapp5 places it: straight ahead of the headset on the
    // first tracked frame and on each Recenter, then moved by the settings
    // relative to that point. With the room on it keeps only the heading.
    ScreenAnchor screen;
    // Frame timing (frame_timing.h): what each history slot holds, which slot
    // the colour buffer holds, and the smoothed delay from capture to depth.
    struct KeptFrame {
        bool valid = false;
        uint64_t sequence = 0, layout = 0;
        double arrival = 0;
    };
    KeptFrame kept[vrx::VulkanFrameHistory::kSlots];
    uint32_t nextKept = 0;
    int sceneSlot = -1;
    DepthDelayEstimate depthDelay;
    uint64_t lastDepthSequence = 0;
    int loggedTiming = -1;
    // The curved screen and what it was built from (negative: not yet built).
    Cylinder cylinder;
    float curveWidth = -1.0f, curveHeight = -1.0f, curveDistance = -1.0f, curveFraction = -1.0f;
    // The room's floor, read from STAGE once per placement (or reference-space change).
    float stageFloorY = std::numeric_limits<float>::quiet_NaN();
    bool floorLatched = false;
    // What each in-flight frame hands on once its GPU work has completed.
    struct SlotWork {
        bool submitted = false;
        bool liveNew = false;            // its model input goes to depth and room glow
        uint64_t sequence = 0, layout = 0;
        double arrival = 0;
        bool heldGpuFrame = false;       // its DMA-BUF goes back to the source
        bool timed = false, roomDrawn = false;
    };
    // The two-second performance report (as xrapp5's), accumulated per frame.
    struct Report {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        int frames = 0, drawn = 0;
        double cpuMs = 0;
        int depthFrames = 0, shownFrames = 0;
        double depthAgeMs = 0, shownAgeMs = 0;
        uint64_t captured = 0, dropped = 0, depthUpdates = 0;
        std::vector<double> gpuMs, prepWarpMs, roomMs, copyMs;
    } report;
#ifdef VRX_HAS_CAPTURE
    if (live) {
        report.captured = capture->Captured();
        report.dropped = capture->Dropped();
    }
#endif
    auto nowSeconds = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    auto median = [](std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    };
    auto p95 = [](std::vector<double>& values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[std::min(values.size() - 1, values.size() * 95 / 100)];
    };
    SlotWork slots[kFrameSlots];
    uint64_t frameIndex = 0;
    // Finish a slot's frame: wait for it (or only if already done), then pass on its results.
    auto completeSlot = [&](uint32_t slot, bool wait) -> bool {
        SlotWork& work = slots[slot];
        if (!work.submitted) return true;
        if (wait) {
            if (vkWaitForFences(device, 1, &fences[slot], VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        } else if (vkGetFenceStatus(device, fences[slot]) != VK_SUCCESS) {
            return true;
        }
        if (vkResetFences(device, 1, &fences[slot]) != VK_SUCCESS) return false;
        work.submitted = false;
        uint64_t ticks[kTimestamps]{};
        if (work.timed && vkGetQueryPoolResults(device, timestamps, slot * kTimestamps, kTimestamps, sizeof(ticks),
                                                ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            report.gpuMs.push_back(double(ticks[3] - ticks[0]) * timestampMs);
            report.prepWarpMs.push_back(double(ticks[1] - ticks[0]) * timestampMs);
            if (work.roomDrawn) report.roomMs.push_back(double(ticks[2] - ticks[1]) * timestampMs);
            report.copyMs.push_back(double(ticks[3] - ticks[2]) * timestampMs);
        }
        if (work.liveNew) {
            const float* input = prep->ModelInput(slot);
#ifdef VRX_HAS_LIVE_DEPTH
            if (liveDepth)
                liveDepth->Submit(std::vector<float>(input, input + 3 * modelPlane),
                                  work.sequence, work.layout, work.arrival);
#endif
            if (room)
                for (uint32_t y = 0; y < ambientHeight; ++y)
                    for (uint32_t x = 0; x < ambientWidth; ++x) {
                        const size_t i = size_t(y * 4 + 2) * vrx::VulkanPrep::width + x * 4 + 2;
                        auto byte = [&](size_t plane) {
                            return uint32_t(std::clamp(input[plane * modelPlane + i], 0.0f, 1.0f) * 255.0f + 0.5f);
                        };
                        ambient[size_t(y) * ambientWidth + x] = byte(0) | (byte(1) << 8) | (byte(2) << 16) | 0xff000000u;
                    }
        }
#ifdef VRX_HAS_CAPTURE
        if (work.heldGpuFrame) capture->ReleaseGpuFrame(work.sequence);
#endif
        work = {};
        return true;
    };
    auto completeAll = [&]() -> bool {
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
            if (!completeSlot(slot, true)) return false;
        return true;
    };
    auto nextSettingsCheck = std::chrono::steady_clock::now();
    while (!stopRequested && (seconds == 0 ||
           std::chrono::duration<double>(std::chrono::steady_clock::now() - beginTime).count() < seconds)) {
        XrEventDataBuffer event{};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult eventResult = xr.pollEvent(instance, &event);
        while (eventResult == XR_SUCCESS) {
            auto* header = reinterpret_cast<XrEventDataBaseHeader*>(&event);
            if (header->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) floorLatched = false;
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
                    sessionExiting = state->state == XR_SESSION_STATE_EXITING;
                    sessionLost = !sessionExiting;
                    break;
                }
            }
            event = {};
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
            eventResult = xr.pollEvent(instance, &event);
        }
        if (XR_FAILED(eventResult)) { std::fprintf(stderr, "xrPollEvent: %d\n", eventResult); break; }
        if (sessionExiting) { std::puts("OpenXR session ended by the runtime"); break; }
        if (sessionLost) { std::fputs("OpenXR session lost\n", stderr); break; }
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
        if (settingsPath && std::chrono::steady_clock::now() >= nextSettingsCheck) {
            nextSettingsCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            vrx::LiveSettings updated;
            if (vrx::ReadLiveSettings(settingsPath, updated)) {
                if (updated.width != settings.width || updated.distance != settings.distance ||
                    updated.height != settings.height || updated.horizontal != settings.horizontal ||
                    updated.strength != settings.strength || updated.room != settings.room ||
                    updated.glass != settings.glass || updated.reflect != settings.reflect ||
                    updated.light != settings.light || updated.lightRgb != settings.lightRgb)
                    std::printf("Live settings: width %.2f m, distance %.2f m, height %.2f m, horizontal %.2f m, stereo %.2fx\n",
                        updated.width, updated.distance, updated.height, updated.horizontal, updated.strength);
                if (updated.recenter != settings.recenter) {
                    screen.pending = true;
                    std::puts("Recenter requested");
                }
                settings = updated;
                warp->SetStrength(settings.strength);
            }
        }
        // Pass on finished frames promptly (depth input, glow, DMA-BUF release).
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
            if (!completeSlot(slot, false)) { std::fputs("Vulkan frame completion failed\n", stderr); return 1; }
        if (!running) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        XrFrameWaitInfo wi{}; wi.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState state{}; state.type = XR_TYPE_FRAME_STATE;
        XR_CHECK(xr.waitFrame(session, &wi, &state));
        // The previous frame has usually finished while xrWaitFrame blocked.
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
            if (!completeSlot(slot, false)) { std::fputs("Vulkan frame completion failed\n", stderr); return 1; }
        XrFrameBeginInfo fbi{}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
        XR_CHECK(xr.beginFrame(session, &fbi));
        const auto frameWorkStart = std::chrono::steady_clock::now();
        uint32_t index = 0;
        bool roomFrameReady = false;
        bool roomDrawn = false;          // the room layer is drawn and submitted this frame
        uint32_t roomIndex = 0;
        XrView roomViews[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        if (state.shouldRender) {
            XrViewLocateInfo locate{};
            locate.type = XR_TYPE_VIEW_LOCATE_INFO;
            locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate.displayTime = state.predictedDisplayTime;
            locate.space = space;
            XrViewState viewState{}; viewState.type = XR_TYPE_VIEW_STATE;
            uint32_t count = 0;
            XR_CHECK(xr.locateViews(session, &locate, &viewState, 2, &count, roomViews));
            const bool validViews = count == 2 &&
                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            if (validViews) {
                const auto& left = roomViews[0].pose.position;
                const auto& right = roomViews[1].pose.position;
                const float dx = right.x - left.x, dy = right.y - left.y, dz = right.z - left.z;
                warp->SetStereoGeometry(settings.distance, settings.width, std::sqrt(dx * dx + dy * dy + dz * dz), true);
                // The heading both eyes share, and the narrower half field of view.
                const XrQuaternionf q0 = roomViews[0].pose.orientation, q1 = roomViews[1].pose.orientation;
                const float sign = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0 ? -1.0f : 1.0f;
                XrQuaternionf shared{q0.x + sign * q1.x, q0.y + sign * q1.y, q0.z + sign * q1.z, q0.w + sign * q1.w};
                const float length = std::sqrt(shared.x * shared.x + shared.y * shared.y + shared.z * shared.z + shared.w * shared.w);
                if (length > 1e-6f) shared = {shared.x / length, shared.y / length, shared.z / length, shared.w / length};
                else shared = q0;
                float tanHalfX = 100.0f;
                for (const auto& view : roomViews)
                    tanHalfX = std::min({tanHalfX, std::tan(std::fabs(view.fov.angleLeft)), std::tan(std::fabs(view.fov.angleRight))});
                if (!(tanHalfX > 0.01f && tanHalfX < 100.0f)) tanHalfX = 1.0f;
                screen.level = roomEnabled && settings.room > 0;
                if (screen.Place(roomViews[0].pose, roomViews[1].pose, shared, tanHalfX, screenAspect)) {
                    std::puts("Screen placed in front of the current headset direction");
                    floorLatched = false;
                }
                screen.Adjust(settings.width, settings.distance, settings.height, settings.horizontal, screenAspect);
                for (auto& quad : quads) {
                    quad.pose = screen.pose;
                    quad.size = screen.size;
                }
            }
            // The curved screen (screen_curve.h) for the placed screen, rebuilt when
            // its size, distance or curve changes; it needs the eye layer.
            if (validViews && !screen.pending) {
                const float wantCurve = eyeLayer ? settings.curve / 100.0f : 0.0f;
                if (screen.size.width != curveWidth || screen.size.height != curveHeight ||
                    settings.distance != curveDistance || wantCurve != curveFraction) {
                    if (!BuildCylinder(screen.size.width, screen.size.height, settings.distance, wantCurve, cylinder))
                        cylinder = Cylinder();
                    if (cylinder.curved)
                        std::printf("Screen curve %d%%: wrap %.1f deg, radius %.2f m, edges %.2f m nearer (%.2f x %.2f m at %.2f m)\n",
                                    settings.curve, cylinder.halfWrap * 2.0f * 57.2958f, cylinder.radius,
                                    CurveSag(screen.size.width, cylinder.halfWrap * 2.0f), screen.size.width,
                                    screen.size.height, settings.distance);
                    else if (curveFraction > 0.0f)
                        std::puts("Screen curve off (flat screen)");
                    curveWidth = screen.size.width; curveHeight = screen.size.height;
                    curveDistance = settings.distance; curveFraction = wantCurve;
                }
            }
            roomFrameReady = eyeLayer && ((roomEnabled && settings.room > 0) || cylinder.curved) &&
                             validViews && !screen.pending;
            if (roomFrameReady) {
                for (int eye = 0; eye < 2; ++eye) {
                    projectionViews[eye].pose = roomViews[eye].pose;
                    projectionViews[eye].fov = roomViews[eye].fov;
                }
                XrSwapchainImageAcquireInfo ai{}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
                XR_CHECK(xr.acquire(roomSwapchain, &ai, &roomIndex));
                XrSwapchainImageWaitInfo wi{}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
                wi.timeout = XR_INFINITE_DURATION;
                XR_CHECK(xr.waitImage(roomSwapchain, &wi));
            }
        }
        if (state.shouldRender) {
            XrSwapchainImageAcquireInfo ai{}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            XR_CHECK(xr.acquire(swapchain, &ai, &index));
            XrSwapchainImageWaitInfo swi{};
            swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            swi.timeout = XR_INFINITE_DURATION;
            XR_CHECK(xr.waitImage(swapchain, &swi));
            std::vector<unsigned char> rgb;   // synthetic scene on the depth grid
            std::vector<float> truth;
            bool depthAvailable = true;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - beginTime).count();
            // Reuse the slot of the frame two back: it has normally completed already.
            const uint32_t slot = uint32_t(frameIndex++ % kFrameSlots);
            const auto slotWaitStart = std::chrono::steady_clock::now();
            if (!completeSlot(slot, true)) { std::fputs("Vulkan frame wait failed\n", stderr); return 1; }
            const double slotWaitMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - slotWaitStart).count();
            command = commands[slot];
            fence = fences[slot];
            warp->SetFrameSlot(slot);
            prep->SetFrameSlot(slot);
            bool liveNew = false;                 // a new source frame enters the colour buffer
            bool heldGpuFrame = false;
            int newSlot = -1, showSlot = -1;      // frame history slots (frame timing)
            uint64_t shownSequence = 0;
            double shownArrival = 0;
            vrx::DmabufImage* scaleSource = nullptr;
#ifdef VRX_HAS_CAPTURE
            if (live) {
                if (capture->DmaBuf() && captureScale) {
                    vrx::PortalCapture::GpuFrame frame;
                    if (capture->AcquireGpuFrame(frame)) {
                        heldGpuFrame = true;
                        // Imported images and their descriptor sets may be in use by
                        // frames in flight: finish those before replacing any.
                        if (frame.bufferGeneration != importGeneration) {
                            if (!completeAll()) return 1;
                            captureScale->ForgetViews();
                            imports.clear();
                            importGeneration = frame.bufferGeneration;
                        }
                        auto& image = imports[frame.bufferId];
                        if (!image || image->Width() != frame.width || image->Height() != frame.height) {
                            if (!completeAll()) return 1;
                            captureScale->ForgetViews();
                            image = std::make_unique<vrx::DmabufImage>(gpu, device, frame.fd, frame.width,
                                frame.height, VK_FORMAT_B8G8R8A8_UNORM, frame.modifier, frame.offset, frame.stride);
                        }
                        scaleSource = image.get();
                        liveNew = true;
                        liveSequence = frame.sequence;
                        liveLayout = frame.layout;
                        liveArrival = frame.arrival;
                    }
                } else if (capture->Latest(liveFrame) && liveFrame.sequence != liveSequence) {
                    color.swap(liveFrame.color);
                    warp->UploadColor(color);
                    liveNew = true;
                    liveSequence = liveFrame.sequence;
                    liveLayout = liveFrame.layout;
                    liveArrival = liveFrame.arrival;
                }
#ifdef VRX_HAS_LIVE_DEPTH
                const auto depth = liveDepth ? liveDepth->Latest() : nullptr;
                if (depth && depth->sourceSequence != lastDepthSequence) {
                    depthDelay.Update(depth->completedAt - depth->captureArrival);
                    lastDepthSequence = depth->sourceSequence;
                }
#endif
                // Keep each new frame; a new layout makes the kept frames unusable.
                if (liveNew) {
                    for (auto& frame : kept)
                        if (frame.valid && frame.layout != liveLayout) frame = {};
                    newSlot = int(nextKept++ % vrx::VulkanFrameHistory::kSlots);
                    kept[newSlot] = {true, liveSequence, liveLayout, liveArrival};
                }
                // The kept frames, oldest first, and the one to show: the newest
                // (Latest), the newest at least the depth delay old but never older
                // than the depth's own frame (Delayed), or the depth's frame (Matched).
                std::vector<TimedFrame> times;
                std::vector<int> timeSlots;
                for (uint32_t i = 0; i < vrx::VulkanFrameHistory::kSlots; ++i) {
                    const uint32_t k = (nextKept + i) % vrx::VulkanFrameHistory::kSlots;   // oldest first
                    if (!kept[k].valid) continue;
                    times.push_back({kept[k].sequence, kept[k].arrival});
                    timeSlots.push_back(int(k));
                }
                if (!times.empty()) {
                    int pick = int(times.size()) - 1;
                    const FrameTiming timing = FrameTiming(settings.timing);
#ifdef VRX_HAS_LIVE_DEPTH
                    int depthFrame = -1;
                    if (depth && depth->sourceLayout == liveLayout)
                        for (int i = 0; i < int(times.size()); ++i)
                            if (times[i].seq == depth->sourceSequence) depthFrame = i;
                    if (timing == FrameTiming::Delayed) {
                        pick = PickDelayedFrame(times, nowSeconds(), depthDelay.value);
                        if (depthFrame >= 0 && times[pick].seq < times[depthFrame].seq) pick = depthFrame;
                    } else if (timing == FrameTiming::Matched && depthFrame >= 0) {
                        pick = depthFrame;
                    }
#endif
                    showSlot = timeSlots[pick];
                    shownSequence = kept[showSlot].sequence;
                    shownArrival = kept[showSlot].arrival;
                }
                if (settings.timing != loggedTiming) {
                    std::printf("Frame timing: %s\n", FrameTimingName(FrameTiming(settings.timing)));
                    loggedTiming = settings.timing;
                }
#ifdef VRX_HAS_LIVE_DEPTH
                depthAvailable = depth && shownSequence && liveLayout == depth->sourceLayout &&
                    UsableDepth(liveDepth->Healthy(), shownSequence, shownArrival,
                                depth->sourceSequence, depth->captureArrival);
                if (depthAvailable) {
                    truth = depth->near;
                    report.depthAgeMs += (nowSeconds() - depth->captureArrival) * 1000.0;
                    ++report.depthFrames;
                }
#else
                depthAvailable = false;
#endif
                if (shownSequence) {
                    report.shownAgeMs += (nowSeconds() - shownArrival) * 1000.0;
                    ++report.shownFrames;
                }
                if (!depthAvailable)
                    truth.assign(size_t(vrx::kSyntheticWidth) * vrx::kSyntheticHeight, 0.0f);
            } else
#endif
            if (stillPath) {
                color = vrx::PackRgb(stillRgb);
                truth = stillDepth;
                warp->UploadColor(color);
            } else {
                vrx::MakeScene(rgb, elapsed);
                vrx::MakeTruth(truth, elapsed);
                color = vrx::PackRgb(rgb);
                if (colorWidth != uint32_t(vrx::kSyntheticWidth) || colorHeight != uint32_t(vrx::kSyntheticHeight)) {
                    const std::vector<uint32_t> grid = std::move(color);
                    vrx::ScaleCaptureColor(reinterpret_cast<const unsigned char*>(grid.data()),
                        size_t(vrx::kSyntheticWidth) * 4, vrx::kSyntheticWidth, vrx::kSyntheticHeight,
                        true, int(colorWidth), int(colorHeight), color);
                }
                warp->UploadColor(color);
            }
            warp->SetStereoGeometry(settings.distance, settings.width, 0.0f, depthAvailable);
            warp->UploadNearness(truth);
            if (roomFrameReady) {
                // Latched, so tracking noise in STAGE does not move the room.
                if (!floorLatched) {
                    stageFloorY = std::numeric_limits<float>::quiet_NaN();
                    floorLatched = stageSpace == XR_NULL_HANDLE;
                    if (stageSpace != XR_NULL_HANDLE) {
                        XrSpaceLocation floorLocation{}; floorLocation.type = XR_TYPE_SPACE_LOCATION;
                        if (XR_SUCCEEDED(xr.locateSpace(stageSpace, space, state.predictedDisplayTime, &floorLocation)) &&
                            (floorLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                            (floorLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                            stageFloorY = floorLocation.pose.position.y;
                            floorLatched = true;
                        }
                    }
                    if (floorLatched)
                        std::printf("Room floor: %s\n", std::isfinite(stageFloorY) ? "from STAGE" : "not tracked, seated estimate");
                }
                const auto prepStart = std::chrono::steady_clock::now();
                // Without --room (or live settings asking for it) only the curve is drawn.
                vrx::LiveSettings drawn = settings;
                if (!roomEnabled) drawn.room = 0;
                roomDrawn = live ? room->Prepare(ambient.data(), ambientWidth, ambientHeight,
                                                 drawn, roomViews, screen, stageFloorY, cylinder)
                                 : room->Prepare(color.data(), colorWidth, colorHeight,
                                                 drawn, roomViews, screen, stageFloorY, cylinder);
                roomPrepMs.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - prepStart).count());
                if (roomDrawn && roomDumpPath && renderedFrames == 0) room->EnableCapture();
            }
            VK_CHECK(vkResetCommandBuffer(command, 0));
            VkCommandBufferBeginInfo cbbi{}; cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(command, &cbbi));
            // Keep GPU frames in order: this frame's writes to shared device-local
            // buffers and images wait for the previous frame's reads and writes.
            VkMemoryBarrier previousFrame{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            previousFrame.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            previousFrame.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &previousFrame, 0, nullptr, 0, nullptr);
            if (timestamps) {
                vkCmdResetQueryPool(command, timestamps, slot * kTimestamps, kTimestamps);
                vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * kTimestamps);
            }
            warp->RecordUpload(command);
            // The first frame is always checked; with --dump-color, one about six seconds in too.
            const bool laterDump = colorDumpPath && !laterDumped && renderedFrames >= 400 && (!live || liveNew);
            if (laterDump) laterDumped = true;
            const bool checkFrame = renderedFrames == 0 || laterDump;
            if (scaleSource)
                captureScale->Record(command, scaleSource->Image(), scaleSource->View(), scaleSource->Width(),
                                     scaleSource->Height(), true, checkFrame);
            // Live: prepare each new source frame for depth. Synthetic: every frame, for the check.
            const bool prepFrame = live ? liveNew : !stillPath;
            if (prepFrame) prep->Record(command);
            // Depth is prepared from the new frame above; the frame shown may be an older one.
            if (history && newSlot >= 0) {
                history->RecordStore(command, uint32_t(newSlot));
                sceneSlot = newSlot;
            }
            if (history && showSlot >= 0 && showSlot != sceneSlot) {
                history->RecordLoad(command, uint32_t(showSlot));
                sceneSlot = showSlot;
            }
            warp->Record(command, checkFrame);
            if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, slot * kTimestamps + 1);
            if (roomDrawn) room->Record(command, roomImages[roomIndex].image);
            if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, slot * kTimestamps + 2);
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
                copies[eye].imageExtent = {colorWidth, colorHeight, 1};
            }
            vkCmdCopyBufferToImage(command, warp->ColorBuffer(), images[index].image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copies);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            if (timestamps) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, slot * kTimestamps + 3);
            VK_CHECK(vkEndCommandBuffer(command));
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            const auto gpuStart = std::chrono::steady_clock::now();
            VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
            SlotWork& work = slots[slot];
            work.submitted = true;
            work.liveNew = live && liveNew;
            work.sequence = liveSequence;
            work.layout = liveLayout;
            work.arrival = liveArrival;
            work.heldGpuFrame = heldGpuFrame;
            work.timed = timestamps != VK_NULL_HANDLE;
            work.roomDrawn = roomDrawn;
            // Checked frames read results back now; other frames finish while
            // the next is recorded.
            if (checkFrame && !completeSlot(slot, true)) { std::fputs("Vulkan frame wait failed\n", stderr); return 1; }
            {
                const double ms = slotWaitMs + std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - gpuStart).count();
                if (!checkFrame) gpuWaitMs.push_back(ms);
                else if (roomDrawn && renderedFrames == 0) std::printf("First room frame GPU submit/wait: %.2f ms\n", ms);
            }
            if (roomDrawn && renderedFrames == 0) room->PrintTiming();
            if (roomDrawn && roomDumpPath && renderedFrames == 0) {
                room->SaveCapture(roomDumpPath);
                referenceMatched = referenceMatched && room->CompareReference();
                std::printf("Room eye capture: %s\n", roomDumpPath);
            }
            if (checkFrame) {
                bool matched = true;
                if (scaleSource) {
                    matched = captureScale->CompareReference();
                    color = captureScale->CheckedColor();
                }
                if (!color.empty() && colorDumpPath) {
                    const std::string dumpPath = std::string(colorDumpPath) + (laterDump ? ".later.ppm" : "");
                    std::FILE* dump = std::fopen(dumpPath.c_str(), "wb");
                    if (dump) {
                        std::fprintf(dump, "P6\n%u %u\n255\n", colorWidth, colorHeight);
                        for (uint32_t pixel : color) {
                            const unsigned char rgbBytes[3] = {static_cast<unsigned char>(pixel),
                                static_cast<unsigned char>(pixel >> 8), static_cast<unsigned char>(pixel >> 16)};
                            std::fwrite(rgbBytes, 1, 3, dump);
                        }
                        std::fclose(dump);
                        std::printf("Colour frame %d written to %s\n", renderedFrames, dumpPath.c_str());
                    }
                }
                if (!color.empty()) {
                    matched = warp->CompareReference(color, truth) && matched;
                    if (prepFrame) matched = prep->CompareReference(color) && matched;
                }
                referenceMatched = referenceMatched && matched;
            }
            XrSwapchainImageReleaseInfo ri{}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            XR_CHECK(xr.release(swapchain, &ri));
            if (roomFrameReady) XR_CHECK(xr.release(roomSwapchain, &ri));
            ++renderedFrames;
        }
        if (!state.shouldRender) ++skippedFrames;
        XrFrameEndInfo fe{};
        fe.type = XR_TYPE_FRAME_END_INFO;
        fe.displayTime = state.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        // Nothing is shown until the screen has been placed from a tracked pose.
        const XrCompositionLayerBaseHeader* frameLayers[3]{};
        uint32_t layerCount = 0;
        if (state.shouldRender && !screen.pending) {
            if (roomDrawn) frameLayers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            // A curved screen is drawn in the eye layer; a flat one is the quads.
            if (!(roomDrawn && room->DrawsScreen())) {
                frameLayers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[0]);
                frameLayers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[1]);
            }
        }
        fe.layerCount = layerCount;
        fe.layers = layerCount ? frameLayers : nullptr;
        const auto endStart = std::chrono::steady_clock::now();
        XR_CHECK(xr.endFrame(session, &fe));
        ++report.frames;
        if (state.shouldRender) {
            ++report.drawn;
            report.cpuMs += std::chrono::duration<double, std::milli>(endStart - frameWorkStart).count();
        }
        if (const double elapsedReport = std::chrono::duration<double>(std::chrono::steady_clock::now() - report.start).count();
            elapsedReport >= 2.0) {
            char capturePart[96] = "", depthPart[224] = "", gpuPart[160] = "";
#ifdef VRX_HAS_CAPTURE
            if (live) {
                const uint64_t captured = capture->Captured(), dropped = capture->Dropped();
                std::snprintf(capturePart, sizeof(capturePart), " | capture %.1f fps, %llu dropped",
                    (captured - report.captured) / elapsedReport, static_cast<unsigned long long>(dropped - report.dropped));
                report.captured = captured;
                report.dropped = dropped;
            }
#endif
#ifdef VRX_HAS_LIVE_DEPTH
            if (liveDepth) {
                const uint64_t completed = liveDepth->Completed();
                const auto latest = liveDepth->Latest();
                std::snprintf(depthPart, sizeof(depthPart), " | depth %.1f updates/s, model %.1f ms, depth age %.1f ms",
                    (completed - report.depthUpdates) / elapsedReport, latest ? latest->modelMs : 0.0,
                    report.depthFrames ? report.depthAgeMs / report.depthFrames : 0.0);
                report.depthUpdates = completed;
            }
#endif
            if (report.shownFrames) {
                const size_t used = std::strlen(depthPart);
                std::snprintf(depthPart + used, sizeof(depthPart) - used, " | %s: frame age %.1f ms, depth delay %.1f ms",
                              FrameTimingName(FrameTiming(settings.timing)), report.shownAgeMs / report.shownFrames,
                              depthDelay.value * 1000.0);
            }
            if (!report.gpuMs.empty())
                std::snprintf(gpuPart, sizeof(gpuPart), " | GPU %.2f / %.2f ms p50/p95: prep+warp %.2f, room %.2f, copy %.2f ms p50",
                    median(report.gpuMs), p95(report.gpuMs), median(report.prepWarpMs), median(report.roomMs), median(report.copyMs));
            std::printf("Performance: %.1f fps (drawn %.1f, CPU %.2f ms/frame)%s%s%s\n",
                report.frames / elapsedReport, report.drawn / elapsedReport,
                report.drawn ? report.cpuMs / report.drawn : 0.0, capturePart, depthPart, gpuPart);
            const uint64_t captured = report.captured, dropped = report.dropped, depthUpdates = report.depthUpdates;
            report = Report{};
            report.captured = captured;
            report.dropped = dropped;
            report.depthUpdates = depthUpdates;
        }
        if (state.shouldRender && renderedFrames > 1) {
            const auto done = std::chrono::steady_clock::now();
            endFrameMs.push_back(std::chrono::duration<double, std::milli>(done - endStart).count());
            frameWorkMs.push_back(std::chrono::duration<double, std::milli>(done - frameWorkStart).count());
        }
        ++frames;
    }
    std::printf("Submitted %d OpenXR frames (%d with an image, %d skipped by runtime)\n",
                frames, renderedFrames, skippedFrames);
    auto printTiming = [](const char* name, std::vector<double> values) {
        if (values.empty()) return;
        std::sort(values.begin(), values.end());
        const size_t p95 = std::min(values.size() - 1, (values.size() * 95) / 100);
        std::printf("%s %.2f / %.2f ms (median / p95, %zu frames)\n",
                    name, values[values.size() / 2], values[p95], values.size());
    };
    printTiming("Room CPU prepare", roomPrepMs);
    printTiming("Vulkan submit and slot wait", gpuWaitMs);
    printTiming("OpenXR endFrame", endFrameMs);
    printTiming("Frame work after beginFrame", frameWorkMs);
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
        std::printf("Live ZipDepth timing (%llu samples): wait %.2f/%.2f ms, model %.2f/%.2f ms, arrival-to-depth %.2f/%.2f ms (median/p95)\n",
            static_cast<unsigned long long>(timings.samples),
            timings.waitMedianMs, timings.waitP95Ms,
            timings.modelMedianMs, timings.modelP95Ms,
            timings.arrivalToDepthMedianMs, timings.arrivalToDepthP95Ms);
    }
#endif
    completeAll();
    vkDeviceWaitIdle(device);
    imports.clear();
    captureScale.reset();
#ifdef VRX_HAS_CAPTURE
    if (capture) capture->ReleaseGpuFrames();
#endif
    for (VkFence slotFence : fences) vkDestroyFence(device, slotFence, nullptr);
    if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    room.reset();
    if (roomSwapchain != XR_NULL_HANDLE) xr.destroySwapchain(roomSwapchain);
    xr.destroySwapchain(swapchain);
    if (stageSpace != XR_NULL_HANDLE) xr.destroySpace(stageSpace);
    xr.destroySpace(space);
    xr.destroySession(session);
    prep.reset();
    warp.reset();
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(vkInstance, nullptr);
    xr.destroyInstance(instance);
    dlclose(library);
    return renderedFrames > 0 && referenceMatched && !captureLost && !depthFailed && !sessionLost ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    double seconds = 10.0;
    const char* stillPath = nullptr;
    // The packaged model when this engine is packaged, else the repository's.
    const std::string defaultModel = vrx::ResourcePath("VRX_MODEL", "models", "zipdepth_faithful_fp16_672x384.onnx",
                                                       "bench/models/zipdepth_faithful_fp16_672x384.onnx");
    const char* modelPath = defaultModel.c_str();
    bool cuda = false, live = false, room = false, durationSeen = false, untilStop = false;
    const char* settingsPath = nullptr;
    const char* roomDumpPath = nullptr;
    unsigned colorWidth = 0, colorHeight = 0;
    bool useDmabuf = true;
    const char* colorDumpPath = nullptr;
    int sourceKind = 0;   // 0 any, 1 window, 2 screen
    int curvePercent = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            std::puts("vrx-engine [seconds=10 | --until-stop] [--still=picture.png] [--model=model.onnx] [--cuda] [--live] [--room] [--room-dump=path] [--settings=path]");
            std::puts("--live selects a portal source; add --cuda for asynchronous ZipDepth");
            std::puts("--color=WxH presents the synthetic scene at that colour size (depth stays 686x392)");
            std::puts("--no-dmabuf receives live frames in shared memory instead of GPU buffers");
            std::puts("--source=window|screen|any limits what the desktop chooser offers (default any)");
            std::puts("--dump-color=frame.ppm writes the first presented colour frame");
            std::puts("--curve=0..100 curves the screen (live settings take precedence)");
            std::puts("VRX_OPENXR_LOADER, VRX_MODEL and the VRX_*_SPV variables override runtime paths");
            return 0;
        }
        if (std::strncmp(argv[i], "--still=", 8) == 0) stillPath = argv[i] + 8;
        else if (std::strncmp(argv[i], "--model=", 8) == 0) modelPath = argv[i] + 8;
        else if (std::strcmp(argv[i], "--cuda") == 0) cuda = true;
        else if (std::strcmp(argv[i], "--live") == 0) live = true;
        else if (std::strcmp(argv[i], "--room") == 0) room = true;
        else if (std::strcmp(argv[i], "--until-stop") == 0) untilStop = true;
        else if (std::strncmp(argv[i], "--settings=", 11) == 0) settingsPath = argv[i] + 11;
        else if (std::strncmp(argv[i], "--room-dump=", 12) == 0) { roomDumpPath = argv[i] + 12; room = true; }
        else if (std::strcmp(argv[i], "--no-dmabuf") == 0) useDmabuf = false;
        else if (std::strcmp(argv[i], "--source=any") == 0) sourceKind = 0;
        else if (std::strcmp(argv[i], "--source=window") == 0) sourceKind = 1;
        else if (std::strcmp(argv[i], "--source=screen") == 0) sourceKind = 2;
        else if (std::strncmp(argv[i], "--dump-color=", 13) == 0 && argv[i][13]) colorDumpPath = argv[i] + 13;
        else if (std::strncmp(argv[i], "--curve=", 8) == 0) {
            char* end = nullptr;
            const long percent = std::strtol(argv[i] + 8, &end, 10);
            if (!end || *end || end == argv[i] + 8 || percent < 0 || percent > 100) {
                std::fputs("Invalid --curve (0..100)\n", stderr); return 2;
            }
            curvePercent = int(percent);
        }
        else if (std::strncmp(argv[i], "--color=", 8) == 0) {
            char extra = 0;
            if (std::sscanf(argv[i] + 8, "%ux%u%c", &colorWidth, &colorHeight, &extra) != 2 ||
                colorWidth < 2 || colorHeight < 2 || colorWidth > 4096 || colorHeight > 4096) {
                std::fputs("Invalid --color size\n", stderr); return 2;
            }
        }
        else if (!durationSeen) {
            char* end = nullptr;
            seconds = std::strtod(argv[i], &end);
            if (!end || *end) { std::fputs("Invalid duration\n", stderr); return 2; }
            durationSeen = true;
        } else { std::fputs("Unexpected argument\n", stderr); return 2; }
    }
    if (untilStop) seconds = 0;
    if ((untilStop && durationSeen) || (!untilStop && (seconds <= 0 || seconds > 120)) ||
        (cuda && !stillPath && !live) || (live && stillPath) ||
        (stillPath && !*stillPath) || (modelPath && !*modelPath) ||
        (settingsPath && !*settingsPath) || (roomDumpPath && !*roomDumpPath) ||
        (colorWidth && (stillPath || live))) {
        std::fputs("usage: vrx-engine [0 < seconds <= 120 | --until-stop] [--still=picture.png] [--model=model.onnx] [--cuda] [--live] [--room] [--room-dump=path] [--settings=path]\n", stderr);
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
    std::signal(SIGINT, OnStopSignal);
    std::signal(SIGTERM, OnStopSignal);
    const char* path = std::getenv("VRX_OPENXR_LOADER");
    try {
        return Run(seconds, path && *path ? path : "libopenxr_loader.so.1", stillPath, modelPath, cuda, live, room, settingsPath, roomDumpPath,
                   colorWidth, colorHeight, useDmabuf, colorDumpPath, sourceKind, curvePercent);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VRX Linux renderer: %s\n", error.what());
        return 1;
    }
}
