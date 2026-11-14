// Compile the complete optional Windows/OpenXR/GPU adapter without a headset or
// model files. This catches SDK/type errors independently of the full renderer.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "controller_menu.h"
int main() { ControllerMenu menu; menu.Configure(L""); }
