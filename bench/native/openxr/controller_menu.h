#pragma once
#include "controller_menu_protocol.h"
#include "controller_menu_math.h"
#include "controller_menu_motion.h"
#include <string>
#include <vector>
#include <cstring>

// Optional OpenXR actions plus a compositor quad. The desktop owns the settings;
// this class transports panel bitmaps/pointer events and the opt-in screen mouse.
// It does not emulate a gamepad or request SteamVR dashboard focus.
class ControllerMenu
{
    struct Surface
    {
        XrPosef pose{{0,0,0,1},{0,0,0}};
        XrExtent2Df size{.85f,.68f};
        vrxmenu::PanelFlight flight;
        uint32_t id = 0, frame = 0;
        bool visible = false;
        float distance = 0;
        float hoverTime = 0;
        XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    } surfaces[vrxmenu::MaxSnapshots+1];
    int activeSurface = 0;
    bool resizing = false, wallDrag = false;
    vrxmenu::PanelResize panelResize;
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    std::wstring name;
    HANDLE mapping = nullptr, mutex = nullptr, fenceEvent = nullptr;
    vrxmenu::Shared* shared = nullptr;
    ULONGLONG nextOpen = 0;
    bool enabled = false, visible = false, attempted = false, ready = false;
    bool pendingOpen = false, clickArmed = false, pressed = false, draggingPanel = false;
    vrxmenu::PanelDrag panelDrag;
    vrxmenu::PanelDrag secondDrag;
    vrxmenu::TwoHandDrag twoHandDrag;
    bool twoHanded = false, previousDown[2]{};
    vrxmenu::MouseLatch mouse;
    vrxmenu::MouseLatch rightMouse;
    vrxmenu::MouseLatch panelRight;
    int panelRightTarget = 0;
    float panelRightU = 0, panelRightV = 0;
    HWND keyboardWindow = nullptr;
    bool pointersVisible = true, controlScreen = false, screenPointerHit = false;
    Room sceneRoom;
    XrPosef screenPose{{0,0,0,1},{0,0,0}};
    XrExtent2Df screenSize{};
    XrFovf viewFov{-.7f,.7f,.65f,-.65f};
    uint32_t lastArrange = 0;
    Cylinder screenCylinder;
    RECT mouseRect{};
    HWND mouseWindow = nullptr;
    vrxmenu::PressLatch toggleLatch[2];
    bool wasFocused = false;
    bool needsDeactivate = false;
    XrTime lastTime = 0;
    int activeHand = -1;
    uint32_t frame = 0;
    uint64_t fenceValue = 0;
    float u = -1, v = -1;
    bool pointerHit = false, beamVisible[2]{};
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, head = XR_NULL_HANDLE, hands[2]{};
    XrPath handPaths[2]{};
    XrActionSet toggleSet = XR_NULL_HANDLE, uiSet = XR_NULL_HANDLE;
    XrAction toggle = XR_NULL_HANDLE, aim = XR_NULL_HANDLE, select = XR_NULL_HANDLE, scroll = XR_NULL_HANDLE;
    XrAction secondary = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D12KHR> images;
    XrCompositionLayerQuad layer{XR_TYPE_COMPOSITION_LAYER_QUAD}, back{XR_TYPE_COMPOSITION_LAYER_QUAD}, cursor{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad beams[2]{{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
    XrCompositionLayerQuad screenCursor{XR_TYPE_COMPOSITION_LAYER_QUAD};
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> commands;
    Ptr<ID3D12Resource> upload;
    Ptr<ID3D12Fence> fence;
    ID3D12CommandQueue* queue = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    bool rgba = false, uploaded = false, textureDirty = false;
    std::vector<uint8_t> pixels;
    std::string error;

#define MENU_FN(n) PFN_##n n##_ = nullptr
    MENU_FN(xrCreateActionSet); MENU_FN(xrDestroyActionSet); MENU_FN(xrCreateAction);
    MENU_FN(xrStringToPath); MENU_FN(xrSuggestInteractionProfileBindings); MENU_FN(xrAttachSessionActionSets);
    MENU_FN(xrCreateActionSpace); MENU_FN(xrCreateReferenceSpace); MENU_FN(xrDestroySpace); MENU_FN(xrLocateSpace);
    MENU_FN(xrSyncActions); MENU_FN(xrGetActionStateBoolean); MENU_FN(xrGetActionStateFloat);
    MENU_FN(xrGetActionStateVector2f); MENU_FN(xrGetActionStatePose);
    MENU_FN(xrEnumerateSwapchainFormats); MENU_FN(xrCreateSwapchain); MENU_FN(xrDestroySwapchain);
    MENU_FN(xrEnumerateSwapchainImages); MENU_FN(xrAcquireSwapchainImage); MENU_FN(xrWaitSwapchainImage); MENU_FN(xrReleaseSwapchainImage);
#undef MENU_FN

    bool Lock()
    {
        if (!mutex) return false;
        DWORD result = WaitForSingleObject(mutex,0);
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    bool XrOk(XrResult result, const char* operation)
    {
        if (XR_SUCCEEDED(result)) return true;
        error = std::string(operation) + " failed (" + std::to_string(int(result)) + "); controller menu disabled, playback continues";
        return false;
    }
    XrPath Path(const char* text)
    {
        XrPath result = XR_NULL_PATH;
        xrStringToPath_(instance,text,&result);
        return result;
    }
    bool Action(XrActionSet set, XrAction& action, const char* actionName, const char* label, XrActionType type)
    {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy_s(ci.actionName,actionName); strcpy_s(ci.localizedActionName,label);
        ci.actionType = type; ci.countSubactionPaths = 2; ci.subactionPaths = handPaths;
        return XrOk(xrCreateAction_(set,&ci,&action),"xrCreateAction");
    }
    void Bindings(const char* profile, const char* leftToggle, const char* rightToggle)
    {
        std::vector<XrActionSuggestedBinding> bindings;
        for (int h = 0; h < 2; ++h)
        {
            const std::string base = h ? "/user/hand/right/input/" : "/user/hand/left/input/";
            const char* button = h ? rightToggle : leftToggle;
            if (button) bindings.push_back({toggle,Path((base+button).c_str())});
            bindings.push_back({aim,Path((base+"aim/pose").c_str())});
            bindings.push_back({select,Path((base+"trigger/value").c_str())});
            bindings.push_back({scroll,Path((base+"thumbstick").c_str())});
            if (h == 1) bindings.push_back({secondary,Path((base+"squeeze/value").c_str())});
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = Path(profile); info.countSuggestedBindings = uint32_t(bindings.size()); info.suggestedBindings = bindings.data();
        // An unsupported optional profile is normal. Other profiles can still bind.
        xrSuggestInteractionProfileBindings_(instance,&info);
    }
    bool Init(PFN_xrGetInstanceProcAddr getProc, ID3D12Device* device, ID3D12CommandQueue* gfxQueue, bool frameProfile, bool genericProfile)
    {
#define LOAD_MENU(n) if (XR_FAILED(getProc(instance,#n,reinterpret_cast<PFN_xrVoidFunction*>(&n##_))) || !n##_) { error = "Missing OpenXR function " #n; return false; }
        LOAD_MENU(xrCreateActionSet); LOAD_MENU(xrDestroyActionSet); LOAD_MENU(xrCreateAction);
        LOAD_MENU(xrStringToPath); LOAD_MENU(xrSuggestInteractionProfileBindings); LOAD_MENU(xrAttachSessionActionSets);
        LOAD_MENU(xrCreateActionSpace); LOAD_MENU(xrCreateReferenceSpace); LOAD_MENU(xrDestroySpace); LOAD_MENU(xrLocateSpace);
        LOAD_MENU(xrSyncActions); LOAD_MENU(xrGetActionStateBoolean); LOAD_MENU(xrGetActionStateFloat);
        LOAD_MENU(xrGetActionStateVector2f); LOAD_MENU(xrGetActionStatePose);
        LOAD_MENU(xrEnumerateSwapchainFormats); LOAD_MENU(xrCreateSwapchain); LOAD_MENU(xrDestroySwapchain);
        LOAD_MENU(xrEnumerateSwapchainImages); LOAD_MENU(xrAcquireSwapchainImage); LOAD_MENU(xrWaitSwapchainImage); LOAD_MENU(xrReleaseSwapchainImage);
#undef LOAD_MENU
        handPaths[0] = Path("/user/hand/left"); handPaths[1] = Path("/user/hand/right");
        XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(si.actionSetName,"vrx_menu_toggle"); strcpy_s(si.localizedActionSetName,"VRX settings menu");
        if (!XrOk(xrCreateActionSet_(instance,&si,&toggleSet),"xrCreateActionSet")) return false;
        strcpy_s(si.actionSetName,"vrx_menu_ui"); strcpy_s(si.localizedActionSetName,"VRX settings pointer");
        if (!XrOk(xrCreateActionSet_(instance,&si,&uiSet),"xrCreateActionSet")) return false;
        if (!Action(toggleSet,toggle,"menu","Show or hide settings (Menu)",XR_ACTION_TYPE_BOOLEAN_INPUT) ||
            !Action(toggleSet,secondary,"screen_right_click","Right-click screen (right grip)",XR_ACTION_TYPE_FLOAT_INPUT) ||
            !Action(uiSet,aim,"aim","Point at settings",XR_ACTION_TYPE_POSE_INPUT) ||
            !Action(uiSet,select,"select","Select or drag setting",XR_ACTION_TYPE_FLOAT_INPUT) ||
            !Action(uiSet,scroll,"scroll","Scroll settings",XR_ACTION_TYPE_VECTOR2F_INPUT)) return false;
        Bindings("/interaction_profiles/oculus/touch_controller","menu/click",nullptr);
        // Index and the generic profile expose no application Menu button.
        // Keep those usable via a stick click; the runtime's system button is reserved.
        Bindings("/interaction_profiles/valve/index_controller","thumbstick/click","thumbstick/click");
        if (frameProfile) Bindings("/interaction_profiles/valve/frame_controller_valve","view/click","menu/click");
        if (genericProfile) Bindings("/interaction_profiles/khr/generic_controller","thumbstick/click","thumbstick/click");
        XrActionSet sets[]{toggleSet,uiSet};
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 2; attach.actionSets = sets;
        if (!XrOk(xrAttachSessionActionSets_(session,&attach),"xrAttachSessionActionSets")) return false;
        for (int h = 0; h < 2; ++h)
        {
            XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            ci.action = aim; ci.subactionPath = handPaths[h]; ci.poseInActionSpace.orientation.w = 1;
            if (!XrOk(xrCreateActionSpace_(session,&ci,&hands[h]),"xrCreateActionSpace")) return false;
        }
        XrReferenceSpaceCreateInfo hi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        hi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; hi.poseInReferenceSpace.orientation.w = 1;
        if (!XrOk(xrCreateReferenceSpace_(session,&hi,&head),"xrCreateReferenceSpace")) return false;

        uint32_t count = 0;
        if (!XrOk(xrEnumerateSwapchainFormats_(session,0,&count,nullptr),"xrEnumerateSwapchainFormats")) return false;
        std::vector<int64_t> formats(count);
        if (!XrOk(xrEnumerateSwapchainFormats_(session,count,&count,formats.data()),"xrEnumerateSwapchainFormats")) return false;
        int64_t format = 0;
        for (auto candidate : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM})
            if (std::find(formats.begin(),formats.end(),int64_t(candidate)) != formats.end()) { format = candidate; break; }
        if (!format) { error = "No RGBA/BGRA controller menu swapchain format"; return false; }
        rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format; ci.sampleCount = 1; ci.width = vrxmenu::AtlasWidth; ci.height = vrxmenu::AtlasHeight;
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        if (!XrOk(xrCreateSwapchain_(session,&ci,&swapchain),"xrCreateSwapchain")) return false;
        if (!XrOk(xrEnumerateSwapchainImages_(swapchain,0,&count,nullptr),"xrEnumerateSwapchainImages")) return false;
        images.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        if (!count || !XrOk(xrEnumerateSwapchainImages_(swapchain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"xrEnumerateSwapchainImages")) return false;

        queue = gfxQueue;
        auto desc = images[0].texture->GetDesc();
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = bytes;
        buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands))) ||
            FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))) { error = "Controller menu GPU allocation failed"; return false; }
        commands->Close();
        fenceEvent = CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (!fenceEvent) { error = "Controller menu fence event failed"; return false; }
        pixels.resize(vrxmenu::AtlasWidth * vrxmenu::AtlasHeight * 4);
        return true;
    }

    void Event(vrxmenu::EventKind kind, float x = -1, float y = -1, float value = 0)
    { if (shared) vrxmenu::Push(*shared,{vrxmenu::EventKind(int(kind)+activeSurface*16),x,y,value}); }
    void ResetToggle() { for (auto& latch : toggleLatch) latch.Reset(); }
    void ReleaseMouse()
    {
        if (mouse.Step(false,false) < 0)
        { INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1,&input,sizeof(input)); }
        if (rightMouse.Step(false,false) < 0)
        { INPUT input{}; input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_RIGHTUP; SendInput(1,&input,sizeof(input)); }
        screenPointerHit = false;
    }
    void ServiceKeyboard(bool active)
    {
        auto& state = shared->keyboard;
        HWND target = mouseWindow ? mouseWindow : keyboardWindow;
        state.targetWindow = reinterpret_cast<uint64_t>(target);
        state.flags = active && target && IsWindow(target) && !IsIconic(target) ? 1u : 0u;
        if (uint32_t(state.write-state.read) > 256) { state.read = state.write; return; }
        if (state.read == state.write) return;
        if (!state.flags) { state.read = state.write; return; }
        SetForegroundWindow(target);
        if (GetAncestor(GetForegroundWindow(),GA_ROOT) != GetAncestor(target,GA_ROOT))
        { state.flags = 0; state.read = state.write; error = "Keyboard target could not receive Windows focus"; return; }
        std::vector<INPUT> input;
        while (state.read != state.write)
        {
            const auto key = state.events[state.read++ % 256];
            INPUT down{}; down.type = INPUT_KEYBOARD;
            if (key.type == 0) { down.ki.wScan = WORD(key.code); down.ki.dwFlags = KEYEVENTF_UNICODE; }
            else if (key.type == 1) down.ki.wVk = WORD(key.code);
            else continue;
            input.push_back(down); down.ki.dwFlags |= KEYEVENTF_KEYUP; input.push_back(down);
        }
        if (!input.empty() && SendInput(UINT(input.size()),input.data(),sizeof(INPUT)) != input.size())
            error = "Windows blocked floating keyboard input (the target may be running elevated)";
    }
    void ScreenMouse(bool allowed, bool down, bool rightDown, float x, float y)
    {
        allowed = allowed && mouseRect.right > mouseRect.left && mouseRect.bottom > mouseRect.top;
        POINT point{mouseRect.left + LONG(std::clamp(x,0.0f,1.0f)*(mouseRect.right-mouseRect.left-1)),
                    mouseRect.top + LONG(std::clamp(y,0.0f,1.0f)*(mouseRect.bottom-mouseRect.top-1))};
        if (allowed && mouseWindow)
        {
            if ((down && mouse.armed && !mouse.held) || (rightDown && rightMouse.armed && !rightMouse.held)) SetForegroundWindow(mouseWindow);
            allowed = IsWindow(mouseWindow) && !IsIconic(mouseWindow) &&
                GetAncestor(WindowFromPoint(point),GA_ROOT) == GetAncestor(mouseWindow,GA_ROOT);
        }
        const int edge = mouse.Step(allowed,down);
        const int rightEdge = rightMouse.Step(allowed,rightDown);
        INPUT inputs[3]{}; UINT count = 0;
        if (allowed)
        {
            auto& input = inputs[count++]; input.type = INPUT_MOUSE;
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            input.mi.dx = LONG(double(point.x-left)*65535/std::max(1,GetSystemMetrics(SM_CXVIRTUALSCREEN)-1));
            input.mi.dy = LONG(double(point.y-top)*65535/std::max(1,GetSystemMetrics(SM_CYVIRTUALSCREEN)-1));
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        }
        if (edge)
        { auto& input = inputs[count++]; input.type = INPUT_MOUSE; input.mi.dwFlags = edge > 0 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; }
        if (rightEdge)
        { auto& input = inputs[count++]; input.type = INPUT_MOUSE; input.mi.dwFlags = rightEdge > 0 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; }
        if (allowed && (edge > 0 || rightEdge > 0))
        {
            keyboardWindow = mouseWindow ? mouseWindow : GetAncestor(WindowFromPoint(point),GA_ROOT);
            if (shared) shared->keyboard.focusSerial++;
        }
        if (count && SendInput(count,inputs,sizeof(INPUT)) != count)
        { error = "Windows blocked controller mouse input (the target may be running elevated)"; ReleaseMouse(); }
    }
    void Close(bool keepWall = false)
    {
        if (visible || pressed) Event(vrxmenu::Cancel);
        visible = keepWall && surfaces[0].flight.stuck; pressed = false; draggingPanel = resizing = false; activeHand = -1; pointerHit = false; clickArmed = false;
        if (!keepWall) surfaces[0].flight.Reset();
        activeSurface = 0;
        surfaces[0].visible = visible;
        twoHanded = false; previousDown[0] = previousDown[1] = true;
        wallDrag = false;
        ReleaseMouse();
        if (panelRight.Step(false,false) < 0 && shared)
            vrxmenu::Push(*shared,{vrxmenu::EventKind(int(vrxmenu::RightUp)+panelRightTarget*16),panelRightU,panelRightV,0});
        beamVisible[0] = beamVisible[1] = false;
        if (shared) shared->header.visible = visible ? 1 : 0;
    }
    bool Locate(XrSpace space, XrTime time, XrPosef& result, XrSpaceLocationFlags* flags = nullptr)
    {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace_(space,local,time,&location)) || (location.locationFlags & valid) != valid) return false;
        result = location.pose; if (flags) *flags = location.locationFlags; return true;
    }
    bool Upload()
    {
        // Keep the previous texture if its upload is still in flight; never stall the headset.
        if (fence->GetCompletedValue() < fenceValue) return uploaded;
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!XrOk(xrAcquireSwapchainImage_(swapchain,&ai,&index),"Menu acquire")) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        if (!XrOk(xrWaitSwapchainImage_(swapchain,&wi),"Menu wait")) return false;
        void* data = nullptr;
        D3D12_RANGE empty{0,0};
        bool copied = SUCCEEDED(upload->Map(0,&empty,&data));
        int rows = vrxmenu::Height;
        for (int s = 1; s <= vrxmenu::MaxSnapshots; ++s) if (surfaces[s].visible) rows = (s/vrxmenu::AtlasColumns+1)*vrxmenu::Height;
        if (copied)
        {
            for (int y = 0; y < rows; ++y)
            {
                auto* dest = static_cast<uint8_t*>(data) + footprint.Offset + y * footprint.Footprint.RowPitch;
                const auto* src = pixels.data() + y * vrxmenu::AtlasWidth * 4;
                std::memcpy(dest,src,vrxmenu::AtlasWidth*4);
                if (rgba) for (int x = 0; x < vrxmenu::AtlasWidth; ++x) std::swap(dest[x*4],dest[x*4+2]);
            }
            upload->Unmap(0,nullptr);
            copied = SUCCEEDED(allocator->Reset()) && SUCCEEDED(commands->Reset(allocator.Get(),nullptr));
            if (copied)
            {
                D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = images[index].texture; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                // OpenXR hands color swapchain images to D3D12 in RENDER_TARGET
                // state and requires that same state when the image is released.
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                commands->ResourceBarrier(1,&barrier);
                D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = images[index].texture; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_BOX region{0,0,0,vrxmenu::AtlasWidth,UINT(rows),1};
                commands->CopyTextureRegion(&to,0,0,0,&from,&region);
                std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter); commands->ResourceBarrier(1,&barrier);
                copied = SUCCEEDED(commands->Close());
                if (copied)
                {
                    ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
                    copied = SUCCEEDED(queue->Signal(fence.Get(),++fenceValue));
                }
            }
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const bool released = XrOk(xrReleaseSwapchainImage_(swapchain,&ri),"Menu release");
        if (!copied) error = "Controller menu GPU upload failed";
        uploaded = copied && released;
        return uploaded;
    }

