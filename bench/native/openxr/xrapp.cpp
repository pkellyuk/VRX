// M2: OpenXR + D3D12 session presenting a 2D quad layer.
//
// Presents a time-varying pattern on a floating 2D quad in the headset. This is
// the "2D display content" stand-in: once the frame loop and layer are proven,
// the quad's texture becomes the captured display frame, and M3 attaches a depth
// image to the layer via XR_KHR_composition_layer_depth.
//
// D3D12 (not D3D11) because the runtime offers XR_KHR_D3D12_enable and our
// DirectML depth module is D3D12-only - one device serves both, no bridge.
//
// usage: xrapp [seconds]      (default 30)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- loader glue

static PFN_xrGetInstanceProcAddr g_getProc = nullptr;

#define XR_FN(type, name) static type name = nullptr
XR_FN(PFN_xrGetInstanceProcAddr, xrGetInstanceProcAddr_);
XR_FN(PFN_xrEnumerateInstanceExtensionProperties, xrEnumerateInstanceExtensionProperties_);
XR_FN(PFN_xrCreateInstance, xrCreateInstance_);
XR_FN(PFN_xrDestroyInstance, xrDestroyInstance_);
XR_FN(PFN_xrGetSystem, xrGetSystem_);
XR_FN(PFN_xrGetSystemProperties, xrGetSystemProperties_);
XR_FN(PFN_xrCreateSession, xrCreateSession_);
XR_FN(PFN_xrDestroySession, xrDestroySession_);
XR_FN(PFN_xrBeginSession, xrBeginSession_);
XR_FN(PFN_xrEndSession, xrEndSession_);
XR_FN(PFN_xrPollEvent, xrPollEvent_);
XR_FN(PFN_xrCreateReferenceSpace, xrCreateReferenceSpace_);
XR_FN(PFN_xrEnumerateSwapchainFormats, xrEnumerateSwapchainFormats_);
XR_FN(PFN_xrCreateSwapchain, xrCreateSwapchain_);
XR_FN(PFN_xrDestroySwapchain, xrDestroySwapchain_);
XR_FN(PFN_xrEnumerateSwapchainImages, xrEnumerateSwapchainImages_);
XR_FN(PFN_xrAcquireSwapchainImage, xrAcquireSwapchainImage_);
XR_FN(PFN_xrWaitSwapchainImage, xrWaitSwapchainImage_);
XR_FN(PFN_xrReleaseSwapchainImage, xrReleaseSwapchainImage_);
XR_FN(PFN_xrWaitFrame, xrWaitFrame_);
XR_FN(PFN_xrBeginFrame, xrBeginFrame_);
XR_FN(PFN_xrEndFrame, xrEndFrame_);
XR_FN(PFN_xrGetD3D12GraphicsRequirementsKHR, xrGetD3D12GraphicsRequirementsKHR_);

