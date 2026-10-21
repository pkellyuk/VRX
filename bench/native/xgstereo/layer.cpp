// Copyright (C) 2026 Paul Kelly. SPDX-License-Identifier: GPL-3.0-or-later
// Explicit Vulkan layer. No background threads, global installation, or UI.
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include "clip_patch.h"
#include "device_chain.h"
#include "alternate_device.h"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

#define XG_EXPORT extern "C" __declspec(dllexport)

XG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
XG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleCreateInfoIdentifierEXT(VkDevice, const VkShaderModuleCreateInfo*, VkShaderModuleIdentifierEXT*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);

namespace mirror { struct MDev; }
namespace {
template<typename Handle> void* Key(Handle handle) { return *reinterpret_cast<void**>(handle); }
struct Instance {
    PFN_vkGetInstanceProcAddr next;
    PFN_vkDestroyInstance destroy;
    PFN_vkCreateDevice createDevice;
    VkInstance handle = VK_NULL_HANDLE;
    uint32_t apiVersion = VK_API_VERSION_1_0;
};
struct Device {
    PFN_vkGetDeviceProcAddr next;
    PFN_vkDestroyDevice destroy;
    PFN_vkCreateShaderModule createShader;
    float shift = 0;
    uint64_t patched = 0, untouched = 0;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties formatProperties = nullptr;
    std::shared_ptr<xg::AlternateDevice> alternate;
    std::shared_ptr<mirror::MDev> mirror;
    VkDevice handle = VK_NULL_HANDLE;
};
struct State {
    std::mutex mutex;
    std::unordered_map<void*, Instance> instances;
    std::unordered_map<void*, Device> devices;
};
// Driver callbacks may outlive C++ static destruction.
State& Data() { static auto* state = new State; return *state; }

void Log(const char* message)
{
    std::fprintf(stderr, "[VRX xgstereo] %s\n", message);
    OutputDebugStringA("[VRX xgstereo] "); OutputDebugStringA(message); OutputDebugStringA("\n");
    // Opt-in diagnostics for games that do not expose a console. Win32 writes
    // also work before the host's C runtime/console has finished initializing.
    wchar_t path[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"VRX_XG_LOG", path, 32768);
    if (length && length < 32768)
    {
        const HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
            CloseHandle(file);
        }
    }
}

// No configuration means inert. Settings are read once per VkDevice; change eye
// by restarting the application. Never register this as an implicit layer.
float EyeShift()
{
    char eye[16]{}, text[64]{};
    if (!GetEnvironmentVariableA("VRX_XG_EYE", eye, sizeof(eye))) return 0;
    const bool left = std::strcmp(eye, "left") == 0, right = std::strcmp(eye, "right") == 0;
    const bool alternate = std::strcmp(eye, "alternate") == 0 || std::strcmp(eye, "mirror") == 0;
    if (!left && !right && !alternate) { Log("invalid VRX_XG_EYE; leaving shaders unchanged"); return 0; }
    float shift = 0.032f;
    const DWORD length = GetEnvironmentVariableA("VRX_XG_SHIFT", text, sizeof(text));
    if (length)
    {
        if (length >= sizeof(text))
        { Log("VRX_XG_SHIFT is too long; leaving shaders unchanged"); return 0; }
        // The application's locale must not change how launch settings parse.
        const auto parsed = std::from_chars(text, text + length, shift);
        if (parsed.ec != std::errc{} || parsed.ptr != text + length || !std::isfinite(shift) || shift < 0 || shift > 64.f)
        { Log("invalid VRX_XG_SHIFT (expected 0..64); leaving shaders unchanged"); return 0; }
    }
    if (!alternate) Log(left ? "fixed LEFT eye enabled (experimental)" : "fixed RIGHT eye enabled (experimental)");
    char message[100]; std::snprintf(message, sizeof(message), "eye offset magnitude: %.6g clip units", shift); Log(message);
    return right ? -shift : shift;
}

