// Read-only Linux prerequisite probe. It does not start a VR session or capture.
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void LibraryProbe(const char* label, const char* soname)
{
    void* library = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    std::printf("%s: %s\n", label, library ? "found" : "unavailable");
    if (library) dlclose(library);
}

void VulkanProbe()
{
    uint32_t apiVersion = VK_API_VERSION_1_0;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (enumerateVersion) enumerateVersion(&apiVersion);
    std::printf("Vulkan loader: %u.%u.%u\n", VK_API_VERSION_MAJOR(apiVersion),
                VK_API_VERSION_MINOR(apiVersion), VK_API_VERSION_PATCH(apiVersion));

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VRX prerequisite probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::printf("Vulkan instance: unavailable (%d)\n", result);
        return;
    }
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS) {
        std::printf("Vulkan devices: enumeration failed (%d)\n", result);
    } else {
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result != VK_SUCCESS) {
            std::printf("Vulkan devices: enumeration failed (%d)\n", result);
        } else {
            std::printf("Vulkan devices: %u\n", count);
            for (uint32_t i = 0; i < count; ++i) {
                VkPhysicalDeviceProperties properties{};
                vkGetPhysicalDeviceProperties(devices[i], &properties);
                std::printf("  %u: %s (vendor 0x%04x, device 0x%04x, API %u.%u)\n",
                            i, properties.deviceName, properties.vendorID, properties.deviceID,
                            VK_API_VERSION_MAJOR(properties.apiVersion),
                            VK_API_VERSION_MINOR(properties.apiVersion));
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
}

template <typename T>
bool Resolve(PFN_xrGetInstanceProcAddr getProc, XrInstance instance, const char* name, T& out)
{
    PFN_xrVoidFunction fn = nullptr;
    XrResult result = getProc(instance, name, &fn);
    if (XR_FAILED(result) || !fn) {
        std::printf("OpenXR function %s: unavailable (%d)\n", name, result);
        return false;
    }
    out = reinterpret_cast<T>(fn);
    return true;
}

void OpenXrProbe(const char* loaderPath)
{
    void* loader = dlopen(loaderPath, RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        std::printf("OpenXR loader: unavailable (%s)\n", dlerror());
        return;
    }
    auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        dlsym(loader, "xrGetInstanceProcAddr"));
    if (!getProc) {
        std::puts("OpenXR loader: missing xrGetInstanceProcAddr");
        dlclose(loader);
        return;
    }
    std::puts("OpenXR loader: found");
    PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
    if (!Resolve(getProc, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", enumerate)) {
        dlclose(loader);
        return;
    }
    uint32_t count = 0;
    XrResult result = enumerate(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        std::printf("OpenXR runtime/extensions: unavailable (%d)\n", result);
        dlclose(loader);
        return;
    }
    std::vector<XrExtensionProperties> extensions(count);
    for (auto& ext : extensions) ext.type = XR_TYPE_EXTENSION_PROPERTIES;
    result = enumerate(nullptr, count, &count, extensions.data());
    if (XR_FAILED(result)) {
        std::printf("OpenXR extensions: enumeration failed (%d)\n", result);
        dlclose(loader);
        return;
    }
    bool vulkan2 = false;
    for (const auto& ext : extensions)
        if (std::strcmp(ext.extensionName, "XR_KHR_vulkan_enable2") == 0) vulkan2 = true;
    std::printf("OpenXR XR_KHR_vulkan_enable2: %s\n", vulkan2 ? "available" : "unavailable");

    PFN_xrCreateInstance create = nullptr;
    if (!Resolve(getProc, XR_NULL_HANDLE, "xrCreateInstance", create)) {
        dlclose(loader);
        return;
    }
    XrInstanceCreateInfo info{};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::strncpy(info.applicationInfo.applicationName, "VRX prerequisite probe",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(info.applicationInfo.engineName, "VRX",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* names[] = {"XR_KHR_vulkan_enable2"};
    if (vulkan2) {
        info.enabledExtensionCount = 1;
        info.enabledExtensionNames = names;
    }
    XrInstance instance = XR_NULL_HANDLE;
    result = create(&info, &instance);
    if (XR_FAILED(result)) {
        std::printf("OpenXR instance: unavailable (%d)\n", result);
        dlclose(loader);
        return;
    }
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrDestroyInstance destroy = nullptr;
    if (Resolve(getProc, instance, "xrDestroyInstance", destroy) &&
        Resolve(getProc, instance, "xrGetSystem", getSystem)) {
        XrSystemGetInfo systemInfo{};
        systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
        systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId system = XR_NULL_SYSTEM_ID;
        result = getSystem(instance, &systemInfo, &system);
        if (XR_SUCCEEDED(result)) std::puts("OpenXR headset system: available");
        else std::printf("OpenXR headset system: unavailable (%d)\n", result);
    }
    if (destroy) destroy(instance);
    dlclose(loader);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::puts("vrx-probe [--openxr-loader=PATH]: report Vulkan, OpenXR, PipeWire and CUDA driver prerequisites");
        return 0;
    }
    const char* loaderPath = "libopenxr_loader.so.1";
    if (argc == 2 && std::strncmp(argv[1], "--openxr-loader=", 16) == 0 && argv[1][16])
        loaderPath = argv[1] + 16;
    else if (argc != 1) {
        std::fprintf(stderr, "usage: vrx-probe [--help] [--openxr-loader=PATH]\n");
        return 2;
    }
    VulkanProbe();
    OpenXrProbe(loaderPath);
    LibraryProbe("PipeWire library", "libpipewire-0.3.so.0");
    LibraryProbe("CUDA driver library", "libcuda.so.1");
    return 0; // A missing prerequisite is a diagnostic, not a probe failure.
}
