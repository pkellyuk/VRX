# OpenXR and depth — API surface investigation

Investigated: 2026-07-14 (project: VRX)
Sources (local copies in `refs/`):
- OpenXR Specification 1.1 (2026-09) — `openxr-spec.html`
- OpenXR Specification 1.0 (2026-07-13) — `openxr-spec-1.0.html`
- OpenXR-SDK-Header (2026-09-01) — `openxr.h` (trimmed, depth-relevant parts)

## TL;DR

OpenXR is a **transport for depth, not a generator of it**. There is no
"AI depth estimation" API in OpenXR proper — but there are four well-defined
places where depth enters/leaves the pipeline, and one vendor extension that
is literally "the compositor runs depth estimation" (Varjo). An AI depth
model slots in *between* "get a frame" and "hand depth to the compositor".

## 1. Core: depth/stencil swapchains (your own Z-buffer)

No extension needed. A regular `xrCreateSwapchain` with:

- `usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT` (`0x2`, core flag)
- `format` = a depth format (e.g. `XR_FORMAT_D32_SFLOAT`, `XR_FORMAT_D24_UNORM_S8_UINT`)

gives you a swapchain you render depth into — i.e. the exact Z-buffer of
whatever you rendered. This is how an app exposes *its own scene depth* to
the runtime (used e.g. by reprojection and by the compositor for occlusion
of 2D layers against your 3D content).

Full core swapchain usage flags (for reference):

```
XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT            = 0x1
XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT    = 0x2
XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT            = 0x4
XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT                = 0x8
XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT                = 0x10
XR_SWAPCHAIN_USAGE_SAMPLED_BIT                     = 0x20
XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT              = 0x40
XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_MND/_KHR  = 0x80   (XR_MND_swapchain_usage_input_attachment_bit)
```

`XrSwapchainCreateInfo` carries `usageFlags`, `format`, `width/height`,
`faceCount`, `arraySize`, `sampleCount` — same shape as the color swapchains
a VR app already creates, so a depth swapchain is just one more swapchain of
a different format/usage.

**Use case:** you render the scene yourself → depth is *known exactly*, no AI
needed. AI is only needed when the content is 2D (no Z) or is the real world.

## 2. `XR_KHR_composition_layer_depth` — attaching depth to 2D content

This is the key hook for "AI depth of the display". When you submit **2D
content** (video frame, screen mirror, texture quad) as a composition layer,
you can attach a per-pixel depth image so the compositor can occlude it and
reproject it correctly:

```c
// XrCompositionLayerDepthInfoKHR extends XrCompositionLayerProjectionView
typedef struct XrCompositionLayerDepthInfoKHR {
    XrStructureType type;
    const void*     next;
    XrSwapchainSubImage subImage;  // swapchain image holding the depth
    float minDepth;
    float maxDepth;
    float nearZ;
    float farZ;
} XrCompositionLayerDepthInfoKHR;
```

- Extension name: `XR_KHR_composition_layer_depth` (spec version 6 in current
  SDK header), structure type `XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR`.
- The depth lives in its own swapchain (created with a depth format +
  `XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`), referenced by
  `subImage`.
- It is chained via `next` into the per-view
  `XrCompositionLayerProjectionView` of the layer's
  `XrCompositionLayerProjection` (one depth image per eye).
- `minDepth`/`maxDepth`/`nearZ`/`farZ` map the depth image's range to view-Z.

**This is exactly the "depth estimation of the display" use case:** display a
2D frame in VR, run an AI model on that frame, write the estimated depth into
the depth swapchain, attach it, submit. The compositor then treats the 2D
content as if it had real geometry.

## 3. `XR_MSFT_composition_layer_reprojection` — why the depth matters

```c
typedef enum XrReprojectionModeMSFT {
    XR_REPROJECTION_MODE_DEPTH_MSFT             = 1,
    XR_REPROJECTION_MODE_PLANAR_FROM_DEPTH_MSFT = 2,
    XR_REPROJECTION_MODE_PLANAR_MANUAL_MSFT     = 3,
    XR_REPROJECTION_MODE_ORIENTATION_ONLY_MSFT  = 4,
    ...
} XrReprojectionModeMSFT;
```

- `DEPTH`: "the corresponding layer may benefit from **per-pixel depth
  reprojection** provided by `XrCompositionLayerDepthInfoKHR` to the
  projection layer. Typically used for world-locked content that should
  remain physically stationary as the user walks around."
- `PLANAR_FROM_DEPTH`: planar reprojection where the plane is *calculated
  from* the depth info — "works better when the application knows the
  content is mostly placed on a plane."
