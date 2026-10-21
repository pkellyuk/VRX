// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Controlled app-unaware presentation test: one device, one simulation, one
// unchanged recorded draw list. Stereo is added entirely by the explicit layer.
#include "render_support.h"
#include "gpu_test_support.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <thread>
#include <atomic>

using namespace xg::render_test;

namespace {
struct WindowSwapchain {
    Gpu& gpu;
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    std::vector<VkImage> images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer copy = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
#define WSI_FUNCTIONS(X) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit2) X(vkQueueSubmit2KHR)
#define MEMBER(fn) PFN_##fn fn = nullptr;
    WSI_FUNCTIONS(MEMBER)
#undef MEMBER
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    explicit WindowSwapchain(Gpu& g) : gpu(g) {}
    void Init(bool tenBit=false)
    {
#define LOAD(fn) fn = reinterpret_cast<PFN_##fn>(gpu.getDeviceProc(gpu.device, #fn)); Require(fn != nullptr, #fn);
        WSI_FUNCTIONS(LOAD)
#undef LOAD
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW; wc.hInstance = module; wc.lpszClassName = L"VRX_XG_present_test";
        const ATOM registered = RegisterClassW(&wc);
        Require(registered != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register test window");
        window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"VRX geometry test",
            WS_POPUP | WS_VISIBLE, 16, 16, Width, Height, nullptr, nullptr, module, nullptr);
        Require(window != nullptr, "create controlled test window");
        auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gpu.gipa(gpu.instance, "vkCreateWin32SurfaceKHR"));
        destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(gpu.gipa(gpu.instance, "vkDestroySurfaceKHR"));
        auto supports = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        auto caps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        auto formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        Require(createSurface && destroySurface && supports && caps && formats, "Win32 surface functions");
        VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; si.hinstance = module; si.hwnd = window;
        Vk(createSurface(gpu.instance, &si, nullptr, &surface), "create Win32 surface");
        VkBool32 supported = VK_FALSE;
        Vk(supports(gpu.physical, gpu.family, surface, &supported), "query present support");
        Require(supported == VK_TRUE, "graphics queue must present on this window");
        VkSurfaceCapabilitiesKHR capabilities{}; Vk(caps(gpu.physical, surface, &capabilities), "surface capabilities");
        uint32_t count = 0; Vk(formats(gpu.physical, surface, &count, nullptr), "surface format count");
        std::vector<VkSurfaceFormatKHR> available(count); Vk(formats(gpu.physical, surface, &count, available.data()), "surface formats");
        VkSurfaceFormatKHR chosen{};
        for (auto candidate : available)
            if (tenBit ? (candidate.format==VK_FORMAT_A2B10G10R10_UNORM_PACK32 || candidate.format==VK_FORMAT_A2R10G10B10_UNORM_PACK32) :
                (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_SRGB)) { chosen = candidate; break; }
        Require(chosen.format != VK_FORMAT_UNDEFINED, "requested surface format for export conversion validation");
        format = chosen.format;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface; ci.minImageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, capabilities.maxImageCount);
        ci.imageFormat = format; ci.imageColorSpace = chosen.colorSpace; ci.imageExtent = {Width, Height};
        ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.preTransform = capabilities.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE;
        Require((capabilities.supportedUsageFlags & ci.imageUsage) != 0, "test swapchain permits transfer destination");
        Vk(vkCreateSwapchainKHR(gpu.device, &ci, nullptr, &swapchain), "create test swapchain (layer adds TRANSFER_SRC)");
        Vk(vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, nullptr), "swapchain image count");
        images.resize(count); Vk(vkGetSwapchainImagesKHR(gpu.device, swapchain, &count, images.data()), "swapchain images");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Vk(vkCreateSemaphore(gpu.device, &semaphore, nullptr, &acquired), "create acquire semaphore");
        rendered.resize(count, VK_NULL_HANDLE);
        for (auto& ready : rendered) Vk(vkCreateSemaphore(gpu.device, &semaphore, nullptr, &ready), "create app render semaphore");
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = gpu.family; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Vk(gpu.vkCreateCommandPool(gpu.device, &pi, nullptr, &pool), "create app copy pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool; ai.commandBufferCount = 1; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        Vk(gpu.vkAllocateCommandBuffers(gpu.device, &ai, &copy), "allocate app copy command");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Vk(gpu.vkCreateFence(gpu.device, &fi, nullptr, &fence), "create app copy fence");
    }
    void SubmitCommand(VkCommandBuffer command, VkFence done, uint32_t method,
        VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE)
    {
        if (!method)
        {
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &command;
            si.waitSemaphoreCount = wait ? 1u : 0u; si.pWaitSemaphores = &wait; si.pWaitDstStageMask = &stage;
            si.signalSemaphoreCount = signal ? 1u : 0u; si.pSignalSemaphores = &signal;
            Vk(gpu.vkQueueSubmit(gpu.queue, 1, &si, done), "app legacy submit");
        }
        else
        {
            VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; ci.commandBuffer = command;
            VkSemaphoreSubmitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, so{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wi.semaphore = wait; wi.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            so.semaphore = signal; so.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &ci;
            si.waitSemaphoreInfoCount = wait ? 1u : 0u; si.pWaitSemaphoreInfos = &wi;
            si.signalSemaphoreInfoCount = signal ? 1u : 0u; si.pSignalSemaphoreInfos = &so;
            Vk((method == 1 ? vkQueueSubmit2 : vkQueueSubmit2KHR)(gpu.queue, 1, &si, done), "app synchronization2 submit");
        }
    }
    void Present(VkImage renderedImage, uint32_t method)
    {
        uint32_t index = 0;
        const auto acquiredResult = vkAcquireNextImageKHR(gpu.device, swapchain, 5000000000ull, acquired, VK_NULL_HANDLE, &index);
        Require(acquiredResult == VK_SUCCESS || acquiredResult == VK_SUBOPTIMAL_KHR, "acquire test swapchain image");
        Vk(gpu.vkResetCommandBuffer(copy, 0), "reset app copy command");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Vk(gpu.vkBeginCommandBuffer(copy, &begin), "begin app copy");
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; image.image = images[index];
        image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        gpu.vkCmdPipelineBarrier(copy, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, 1, &image);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.extent = {Width, Height, 1};
        gpu.vkCmdCopyImage(copy, renderedImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        image.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; image.dstAccessMask = 0;
        gpu.vkCmdPipelineBarrier(copy, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
            0, nullptr, 0, nullptr, 1, &image);
        Vk(gpu.vkEndCommandBuffer(copy), "end app copy");
        Vk(gpu.vkResetFences(gpu.device, 1, &fence), "reset app copy fence");
        SubmitCommand(copy, fence, method, acquired, rendered[index]);
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered[index];
        present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
        const auto result = vkQueuePresentKHR(gpu.queue, &present);
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "present with layer capture");
        Vk(gpu.vkWaitForFences(gpu.device, 1, &fence, VK_TRUE, 5000000000ull), "app copy fence completes");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    void Close()
    {
        gpu.vkDeviceWaitIdle(gpu.device);
        if (fence) gpu.vkDestroyFence(gpu.device, fence, nullptr);
        if (pool) gpu.vkDestroyCommandPool(gpu.device, pool, nullptr);
        if (acquired) vkDestroySemaphore(gpu.device, acquired, nullptr);
        for (auto semaphore : rendered) if (semaphore) vkDestroySemaphore(gpu.device, semaphore, nullptr);
        if (swapchain) vkDestroySwapchainKHR(gpu.device, swapchain, nullptr);
        if (surface) destroySurface(gpu.instance, surface, nullptr);
        if (window) DestroyWindow(window);
        fence = VK_NULL_HANDLE; pool = VK_NULL_HANDLE; acquired = VK_NULL_HANDLE; rendered.clear();
        swapchain = VK_NULL_HANDLE; surface = VK_NULL_HANDLE; window = nullptr;
    }
    ~WindowSwapchain() { Close(); }
};

void CheckPair(const xg::PairFrame& pair, uint64_t frame, double scale = 1, double convergence = 0, bool sameFrame = false, double movement = 0)
{
    if (sameFrame) Require(pair.header.leftFrame == frame && pair.header.rightFrame == frame, "both eyes rendered from the same app frame");
    else Require(pair.header.leftFrame == frame && pair.header.rightFrame == frame + 1, "pair consists of consecutive game frames");
    std::vector<uint8_t> left(size_t(Width) * Height * 4), right(left.size());
    for (uint32_t y = 0; y < Height; ++y)
    {
        std::memcpy(left.data() + size_t(y) * Width * 4, pair.rgba.data() + size_t(y) * Width * 8, Width * 4);
        std::memcpy(right.data() + size_t(y) * Width * 4, pair.rgba.data() + size_t(y) * Width * 8 + Width * 4, Width * 4);
    }
    const double expected[] = {4 * scale - convergence, 2 * scale - convergence, 0};
    for (uint32_t post = 0; post < 3; ++post)
    {
        const double l = Centroid(left, post), r = Centroid(right, post);
        std::printf("present pair %llu/%llu post %u: left %.2f right %.2f\n",
            (unsigned long long)pair.header.leftFrame, (unsigned long long)pair.header.rightFrame, post, l, r);
        Require(std::abs(l - 63.5 - movement - expected[post]) < .01 && std::abs(63.5 + movement - r - expected[post]) < .01,
            "live BDA eye shift, cached draw commands, swapchain export and BGRA conversion");
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool serve = argc == 2 && std::string(argv[1]).rfind("--serve=", 0) == 0;
        const bool serveMirror = argc == 2 && std::string(argv[1]).rfind("--serve-mirror-gpu=",0)==0;
        const bool serveGpu = serveMirror || argc == 2 && std::string(argv[1]).rfind("--serve-gpu=", 0) == 0;
        const bool contention = argc == 2 && std::string(argv[1]) == "--mirror-contention";
        const bool tenBit=argc==2 && std::string(argv[1])=="--mirror-ten";
        const bool externalImport=argc==2 && std::string(argv[1])=="--mirror-win32-import";
        const bool mirrorLive=argc==2 && std::string(argv[1])=="--mirror-live-tuning";
        const bool mirrorFallback=argc==2 && std::string(argv[1])=="--mirror-fallback";
        const bool mirror = mirrorFallback || mirrorLive || externalImport || tenBit || contention || serveMirror || (argc == 2 && (std::string(argv[1]) == "--mirror" || std::string(argv[1]) == "--mirror-inline"));
        const bool gpuTransport = mirror || serveGpu || (argc == 2 && std::string(argv[1]) == "--gpu");
        const bool unsupported = argc == 2 && std::string(argv[1]) == "--unsupported-queues";
        const bool unsupportedShader = argc == 2 && std::string(argv[1]) == "--unsupported-shader";
        const bool liveTuning = argc == 2 && std::string(argv[1]) == "--live-tuning";
        const bool inlineShader = (mirror && std::string(argv[1]) == "--mirror-inline") || liveTuning || (argc == 2 && std::string(argv[1]) == "--inline-shader");
        Require(argc == 1 || serve || gpuTransport || unsupported || unsupportedShader || inlineShader, "usage: present_test.exe [--serve=session | --serve-gpu=session | --gpu | --unsupported-queues | --unsupported-shader | --inline-shader | --live-tuning]");
        const auto session = serveMirror ? std::string(argv[1]).substr(19) : serve ? std::string(argv[1]).substr(8) : serveGpu ? std::string(argv[1]).substr(12) : "present-" + std::to_string(GetCurrentProcessId());
        const std::wstring wide(session.begin(), session.end());
        Require(xg::ValidSession(wide), "valid presentation session name");
        SetEnvironmentVariableA("VRX_XG_EYE", mirror ? "mirror" : "alternate");
        SetEnvironmentVariableA("VRX_XG_SHIFT", "0.125");
        SetEnvironmentVariableW(L"VRX_XG_SESSION", wide.c_str());
        SetEnvironmentVariableA("VRX_XG_TRANSPORT", gpuTransport ? "gpu" : nullptr);
        struct TuningFile {
            std::wstring path;
            void Write(const char* text) {
                const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                Require(file != INVALID_HANDLE_VALUE, "create tuning fixture file");
                DWORD written = 0; const DWORD size = static_cast<DWORD>(std::strlen(text));
                const bool ok = WriteFile(file, text, size, &written, nullptr) && written == size;
                CloseHandle(file); Require(ok, "write tuning fixture file");
            }
            ~TuningFile() { if (!path.empty()) {DeleteFileW(path.c_str());DeleteFileW((path+L".live.json").c_str());} }
        } tuning;
        if (liveTuning || mirrorLive) {
            wchar_t executable[32768]; Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "get test path");
            tuning.path = std::wstring(executable) + L"." + std::to_wstring(GetCurrentProcessId()) + L".stereo.txt";
            tuning.Write("0.125 0\n"); SetEnvironmentVariableW(L"VRX_XG_TUNING", tuning.path.c_str());
        }
        else SetEnvironmentVariableW(L"VRX_XG_TUNING", nullptr);
        std::wstring layerLog;
        if (mirrorFallback) {
            wchar_t temporary[MAX_PATH]{}; Require(GetTempPathW(MAX_PATH, temporary) != 0, "temporary folder");
            layerLog = std::wstring(temporary) + L"vrx-fallback-" + std::to_wstring(GetCurrentProcessId()) + L".log";
            DeleteFileW(layerLog.c_str()); SetEnvironmentVariableW(L"VRX_XG_LOG", layerLog.c_str());
        }
        struct LogCleanup { std::wstring path; ~LogCleanup() { if (!path.empty()) DeleteFileW(path.c_str()); } } logCleanup{layerLog};
        if (gpuTransport) {
            xg::ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        }
        Gpu gpu; gpu.Init(true, unsupported ? 2 : 1, inlineShader,externalImport || mirrorFallback);
        WindowSwapchain wsi(gpu); wsi.Init(tenBit);
        if(externalImport) {
            // Steam can import a shared Windows texture after the first pair.
            // Import a real exported allocation, then verify stereo still runs.
            VkBuffer buffers[2]{}; VkDeviceMemory memories[2]{};
            VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.pNext=&external;
            buffer.size=4096;buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            for(auto& b:buffers) Vk(gpu.vkCreateBuffer(gpu.device,&buffer,nullptr,&b),"create external buffer");
            VkMemoryRequirements needs{};gpu.vkGetBufferMemoryRequirements(gpu.device,buffers[0],&needs);
            VkExportMemoryAllocateInfo exporting{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};exporting.handleTypes=external.handleTypes;
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.pNext=&exporting;
            allocate.allocationSize=needs.size;allocate.memoryTypeIndex=gpu.MemoryType(needs.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Vk(gpu.vkAllocateMemory(gpu.device,&allocate,nullptr,&memories[0]),"allocate exportable memory");
            Vk(gpu.vkBindBufferMemory(gpu.device,buffers[0],memories[0],0),"bind exported buffer");
            auto getHandle=reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(gpu.getDeviceProc(gpu.device,"vkGetMemoryWin32HandleKHR"));
            Require(getHandle!=nullptr,"Win32 export function");
            VkMemoryGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
            get.memory=memories[0];get.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
            HANDLE handle=nullptr;Vk(getHandle(gpu.device,&get,&handle),"export Windows memory handle");
            VkImportMemoryWin32HandleInfoKHR importing{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
            importing.handleType=get.handleType;importing.handle=handle;allocate.pNext=&importing;
            Vk(gpu.vkAllocateMemory(gpu.device,&allocate,nullptr,&memories[1]),"import external memory while mirroring");
            DWORD flags=0;Require(GetHandleInformation(handle,&flags)!=FALSE,"both imports retain caller ownership of the Windows handle");
            CloseHandle(handle);
            Vk(gpu.vkBindBufferMemory(gpu.device,buffers[1],memories[1],0),"bind imported buffer");
            for(int i=1;i>=0;--i) {gpu.vkDestroyBuffer(gpu.device,buffers[i],nullptr);gpu.vkFreeMemory(gpu.device,memories[i],nullptr);}
        }
        if (gpuTransport)
        {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &id;
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gpu.gipa(gpu.instance, "vkGetPhysicalDeviceProperties2"))(gpu.physical, &props);
            LUID luid{}; std::memcpy(&luid, id.deviceLUID, sizeof(luid));
            std::unique_ptr<xg::gpu_test::Snapshot> snapshot;
            if (!serveGpu) { snapshot = std::make_unique<xg::gpu_test::Snapshot>(); snapshot->Init(luid, Width, Height, wide); }
            Scene scene(gpu); scene.Draw(true, wsi.format);
            if (mirrorFallback) {
                // Two stereo frames, then an external semaphore import (which a
                // right-eye twin cannot share) stops the mirror. The SAME recorded command buffer (left-eye shader
                // patched with a live offset) must then render unshifted: the
                // layer zeroes the offsets on the GPU at the next submission.
                for (uint64_t frame = 0; frame < 2; ++frame) {
                    if (frame) {
                        Vk(gpu.vkResetFences(gpu.device, 1, &scene.fence), "reset stereo fence");
                        wsi.SubmitCommand(scene.command, scene.fence, uint32_t(frame % 3));
                        Vk(gpu.vkWaitForFences(gpu.device, 1, &scene.fence, VK_TRUE, 5000000000ull), "stereo frame completes");
                    }
                    wsi.Present(scene.image, uint32_t((frame + 1) % 3));
                    xg::PairFrame pair; const auto timeout = GetTickCount64() + 5000;
                    while (!snapshot->Read(pair) && GetTickCount64() < timeout) Sleep(1);
                    Require(pair.header.sequence != 0, "stereo pair before the stop"); CheckPair(pair, frame + 1, 1, 0, true);
                }
                auto getHandle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(gpu.getDeviceProc(gpu.device, "vkGetSemaphoreWin32HandleKHR"));
                auto importHandle = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(gpu.getDeviceProc(gpu.device, "vkImportSemaphoreWin32HandleKHR"));
                Require(getHandle && importHandle, "Win32 semaphore functions");
                VkExportSemaphoreCreateInfo exporting{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; exporting.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                VkSemaphoreCreateInfo exportable{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; exportable.pNext = &exporting;
                VkSemaphoreCreateInfo plain{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                VkSemaphore source{}, target{};
                Vk(wsi.vkCreateSemaphore(gpu.device, &exportable, nullptr, &source), "exportable semaphore");
                Vk(wsi.vkCreateSemaphore(gpu.device, &plain, nullptr, &target), "import target semaphore");
                VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR}; get.semaphore = source; get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
                HANDLE handle = nullptr; Vk(getHandle(gpu.device, &get, &handle), "export semaphore handle");
                VkImportSemaphoreWin32HandleInfoKHR importing{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
                importing.semaphore = target; importing.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT; importing.handle = handle;
                Vk(importHandle(gpu.device, &importing), "import still succeeds for the game");
                CloseHandle(handle);
                wsi.vkDestroySemaphore(gpu.device, target, nullptr); wsi.vkDestroySemaphore(gpu.device, source, nullptr);
                for (uint32_t frame = 0; frame < 3; ++frame) {
                    Vk(gpu.vkResetFences(gpu.device, 1, &scene.fence), "reset fallback fence");
                    wsi.SubmitCommand(scene.command, scene.fence, frame % 3);
                    Vk(gpu.vkWaitForFences(gpu.device, 1, &scene.fence, VK_TRUE, 5000000000ull), "fallback frame completes");
                    wsi.Present(scene.image, (frame + 1) % 3);
                }
                void* mapped = nullptr;
                Vk(gpu.vkMapMemory(gpu.device, scene.readMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "map fallback readback");
                std::vector<uint8_t> pixels(static_cast<const uint8_t*>(mapped), static_cast<const uint8_t*>(mapped) + size_t(Width) * Height * 4);
                gpu.vkUnmapMemory(gpu.device, scene.readMemory);
                if (wsi.format == VK_FORMAT_B8G8R8A8_UNORM) for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
                for (uint32_t post = 0; post < 3; ++post) {
                    std::printf("fallback post %u: centroid %.2f\n", post, Centroid(pixels, post));
                    Require(std::abs(Centroid(pixels, post) - 63.5) < .01, "after a mirror stop the game renders unshifted (offsets zeroed on the GPU)");
                }
                Vk(gpu.vkDeviceWaitIdle(gpu.device), "complete fallback fixture");
                std::string log;
                { std::ifstream input{std::filesystem::path(layerLog)}; log.assign(std::istreambuf_iterator<char>(input), {}); }
                Require(log.find("mirror STOPPED: external semaphore import") != std::string::npos && log.find("eye offsets zeroed on the GPU") != std::string::npos,
                    "stop logged and offsets zeroed");
                Require(log.find("right-eye allocations") != std::string::npos, "right-eye resources released after the stop");
                std::puts("PASS: mirror fallback zeroes both eye offsets on the GPU, renders cached commands unshifted and releases right-eye resources");
                return 0;
            }
            if(mirrorLive) {
                auto state=[&]() {std::ifstream input(std::filesystem::path(tuning.path+L".live.json"));return std::string(std::istreambuf_iterator<char>(input),{});};
                auto value=[](const std::string& json,const char* key) {auto at=json.find(key);Require(at!=std::string::npos,"live tuning acknowledgement field");return std::strtof(json.c_str()+at+std::strlen(key),nullptr);};
                uint64_t frame=0;
                auto phase=[&](const char* text,float separation,float convergence) {
                    tuning.Write(text);const auto start=GetTickCount64(),until=start+5000;uint32_t stable=0;
                    while(GetTickCount64()<until) {
                        if(frame) {
                            Vk(gpu.vkResetFences(gpu.device,1,&scene.fence),"reset live tuning scene fence");
                            wsi.SubmitCommand(scene.command,scene.fence,uint32_t(frame%3));
                            Vk(gpu.vkWaitForFences(gpu.device,1,&scene.fence,VK_TRUE,5000000000ull),"reuse the same recorded draw after tuning");
                        }
                        wsi.Present(scene.image,uint32_t((frame+1)%3));++frame;
                        xg::PairFrame pair;const auto readUntil=GetTickCount64()+3000;
                        while(!snapshot->Read(pair) && GetTickCount64()<readUntil) Sleep(1);
                        Require(pair.header.sequence!=0,"live tuning keeps publishing GPU pairs");
                        const auto json=state();
                        bool applied=!json.empty() && value(json,"\"Separation\":")==separation && value(json,"\"Convergence\":")==convergence;
                        stable=applied?stable+1:0;
                        if(stable>=3 && GetTickCount64()-start>=250) {
                            CheckPair(pair,frame,separation/.125,convergence*Width/2,true);
                            return value(json,"\"Revision\":");
                        }
                        Sleep(5);
                    }
                    throw std::runtime_error("live tuning did not reach the expected GPU values");
                };
                phase("0.125 0\n",.125f,0);
                phase("0.25 0\n",.25f,0);
                phase("0.25 0.015625\n",.25f,.015625f);
                const auto revision=phase("0.0625 0\n",.0625f,0);
                Require(phase("NaN 0\n",.0625f,0)==revision,"invalid tuning retains the last acknowledged values");
                Require(phase("0.5 ",.0625f,0)==revision,"partial tuning cannot update only one eye/value");
                phase("0.125 0\n",.125f,0);
                Vk(gpu.vkDeviceWaitIdle(gpu.device),"complete live tuning fixture");
                std::puts("PASS: live same-frame separation and convergence; GPU-ordered paired updates; reused shaders/commands; HUD unchanged; invalid/partial input ignored");
                return 0;
            }
            const auto deadline = GetTickCount64() + (serveGpu ? 120000 : 15000);
            void* vertices = nullptr;
            const size_t vertexBytes = 18 * 4 * sizeof(float);
            std::vector<float> original(18*4);
            if (mirror) {
                Vk(gpu.vkMapMemory(gpu.device, scene.vertexMemory, 0, VK_WHOLE_SIZE, 0, &vertices), "map persistent simulation input");
                memcpy(original.data(),vertices,vertexBytes);
            }
            struct Contention {
                std::atomic<bool> stop{false}; std::atomic<uint32_t> holds{0};
                HANDLE mutex = nullptr, mapping = nullptr; const xg::GpuHeader* header = nullptr; std::thread worker;
                void Start(const std::wstring& session) {
                    mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, xg::GpuName(session,L".lock").c_str());
                    Require(mutex!=nullptr,"open contention mutex");
                    mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,xg::GpuName(session,L".frames").c_str());
                    Require(mapping!=nullptr,"open contention metadata");
                    header=(const xg::GpuHeader*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(xg::GpuHeader));
                    Require(header!=nullptr,"map contention metadata");
                    worker=std::thread([this] {
                        std::array<uint64_t,xg::GpuSlots> previous{};
                        while (!stop) {
                            if (WaitForSingleObject(mutex,0)==WAIT_OBJECT_0) {
                                bool reserved=false;
                                for (uint32_t i=0;i<xg::GpuSlots;++i) {
                                    const auto seq=header->slots[i].sequence;
                                    reserved |= previous[i]!=0 && seq==0;
                                    previous[i]=seq;
                                }
                                // Hold only AFTER a real reservation revokes its
                                // old metadata, covering the publication attempt.
                                if (reserved) { ++holds; Sleep(25); }
                                ReleaseMutex(mutex);
                            }
                            SwitchToThread();
                        }
                    });
                }
                void Finish() { stop=true; if (worker.joinable()) worker.join(); }
                ~Contention() { Finish(); if (header) UnmapViewOfFile(header); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
            } busy;
            if (contention) busy.Start(wide);
            uint64_t frame = 0;
            bool contentionDone = !contention;
            uint32_t recoveredPairs = 0;
            while (GetTickCount64() < deadline && (serveGpu || (contention ? recoveredPairs < 10 : frame < 10)))
            {
                if (mirror && frame) {
                    auto* v = static_cast<float*>(vertices);
                    for (size_t i=0;i<18;++i) v[i*4]=original[i*4]+float(frame%16)*.03125f*original[i*4+3];
                }
                if (frame) {
                    Vk(gpu.vkResetFences(gpu.device, 1, &scene.fence), "reset scene fence");
                    wsi.SubmitCommand(scene.command, scene.fence, uint32_t(frame % 3));
                    Vk(gpu.vkWaitForFences(gpu.device, 1, &scene.fence, VK_TRUE, 5000000000ull), "scene replay completes");
                }
                wsi.Present(scene.image, uint32_t((frame + 1) % 3));
                if (contention && !contentionDone) {
                    // Scheduling determines how often the worker observes the
                    // short reservation window. Keep the coverage target, but
                    // bound it by the test deadline rather than 40 frames.
                    if (frame >= 40 && busy.holds >= 6) { busy.Finish(); contentionDone = true; }
                    ++frame; Sleep(4); continue;
                }
                if (snapshot && (mirror || (frame & 1))) {
                    xg::PairFrame pair; const auto timeout = GetTickCount64() + 5000;
                    while (!snapshot->Read(pair) && GetTickCount64() < timeout) Sleep(1);
                    Require(pair.header.sequence != 0, "GPU pair completed asynchronously"); CheckPair(pair, mirror ? frame + 1 : frame, 1, 0, mirror, mirror ? double(frame%16)*2 : 0);
                    if (contention) ++recoveredPairs;
                }
                ++frame; if (serveGpu) Sleep(16);
            }
            Require(!contention || (busy.holds >= 6 && recoveredPairs == 10), "six real metadata holds followed by ten complete fresh pairs");
            if (vertices) gpu.vkUnmapMemory(gpu.device,scene.vertexMemory);
            Vk(gpu.vkDeviceWaitIdle(gpu.device), "wait test producer idle");
            std::puts("PASS: Vulkan swapchain -> shared D3D12 array/fences -> D3D12 consumer; no CPU transport pixels");
            return 0;
        }
        xg::PairReader reader;
        if (unsupported)
        {
            Require(!reader.Open(wide), "unsupported graphics queue configuration must not create a stereo stream");
            Scene scene(gpu);
            auto pixels = scene.Draw(true, wsi.format);
            for (size_t i = 0; i < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            for (uint32_t post = 0; post < 3; ++post) Require(std::abs(Centroid(pixels, post) - 63.5) < .01, "unsupported setup keeps original mono geometry");
            wsi.Present(scene.image, 0);
            std::puts("PASS: multiple graphics queues disable the layer's stereo changes; original rendering and presentation continue");
            return 0;
        }
        Require(reader.Open(wide), "layer must create the export session");
        if (!unsupportedShader)
        {
            Scene loading(gpu);
            loading.Draw(true, wsi.format, true);
            wsi.Present(loading.image, 0);
            wsi.Present(loading.image, 1);
            xg::PairFrame none;
            Require(reader.Read(none) == xg::PairRead::Empty, "startup presents wait for supported geometry without exporting mono");
        }
        Scene scene(gpu);
        scene.Draw(true, wsi.format, unsupportedShader, inlineShader); // creates the pipeline/draw commands only once
        if (unsupportedShader)
        {
            wsi.Present(scene.image, 0);
            xg::PairFrame none;
            Require(reader.Read(none) == xg::PairRead::Empty, "unsupported shaders must leave the stream empty instead of exporting mono as stereo");
            std::puts("PASS: unsupported vertex module leaves rendering intact without exporting mono as stereo");
            return 0;
        }
        xg::PairFrame pair;
        const auto deadline = GetTickCount64() + (serve ? 120000 : 15000);
        uint64_t frame = 0, pairs = 0;
        while (GetTickCount64() < deadline && (serve || frame < (liveTuning ? 8u : 6u)))
        {
            if (liveTuning && (frame == 2 || frame == 4 || frame == 6)) {
                tuning.Write(frame == 2 ? "0.0625 0.015625\n" : frame == 4 ? "nan 0\n" : "0 0\n");
                Sleep(250); // cross the bounded file-poll interval
            }
            if (frame)
            {
                Vk(gpu.vkResetFences(gpu.device, 1, &scene.fence), "reset reused scene fence");
                wsi.SubmitCommand(scene.command, scene.fence, (uint32_t)(frame % 3));
                Vk(gpu.vkWaitForFences(gpu.device, 1, &scene.fence, VK_TRUE, 5000000000ull), "cached draw list completes");
            }
            wsi.Present(scene.image, (uint32_t)((frame + 1) % 3));
            const auto read = reader.Read(pair, pairs);
            if (frame & 1)
            {
                Require(read == xg::PairRead::NewPair, "right present publishes a complete pair");
                if (!serve || frame == 1) CheckPair(pair, frame,
                    liveTuning && frame >= 6 ? 0 : liveTuning && frame >= 2 ? .5 : 1,
                    liveTuning && frame >= 2 && frame < 6 ? 1 : 0);
                ++pairs;
            }
            else Require(read == (frame ? xg::PairRead::Unchanged : xg::PairRead::Empty), "left present never publishes a half pair");
            ++frame;
            if (serve) Sleep(16);
        }
        Require(pairs >= 3 || serve, "three alternating pairs completed");
        wsi.Close();
        Require(reader.Read(pair) == xg::PairRead::Closed, "destroying the producer swapchain closes the stream");
        std::puts("PASS: single-process alternate eyes, live shader buffer, cached commands, submit/submit2/KHR, real presents, paired export and shutdown");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