bool MirrorRequested() { char value[32]{}; return GetEnvironmentVariableA("VRX_XG_EYE", value, 32) && std::strcmp(value, "mirror") == 0; }

bool AlternateRequested()
{
    char eye[16]{};
    return GetEnvironmentVariableA("VRX_XG_EYE", eye, sizeof(eye)) && std::strcmp(eye, "alternate") == 0;
}
bool GpuRequested()
{
    char text[16]{};
    return GetEnvironmentVariableA("VRX_XG_TRANSPORT", text, sizeof(text)) && !std::strcmp(text, "gpu");
}

Instance FindInstance(VkInstance instance)
{
    auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
    const auto it = data.instances.find(Key(instance));
    return it == data.instances.end() ? Instance{} : it->second;
}
Device FindDevice(VkDevice device)
{
    auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
    const auto it = data.devices.find(Key(device));
    return it == data.devices.end() ? Device{} : it->second;
}

// Inline shader code (used by DXVK) bypasses vkCreateShaderModule entirely.
// Preserve all stage metadata and app-owned pNext links while replacing code.
struct InlineShader {
    xg::DeviceChain chain;
    std::vector<uint32_t> code;
};
bool PatchCode(const Device& state, const VkShaderModuleCreateInfo& info, std::vector<uint32_t>& code)
{
    if ((!state.shift && !state.alternate) || !info.pCode || info.codeSize % sizeof(uint32_t)) return false;
    code.assign(info.pCode, info.pCode + info.codeSize / sizeof(uint32_t));
    return xg::PatchClipX(code, state.shift, state.alternate ? state.alternate->Address() : 0, bool(state.alternate)).changed;
}
void PatchedInline(const Device& state)
{
    if (state.alternate) state.alternate->ShaderPatched();
    auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
    const auto it = data.devices.find(Key(state.handle));
    if (it != data.devices.end())
    {
        if (!it->second.patched) Log("first supported inline vertex shader patched");
        ++it->second.patched;
    }
}
} // namespace