public:
    void CopyPanelPixels(int surface, const uint8_t* source)
    {
        const int column = surface%vrxmenu::AtlasColumns, row = surface/vrxmenu::AtlasColumns;
        for (int y = 0; y < vrxmenu::Height; ++y)
            memcpy(pixels.data()+(size_t(row*vrxmenu::Height+y)*vrxmenu::AtlasWidth+column*vrxmenu::Width)*4,
                source+size_t(y)*vrxmenu::Width*4,size_t(vrxmenu::Width)*4);
    }
    void SetScene(const Room& room, XrPosef screen, XrExtent2Df extent, const Cylinder& cylinder, RECT target, HWND window, XrFovf fov = {-.7f,.7f,.65f,-.65f})
    {
        for (auto& surface : surfaces)
            if (surface.flight.stuck && (!room.valid || room.env == kRoomEnvOpenAir)) surface.flight.Reset();
        sceneRoom = room; screenPose = screen; screenSize = extent; screenCylinder = cylinder; mouseRect = target; mouseWindow = window;
        viewFov = fov;
        for (auto& surface : surfaces) if (surface.visible)
            vrxmenu::ContainPanel(surface.pose,surface.size,sceneRoom,screenPose);
    }
    void Configure(std::wstring mappingName) { name = std::move(mappingName); }
    const std::string& Error() const { return error; }
    bool TakeSnapshotRequest()
    {
        if (!shared || !Lock()) return false;
        const bool requested = enabled && shared->header.snapshotRequest != shared->header.snapshotAck;
        if (requested) shared->header.snapshotAck++;
        ReleaseMutex(mutex); return requested;
    }
    bool CompleteSnapshot(const std::wstring& path)
    {
        if (!shared) { if (!path.empty()) DeleteFileW(path.c_str()); return true; }
        if (!Lock()) return false;
        bool accepted = false;
        if (!path.empty() && path.size() < vrxmenu::SnapshotPathChars)
            for (auto& snapshot : shared->snapshots) if (!snapshot.active && !snapshot.ready)
            {
                snapshot.id++; if (!snapshot.id) snapshot.id++;
                snapshot.frame = 0;
                snapshot.kind = vrxmenu::SnapshotPanel;
                memcpy(snapshot.path,path.c_str(),(path.size()+1)*sizeof(char16_t));
                snapshot.ready = 1; accepted = true; break;
            }
        if (!accepted) { shared->header.snapshotError++; if (!path.empty()) DeleteFileW(path.c_str()); }
        ReleaseMutex(mutex); return true;
    }

    void Update(PFN_xrGetInstanceProcAddr getProc, XrInstance xrInstance, XrSession xrSession, XrSpace space,
        ID3D12Device* device, ID3D12CommandQueue* gfxQueue, bool focused, bool shouldRender, XrTime time,
        bool frameProfile, bool genericProfile)
    {
        pointerHit = false;
        screenPointerHit = false;
        if (!focused || !shouldRender) ReleaseMouse();
        beamVisible[0] = beamVisible[1] = false;
        const float dt = lastTime ? std::clamp(float(time-lastTime)*1e-9f,0.0f,.05f) : 1.0f/90;
        lastTime = time;
        if (name.empty()) return;
        if (!shared)
        {
            const auto now = GetTickCount64(); if (now < nextOpen) return; nextOpen = now + 500;
            mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
            if (!mapping) return; // Off by default: no mapping, no actions, no GPU resources.
            shared = static_cast<vrxmenu::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(vrxmenu::Shared)));
            mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE,FALSE,(name+L".Lock").c_str());
            if (!shared || !mutex) { Shutdown(); return; }
        }
        if (!Lock()) return;
        if (shared->header.magic != vrxmenu::Magic || shared->header.version != vrxmenu::Version)
        { ReleaseMutex(mutex); return; }
        const bool live = GetTickCount64()-shared->header.heartbeat < 2000;
        const bool wanted = shared->header.enabled && live;
        controlScreen = wanted && shared->header.controlScreen;
        ServiceKeyboard(wanted && focused && shouldRender && controlScreen);
        const bool becameEnabled = wanted && !enabled;
        if (!wanted)
        {
            Close(live);
            if (enabled && ready) needsDeactivate = true;
            if (needsDeactivate && ready && focused)
            {
                XrActionsSyncInfo clear{XR_TYPE_ACTIONS_SYNC_INFO};
                if (xrSyncActions_(session,&clear) == XR_SUCCESS) needsDeactivate = false;
            }
            enabled = false; pendingOpen = false; ResetToggle();
            ReleaseMutex(mutex); return;
        }
        enabled = true;
        if (becameEnabled) { pendingOpen = !surfaces[0].flight.stuck; pointersVisible = true; ResetToggle(); }
        if (!attempted)
        {
            instance = xrInstance; session = xrSession; local = space;
            attempted = true;
            // Drop the IPC lock during allocation and runtime calls.
            ReleaseMutex(mutex);
            ready = Init(getProc,device,gfxQueue,frameProfile,genericProfile);
            if (!Lock()) return;
        }
        if (!ready) { Close(); ReleaseMutex(mutex); return; }
        if (!focused || !shouldRender)
        {
            Close(true); ResetToggle(); wasFocused = false;
            ReleaseMutex(mutex); return;
        }
        if (!wasFocused) { ResetToggle(); clickArmed = false; }
        wasFocused = true;
        bool anySnapshot = false;
        for (int s = 1; s <= vrxmenu::MaxSnapshots; ++s) anySnapshot |= shared->snapshots[s-1].active != 0;
        const bool uiWasActive = (visible || controlScreen || anySnapshot) && pointersVisible;
        XrActiveActionSet sets[]{{toggleSet,XR_NULL_PATH},{uiSet,XR_NULL_PATH}};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets = uiWasActive ? 2 : 1; sync.activeActionSets = sets;
        XrResult result = xrSyncActions_(session,&sync);
        if (result != XR_SUCCESS) { Close(true); ResetToggle(); ReleaseMutex(mutex); return; }
        bool togglePressed = false;
        XrActionStateGetInfo secondaryInfo{XR_TYPE_ACTION_STATE_GET_INFO}; secondaryInfo.action = secondary; secondaryInfo.subactionPath = handPaths[1];
        XrActionStateFloat secondaryState{XR_TYPE_ACTION_STATE_FLOAT};
        const bool rightClickDown = XR_SUCCEEDED(xrGetActionStateFloat_(session,&secondaryInfo,&secondaryState)) && secondaryState.isActive && secondaryState.currentState > .55f;
        for (int h = 0; h < 2; ++h)
        {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = toggle; gi.subactionPath = handPaths[h];
            XrActionStateBoolean value{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean_(session,&gi,&value)) && value.isActive)
                togglePressed |= toggleLatch[h].Press(value.currentState != 0);
            else toggleLatch[h].Reset();
        }
        if (visible && togglePressed)
        {
            pendingOpen = false;
            if (!surfaces[0].flight.stuck) { Close(); ReleaseMutex(mutex); return; }
            Close(); pendingOpen = true; pointersVisible = true;
        }
        if (!visible && (pendingOpen || togglePressed))
        {
            XrPosef eye;
            if (Locate(head,time,eye))
            {
                surfaces[0].pose = vrxmenu::Place(eye);
                pointersVisible = true; surfaces[0].flight.Reset();
                visible = true; shared->header.visible = 1; pendingOpen = false; clickArmed = false;
                Event(vrxmenu::Cancel);
            }
        }
        surfaces[0].visible = visible;
        for (int s = 1; s <= vrxmenu::MaxSnapshots; ++s)
        {
            auto& surface = surfaces[s]; const auto& snapshot = shared->snapshots[s-1];
            surface.visible = snapshot.active && snapshot.ready == 2;
            if (surface.visible && surface.id != snapshot.id)
            {
                surface.id = snapshot.id; surface.frame = 0; surface.flight.Reset(); surface.size = {.85f,.68f};
                XrPosef eye;
                if (Locate(head,time,eye))
                {
                    surface.pose = vrxmenu::Place(eye);
                    surface.pose.position = vrxmenu::Add(surface.pose.position,vrxmenu::Rotate(eye.orientation,{.18f*s,.05f,-.05f*s}));
                }
            }
        }
        if (shared->header.arrange != lastArrange)
        {
            XrPosef eye;
            if (Locate(head,time,eye))
            {
                lastArrange = shared->header.arrange;
                XrPosef poses[7]{}; XrExtent2Df sizes[7]{}; int indices[7]{}, count = 0;
                for (int s = 0; s <= vrxmenu::MaxSnapshots; ++s) if (surfaces[s].visible)
                { indices[count] = s; poses[count] = surfaces[s].pose; sizes[count++] = surfaces[s].size; }
                vrxmenu::ArrangePanels(eye,viewFov,poses,sizes,count,sceneRoom,screenPose,uint32_t(GetTickCount64()) ^ lastArrange*2654435761u);
                for (int i = 0; i < count; ++i)
                {
                    auto& surface = surfaces[indices[i]];
                    surface.pose = poses[i]; surface.size = sizes[i]; surface.flight.Reset(); surface.hoverTime = 0;
                    activeSurface = indices[i]; Event(vrxmenu::Cancel);
                }
                activeSurface = 0; pressed = draggingPanel = resizing = twoHanded = wallDrag = false;
                activeHand = -1; clickArmed = false; ReleaseMouse();
            }
        }
        if (!visible && !controlScreen && !anySnapshot) { ReleaseMouse(); ReleaseMutex(mutex); return; }
        for (auto& surface : surfaces) if (surface.visible)
        {
            if (!(pressed && activeSurface == int(&surface-surfaces)))
                surface.flight.Step(surface.pose,surface.size,sceneRoom,screenPose,dt);
            vrxmenu::ContainPanel(surface.pose,surface.size,sceneRoom,screenPose);
        }

        bool hit[2]{}, down[2]{}, tracked[2]{}, selectActive = false; float us[2]{}, vs[2]{}, sticks[2]{};
        XrPosef rays[2]{};
        XrSpaceLocationFlags trackingFlags[2]{};
        for (int h = 0; h < 2; ++h)
        {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.subactionPath = handPaths[h]; gi.action = select;
            XrActionStateFloat value{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat_(session,&gi,&value)))
            { down[h] = value.isActive && value.currentState > .55f; selectActive |= value.isActive != 0; }
            gi.action = scroll; XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f_(session,&gi,&stick)) && stick.isActive) sticks[h] = stick.currentState.y;
            gi.action = aim; XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
            tracked[h] = pointersVisible && XR_SUCCEEDED(xrGetActionStatePose_(session,&gi,&state)) && state.isActive && Locate(hands[h],time,rays[h],&trackingFlags[h]);
        }
        int targets[2]{-1,-1}; float targetU[2]{}, targetV[2]{}, targetDistance[2]{1e9f,1e9f};
        auto findTargets = [&]()
        {
        targets[0] = targets[1] = -1; targetDistance[0] = targetDistance[1] = 1e9f;
        for (int h = 0; h < 2; ++h) if (tracked[h])
            for (int s = 0; s <= vrxmenu::MaxSnapshots; ++s) if (surfaces[s].visible)
            {
                float hu = 0, hv = 0;
                const auto& surface = surfaces[s];
                if (!vrxmenu::Hit(surface.pose,surface.size,rays[h],hu,hv,true,true)) continue;
                float physicalU = hu;
                if (vrxmenu::Rotate(vrxmenu::Inverse(surface.pose.orientation),vrxmenu::Sub(rays[h].position,surface.pose.position)).z < 0) physicalU = 1-hu;
                const auto point = vrxmenu::Add(surface.pose.position,vrxmenu::Rotate(surface.pose.orientation,{(physicalU-.5f)*surface.size.width,(.5f-hv)*surface.size.height,0}));
                const auto delta = vrxmenu::Sub(point,rays[h].position);
                const float distance = std::sqrt(vrxmenu::Dot(delta,delta));
                if (distance < targetDistance[h]) { targets[h] = s; targetU[h] = hu; targetV[h] = hv; targetDistance[h] = distance; }
            }
        };
        findTargets();
        XrPosef viewer;
        if (Locate(head,time,viewer))
        {
            for (int s = 0; s <= vrxmenu::MaxSnapshots; ++s) if (surfaces[s].visible)
            {
                auto& surface = surfaces[s];
                surface.hoverTime = std::max(0.0f,surface.hoverTime-dt);
                if (targets[0] == s || targets[1] == s) surface.hoverTime = .35f;
                const bool clicking = (targets[0] == s && down[0]) || (targets[1] == s && down[1]);
                if (surface.hoverTime > 0 && !(pressed && activeSurface == s) && !clicking)
                {
                    vrxmenu::FaceViewer(surface.pose,viewer.position,dt);
                    surface.flight.angularVelocity = {0,0,0};
                    vrxmenu::ContainPanel(surface.pose,surface.size,sceneRoom,screenPose);
                }
            }
            findTargets(); // pick against the rotated panel, so buttons still line up
        }
        if (pressed && !surfaces[activeSurface].visible)
        { Event(vrxmenu::Cancel); pressed = draggingPanel = resizing = twoHanded = false; activeHand = -1; clickArmed = false; }
        if (!pressed)
            activeSurface = targets[1] >= 0 && down[1] ? targets[1] : targets[0] >= 0 && down[0] ? targets[0] :
                targets[1] >= 0 ? targets[1] : targets[0] >= 0 ? targets[0] : 0;
        auto& pose = surfaces[activeSurface].pose;
        auto& size = surfaces[activeSurface].size;
        auto& flight = surfaces[activeSurface].flight;
        if (resizing && activeHand >= 0 && tracked[activeHand] && down[activeHand])
        { panelResize.Move(pose,size,rays[activeHand]); flight.FitToWall(pose,size,sceneRoom,screenPose); }
        if (draggingPanel && activeHand >= 0)
        {
            const int other = 1-activeHand;
            if (twoHanded && !(tracked[activeHand] && down[activeHand] && tracked[other] && down[other]))
            {
                const int remaining = tracked[activeHand] && down[activeHand] ? activeHand : (tracked[other] && down[other] ? other : -1);
                if (remaining >= 0)
                {
                    const auto& heldDrag = remaining == activeHand ? panelDrag : secondDrag;
                    float gu = .5f + heldDrag.grab.x/twoHandDrag.initialSize.width;
                    const float gv = .5f - heldDrag.grab.y/twoHandDrag.initialSize.height;
                    if (vrxmenu::Rotate(vrxmenu::Inverse(pose.orientation),vrxmenu::Sub(rays[remaining].position,pose.position)).z < 0) gu = 1-gu;
                    panelDrag.Begin(pose,size,rays[remaining],gu,gv);
                    activeHand = remaining;
                }
                twoHanded = false;
            }
            if (tracked[activeHand] && down[activeHand])
            {
                if (twoHanded)
                {
                    (activeHand == 1 ? panelDrag : secondDrag).PushPull(sticks[1],dt);
                    twoHandDrag.Move(pose,size,panelDrag,secondDrag,rays[activeHand],rays[1-activeHand],dt);
                }
                else
                {
                    if (activeHand == 1) panelDrag.PushPull(sticks[1],dt);
                    panelDrag.Move(pose,rays[activeHand],wallDrag && flight.stuck ? 0 : dt);
                }
            }
        }
        const auto bounds = vrxmenu::ContainPanel(pose,size,sceneRoom,screenPose);
        if (draggingPanel && activeHand >= 0 && tracked[activeHand] && down[activeHand])
        {
            if (!twoHanded && bounds.wallDistance <= (wallDrag ? .14f : .025f))
                flight.Dock(pose,size,sceneRoom,screenPose,bounds.wall);
            else flight.stuck = false;
        }
        for (int h = 0; h < 2; ++h)
            if (tracked[h])
            {
                // A swinging sheet can turn edge-on or away from the pointer.
                // Keep its pinched pixel captured until release or tracking loss.
                if (resizing && activeHand == h)
                { hit[h] = true; us[h] = u; vs[h] = v; }
                else if (draggingPanel && activeHand == h)
                { hit[h] = true; us[h] = panelDrag.grabU; vs[h] = panelDrag.grabV; }
                else if (draggingPanel && twoHanded)
                { hit[h] = true; us[h] = secondDrag.grabU; vs[h] = secondDrag.grabV; }
                else if (targets[h] == activeSurface) { hit[h] = true; us[h] = targetU[h]; vs[h] = targetV[h]; }
            }
        if (draggingPanel && !twoHanded && activeHand >= 0 && down[activeHand] && tracked[activeHand])
        {
            const int other = 1-activeHand;
            if (tracked[other] && down[other] && !previousDown[other] && hit[other] && vrxmenu::TitleHit(us[other],vs[other]))
            {
                secondDrag.Begin(pose,size,rays[other],us[other],vs[other]);
                const auto span = vrxmenu::Sub(secondDrag.grab,panelDrag.grab);
                if (vrxmenu::Dot(span,span) >= .03f*.03f)
                { twoHandDrag.Begin(pose,size,panelDrag,secondDrag,rays[activeHand],rays[other]); twoHanded = true; }
            }
        }
        if (uiWasActive && selectActive && !down[0] && !down[1]) clickArmed = true;
        const int hand = pressed ? activeHand : (hit[1] && down[1] ? 1 : (hit[0] && down[0] ? 0 : (hit[1] ? 1 : (hit[0] ? 0 : -1))));
        if (pressed && (hand < 0 || !tracked[hand] || !hit[hand])) { Event(vrxmenu::Cancel); pressed = false; draggingPanel = resizing = false; activeHand = -1; clickArmed = false; }
        else if (hand >= 0)
        {
            u = us[hand]; v = vs[hand]; pointerHit = u>=0 && u<=1 && v>=0 && v<=1;
            if (!draggingPanel && !resizing) Event(vrxmenu::Move,u,v);
            if (down[hand] && !pressed && clickArmed)
            {
                pressed = true; activeHand = hand; clickArmed = false;
                resizing = vrxmenu::EdgeHit(u,v);
                draggingPanel = !resizing && vrxmenu::TitleHit(u,v);
                if (resizing) { flight.flying = false; panelResize.Begin(pose,size,rays[hand],u,v); Event(vrxmenu::Cancel); }
                else if (draggingPanel)
                {
                    wallDrag = flight.stuck;
                    const int wallFace = flight.dockFace;
                    flight.Reset();
                    if (wallDrag) { flight.stuck = true; flight.dockFace = wallFace; }
                    panelDrag.Begin(pose,size,rays[hand],u,v); Event(vrxmenu::Cancel);
                    const int other = 1-hand;
                    if (tracked[other] && down[other] && !previousDown[other] && hit[other] && vrxmenu::TitleHit(us[other],vs[other]))
                    {
                        secondDrag.Begin(pose,size,rays[other],us[other],vs[other]);
                        const auto span = vrxmenu::Sub(secondDrag.grab,panelDrag.grab);
                        if (vrxmenu::Dot(span,span) >= .03f*.03f)
                        { twoHandDrag.Begin(pose,size,panelDrag,secondDrag,rays[hand],rays[other]); twoHanded = true; }
                    }
                }
                else Event(vrxmenu::Down,u,v);
            }
            if (!down[hand] && pressed)
            {
                if (draggingPanel)
                {
                    const auto released = vrxmenu::ContainPanel(pose,size,sceneRoom,screenPose);
                    if (released.wallDistance <= (wallDrag ? .14f : .04f)) flight.Dock(pose,size,sceneRoom,screenPose,released.wall);
                    else flight.Release(pose,panelDrag);
                }
                Event(draggingPanel || resizing ? vrxmenu::Cancel : vrxmenu::Up,u,v);
                pressed = false; draggingPanel = resizing = wallDrag = false; activeHand = -1;
            }
            if (!pressed && std::fabs(sticks[hand]) > .2f) Event(vrxmenu::Scroll,u,v,sticks[hand]*1600*dt);
        }
        float su = 0, sv = 0;
        XrPosef screenPoint{};
        constexpr auto mouseTracking = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        const bool screenHit = controlScreen && pointersVisible && tracked[1] && (trackingFlags[1] & mouseTracking) == mouseTracking && targets[1] < 0 && !pressed &&
            vrxmenu::ScreenHit(screenPose,screenSize,screenCylinder,rays[1],su,sv,screenPoint);
        ScreenMouse(screenHit,down[1],rightClickDown,su,sv);
        const bool contextAllowed = tracked[1] && !pressed && targets[1] > 0 &&
            shared->snapshots[targets[1]-1].kind == vrxmenu::BrowserPanel && (!panelRight.held || targets[1] == panelRightTarget);
        const int contextEdge = panelRight.Step(contextAllowed,rightClickDown);
        if (contextEdge > 0) panelRightTarget = targets[1];
        if (contextAllowed) { panelRightU = targetU[1]; panelRightV = targetV[1]; }
        if (contextEdge)
            vrxmenu::Push(*shared,{vrxmenu::EventKind(int(contextEdge > 0 ? vrxmenu::RightDown : vrxmenu::RightUp)+panelRightTarget*16),panelRightU,panelRightV,0});
        screenPointerHit = screenHit;
        screenCursor.space = local; screenCursor.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        screenCursor.pose = screenPoint; screenCursor.size = {.012f,.012f};
        screenCursor.subImage = {swapchain,{{0,0},{8,8}},0};
        XrPosef eye;
        if (Locate(head,time,eye)) for (int h = 0; h < 2; ++h) if (tracked[h])
        {
            if (!pointersVisible || (!visible && !anySnapshot && !(h == 1 && controlScreen))) continue;
            float length = 2;
            if (draggingPanel && activeHand == h) length = panelDrag.distance;
            else if (draggingPanel && twoHanded) length = secondDrag.distance;
            else if (h == 1 && screenHit)
            { const auto delta = vrxmenu::Sub(screenPoint.position,rays[h].position); length = std::sqrt(vrxmenu::Dot(delta,delta)); }
            else if (targets[h] >= 0) length = targetDistance[h];
            beams[h].space = local; beams[h].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            beams[h].pose = vrxmenu::Beam(rays[h],length,eye.position); beams[h].size = {.0025f,length};
            beams[h].subImage = {swapchain,{{0,0},{8,8}},0}; beamVisible[h] = true;
        }
        if (!pressed && (down[0] || down[1])) clickArmed = false;
        shared->header.pointerX = pointerHit && activeSurface == 0 ? u : -1; shared->header.pointerY = pointerHit && activeSurface == 0 ? v : -1;
        const uint32_t nextFrame = shared->header.frame;
        bool newFrame = nextFrame != 0 && nextFrame != frame;
        if (newFrame) CopyPanelPixels(0,shared->pixels);
        for (int s = 1; s <= vrxmenu::MaxSnapshots; ++s)
        {
            const auto& snapshot = shared->snapshots[s-1];
            if (surfaces[s].visible && snapshot.frame != surfaces[s].frame)
            {
                CopyPanelPixels(s,snapshot.pixels);
                surfaces[s].frame = snapshot.frame; newFrame = true;
            }
        }
        textureDirty |= newFrame;
        ReleaseMutex(mutex);
        if (textureDirty && fence->GetCompletedValue() >= fenceValue && Upload()) { frame = nextFrame; textureDirty = false; }
        layer.space = local; layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH; layer.pose = pose; layer.size = size;
        layer.subImage = {swapchain,{{0,0},{vrxmenu::Width,vrxmenu::Height}},0};
        back = layer;
        back.pose.orientation = vrxmenu::Multiply(pose.orientation,{0,1,0,0});
        cursor = layer;
        const bool fromBack = hand >= 0 && vrxmenu::Rotate(vrxmenu::Inverse(pose.orientation),vrxmenu::Sub(rays[hand].position,pose.position)).z < 0;
        const float cursorU = fromBack ? 1-u : u;
        cursor.pose = fromBack ? back.pose : pose;
        cursor.pose.position = vrxmenu::Add(pose.position,vrxmenu::Rotate(pose.orientation,{(cursorU-.5f)*size.width,(.5f-v)*size.height,fromBack ? -.003f : .003f}));
        if (draggingPanel) pointerHit = false;
        previousDown[0] = down[0]; previousDown[1] = down[1];
        cursor.size = {.012f,.012f}; cursor.subImage.imageRect = {{0,0},{8,8}};
        XrPosef eyePose;
        if (Locate(head,time,eyePose)) for (int s = 0; s <= vrxmenu::MaxSnapshots; ++s)
        {
            auto& surface = surfaces[s]; auto& quad = surface.layer;
            quad.space = local; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.pose = surface.pose; quad.size = surface.size;
            const auto delta = vrxmenu::Sub(surface.pose.position,eyePose.position);
            surface.distance = vrxmenu::Dot(delta,delta);
            if (vrxmenu::Rotate(vrxmenu::Inverse(surface.pose.orientation),vrxmenu::Sub(eyePose.position,surface.pose.position)).z < 0)
                quad.pose.orientation = vrxmenu::Multiply(surface.pose.orientation,{0,1,0,0});
            quad.subImage = {swapchain,{{(s%vrxmenu::AtlasColumns)*vrxmenu::Width,(s/vrxmenu::AtlasColumns)*vrxmenu::Height},{vrxmenu::Width,vrxmenu::Height}},0};
        }
    }

    void Append(const XrCompositionLayerBaseHeader** layers, uint32_t& count) const
    {
        if (!uploaded) return;
        std::vector<int> order;
        for (int s = 0; s <= vrxmenu::MaxSnapshots; ++s)
            if (surfaces[s].visible && (s != 0 || enabled || surfaces[0].flight.stuck))
                order.push_back(s);
        std::sort(order.begin(),order.end(),[&](int a, int b) { return surfaces[a].distance > surfaces[b].distance; });
        for (int s : order) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&surfaces[s].layer);
        for (int h = 0; h < 2; ++h) if (beamVisible[h]) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&beams[h]);
        if (pointerHit) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cursor);
        if (screenPointerHit) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&screenCursor);
    }

    void Recenter() { if (Lock()) { if (visible) { Close(); pendingOpen = true; } ReleaseMutex(mutex); } }
    bool IsVisible() const { return enabled && visible; }

    void Shutdown()
    {
        ReleaseMouse();
        if (fence && fence->GetCompletedValue() < fenceValue && fenceEvent &&
            SUCCEEDED(fence->SetEventOnCompletion(fenceValue,fenceEvent))) WaitForSingleObject(fenceEvent,2000);
        if (Lock())
        {
            Close();
            for (auto& snapshot : shared->snapshots)
                if (snapshot.ready == 1) DeleteFileW(reinterpret_cast<const wchar_t*>(snapshot.path));
            ReleaseMutex(mutex);
        }
        if (head && xrDestroySpace_) xrDestroySpace_(head);
        for (auto hand : hands) if (hand && xrDestroySpace_) xrDestroySpace_(hand);
        if (swapchain && xrDestroySwapchain_) xrDestroySwapchain_(swapchain);
        if (toggleSet && xrDestroyActionSet_) xrDestroyActionSet_(toggleSet);
        if (uiSet && xrDestroyActionSet_) xrDestroyActionSet_(uiSet);
        head = XR_NULL_HANDLE; hands[0] = hands[1] = XR_NULL_HANDLE;
        swapchain = XR_NULL_HANDLE; toggleSet = uiSet = XR_NULL_HANDLE;
        upload.Reset(); commands.Reset(); allocator.Reset(); fence.Reset();
        if (fenceEvent) CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        if (shared) UnmapViewOfFile(shared);
        shared = nullptr;
        if (mapping) CloseHandle(mapping);
        mapping = nullptr;
        if (mutex) CloseHandle(mutex);
        mutex = nullptr;
    }
    ~ControllerMenu() { Shutdown(); }
};