static bool ResolveInstanceFns(XrInstance instance)
{
    struct Entry { const char* name; PFN_xrVoidFunction* fn; };
    Entry entries[] = {
        { "xrDestroyInstance", (PFN_xrVoidFunction*)&xrDestroyInstance_ },
        { "xrGetSystem", (PFN_xrVoidFunction*)&xrGetSystem_ },
        { "xrGetSystemProperties", (PFN_xrVoidFunction*)&xrGetSystemProperties_ },
        { "xrCreateSession", (PFN_xrVoidFunction*)&xrCreateSession_ },
        { "xrDestroySession", (PFN_xrVoidFunction*)&xrDestroySession_ },
        { "xrBeginSession", (PFN_xrVoidFunction*)&xrBeginSession_ },
        { "xrEndSession", (PFN_xrVoidFunction*)&xrEndSession_ },
        { "xrPollEvent", (PFN_xrVoidFunction*)&xrPollEvent_ },
        { "xrCreateReferenceSpace", (PFN_xrVoidFunction*)&xrCreateReferenceSpace_ },
        { "xrEnumerateSwapchainFormats", (PFN_xrVoidFunction*)&xrEnumerateSwapchainFormats_ },
        { "xrCreateSwapchain", (PFN_xrVoidFunction*)&xrCreateSwapchain_ },
        { "xrDestroySwapchain", (PFN_xrVoidFunction*)&xrDestroySwapchain_ },
        { "xrEnumerateSwapchainImages", (PFN_xrVoidFunction*)&xrEnumerateSwapchainImages_ },
        { "xrAcquireSwapchainImage", (PFN_xrVoidFunction*)&xrAcquireSwapchainImage_ },
        { "xrWaitSwapchainImage", (PFN_xrVoidFunction*)&xrWaitSwapchainImage_ },
        { "xrReleaseSwapchainImage", (PFN_xrVoidFunction*)&xrReleaseSwapchainImage_ },
        { "xrWaitFrame", (PFN_xrVoidFunction*)&xrWaitFrame_ },
        { "xrBeginFrame", (PFN_xrVoidFunction*)&xrBeginFrame_ },
        { "xrEndFrame", (PFN_xrVoidFunction*)&xrEndFrame_ },
        { "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&xrGetD3D12GraphicsRequirementsKHR_ },
    };

    for (auto& e : entries)
    {
        if (XR_FAILED(g_getProc(instance, e.name, e.fn)) || *e.fn == nullptr)
        {
            printf("FAIL: could not resolve %s\n", e.name);
            return false;
        }
    }
    return true;
}

static const char* ResultStr(XrResult r)
{
    switch (r)
    {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
    case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
    case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
    case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
    case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
    case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
    case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    default: return "(other)";
    }
}

// --------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    double runSeconds = argc > 1 ? atof(argv[1]) : 30.0;

    // ---- loader -----------------------------------------------------------
    HMODULE loader = LoadLibraryW(
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openxr_loader.dll");
    if (!loader) loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader) { printf("FAIL: openxr_loader.dll\n"); return 1; }

    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    xrEnumerateInstanceExtensionProperties_ =
        (PFN_xrEnumerateInstanceExtensionProperties)GetProcAddress(loader, "xrEnumerateInstanceExtensionProperties");
    xrCreateInstance_ = (PFN_xrCreateInstance)GetProcAddress(loader, "xrCreateInstance");
    if (!g_getProc || !xrEnumerateInstanceExtensionProperties_ || !xrCreateInstance_)
    { printf("FAIL: loader exports\n"); return 1; }

    // ---- instance ---------------------------------------------------------
    const char* wantExt[] = { "XR_KHR_D3D12_enable" };
    XrInstanceCreateInfo ci{ XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strcpy_s(ci.applicationInfo.applicationName, "VRX xrapp");
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = wantExt;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance_(&ci, &instance);
    if (XR_FAILED(r)) { printf("xrCreateInstance: %s\n", ResultStr(r)); return 1; }
    if (!ResolveInstanceFns(instance)) return 1;

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    r = xrGetSystem_(instance, &sgi, &systemId);
    if (XR_FAILED(r)) { printf("xrGetSystem: %s\n", ResultStr(r)); return 1; }

    XrSystemProperties sysProps{ XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties_(instance, systemId, &sysProps);
    printf("system : %s\n", sysProps.systemName);

    // ---- D3D12 device on the adapter the runtime wants ---------------------
    XrGraphicsRequirementsD3D12KHR reqs{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
    r = xrGetD3D12GraphicsRequirementsKHR_(instance, systemId, &reqs);
    if (XR_FAILED(r)) { printf("xrGetD3D12GraphicsRequirementsKHR: %s\n", ResultStr(r)); return 1; }
    printf("adapter LUID %08lx:%08lx  minFeatureLevel %d\n",
           (unsigned long)(reqs.adapterLuid.HighPart), (unsigned long)reqs.adapterLuid.LowPart,
           (int)reqs.minFeatureLevel);

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { printf("FAIL: CreateDXGIFactory1\n"); return 1; }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(reqs.adapterLuid, IID_PPV_ARGS(&adapter))))
    { printf("FAIL: EnumAdapterByLuid\n"); return 1; }
    { DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d); wprintf(L"adapter: %s\n", d.Description); }

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), reqs.minFeatureLevel, IID_PPV_ARGS(&device))))
    { printf("FAIL: D3D12CreateDevice\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { printf("FAIL: CreateCommandQueue\n"); return 1; }

    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    cmdList->Close();

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fenceVal = 0;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto gpuWait = [&]() {
        queue->Signal(fence.Get(), ++fenceVal);
        fence->SetEventOnCompletion(fenceVal, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    };

    // ---- session ----------------------------------------------------------
    XrGraphicsBindingD3D12KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D12_KHR };
    binding.device = device.Get();
    binding.queue = queue.Get();

    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = systemId;

    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession_(instance, &sci, &session);
    printf("xrCreateSession: %s\n", ResultStr(r));
    if (XR_FAILED(r)) return 1;

    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = { {0,0,0,1}, {0,0,0} };
    XrSpace space = XR_NULL_HANDLE;
    xrCreateReferenceSpace_(session, &rsci, &space);

    // ---- swapchain --------------------------------------------------------
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats_(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats_(session, fmtCount, &fmtCount, formats.data());

    int64_t chosen = 0;
    for (auto f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { chosen = f; break; }
    if (!chosen) for (auto f : formats)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM) { chosen = f; break; }
    if (!chosen) { printf("FAIL: no usable swapchain format (%u offered)\n", fmtCount); return 1; }
    printf("swapchain format: %lld (of %u offered)\n", (long long)chosen, fmtCount);

    const uint32_t SC_W = 1280, SC_H = 720;
    XrSwapchainCreateInfo swci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swci.format = chosen;
    swci.sampleCount = 1;
    swci.width = SC_W;
    swci.height = SC_H;
    swci.faceCount = 1;
    swci.arraySize = 1;             // one texture, shared by both eyes on a quad
    swci.mipCount = 1;

    XrSwapchain swapchain = XR_NULL_HANDLE;
    r = xrCreateSwapchain_(session, &swci, &swapchain);
    printf("xrCreateSwapchain: %s\n", ResultStr(r));
    if (XR_FAILED(r)) return 1;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages_(swapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D12KHR> images(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
    xrEnumerateSwapchainImages_(swapchain, imgCount, &imgCount, (XrSwapchainImageBaseHeader*)images.data());
    printf("swapchain: %ux%u, %u images\n", SC_W, SC_H, imgCount);

    // One RTV per swapchain image, created once (not per frame).
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs(imgCount);
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = imgCount;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap))))
        { printf("FAIL: CreateDescriptorHeap\n"); return 1; }

        UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < imgCount; i++)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = (DXGI_FORMAT)chosen;
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(images[i].texture, &rd, h);
            rtvs[i] = h;
            h.ptr += stride;
        }
    }

    // ---- frame loop -------------------------------------------------------
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, exitLoop = false;

    auto start = std::chrono::steady_clock::now();
    uint64_t frames = 0, drawn = 0, waitedTimeouts = 0;
    double sumFrameMs = 0;

    printf("\nentering frame loop (%.0fs)...\n", runSeconds);

    while (!exitLoop)
    {
        // events
        XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent_(instance, &ev) == XR_SUCCESS)
        {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            {
                auto* sc = (XrEventDataSessionStateChanged*)&ev;
                if (sc->state != state)
                {
                    static const char* names[] = { "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED",
                                                   "VISIBLE", "FOCUSED", "STOPPING", "LOST", "EXITING" };
                    const char* n = ((int)sc->state >= 0 && (int)sc->state <= 8) ? names[(int)sc->state] : "?";
                    printf("  state %d -> %d (%s)\n", (int)state, (int)sc->state, n);
                    if (sc->state == XR_SESSION_STATE_VISIBLE || sc->state == XR_SESSION_STATE_FOCUSED)
                        printf("    ^ compositor should now request rendering\n");
                }
                state = sc->state;
                if (state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult br = xrBeginSession_(session, &bi);
                    printf("session READY -> xrBeginSession: %s\n", ResultStr(br));
                    running = XR_SUCCEEDED(br);
                }
                else if (state == XR_SESSION_STATE_STOPPING)
                {
                    printf("session STOPPING -> xrEndSession\n");
                    xrEndSession_(session);
                    running = false;
                }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    printf("session state %d -> exit\n", (int)state);
                    exitLoop = true;
                }
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }

        if (exitLoop) break;

        XrFrameState fs{ XR_TYPE_FRAME_STATE };
        XrResult wr = xrWaitFrame_(session, nullptr, &fs);
        if (XR_FAILED(wr)) { printf("xrWaitFrame: %s\n", ResultStr(wr)); break; }
        if (wr == XR_TIMEOUT_EXPIRED) { waitedTimeouts++; continue; }
        xrBeginFrame_(session, nullptr);

        frames++;
        auto f0 = std::chrono::steady_clock::now();

        const XrCompositionLayerBaseHeader* layers[1] = { nullptr };
        XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };

        if (running && fs.shouldRender)
        {
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (XR_SUCCEEDED(xrAcquireSwapchainImage_(swapchain, &ai, &idx)))
            {
                XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                if (XR_SUCCEEDED(xrWaitSwapchainImage_(swapchain, &wi)))
                {
                    // paint a time-varying gradient so the layer is visibly live
                    float t = (float)(frames % 240) / 240.0f;
                    const float col[4] = { 0.15f + 0.5f * t, 0.25f + 0.4f * (1.0f - t), 0.85f - 0.5f * t, 1.0f };

                    cmdAlloc->Reset();
                    cmdList->Reset(cmdAlloc.Get(), nullptr);

                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = images[idx].texture;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    cmdList->ResourceBarrier(1, &b);

                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvs[idx];
                    cmdList->ClearRenderTargetView(rtv, col, 0, nullptr);

                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                    cmdList->ResourceBarrier(1, &b);

                    cmdList->Close();
                    ID3D12CommandList* lists[] = { cmdList.Get() };
                    queue->ExecuteCommandLists(1, lists);
                    gpuWait();

                    XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                    xrReleaseSwapchainImage_(swapchain, &ri);

                    drawn++;

                    // 2D quad carrying that image, 1.6m wide, 1.5m ahead
                    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    quad.space = space;
                    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    quad.subImage.swapchain = swapchain;
                    quad.subImage.imageRect = { {0, 0}, { (int32_t)SC_W, (int32_t)SC_H } };
                    quad.subImage.imageArrayIndex = 0;
                    quad.pose = { {0, 0, 0, 1}, {0, 0, -1.5f} };
                    quad.size = { 1.6f, 0.9f };
                    layers[0] = (XrCompositionLayerBaseHeader*)&quad;
                }
            }
        }

        XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = (running && fs.shouldRender && layers[0]) ? 1 : 0;
        fei.layers = layers;
        XrResult er = xrEndFrame_(session, &fei);
        if (XR_FAILED(er)) { printf("xrEndFrame: %s\n", ResultStr(er)); }

        auto f1 = std::chrono::steady_clock::now();
        sumFrameMs += std::chrono::duration<double, std::milli>(f1 - f0).count();

        if (frames % 120 == 0)
        {
            double el = std::chrono::duration<double>(f1 - start).count();
            printf("frame %6llu  drawn %6llu  %.1f fps  state %d\n",
                   (unsigned long long)frames, (unsigned long long)drawn, frames / (el > 0 ? el : 1), (int)state);
        }

        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > runSeconds)
            exitLoop = true;
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    printf("\n--- summary ---\n");
    printf("frames        : %llu\n", (unsigned long long)frames);
    printf("frames drawn  : %llu\n", (unsigned long long)drawn);
    printf("waitFrame timeouts: %llu\n", (unsigned long long)waitedTimeouts);
    printf("elapsed       : %.1f s  (%.1f fps)\n", elapsed, frames / (elapsed > 0 ? elapsed : 1));
    printf("cpu/frame     : %.2f ms\n", frames ? sumFrameMs / frames : 0.0);
    printf("final state   : %d\n", (int)state);

    xrDestroySwapchain_(swapchain);
    xrDestroySession_(session);
    xrDestroyInstance_(instance);
    return 0;
}