#include "mirror_backend.h"

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* instance)
{
    auto* chain = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    VkLayerInstanceCreateInfo* link = nullptr;
    for (; chain; chain = chain->pNext)
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
        {
            auto* candidate = const_cast<VkLayerInstanceCreateInfo*>(reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain));
            if (candidate->function == VK_LAYER_LINK_INFO) { link = candidate; break; }
        }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) return result;
    Instance state{next,
        reinterpret_cast<PFN_vkDestroyInstance>(next(*instance, "vkDestroyInstance")),
        reinterpret_cast<PFN_vkCreateDevice>(next(*instance, "vkCreateDevice"))};
    state.handle = *instance;
    if (info->pApplicationInfo && info->pApplicationInfo->apiVersion) state.apiVersion = info->pApplicationInfo->apiVersion;
    char message[160];
    std::snprintf(message, sizeof(message), "instance created: Vulkan %u.%u.%u",
        VK_API_VERSION_MAJOR(state.apiVersion), VK_API_VERSION_MINOR(state.apiVersion), VK_API_VERSION_PATCH(state.apiVersion));
    Log(message);
    try
    {
        auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
        data.instances.emplace(Key(*instance), state);
    }
    catch (...)
    {
        state.destroy(*instance, allocator); *instance = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
    if (!instance) return;
    const auto state = FindInstance(instance);
    { auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex); data.instances.erase(Key(instance)); }
    if (state.destroy) state.destroy(instance, allocator);
}

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device)
{
    // Physical devices share their instance's loader dispatch key.
    Instance parent{};
    {
        auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
        const auto it = data.instances.find(Key(physical));
        if (it != data.instances.end()) parent = it->second;
    }
    auto* chain = reinterpret_cast<const VkBaseInStructure*>(info->pNext);
    VkLayerDeviceCreateInfo* link = nullptr;
    for (; chain; chain = chain->pNext)
        if (chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
        {
            auto* candidate = const_cast<VkLayerDeviceCreateInfo*>(reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain));
            if (candidate->function == VK_LAYER_LINK_INFO) { link = candidate; break; }
        }
    if (!parent.createDevice || !link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkSetDeviceLoaderData setLoaderData = nullptr;
    for (auto* node = reinterpret_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
        {
            const auto* loader = reinterpret_cast<const VkLayerDeviceCreateInfo*>(node);
            if (loader->function == VK_LOADER_DATA_CALLBACK) setLoaderData = loader->u.pfnSetDeviceLoaderData;
        }
    const auto next = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const bool mirrorRequested = MirrorRequested();
    const bool requested = AlternateRequested() || mirrorRequested;
    bool enable = false;
    uint32_t graphicsFamily = 0;
    xg::DeviceChain copied;
    VkPhysicalDeviceBufferDeviceAddressFeatures addressFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    const bool gpuRequested = GpuRequested();
    LUID adapter{};
    std::vector<const char*> extensions;
    VkDeviceCreateInfo changed = *info;
    VkPhysicalDeviceMemoryProperties memory{};
    if (requested && parent.apiVersion >= VK_API_VERSION_1_2)
    {
        try
        {
            auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(parent.next(parent.handle, "vkGetPhysicalDeviceFeatures2"));
            auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(parent.next(parent.handle, "vkGetPhysicalDeviceQueueFamilyProperties"));
            auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(parent.next(parent.handle, "vkGetPhysicalDeviceProperties"));
            auto mem = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(parent.next(parent.handle, "vkGetPhysicalDeviceMemoryProperties"));
            VkPhysicalDeviceProperties properties{}; props(physical, &properties);
            VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; supported.pNext = &addressFeature;
            if (gpuRequested) addressFeature.pNext = &timelineFeature;
            features(physical, &supported);
            uint32_t count = 0; queues(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count); queues(physical, &count, families.data());
            uint32_t graphicsQueues = 0;
            bool protectedQueue = false, swapchainExtension = false;
            bool legacyAddressFeature = false, multiplePhysicalDevices = false;
            for (auto* node = static_cast<const VkBaseInStructure*>(info->pNext); node; node = node->pNext)
            {
                if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT) legacyAddressFeature = true;
                if (node->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO &&
                    reinterpret_cast<const VkDeviceGroupDeviceCreateInfo*>(node)->physicalDeviceCount != 1) multiplePhysicalDevices = true;
            }
            for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
            {
                const auto& q = info->pQueueCreateInfos[i];
                if (q.flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) protectedQueue = true;
                if (q.queueFamilyIndex < count && (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                { graphicsQueues += q.queueCount; graphicsFamily = q.queueFamilyIndex; }
            }
            for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
                if (!std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchainExtension = true;
            enable = properties.apiVersion >= VK_API_VERSION_1_2 && addressFeature.bufferDeviceAddress && graphicsQueues == 1 &&
                !protectedQueue && !legacyAddressFeature && !multiplePhysicalDevices && swapchainExtension && setLoaderData && copied.Clone(info->pNext);
            if (enable && gpuRequested)
            {
                const auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(parent.next(parent.handle, "vkEnumerateDeviceExtensionProperties"));
                uint32_t n = 0;
                if (!timelineFeature.timelineSemaphore || enumerate(physical, nullptr, &n, nullptr) != VK_SUCCESS) enable = false;
                std::vector<VkExtensionProperties> available(n);
                if (enable && enumerate(physical, nullptr, &n, available.data()) != VK_SUCCESS) enable = false;
                if (info->enabledExtensionCount) extensions.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
                for (const char* name : {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME})
                {
                    bool found = false, already = false;
                    for (const auto& e : available) if (!std::strcmp(e.extensionName, name)) found = true;
                    for (const auto* e : extensions) if (!std::strcmp(e, name)) already = true;
                    if (!found) enable = false;
                    if (!already) extensions.push_back(name);
                }
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties2.pNext = &id;
                const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(parent.next(parent.handle, "vkGetPhysicalDeviceProperties2"));
                getProperties(physical, &properties2);
                if (!id.deviceLUIDValid || id.deviceNodeMask != 1) enable = false;
                std::memcpy(&adapter, id.deviceLUID, sizeof(adapter));
                VkPhysicalDeviceExternalImageFormatInfo externalImage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
                externalImage.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
                VkPhysicalDeviceImageFormatInfo2 formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
                formatInfo.pNext = &externalImage; formatInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
                formatInfo.type = VK_IMAGE_TYPE_2D; formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL; formatInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; imageProperties.pNext = &externalProperties;
                const auto imageFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(parent.next(parent.handle, "vkGetPhysicalDeviceImageFormatProperties2"));
                if (imageFormats(physical, &formatInfo, &imageProperties) != VK_SUCCESS || imageProperties.imageFormatProperties.maxArrayLayers < 2 ||
                    !(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) enable = false;
                VkSemaphoreTypeCreateInfo semaphoreType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
                semaphoreInfo.pNext = &semaphoreType; semaphoreInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
                VkExternalSemaphoreProperties semaphoreProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
                const auto semaphoreFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(parent.next(parent.handle, "vkGetPhysicalDeviceExternalSemaphoreProperties"));
                semaphoreFormats(physical, &semaphoreInfo, &semaphoreProperties);
                if (!(semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) enable = false;
            }
            if (enable)
            {
                changed.pNext = copied.head;
                if (!copied.EnableAddressFeature())
                { addressFeature.pNext = copied.head; addressFeature.bufferDeviceAddress = VK_TRUE; changed.pNext = &addressFeature; }
                if (gpuRequested)
                {
                    if (!copied.EnableTimelineFeature())
                    { timelineFeature.pNext = const_cast<void*>(changed.pNext); timelineFeature.timelineSemaphore = VK_TRUE; changed.pNext = &timelineFeature; }
                    changed.enabledExtensionCount = uint32_t(extensions.size()); changed.ppEnabledExtensionNames = extensions.data();
                }
                mem(physical, &memory);
            }
        }
        catch (...) { enable = false; }
    }
    if (requested && !enable) Log(gpuRequested ? "GPU geometry unavailable: requires one graphics queue, supported device chain, buffer addresses, timeline semaphores, D3D12 texture/fence import and an adapter LUID" :
        "alternate mode unavailable: requires Vulkan 1.2, buffer device addresses, one unprotected graphics queue, swapchains and a supported device chain");
    const VkResult result = parent.createDevice(physical, enable ? &changed : info, allocator, device);
    if (result != VK_SUCCESS) return result;
    Device state{next,
        reinterpret_cast<PFN_vkDestroyDevice>(next(*device, "vkDestroyDevice")),
        reinterpret_cast<PFN_vkCreateShaderModule>(next(*device, "vkCreateShaderModule")), EyeShift()};
    state.physical = physical;
    state.handle = *device;
    state.surfaceCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(parent.next(parent.handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    state.formatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(parent.next(parent.handle, "vkGetPhysicalDeviceFormatProperties"));
    if (requested)
    {
        const float shift = state.shift;
        state.shift = 0; // a failed alternate setup must never become a fixed left eye
        try
        {
            wchar_t name[65]{};
            const DWORD length = GetEnvironmentVariableW(L"VRX_XG_SESSION", name, 65);
            if (enable && shift > 0 && length > 0 && length < 65 && xg::ValidSession(name))
            {
                auto alternate = std::make_shared<xg::AlternateDevice>();
                if (!mirrorRequested && alternate->Init(*device, next, setLoaderData, memory, graphicsFamily, shift, name, Log, gpuRequested, adapter))
                { state.alternate = std::move(alternate); Log(gpuRequested ? "alternate-eye GPU geometry enabled (half rate per eye, asynchronous shared textures)" : "alternate-eye geometry export enabled (single queue, half rate per eye, synchronous readback)"); }
                else if (mirrorRequested && gpuRequested)
                {
                    // Extensions/versions the generated mirror does not cover could
                    // render a silently wrong right eye: keep ordinary rendering.
                    if (const char* unknown = mirror::unsupported_device_feature(info, parent.apiVersion))
                    {
                        char message[320];
                        std::snprintf(message, sizeof(message), "mirror mode unavailable: %s is not handled by the mirror; ordinary rendering kept", unknown);
                        Log(message);
                    }
                    else state.mirror = mirror::create(*device, next, setLoaderData, memory, graphicsFamily, adapter, shift, name);
                    if (!state.mirror) Log("mirror mode not active for this device");
                }
                else Log("alternate mode unavailable: live buffer or session initialization failed");
            }
            else if (enable) Log("alternate mode unavailable: requires positive shift and a valid VRX_XG_SESSION");
        }
        catch (...) { Log("alternate mode unavailable: resource allocation failed"); }
    }
    try
    {
        auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
        data.devices.emplace(Key(*device), state);
    }
    catch (...)
    {
        state.destroy(*device, allocator); *device = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return result;
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    if (!device) return;
    const auto state = FindDevice(device);
    if (state.mirror) mirror::shutdown(state.mirror.get());
    { auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex); data.devices.erase(Key(device)); }
    if (state.alternate) state.alternate->Shutdown();
    if (state.shift || state.alternate)
    {
        char message[160];
        std::snprintf(message, sizeof(message), "device closed: %llu shader modules shifted, %llu unchanged",
            static_cast<unsigned long long>(state.patched), static_cast<unsigned long long>(state.untouched));
        Log(message);
    }
    if (state.destroy) state.destroy(device, allocator);
}

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device,
    const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator, VkShaderModule* module)
{
    const auto state = FindDevice(device);
    if (!state.createShader) return VK_ERROR_INITIALIZATION_FAILED;
    if ((!state.shift && !state.alternate) || !info->pCode || info->codeSize % sizeof(uint32_t))
        return state.createShader(device, info, allocator, module);
    try
    {
        std::vector<uint32_t> code(info->pCode, info->pCode + info->codeSize / sizeof(uint32_t));
        const auto patch = xg::PatchClipX(code, state.shift, state.alternate ? state.alternate->Address() : 0, bool(state.alternate));
        VkShaderModuleCreateInfo changed = *info;
        if (patch.changed) { changed.pCode = code.data(); changed.codeSize = code.size() * sizeof(uint32_t); }
        const VkResult result = state.createShader(device, &changed, allocator, module);
        if (patch.changed && result == VK_SUCCESS && state.alternate) state.alternate->ShaderPatched();
        // Driver rejection is reported normally: silently retrying mono would
        // conceal a broken shader transformation during prototype testing.
        {
            auto& data = Data(); std::lock_guard<std::mutex> lock(data.mutex);
            const auto it = data.devices.find(Key(device));
            if (it != data.devices.end())
            {
                if (patch.changed && result == VK_SUCCESS) ++it->second.patched;
                else ++it->second.untouched;
            }
        }
        return result;
    }
    catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
    uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    const auto state = FindDevice(device);
    const auto next = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(state.next(device, "vkCreateGraphicsPipelines"));
    if ((!state.shift && !state.alternate) || !count) return next(device, cache, count, infos, allocator, pipelines);
    try
    {
        std::vector<VkGraphicsPipelineCreateInfo> changed(infos, infos + count);
        std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(count);
        std::vector<std::unique_ptr<InlineShader>> shaders;
        std::vector<bool> patched(count, false);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!infos[i].stageCount) continue; // pipeline library links can have no stages
            stages[i].assign(infos[i].pStages, infos[i].pStages + infos[i].stageCount);
            changed[i].pStages = stages[i].data();
            for (auto& stage : stages[i])
            {
                if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT || stage.module) continue;
                const VkShaderModuleCreateInfo* module = nullptr;
                for (auto* node = static_cast<const VkBaseInStructure*>(stage.pNext); node; node = node->pNext)
                    if (node->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
                        module = reinterpret_cast<const VkShaderModuleCreateInfo*>(node);
                if (!module) continue;
                auto shader = std::make_unique<InlineShader>();
                if (!PatchCode(state, *module, shader->code)) continue;
                if (!shader->chain.Clone(stage.pNext))
                {
                    if (state.alternate) state.alternate->Unsupported("alternate mode stopped: unsupported inline shader stage chain");
                    continue;
                }
                for (auto* node = static_cast<VkBaseOutStructure*>(shader->chain.head); node; node = node->pNext)
                    if (node->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
                    {
                        auto* copy = reinterpret_cast<VkShaderModuleCreateInfo*>(node);
                        copy->pCode = shader->code.data(); copy->codeSize = shader->code.size() * sizeof(uint32_t);
                    }
                stage.pNext = shader->chain.head;
                shaders.push_back(std::move(shader));
                patched[i] = true;
            }
        }
        const auto result = next(device, cache, count, changed.data(), allocator, pipelines);
        for (uint32_t i = 0; i < count; ++i)
            if (patched[i] && pipelines[i]) PatchedInline(state);
        // Fixed/alternate modes shift the vertex stage only. A later
        // tessellation, geometry or mesh stage may recompute Position and
        // render flat: say so (rate limited) instead of failing silently.
        static std::atomic<uint64_t> lateStages{0};
        for (uint32_t i = 0; i < count; ++i)
        {
            bool late = false;
            for (uint32_t s = 0; s < infos[i].stageCount && infos[i].pStages; ++s)
                late = late || (infos[i].pStages[s].stage & (VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                    VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT));
            if (!late) continue;
            const auto n = ++lateStages;
            if (n > 8 && n % 500) continue;
            char message[200];
            std::snprintf(message, sizeof(message), "pipeline with a tessellation/geometry/mesh stage: only the vertex stage is shifted, it may render flat (%llu so far)",
                static_cast<unsigned long long>(n));
            Log(message);
        }
        return result;
    }
    catch (...) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetShaderModuleCreateInfoIdentifierEXT(VkDevice device,
    const VkShaderModuleCreateInfo* info, VkShaderModuleIdentifierEXT* identifier)
{
    const auto state = FindDevice(device);
    const auto next = reinterpret_cast<PFN_vkGetShaderModuleCreateInfoIdentifierEXT>(state.next(device, "vkGetShaderModuleCreateInfoIdentifierEXT"));
    try
    {
        std::vector<uint32_t> code;
        if (PatchCode(state, *info, code))
        {
            // Identifiers must describe the same patched code as pipeline
            // creation, or the driver may retrieve the original mono shader.
            auto changed = *info; changed.pCode = code.data(); changed.codeSize = code.size() * sizeof(uint32_t);
            next(device, &changed, identifier);
            if (identifier->identifierSize) PatchedInline(state);
            return;
        }
    }
    catch (...) { if (state.alternate) state.alternate->Unsupported("alternate mode stopped: shader identifier allocation failed"); }
    next(device, info, identifier);
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    const auto state = FindDevice(device);
    const auto next = reinterpret_cast<PFN_vkGetDeviceQueue>(state.next(device, "vkGetDeviceQueue"));
    next(device, family, index, queue);
    if (state.alternate) state.alternate->SetQueue(*queue, family);
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    const auto state = FindDevice(device);
    const auto next = reinterpret_cast<PFN_vkGetDeviceQueue2>(state.next(device, "vkGetDeviceQueue2"));
    next(device, info, queue);
    if (state.alternate) state.alternate->SetQueue(*queue, info->queueFamilyIndex);
}

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    const auto state = FindDevice(reinterpret_cast<VkDevice>(queue));
    if (state.alternate)
    {
        try { return state.alternate->Submit(queue, count, submits, fence); }
        catch (...) { state.alternate->Unsupported("alternate mode stopped: submit allocation failed"); }
    }
    const auto next = reinterpret_cast<PFN_vkQueueSubmit>(state.next(state.handle, "vkQueueSubmit"));
    return next(queue, count, submits, fence);
}

static VkResult QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, bool khr)
{
    const auto state = FindDevice(reinterpret_cast<VkDevice>(queue));
    if (state.alternate)
    {
        try { return state.alternate->Submit2(queue, count, submits, fence, khr); }
        catch (...) { state.alternate->Unsupported("alternate mode stopped: submit2 allocation failed"); }
    }
    const auto next = reinterpret_cast<PFN_vkQueueSubmit2>(state.next(state.handle, khr ? "vkQueueSubmit2KHR" : "vkQueueSubmit2"));
    return next ? next(queue, count, submits, fence) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{ return QueueSubmit2(queue, count, submits, fence, false); }
XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{ return QueueSubmit2(queue, count, submits, fence, true); }

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
    const auto state = FindDevice(reinterpret_cast<VkDevice>(queue));
    if (state.mirror) return mirror::present(state.mirror.get(), queue, info);
    if (state.alternate)
    {
        // Capture allocates before consuming any present semaphores. Once its
        // copy is queued, Present always forwards the replacement semaphore.
        try { return state.alternate->Present(queue, info); }
        catch (...) { state.alternate->Unsupported("alternate mode stopped: capture allocation failed"); }
    }
    const auto next = reinterpret_cast<PFN_vkQueuePresentKHR>(state.next(state.handle, "vkQueuePresentKHR"));
    return next(queue, info);
}

XG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    const auto state = FindDevice(device);
    const auto next = reinterpret_cast<PFN_vkCreateSwapchainKHR>(state.next(device, "vkCreateSwapchainKHR"));
    if (state.mirror) return mirror::create_swap(state.mirror.get(), info, allocator, swapchain);
    auto changed = *info;
    const bool active = state.alternate && state.alternate->Active();
    if (active)
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        const auto format = info->imageFormat;
        // Exported bytes are display-referred sRGB values: HDR/wide-gamut
        // colour spaces are declined (as in mirror mode).
        bool formatOk = (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
            format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) && info->imageColorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        if (state.alternate->IsGpu())
        {
            // A blit from sRGB to UNORM decodes gamma. Until an exact sRGB
            // export path exists, decline it instead of silently darkening it.
            formatOk = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM;
            VkFormatProperties source{}, target{};
            if (state.formatProperties) {
                state.formatProperties(state.physical, format, &source);
                state.formatProperties(state.physical, VK_FORMAT_R8G8B8A8_UNORM, &target);
            }
            formatOk = formatOk && (source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
                (target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        }
        if (!state.surfaceCapabilities || state.surfaceCapabilities(state.physical, info->surface, &capabilities) != VK_SUCCESS ||
            !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !formatOk || info->imageArrayLayers != 1 ||
            (info->flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) ||
            !(state.alternate->IsGpu() ? xg::GpuShape(info->imageExtent.width, info->imageExtent.height) :
                xg::ValidPairShape(info->imageExtent.width, info->imageExtent.height, size_t(info->imageExtent.width) * info->imageExtent.height * 8)))
            state.alternate->Unsupported("alternate mode stopped: unsupported swapchain format, size, protection or transfer usage");
        else changed.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    const auto result = next(device, &changed, allocator, swapchain);
    if (result == VK_SUCCESS && active)
    {
        try { state.alternate->Created(*swapchain, changed); }
        catch (...) { state.alternate->Unsupported("alternate mode stopped: swapchain capture allocation failed"); }
    }
    return result;
}

XG_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator)
{
    const auto state = FindDevice(device);
    if (state.mirror) mirror::swapchain_destroyed(state.mirror.get(), swapchain);
    if (state.alternate) state.alternate->Destroyed(swapchain);
    const auto next = reinterpret_cast<PFN_vkDestroySwapchainKHR>(state.next(device, "vkDestroySwapchainKHR"));
    next(device, swapchain, allocator);
}

XG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
    if (!name) return nullptr;
    // Mirror entry points are returned only by vkGetDeviceProcAddr, and only
    // for a device whose mirror was actually created. Device commands queried
    // here reach a device through the loader's trampolines, which dispatch via
    // vkGetDeviceProcAddr, so a device without a mirror keeps driver functions.
#define XG_INSTANCE_HOOK(fn) if (std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    XG_INSTANCE_HOOK(vkGetInstanceProcAddr);
    XG_INSTANCE_HOOK(vkGetDeviceProcAddr);
    XG_INSTANCE_HOOK(vkCreateInstance);
    if (!instance) return nullptr;
    XG_INSTANCE_HOOK(vkDestroyInstance);
    XG_INSTANCE_HOOK(vkCreateDevice);
    XG_INSTANCE_HOOK(vkDestroyDevice);
    XG_INSTANCE_HOOK(vkCreateShaderModule);
    XG_INSTANCE_HOOK(vkCreateGraphicsPipelines);
    XG_INSTANCE_HOOK(vkGetDeviceQueue);
#define XG_OPTIONAL_INSTANCE(fn) if (std::strcmp(name, #fn) == 0) { const auto s = FindInstance(instance); return s.next(instance, name) ? reinterpret_cast<PFN_vkVoidFunction>(fn) : nullptr; }
    XG_OPTIONAL_INSTANCE(vkGetDeviceQueue2);
    XG_OPTIONAL_INSTANCE(vkGetShaderModuleCreateInfoIdentifierEXT);
    XG_OPTIONAL_INSTANCE(vkQueueSubmit);
    XG_OPTIONAL_INSTANCE(vkQueueSubmit2);
    XG_OPTIONAL_INSTANCE(vkQueueSubmit2KHR);
    XG_OPTIONAL_INSTANCE(vkQueuePresentKHR);
    XG_OPTIONAL_INSTANCE(vkCreateSwapchainKHR);
    XG_OPTIONAL_INSTANCE(vkDestroySwapchainKHR);
#undef XG_OPTIONAL_INSTANCE
#undef XG_INSTANCE_HOOK
    const auto state = FindInstance(instance);
    return state.next ? state.next(instance, name) : nullptr;
}

XG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    if (!name || !device) return nullptr;
    const auto mirrored = FindDevice(device);
    if (mirrored.mirror && mirrored.next(device, name)) if (auto fn = mirror::lookup(name)) return fn;
#define XG_DEVICE_HOOK(fn) if (std::strcmp(name, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    XG_DEVICE_HOOK(vkGetDeviceProcAddr);
    XG_DEVICE_HOOK(vkDestroyDevice);
    XG_DEVICE_HOOK(vkCreateShaderModule);
    XG_DEVICE_HOOK(vkCreateGraphicsPipelines);
    XG_DEVICE_HOOK(vkGetDeviceQueue);
#define XG_OPTIONAL_DEVICE(fn) if (std::strcmp(name, #fn) == 0) { const auto s = FindDevice(device); return s.next(device, name) ? reinterpret_cast<PFN_vkVoidFunction>(fn) : nullptr; }
    XG_OPTIONAL_DEVICE(vkGetDeviceQueue2);
    XG_OPTIONAL_DEVICE(vkGetShaderModuleCreateInfoIdentifierEXT);
    XG_OPTIONAL_DEVICE(vkQueueSubmit);
    XG_OPTIONAL_DEVICE(vkQueueSubmit2);
    XG_OPTIONAL_DEVICE(vkQueueSubmit2KHR);
    XG_OPTIONAL_DEVICE(vkQueuePresentKHR);
    XG_OPTIONAL_DEVICE(vkCreateSwapchainKHR);
    XG_OPTIONAL_DEVICE(vkDestroySwapchainKHR);
#undef XG_OPTIONAL_DEVICE
#undef XG_DEVICE_HOOK
    const auto state = FindDevice(device);
    return state.next ? state.next(device, name) : nullptr;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info)
{
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    info->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    info->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
