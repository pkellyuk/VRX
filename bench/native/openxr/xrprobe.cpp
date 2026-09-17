// M2 step 1: OpenXR bootstrap diagnostic.
//
// Before building a render loop, establish what the runtime will actually give
// us: which graphics-binding extensions exist (D3D11 vs D3D12 matters because
// our DirectML depth module is D3D12-only), whether a headset is present, and
// what the runtime calls itself.
//
// Deliberately loads openxr_loader.dll dynamically and resolves everything
// through xrGetInstanceProcAddr, so no import library is needed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// d3d11.h must come before openxr_platform.h: the graphics-binding section
// needs ID3D11Device / IUnknown / D3D_FEATURE_LEVEL to already be declared.
#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char* ResultStr(XrResult r)
{
    switch (r)
    {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    default: return "(other)";
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ---- 1. load the runtime's loader ------------------------------------
    const wchar_t* candidates[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll",
        L"openxr_loader.dll",
    };

    HMODULE loader = nullptr;
    const wchar_t* used = nullptr;
    for (auto c : candidates)
    {
        loader = LoadLibraryW(c);
        if (loader) { used = c; break; }
    }
    if (!loader)
    {
        printf("FAIL: could not load openxr_loader.dll (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    printf("loader : %ls\n", used);

    auto getProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    if (!getProcAddr) { printf("FAIL: no xrGetInstanceProcAddr export\n"); return 1; }

    // ---- 2. resolve the entry points we need ------------------------------
    PFN_xrEnumerateInstanceExtensionProperties pEnumerateExt = nullptr;
    PFN_xrCreateInstance pCreateInstance = nullptr;
    PFN_xrGetInstanceProperties pInstanceProps = nullptr;
    PFN_xrGetSystem pGetSystem = nullptr;
    PFN_xrGetSystemProperties pSystemProps = nullptr;
    PFN_xrEnumerateViewConfigurations pEnumViewConfigs = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes pEnumBlend = nullptr;
    PFN_xrDestroyInstance pDestroyInstance = nullptr;

    // Only the global entry points may be queried with XR_NULL_HANDLE. Anything
    // that takes an XrInstance must be resolved once an instance exists,
    // otherwise the loader hands back nullptr and calling it crashes.
    getProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&pEnumerateExt);
    getProcAddr(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&pCreateInstance);

    if (!pEnumerateExt || !pCreateInstance) { printf("FAIL: core entry points missing\n"); return 1; }

    // ---- 3. what extensions does the runtime offer? -----------------------
    uint32_t extCount = 0;
    if (XR_FAILED(pEnumerateExt(nullptr, 0, &extCount, nullptr))) { printf("FAIL: enumerate extensions\n"); return 1; }
    std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    pEnumerateExt(nullptr, extCount, &extCount, exts.data());

    printf("extensions: %u\n", extCount);

    bool hasD3D11 = false, hasD3D12 = false;
    const char* interesting[] = {
        "XR_KHR_D3D11_enable", "XR_KHR_D3D12_enable", "XR_KHR_composition_layer_depth",
        "XR_MSFT_composition_layer_reprojection", "XR_KHR_win32_convert_performance_counter_time",
        "XR_EXT_win32_appcontainer_compatible", "XR_KHR_vulkan_enable",
    };
    for (auto& e : exts)
    {
        if (strcmp(e.extensionName, "XR_KHR_D3D11_enable") == 0) hasD3D11 = true;
        if (strcmp(e.extensionName, "XR_KHR_D3D12_enable") == 0) hasD3D12 = true;
        for (auto want : interesting)
            if (strcmp(e.extensionName, want) == 0)
                printf("  [x] %s (v%u)\n", e.extensionName, e.extensionVersion);
    }
    printf("D3D11 binding: %s   D3D12 binding: %s\n",
           hasD3D11 ? "yes" : "NO", hasD3D12 ? "yes" : "NO");

    // ---- 4. create an instance --------------------------------------------
    std::vector<const char*> enabled;
    if (hasD3D11) enabled.push_back("XR_KHR_D3D11_enable");
    if (hasD3D12) enabled.push_back("XR_KHR_D3D12_enable");

    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strcpy_s(ci.applicationInfo.applicationName, "VRX probe");
    strcpy_s(ci.applicationInfo.engineName, "none");
    ci.applicationInfo.applicationVersion = 1;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = pCreateInstance(&ci, &instance);
    printf("xrCreateInstance: %s\n", ResultStr(r));
    if (XR_FAILED(r)) return 1;

    pCreateInstance = nullptr; // no longer needed

    // Now that an instance exists, the instance-level entry points resolve.
    getProcAddr(instance, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&pInstanceProps);
    getProcAddr(instance, "xrGetSystem", (PFN_xrVoidFunction*)&pGetSystem);
    getProcAddr(instance, "xrGetSystemProperties", (PFN_xrVoidFunction*)&pSystemProps);
    getProcAddr(instance, "xrEnumerateViewConfigurations", (PFN_xrVoidFunction*)&pEnumViewConfigs);
    getProcAddr(instance, "xrEnumerateEnvironmentBlendModes", (PFN_xrVoidFunction*)&pEnumBlend);
    getProcAddr(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&pDestroyInstance);

    if (!pGetSystem) { printf("FAIL: xrGetSystem did not resolve\n"); return 1; }

    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (pInstanceProps && XR_SUCCEEDED(pInstanceProps(instance, &ip)))
        printf("runtime: %s %u.%u.%u\n", ip.runtimeName,
               XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
               XR_VERSION_PATCH(ip.runtimeVersion));

    // ---- 5. is a headset present? ----------------------------------------
    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    r = pGetSystem(instance, &sgi, &systemId);
    printf("xrGetSystem(HMD): %s\n", ResultStr(r));

    if (XR_FAILED(r))
    {
        printf("\n=> No headset available to this runtime. Instance creation and\n");
        printf("   extension negotiation work, but xrCreateSession/xrGetSystem\n");
        printf("   cannot proceed until an HMD is connected and SteamVR is running.\n");
        if (pDestroyInstance) pDestroyInstance(instance);
        return 2;
    }

    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(pSystemProps(instance, systemId, &sp)))
    {
        printf("system: \"%s\"  vendor=0x%04x  maxSwapchain=%ux%u  layers=%u\n",
               sp.systemName, sp.vendorId,
               sp.graphicsProperties.maxSwapchainImageWidth,
               sp.graphicsProperties.maxSwapchainImageHeight,
               sp.graphicsProperties.maxLayerCount);
    }

    uint32_t vcCount = 0;
    if (XR_SUCCEEDED(pEnumViewConfigs(instance, systemId, 0, &vcCount, nullptr)) && vcCount)
    {
        std::vector<XrViewConfigurationType> vcs(vcCount);
        pEnumViewConfigs(instance, systemId, vcCount, &vcCount, vcs.data());
        for (uint32_t i = 0; i < vcCount; i++)
        {
            const char* n = "?";
            if (vcs[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) n = "PRIMARY_STEREO";
            else if (vcs[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO) n = "PRIMARY_MONO";
            printf("view config: %s\n", n);

            uint32_t blendCount = 0;
            if (XR_SUCCEEDED(pEnumBlend(instance, systemId, vcs[i], 0, &blendCount, nullptr)) && blendCount)
            {
                std::vector<XrEnvironmentBlendMode> blends(blendCount);
                pEnumBlend(instance, systemId, vcs[i], blendCount, &blendCount, blends.data());
                printf("  blend modes:");
                for (uint32_t b = 0; b < blendCount; b++)
                    printf(" %s", blends[b] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE ? "OPAQUE" :
                                   blends[b] == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE ? "ADDITIVE" : "ALPHA_BLEND");
                printf("\n");
            }
        }
    }

    printf("\n=> Headset present. Ready to create a session and a 2D layer.\n");

    if (pDestroyInstance) pDestroyInstance(instance);
    return 0;
}