- `PLANAR_MANUAL` / `ORIENTATION_ONLY`: the cheaper fallbacks.

Without depth, a 2D layer is reprojected as a plane → it "floats" or pops
when the user moves their head. With per-frame AI-estimated depth, the layer
stays glued to the scene. This is the user-visible payoff of the feature.

## 4. Environment (passthrough) depth — the real world

- **`XR_META_environment_depth`** (Meta Quest):
  `xrCreateEnvironmentDepthProviderMETA`, `xrAcquireEnvironmentDepthImageMETA`,
  `xrSetEnvironmentDepthHandRemovalMETA`, `xrDestroyEnvironmentDepthProviderMETA`.
  The headset's depth sensor hands you the environment depth image directly.
- **`XR_FB_passthrough`**: `XR_PASSTHROUGH_LAYER_DEPTH_BIT_FB` — "the
  passthrough system sends depth information to the compositor. Only
  applicable to layer objects."
- **`XR_VARJO_environment_depth_estimation`** — the most interesting one:
  ```c
  XrResult xrSetEnvironmentDepthEstimationVARJO(XrSession session, XrBool32 enabled);
  ```
  "Toggles **environment depth estimation** in the compositor. Toggling depth
  estimation is an asynchronous operation and the feature may not be
  activated immediately… Compositor will disable depth estimation
  functionality if environment blend mode is not
  `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`…"
  → A vendor (Varjo) already ships *compositor-side* depth estimation for the
  passthrough environment. Prior art that exactly this concept is a real,
  shipping feature — with the estimator living in the runtime, not the app.
- **`XR_ANDROID_composition_layer_passthrough_mesh`** — passthrough as a mesh
  (Android spatial computing).
- **`XR_FB_composition_layer_depth_test`** / **`XR_VARJO_composition_layer_depth_test`** —
  depth-test flags for composition layers (structures
  `XR_TYPE_COMPOSITION_LAYER_DEPTH_TEST_FB` / `_VARJO`).

## 5. Depth-range configuration

- **`XR_EXT_view_configuration_depth_range`** — per-view-configuration
  near/far depth range (so the runtime/compositor knows the Z extent of your
  content).
- **`XR_ML_view_configuration_depth_range_change`** — event for dynamic
  depth-range changes.

## 6. Android spatial / OS integration

- `XR_SPATIAL_CAPABILITY_DEPTH_RAYCAST_ANDROID` — depth raycast capability.
- `XR_TRACKABLE_TYPE_DEPTH_ANDROID` — depth trackable type.
- `XR_OVERLAY_MAIN_SESSION_ENABLED_COMPOSITION_LAYER_INFO_DEPTH_BIT_EXTX` —
  depth bit in overlay composition layer info.

(These show the direction of travel: depth as a *spatial-computing* primitive
shared with the OS — Windows MR / Android — not just a rendering detail.)

## What this means for "AI depth estimation of the display"

Three distinct sub-problems, each with a clean OpenXR answer:

| Content | Where depth comes from | OpenXR surface |
|---|---|---|
| Your own 3D scene | Z-buffer (exact) | depth swapchain (§1) |
| 2D content shown in VR (video, screen, image) | **AI model on the frame** | `XR_KHR_composition_layer_depth` (§2) + reprojection mode DEPTH (§3) |
| Real world (passthrough) | Sensor depth, or **AI model on passthrough RGB** | `XR_META_environment_depth` / `XR_FB_passthrough` / `XR_VARJO_environment_depth_estimation` (§4) |

The AI model sits between "frame is available" and "depth is attached".
Its latency budget is the frame interval: 72 Hz → 13.9 ms, 90 Hz → 11.1 ms,
120 Hz → 8.3 ms, 144 Hz → 6.9 ms. Whether that's achievable is the
benchmark question (see `bench/`).

## Open questions (to resolve in the report)

1. Does the *compositor* (not the app) already get depth for 3D content?
   → For a depth swapchain, yes — the runtime can read it. So for a fully
   3D app, "depth of the display" already exists without AI.
2. For 2D layers, is the depth image per-eye or shared? → Per-eye
   (`XrCompositionLayerProjectionView` is per-view). One model run can
   produce both (rectify or run per-eye; stereo is nearly free since the
   frames differ only by parallax).
3. Cost of the depth swapchain: same format/size constraints as color
   swapchains? → Same `XrSwapchainCreateInfo` path; check
   `maxImageRectWidth/Height` from the view configuration.
4. Which runtimes actually *use* the attached depth (reprojection quality)?
   → Requires `XR_MSFT_composition_layer_reprojection` support; SteamVR /
   Oculus / WMR support varies — validate per-runtime in the prototype.
