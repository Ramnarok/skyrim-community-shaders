# Community Shaders — notes for SkyrimRT

What we learned about CS's internals that the RT feature depends on. Paths are relative to the repo root; line numbers are as of upstream `dev` @ `44826d2` (2026-09-23) and will drift.

CS also ships its own agent guide at `.claude/CLAUDE.md` (build wrappers, feature pattern, i18n, commit style). This file only covers what matters for RT.

## Pinned versions

| Dependency | Pin | Notes |
|---|---|---|
| CommonLibSSE-NG | `extern/CommonLibSSE-NG` @ `70c1acd` = **v6.7.1** (2026-08-26) | From `alandtse/CommonLibVR` branch `ng`. Defines `RUNTIME_SSE_1_7_99`, not `1_7_104`. |
| Streamline | `extern/Streamline-DX12` @ v2.10.3 | Used by Upscaling (DLSS / frame gen / Reflex). |
| FidelityFX | `extern/FidelityFX-SDK` @ `054f0ad` | FSR, DX11 port. |
| vcpkg baseline | `dddca6f` | `vcpkg.json` `builtin-baseline`. |

**Runtime coverage.** `SKSEPlugin_Version` declares `UsesAddressLibrary()` + `UsesNoStructs()` (`src/XSEPlugin.cpp:61-68`), so SKSE loads CS on any runtime that has an Address Library bin. Version-specific offsets go through `Util::VersionedRelocation::Select(se, ae, ae1799)` (`src/Utils/VersionedRelocation.h`), whose third branch is "AE 1.7.99 or newer". So 1.7.104 takes the 1.7.99 path. Whether every in-function patch offset tuned for 1.7.99 still holds on 1.7.104 is only provable in game.

**Hard runtime requirement:** CS refuses to install any hooks if `Data/SKSE/Plugins/EngineFixes.dll` is missing (`src/XSEPlugin.cpp:231`). It also refuses on a list of incompatible DLLs (`:179-199`).

## Plugin lifecycle

`SKSEPlugin_Load` (`src/XSEPlugin.cpp:49`) → `Load()`:
1. `globals::OnInit()` creates the singletons (ShaderCache, State, Menu, Deferred, …); `globals::ReInit()` resolves game pointers (the renderer isn't up yet).
2. `State::Load()` reads `SettingsDefault.json` → `SettingsUser.json` → overrides; each feature's `Feature::Load(json&)` checks its `.ini` version and sets `loaded`.
3. `Hooks::InstallEarlyHooks()` IAT-patches `D3D11CreateDeviceAndSwapChain` (unless Upscaling owns it) and `CreateDXGIFactory` (`src/Hooks.cpp:1110`).
4. `Feature::Load()` (no-arg virtual) is called on every loaded feature — "only for critical hooks like D3D".

SKSE messages (`MessageHandler`, `src/XSEPlugin.cpp:78`):
- `kPostPostLoad` → `Deferred::Hooks::Install()`, `Hooks::Install()`, then `Feature::PostPostLoad()` (features may disable themselves here), then disk-cache validation.
- `kDataLoaded` → `globals::OnDataLoaded()`, waits for shader compilation, then `Feature::DataLoaded()`.
- `kPostLoadGame` → `Feature::GameLoaded()`.

Renderer bring-up (`src/Hooks.cpp`):
- `BSGraphics_Renderer_Init_InitD3D::thunk` (`:576`) → after the game creates D3D11: `globals::ReInit()` (device/context/swapchain now valid), Present/sampler hooks, `InstallD3DHooks` (Map/Unmap on the immediate context to capture the per-frame CB), `Menu::Init()`.
- `BSShaderRenderTargets_Create::thunk` (`:519`) → after the game creates its render targets: `ReInit()` + `State::Setup()`, which calls `Feature::SetupResources()` on every loaded feature and then `Deferred::SetupResources()` (`src/State.cpp:287-295`). **This is where an RT feature creates its D3D12 sidecar**: the D3D11 device and every game render target exist by then.

Per-draw: `Hooks::BSGraphics_SetDirtyStates::thunk` (`src/Hooks.cpp:462`) → `State::Draw()`.

### Debug-build gotchas
- Debug builds **spin at load until a debugger attaches** (`src/XSEPlugin.cpp:51-53`).
- Debug builds log to the MSVC debugger sink, **not a file** (`:23-33`). The verification loop reads log files, so test with Release/Dev builds; use Debug only with a debugger attached.

## Feature registration API

A feature is a `struct X : Feature` (`src/Feature.h`) with:
- identity: `GetName()`, `GetShortName()` (JSON/ini key), `GetDisplayName()` (i18n `T(...)`), `GetCategory()` (`FeatureCategories::kLighting` etc.), `GetFeatureSummary()`;
- shader defines: `GetShaderDefineName()` + `HasShaderDefine(RE::BSShader::Type)` (+ optional `GetShaderDefineOptions()`, ImageSpace only);
- lifecycle virtuals: `Load()`, `PostPostLoad()`, `DataLoaded()`, `GameLoaded()`, `SetupResources()`, `Reset()` (per frame, `src/State.cpp:237`), `EarlyPrepass()`, `Prepass()`, `ReflectionsPrepass()`, `ClearShaderCache()`;
- settings: `DrawSettings()` (ImGui), `LoadSettings/SaveSettings(json&)`, `RestoreDefaultSettings()`. Settings structs typically use `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT`.

Registering a new feature takes **four edits**:
1. `src/Globals.h` — forward-declare the struct and `extern X x;` in `globals::features`.
2. `src/Globals.cpp` — include the header and define `X x{};`.
3. `src/Feature.cpp` — `#include "Features/X.h"` (the list needs the complete type) and add `&globals::features::x` to the static list in `Feature::GetFeatureList()` (~`:222`). List order = call order for every lifecycle hook.
4. `features/<Folder Name>/Shaders/Features/<ShortName>.ini` with `[Info] Version = x-y-z`. Without the ini the feature doesn't load. An optional `CORE` marker file in the folder bundles it into the core package. Pre-release flags `Alpha`/`Beta`/`Unreleased` go in the same `[Info]` block (see `.claude/CLAUDE.md`).

Template: `docs/new-feature-template/` (`NewFeature.h/.cpp`, ini, shader folder). CS's `.claude/CLAUDE.md` mentions a `template/` directory; that's stale.

Feature shaders live in `features/<Folder>/Shaders/<ShortName>/` and are loaded at runtime from `Data\Shaders\<ShortName>\...`, compiled with `Util::CompileShader(L"Data\\Shaders\\...", defines, "cs_5_0")` (e.g. `src/Features/ScreenSpaceShadows.cpp:101`). All UI strings go through `T()`/`TKEY()`; run `python tools/extract-i18n.py --write` after adding strings.

## Getting the D3D11 device and context

`globals::ReInit()` (`src/Globals.cpp:186`):
```cpp
d3d::device    = (ID3D11Device*)       renderer->GetRuntimeData().forwarder;
d3d::context   = (ID3D11DeviceContext*)renderer->GetRuntimeData().context;
d3d::swapChain = (IDXGISwapChain*)     renderer->GetRuntimeData().renderWindows->swapChain;  // renderWindows[0]
```
Use `globals::d3d::device/context/swapChain`; don't re-query. `renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET]` and `renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL]` give every game RT/DS (texture, SRV, RTV/UAV). The adapter description is captured in `hk_D3D11CreateDeviceAndSwapChain` (`src/Hooks.cpp:427`) into `State::adapterDescription`.

Per-frame camera data: `globals::game::frameBufferCached` (captured from the game's per-frame CB via the Map/Unmap hooks). It holds view/proj (jittered and unjittered), their inverses, `CameraPosAdjust` and previous-frame values. Shader side: `Common/FrameBuffer.hlsli`, bound at CS slot b12 during deferred passes.
  - ⚠ **`GetCameraProjUnjittered()` holds the *inverse* unjittered projection** (measured 2026-09-25 at `SkyrimRT::Prepass`, dump 3577): its diagonal is 0.839 / 0.472 while `CameraViewProjUnjittered` scales the view rows by 1.192 / 2.119, and its last rows are (0,0,0,1), (0,0,−1/15,1/15), exactly the inverse of the projection (near 15, `w = z`, infinite far). `CameraViewProjUnjittered` and `CameraPreviousViewProjUnjittered` are the real ones (they reproduce the game's `kMOTION_VECTOR` to 0.0004 px through `MotionBlur::GetSSMotionVector`'s formula); the true projection is `CameraViewProjUnjittered × CameraViewInverse`. CS's own Streamline / FidelityFX frame-generation setup passes `GetCameraProjUnjittered().Transpose()` as `cameraViewToClip`; not checked whether that is affected.

## Existing D3D11↔D3D12 interop in CS (precedent for M2)

`src/Features/Upscaling/DX12SwapChain.{h,cpp}` already runs a D3D12 device next to the game's D3D11 device (frame generation / Streamline):
- `CreateD3D12Device(adapter)` → `D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, …)` on the game's adapter (`:11-13`).
- `CreateInterop()` (`:100-105`): `ID3D12Device::CreateFence(D3D12_FENCE_FLAG_SHARED)` → `CreateSharedHandle` → `ID3D11Device5::OpenSharedFence`.
- Sync (`:233-277`): `ID3D11DeviceContext4::Signal` → `ID3D12CommandQueue::Wait` … `queue->Signal` → `ctx4->Wait`.
- `WrappedResource` (`:330`): **created in D3D11** with `D3D11_RESOURCE_MISC_SHARED | SHARED_NTHANDLE`, `IDXGIResource1::CreateSharedHandle`, then `ID3D12Device::OpenSharedHandle`. That's the opposite direction from ARCHITECTURE §2 (create in D3D12, open in D3D11). Both are legal; this one is proven in this codebase for 2D textures.

⚠ **On Jake's machine the DX12 swap-chain path is active by default** (`[DX12SwapChain] Swap chain format negotiation` at boot, Streamline DLSS 4.5 enabled), so a D3D12 device already exists on the adapter from startup. With DLSS on, the internal render resolution was 1280×720 (dynRes 0.667) for 1920×1080 output; RT buffers should follow the internal resolution. The RT sidecar should either share it or be proven to coexist.

## Frame order (where hooks fire)

From `src/Deferred.h:151` (`Deferred::Hooks::Install`) and `src/Deferred.cpp:642-728`:
1. `Main_RenderShadowMaps` → after shadow maps: `Deferred::EarlyPrepasses()` → uploads sun cascade data to t98, `Feature::EarlyPrepass()`.
2. `Main_RenderWorld` → sets `state->inWorld`. The game's depth pre-pass happens inside.
3. `Main_RenderWorld_Start` (before the first opaque batch) → `Deferred::StartDeferred()`: binds the G-buffer MRTs, runs `PrepassPasses()` → **`Feature::Prepass()`**, then swaps in deferred blend states.
4. Opaque geometry renders with CS's `Lighting.hlsl`. It computes **direct lighting (sun + point lights, including the sun shadow term)** and writes the G-buffer in the same pass.
5. `Main_RenderWorld_BlendedDecals` → TerrainBlending passes, decals, then `Deferred::EndDeferred()` → **`Deferred::DeferredPasses()`**: SSGI → SubsurfaceScattering → DynamicCubemaps → **DeferredCompositeCS** (ambient/GI/reflections added into kMAIN).
6. `kMAIN` depth is copied to `kPOST_ZPREPASS_COPY` (`src/Deferred.cpp:694`); water and transparents follow.

Cubemap reflections: `BSCubeMapCamera_RenderCubemap` → `ReflectionsPrepasses()` → `Feature::ReflectionsPrepass()`.

## How lighting shaders are hooked

- CS replaces the game's compiled shaders with its own, compiled at runtime from `package/Shaders/*.hlsl` (`Lighting.hlsl`, `RunGrass.hlsl`, `DistantTree.hlsl`, `Sky.hlsl`, `Water.hlsl`, `IS*.hlsl`, …) by `SIE::ShaderCache` (`src/ShaderCache.cpp`), keyed on the game's technique descriptors. `State::ModifyShaderLookup` rewrites descriptor bits for CS's pipeline.
- Vtable hooks on `SetupGeometry` for each shader class (`src/Hooks.cpp:1072-1078`), e.g. `LightingExtensions::BSLightingShader_SetupGeometry`. There's also a byte patch forcing world-space render (`:1088-1105`), which uses **different offsets for AE and SE** and has no 1.7.99-specific branch. It's a candidate breakage point on 1.7.104 if that function changed.
- CS constant buffers: `State::permutationCB`/`sharedDataCB`/`featureDataCB` at PS b4–b6 and CS b5–b6 (`src/Deferred.cpp:725-727`, re-bound after `Renderer::ResetState`).
- Compiled shaders are cached on disk. After changing HLSL or a feature's define set, the disk cache must be invalidated (`Feature::ClearShaderCache`, disk-cache validation at PostPostLoad).

## Shader defines per feature

When a shader of type T is compiled, every **loaded** feature with `HasShaderDefine(T) == true` adds `GetShaderDefineName()` as a macro (`src/ShaderCache.cpp:153-156` and the same loop for each `RE::BSShader::Type`; ImageSpace also appends `GetShaderDefineOptions()`, `:678-687`). Defines are therefore compile-time and depend only on "feature loaded". Runtime on/off goes through settings in a CB (e.g. `bendSettings.Enable`). The deferred composite is compiled separately with a hand-written define list in `Deferred::GetComputeMainComposite()` (`src/Deferred.cpp:586`): `DYNAMIC_CUBEMAPS`, `SKYLIGHTING`, `SSGI`, `IBL`, `TERRAIN_BLENDING`, each gated on that feature being loaded. The interior variant (`:615`) adds `INTERIOR` and drops `SKYLIGHTING`. **An RT GI path has to be added to both lists by hand.**

## Screen-Space Shadows (integration point for M5)

`src/Features/ScreenSpaceShadows.{h,cpp}`, shaders in `features/Screen-Space Shadows/Shaders/ScreenSpaceShadows/`. Define `SCREEN_SPACE_SHADOWS`; `HasShaderDefine` returns true for all types.
- **Produce:** `Prepass()` (`.cpp:231`) clears `screenSpaceShadowsTexture` to white, dispatches `RaymarchCS.hlsl` (Bend Studio SSS) when the sky mode is full, then **`PSSetShaderResources(45, …)` — PS slot t45.** The texture is `R8_UNORM`, the same size as the game's `kSHADOW_MASK` (`SetupResources`, `.cpp:291-313`).
- **Inputs:** only depth — `Util::GetCurrentSceneDepthSRV(false)` (below) — plus the sun direction from `accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight`.
- **Consume:** `Lighting.hlsl:830` includes `ScreenSpaceShadows/ScreenSpaceShadows.hlsli`; `Lighting.hlsl:2223-2226`: in the DEFERRED path, exteriors only, `dirDetailedShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(...)`. Also used by `RunGrass.hlsl` and `DistantTree.hlsl`.
- **Implication for RT:** the mask must exist **before the opaque pass**, i.e. at `Prepass()` time, when only the depth pre-pass result is available (no normals). RT sun shadows must either trace from depth alone within the frame (D3D11 waits for D3D12 mid-frame) or use last frame's result with reprojection.

## Screen-Space GI (integration point for M6)

`src/Features/ScreenSpaceGI.{h,cpp}`, shaders in `features/Screen Space GI/Shaders/`. Composite define `SSGI`.
- **Produce:** `ScreenSpaceGI::DrawSSGI()` (`.cpp:721`), called from `Deferred::DeferredPasses()` (`src/Deferred.cpp:328-331`) once the G-buffer is complete. `GetOutputTextures()` returns four SRVs: AO, luminance-Y as SH (`float4`), CoCg chroma, and HQ specular.
- **Consume:** `package/Shaders/DeferredCompositeCS.hlsl` t10–t13 (`SsgiAoTexture`, `SsgiYTexture`, `SsgiCoCgTexture`, `SsgiSpecularTexture`); `SampleSSGI` (`:47`) evaluates Y via `SHHallucinateZH3Irradiance` and adds `il` to the ambient term; AO multiplies the directional ambient (`:117-176`). SRV table at `src/Deferred.cpp:350-367`.
- **Implication for RT:** an RT GI pass that writes the same four textures (AO, SH-Y, CoCg, spec) could reuse the composite path unchanged. It runs after the full G-buffer, which matches ARCHITECTURE's assumptions.

## Depth and normal buffers

- **Depth, pre-pass copy:** `Util::GetCurrentSceneDepthSRV(bool prefer16bit)` (`src/Utils/D3D.cpp:15`) → `depthStencils[kPOST_ZPREPASS_COPY].depthSRV` (`R24_UNORM_X8_TYPELESS`, HLSL `Texture2D<unorm float>`). If TerrainBlending is loaded and enabled, it returns `terrainBlending.blendedDepthTexture` (`R32_FLOAT`) instead. Shaders switch type on the `TERRAIN_BLENDING` define. Depth is standard, not reversed: the far plane = 1.0 (`DeferredCompositeCS.hlsl:109`, SSS uses `FarDepthValue = 1`).
- **Depth, main:** `depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN]` (the depth-stencil the opaque pass writes).
- **G-buffer** (`src/Deferred.h:9-14`, created in `Deferred::SetupResources`; they reuse game RT slots):
  - `NORMALROUGHNESS` = `kRAWINDIRECT_DOWNSCALED`, `R10G10B10A2_UNORM`: xy = encoded **view-space** normal (`GBuffer::DecodeNormal`), z = glossiness.
  - `ALBEDO` = `kINDIRECT`, `SPECULAR` = `kINDIRECT_DOWNSCALED`, `REFLECTANCE` = `kRAWINDIRECT`, `MASKS` = `kRAWINDIRECT_PREVIOUS`, `MASKS2` = `kRAWINDIRECT_PREVIOUS_DOWNSCALED` (x = 1 − vertex AO).
  - MRT order during the deferred pass: kMAIN, kMOTION_VECTOR, NORMALROUGHNESS, ALBEDO, SPECULAR, REFLECTANCE, MASKS, MASKS2 (`src/Deferred.cpp:266-275`).
- World-position reconstruction as done by the composite: `mul(FrameBuffer::CameraViewProjInverse, float4(ndc.xy, depth, 1))`, divided by w. That's **camera-relative** world space: absolute = relative + `FrameBuffer::CameraPosAdjust` (see `Common/SharedData.hlsli:411`, which does exactly that to get cell coordinates). The RT TLAS must use the same origin (`CameraPosAdjust`), not the NiCamera position.

## CommonLib check of ARCHITECTURE.md names (v6.7.1 headers)

| Name in ARCHITECTURE.md | Result | Where |
|---|---|---|
| `BSGraphics::Renderer::GetRuntimeData()` `.forwarder` / `.context` | ✅ | `RE/R/Renderer.h:87-88` |
| swapchain on runtime data | ✅ but it's `renderWindows[i].swapChain` (`RendererWindow`) | `RE/R/Renderer.h:27,89` |
| `RE::Main::WorldRootNode()` | ✅ returns `SceneGraph*` | `RE/M/Main.h:79` |
| `RE::BSVisit::TraverseScenegraphGeometries` | ✅ `(NiAVObject*, std::function<BSVisitControl(BSGeometry*)>)` | `RE/B/BSVisit.h:19` |
| `BSGeometry::GetGeometryRuntimeData().rendererData` | ✅ `BSGraphics::TriShape*`; also `.skinInstance` (`NiPointer<NiSkinInstance>`) | `RE/B/BSGeometry.h:58-101` |
| `BSGraphics::TriShape` VB/IB/vertexDesc | ✅ `vertexBuffer`, `indexBuffer`, `vertexDesc`, **plus `rawVertexData` / `rawIndexData` CPU pointers** | defined in `RE/N/NiSkinPartition.h:14` (not its own header) |
| Vertex/triangle counts on `BSTriShape` | ✅ `GetTrishapeRuntimeData().vertexCount/.triangleCount`, both `uint16_t` | `RE/B/BSTriShape.h:15-37` |
| `BSGraphics::VertexDesc`, `VF_FULLPREC` | ✅ `Vertex::VF_FULLPREC = 0x400`; flags at bits 44+; `GetAttributeOffset(attr)` | `RE/V/VertexDesc.h` |
| `BSDynamicTriShape` | ✅ | `RE/B/BSDynamicTriShape.h:8` |
| `BSLODTriShape` | ❌ **not in CommonLib.** Related classes that do exist: `BSMultiIndexTriShape`, `BSSubIndexTriShape`, `BSInstanceTriShape`, `BSMultiStreamInstanceTriShape` | `RE/B/` |
| `NiAVObject::world` | ✅ direct member at 0x7C for all runtimes; `previousWorld` at 0xB0 too; flags need `GetFlags()` | `RE/N/NiAVObject.h:157-170` |
| renderer's `posAdjust` | ⚠ lives on `RendererShadowState` runtime data (`EYE_POSITION<NiPoint3>`), not the renderer. CS exposes it as `frameBufferCached.GetCameraPosAdjust()` | `RE/R/RendererShadowState.h:137` |
| loaded cells via `RE::TES` | ✅ `TES::ForEachCell`, `ForEachCellInRange`, `gridCells` | `RE/T/TES.h:79-80,211` |

⚠ `VertexDesc::GetSize()` always counts position as a float4 (16 B), even without `VF_FULLPREC`, where position is half4 (8 B). **Don't use it for the BLAS stride.** Per the nifskope layout the stride should be `(desc & 0xF) * 4`; confirm against real buffers in M3.

⚠ `rawVertexData`/`rawIndexData` may let M3 upload meshes from CPU memory without copying game-owned D3D11 buffers. It's unknown whether the game keeps these alive after upload; check at runtime (count non-null in the M3 dump).

## Open questions for M0 sign-off

- [x] Does the unmodified build load and run on **1.7.104**? **Yes** (2026-09-23): all hooks installed without errors; a save loaded and rendered with no crash.
- [x] Does the scene depth at `Prepass()` time hold this frame's complete depth pre-pass? **Yes** (M5, 2026-09-23). The M4 depth trace run at `Prepass()` matched 334,296 of 334,298 counted pixels against `Util::GetCurrentSceneDepthSRV(false)` (TerrainBlending active). Which geometry the pre-pass skips (first person?) is still unchecked.

## SkyrimRT feature (M1)

- Feature: `src/Features/SkyrimRT.{h,cpp}`; ini `features/Skyrim RT/Shaders/Features/SkyrimRT.ini` (0-1-0, `Alpha = True`). No shader defines until M8 (`SKYRIM_RT`, below).
- D3D12 code: `src/RT/RT.{h,cpp}`. `RT::Init(globals::d3d::device)` runs from `SkyrimRT::SetupResources()`: IDXGIDevice → adapter → name + LUID → probe `D3D12CreateDevice(FL 12_0)` → `D3D12_FEATURE_D3D12_OPTIONS5.RaytracingTier`. The probe device is released; no D3D12 object stays resident until M2.
- Below DXR 1.1 (or probe failure) the feature sets `loaded = false` + `failedLoadedMessage`, following the HorizonFix pattern.
- Jake's machine (2026-09-23): RTX 4080 SUPER, LUID `00000000:0000D324`, driver reports a raytracing tier **above 1.1** (enum value > 11, most likely 1.2). The Windows SDK 10.0.26100 headers name only up to `TIER_1_1`, so `RT::GetTierName` derives `major.minor` from the enum value.

## SkyrimRT sidecar (M2)

- `src/RT/Sidecar.{h,cpp}`: persistent D3D12 device. **Until 2026-09-24 this was in fact the process-wide singleton that Upscaling's DX12SwapChain created, since `D3D12CreateDevice` returns one device per adapter. Now its own, through `ID3D12DeviceFactory` (`RT::CreateSidecarDevice`)**, DIRECT queue, one shared fence used both ways, 3 frame slots (a busy slot is skipped, never CPU-waited). `src/RT/DebugDump.{h,cpp}`: F10 → `frame_<n>.json` + `debug_testpattern_<n>.png` on a worker thread (WIC via DirectXTex, COM initialised on that thread).
- Per-frame hook: `SkyrimRT::Reset()`, which `State::Reset()` calls first thing in the Present hook (`src/Hooks.cpp:385`), before `HDRDisplay::HandleSwapChainPresent` draws the ImGui overlay. So D3D11 `Wait` precedes the overlay's read in queue order, and next frame's `Signal` follows it.
- `SkyrimRT` is an `OverlayFeature`: `OverlayRenderer::RenderFeatureOverlays` calls `DrawOverlay()` on every loaded overlay feature each frame; the feature decides visibility itself.
- Debug layer/DRED (debug builds) are enabled in `SkyrimRT::Load()` because enabling them after any D3D12 device exists removes that device (CS's frame-gen device included).
- D3D12 shaders: `D3DCompileFromFile(... "cs_5_1")` on `Data\\Shaders\\SkyrimRT\\*.hlsl`. RayQuery (M4) will need DXC / SM 6.5.
- Sharing results and timings: ARCHITECTURE §2.

## SkyrimRT scene extraction (M3)

- `src/RT/Scene.{h,cpp}`: `TES::ForEachCell` → `cell->GetRuntimeData().loadedData->cell3D`, manual walk (prunes `NiAVObject::Flag::kHidden` subtrees), classification by `BSGeometry::GetType().get()`, `skinInstance`, shader property RTTI (`netimmerse_cast`), terrain = lighting material `Feature::kMultiTexLand[LODBlend]` (same test as `TruePBR.cpp`).
- `src/RT/MeshCache.{h,cpp}`: 64 MB DEFAULT pages (first fit), 32 MB UPLOAD ring, 8 MB / 128 meshes / 16 readbacks per frame, evict after 120 unseen frames. Uploads run in a second command list after the D3D11 handoff signal.
- CommonLib declares its own `RE::ID3D11Buffer`; `reinterpret_cast` to `::ID3D11Buffer*` to call D3D11 on it.
- `VertexDesc` keeps its bits private and `GetSize()` is non-const; copy the struct (`memcpy` for the raw bits).
- M3 costs (RTX 4080 SUPER, 26 exterior cells / big dungeon): scene walk 0.5–1.2 ms CPU per frame (the main CPU cost; candidate for walking every N frames or event-driven attach/detach), cache update 0.05–0.12 ms.

## SkyrimRT ray tracing (M4)

- `src/RT/Raytracer.{h,cpp}`: BLAS builds (via `MeshCache::BuildBLASes`), exclusion AABB BLAS, TLAS, trace, counter/timestamp readback. `src/RT/BufferPool.{h,cpp}`: shared first-fit pool (mesh data and BLAS memory).
- DXC shaders: sources in `src/RT/Shaders/*.hlsl` → `cmake/SkyrimRTShaders.cmake` (Windows SDK `dxc.exe`, `cs_6_5`, signed via `dxil.dll`) → `Data/Shaders/SkyrimRT/*.cso`. Kept out of `features/` so CS's FXC validation ignores them.
- **CMake gotcha:** with `AIO_ZIP_TO_DIST`/`AUTO_PLUGIN_DEPLOYMENT` the `AIO` target copies instead of installing, and `CleanupStaleEntries` deletes untracked files under `aio/Shaders`. The `.cso` copy is therefore a `POST_BUILD` step of the `AIO` target, and `SkyrimRTShaders.cmake` is included at the end of `CMakeLists.txt` (after `AIO` exists).
- Camera capture: `SkyrimRT::Prepass()` (main deferred prepass) copies `frameBufferCached` + render size; the trace runs only if the capture's `frameCount` equals the frame being presented.
- Grass: `GrassOptimizations::Hooks::LoadGrassType::lastGrassManager` (atomic, added for SkyrimRT) holds the game's `BGSGrassManager*`.
- Overlay: the debug view is drawn bottom-right via `ImGui::Image` with UVs cropped to the render region.

## SkyrimRT GI (M6)

- **Integration point:** `Deferred::DeferredPasses` (`src/Deferred.cpp`), the second edit to upstream CS code. `SkyrimRT::DrawGlobalIllumination` runs first; if it returns true, `DrawSSGI()` is skipped and its AO / Y / CoCg SRVs replace SSGI's in the composite's t10–t12. `ssgi_hq_spec` is forced off, so t13 stays null. The composite's `SSGI` define (both exterior and `INTERIOR` variants) depends only on the SSGI feature being loaded; its `Enabled` setting doesn't matter.
- **What SSGI gathers:** `forwardRenderTargets[0]` = `kMAIN` at `DeferredPasses` time, through `Color::RadianceToLinear`. That's the opaque pass's output: direct sun + point lights + the vanilla DALC ambient, since the composite later subtracts `directionalAmbientColor` and re-adds it scaled by AO. The composite then adds `il * linAlbedo`, with `il = YCoCgToRGB(SHHallucinateZH3Irradiance(Y, N), CoCg)`.
- **Lighting sources (as `State::UpdateSharedData`):** `DirLightColor` = `smState->shadowSceneNode[0]` sun `diffuse × fade × imageSpaceManager hdr.sunlightScale`. Ambient = `SphericalHarmonics::DALCToSH` of the six colours from `smState->directionalAmbientTransform` (`src/Utils/SphericalHarmonics.h`). Linear Lighting (`globals::features::linearLighting.settings`) is off by default.
- **Conventions:** `kMOTION_VECTOR` holds `(-0.5, 0.5) × (currNDC − prevNDC)` = `prevUV − currUV` (`Common/MotionBlur.hlsli`). G-buffer normals are view-space octahedral (`GBuffer::DecodeNormal`, note the sign flips), turned into world space with `CameraViewInverse`. HLSL 2021 (the SDK's DXC) needs `select()` for vector conditionals.
- **Point lights (M8):** `LightLimitFix::UpdateLights` (from its `Prepass`) builds the CPU list Lighting.hlsl's clustered loop reads; its local vector is now the member `lightsData` (an edit to upstream CS code) so SkyrimRT can read it later the same frame. `LightData::invRadius` is filled only on the Inverse Square Lighting path and for particle lights; without ISL, Lighting.hlsl uses `radius` directly. Lights with `shadowMaskIndex == 255` (inactive shadow lights) are already dropped. Attenuation: `Lighting.hlsl` ~2395 and `features/Inverse Square Lighting/Shaders/InverseSquareLighting/InverseSquareLighting.hlsli`; colour: `Color::PointLight` × `Color::VanillaNormalization` (`Common/Color.hlsli`, `Common/LightingEval.hlsli:136`).
- **The composite's reflection term (M8 reflections):** only under `DYNAMIC_CUBEMAPS` (Dynamic Cubemaps loaded; else t5 is null). `REFLECTANCE` (`kRAWINDIRECT`, R11G11B10) holds `indirectLobeWeights.specular` = `F0 × EnvBRDF.x + EnvBRDF.y` (split sum, `LightingEval.hlsli:152`), × envMask / `MaterialData.y` where those apply. It's non-zero only where `material.F0 > 0`: True PBR, envmaps whose cubemap is Dynamic Cubemaps' black sentinel (`Lighting.hlsl` ~1990; vanilla cubemaps keep the old forward `envColor` path and write 0), CS Skin, CS Hair and Wetness Effects (whose water film also overrides the G-buffer normal and glossiness). The composite adds `reflectance × finalIrradiance`, where `finalIrradiance` is the cubemap sample normalised to the DALC luminance along R (× 0.65 `ReflectionNormalisationScale`), with IBL / Skylighting variants, × the SSGI AO, plus SSGI's specular. SkyrimRT's t17 replaces `finalIrradiance` where its alpha is 1. Using t13 (HQ specular) instead would not work: its path halves the chroma (lerp towards the cubemap's) and adds the diffuse SH projected onto the lobe. The `INTERIOR` composite gets `SKYRIM_RT` too now.
- **Distant LOD (M8):** `TES::lodLandRoot` (direct member, 0x88) roots the distant land and object LOD in the scene graph; `TES::objLODWaterRoot` holds LOD water. `TESWorldSpace::GetTerrainManager()` (follows `kUseLODData` parents) → `BGSTerrainManager::rootNode` → `BGSTerrainNode` quadtree (`terrain` / `objects` / `trees` layers; `GetLODLevel()`); `BGSObjectBlock` isn't in CommonLib. ⚠ **CommonLib mistypes `BGSTerrainNode::children`** as `BGSTerrainNode* (*)[4]` (a pointer to four pointers). On 1.7.104 it points at the four child nodes themselves, stored contiguously (the root's `children` = root + 0x50; each child's `manager` and `parent` check out). Reading it as pointers crashed the first tree-LOD census every frame. Manager bytes seen in Tamriel: `minCellX/Y` = -96, `maxLevel` 32, `minLevel` 4. CS's UnifiedWater hooks `BGSTerrainBlock` attach/detach (`RelocationID(30934, 31737)` / `(30936, 31739)`) and `BGSTerrainNode::UpdateWaterMeshSubVisibility`, which at LOD level 4 `SetAppCulled`s the children of a block whose cells are attached (on 1.7.99+ under the terrain-cell-map lock, `REL::ID(564236)`). True PBR reads the terrain manager's `hasLOD` byte (0x36) for `kNoLODLandBlend`.
- **Materials:** `BSLightingShaderMaterialBase::diffuseTexture` (+0x48) → `NiSourceTexture::rendererTexture->resourceView`. True PBR's material classes also derive from `BSLightingShaderMaterialBase`, but its PBR landscape reports `kMultiTexLandLODBlend` with a different layout. So the vanilla landscape is detected with `skyrim_cast<BSLightingShaderMaterialLandscape*>` (the first use of that RTTI ID in this codebase; it resolves on 1.7.104 unless the test run crashes at startup).
- **Emission (M9 phase 4):** `Lighting.hlsl` gets `EmitColor` = the property's `emissiveColor × emissiveMult` (what Linear Lighting's `Color::EmitColor` divides back by `emissiveMult`, `LinearLighting.cpp` `SetupGeometry`). A NIF's emissive colour is kept only with `kOwnEmit` (True PBR's copy of the loader, `TruePBR.cpp` ~704), and every glowing shape in the census had it. Vanilla (not True PBR) adds it to the diffuse light before the albedo: `diffuseColor += emitColor`, then `× baseColor`, then `× vertexColor`. `BSLightingShaderMaterialGlowmap` shapes multiply it by the glow map at t6, sampled at the diffuse `uv`; without Linear Lighting that is `LinearToSrgb(SrgbToLinear(emit) × SrgbToLinear(glow))`, and `Color::SrgbToLinear` is `pow(x, 2.2)`, so exactly `emit × glow`. True PBR adds `emitColor` (× its emissive texture) **after** the albedo and only with `PBR::Flags::HasEmissive`. **CS marks True PBR shapes with `kVertexLighting`** (`TruePBR.cpp` ~740: the NIF's PBR flag is remapped onto it); their `emissiveColor` is set to white and `emissiveMult` to 1 or 0 by whether the emissive texture is real. A `BSLightingShaderMaterialPBR` is recognised as CS does: `kVertexLighting` and `GetFeature() == kDefault` (it overrides `GetFeature` to return `kDefault`; the PBR landscape reports `kMultiTexLandLODBlend`), then `static_cast`; it's CS's own C++ class, so the game's RTTI (`skyrim_cast`) can't find it. True PBR's emission: `HasEmissive` iff `emissiveTexture` isn't `defaultTextureBlack` (`TruePBR.cpp` ~1010, bound at t6); `Color::Glowmap` is `LinearToSrgb(texture)` without Linear Lighting (× `glowmapMult` with it); `emitVertexColor` = the linear vertex colour normalised by its largest channel, lerped back by `truePBRSettings.VertexAOStrength`; and after `color += emitColor` the whole colour is × `Color::PBRLightingScale` (0.65, 1 with Linear Lighting) **unless `IBL` is defined** (`Lighting.hlsl` ~2755 is `#if defined(IBL) … #elif defined(TRUE_PBR)`; `IBL` is undefined without `DYNAMIC_CUBEMAPS`). TRUE_PBR sets `vertexColor = 1`, so the final `× vertexColor` doesn't touch it.

## SkyrimRT sun shadows (M5)

- **The round trip now runs in `SkyrimRT::Prepass()`**, before the opaque pass. It happens at most once per game frame (`Sidecar::Submit`). `SkyrimRT::Reset()` (Present) calls `Sidecar::OnPresent`, which submits an untraced round trip only if Prepass didn't run one (menus, loading), then drives the dump. The M4 debug trace moved along with it, so it now compares against the depth *pre-pass*.
- `Prepass()` is reached via `Main_RenderWorld_Start` → `Deferred::StartDeferred` → `PrepassPasses`. By then TerrainBlending has already blended the pre-pass depth: `TerrainBlending::Hooks::Main_RenderDepth` ends with `BlendPrepassDepths()`. So `Util::GetCurrentSceneDepthSRV(false)` holds this frame's pre-pass depth, and `kPOST_ZPREPASS_COPY` is only overwritten with the post-opaque depth later, in `Main_RenderWorld_BlendedDecals`.
- `SkyrimRT` is last in `Feature::GetFeatureList()`, after `screenSpaceShadows` and `terrainBlending`. `ScreenSpaceShadows::Prepass()` returns early when `skyrimRT.ProvidesSunShadowMask()` is true. That decision is cached per frame, so both features agree, and `SkyrimRT::Prepass()` binds the RT mask at PS t45. This is the only edit to upstream CS code.
- The mask is consumed only where `SCREEN_SPACE_SHADOWS` is compiled in, i.e. when the SSS feature is **loaded**. It's a CORE feature, but its runtime `Enable` toggle doesn't matter. Consumers: `Lighting.hlsl` (deferred, exteriors, `dirLightAngle >= 0`), `RunGrass.hlsl` and `DistantTree.hlsl` (`lerp(1, mask, 0.8)`). It **multiplies** the game's shadow-map term (`dirDetailedShadow`), so shadow-map shadows from actors, foliage and distant land remain.
- Sun direction: the same source SSS uses, `currentAccumulator → activeShadowSceneNode → sunLight → light` as `NiDirectionalLight::GetWorldDirection()`. That vector points away from the light (model direction (1,0,0) is where the light shines), so the ray direction is its negation. At night this is the moon.
- Weapons worn by actors (e.g. on the back) are non-skinned `BSTriShape`s under the actor's 3D, so the M3 walk puts them in the TLAS as static geometry with this frame's world transform. They cast and receive RT shadows; the skinned body does not until M7.
- The game's `kSHADOW_MASK` render target is already written by `Prepass()` time. `.x` is the sun shadow-map visibility, and it matched the RT mask 98% where both see the same casters.
- `RE::BSGraphics::RenderTargetData` members are the real `::ID3D11*` types (forward-declared globally in `RE/R/RenderTargetData.h`), unlike CommonLib's `RE::ID3D11Buffer`.
- Code: `src/RT/SunShadows.{h,cpp}` (three DXC passes: `SunShadowTraceCS`, `SunShadowTemporalCS`, `SunShadowSpatialCS`, plus `SunShadowCommon.hlsli`); `src/RT/SharedTexture.{h,cpp}` (the D3D11-created shared-texture helper, now shared with `Raytracer`); `src/RT/FrameCapture.{h,cpp}` (D3D11 `kFRAMEBUFFER` → staging, polled with `D3D11_MAP_FLAG_DO_NOT_WAIT`).
- `cmake/SkyrimRTShaders.cmake` now passes `-I src/RT/Shaders` and makes every `.hlsl` depend on the `.hlsli` files.
- Dump sequence (F10): the next round trip copies the masks, and on shadow frames also the game's `kSHADOW_MASK` for a confusion matrix. The following Present captures `final_rt_on`. SSS then takes over for 3 frames (`Sidecar::IsSunShadowSuppressed`), and the Present of the third captures `final_rt_off`. The JSON is written once the fence and both captures are done.

## SkyrimRT point-light shadows (M8)

- **First shader define:** `SkyrimRT::GetShaderDefineName()` = `SKYRIM_RT`, Lighting shaders only. Adding it changes every Lighting permutation's cache key, so the first launch after it recompiles CS's shader cache.
- **Edit to upstream `package/Shaders/Lighting.hlsl`:** it includes `SkyrimRT/PointLightShadows.hlsli` under `SKYRIM_RT && LIGHT_LIMIT_FIX`; in the LLF loop (`DEFERRED` only), lights without `Shadow`/`PortalStrict` multiply `lightShadow` by the t46 mask. `lightShadow` scales that light's diffuse, specular and transmission (via `CreateDirectLightingContext`). `shadowComponent`, which gates EMAT parallax shadows, is unchanged.
- **Light Limit Fix's HLSL flags** are `LightLimitFix::LightFlags::*` (`features/Light Limit Fix/Shaders/LightLimitFix/Common.hlsli`). Portal-strict lights also arrive per geometry through `StrictLights` (PS b3), and `IsLightIgnored` culls clustered ones by `RoomIndex`, a per-draw value.
- **Unbound SRVs:** in D3D11, `Load` on an unbound slot returns 0 and `GetDimensions` returns 0 × 0. The t46 reader uses that to fall back to lit.
- **Validating a Lighting.hlsl edit without Python/hlslkit:** the Windows SDK's `fxc.exe /T ps_5_0 /E main /I build\ALL\aio\Shaders` with the `PSHADER` common defines from `.github/configs/shader-validation.yaml` plus the permutation's defines, on `build\ALL\aio\Shaders\Lighting.hlsl`.

## SkyrimRT actors and foliage (M7)

- **Trees are skinned.** Their branch geometry has a `skinInstance` (the branches sway on bones), so the scene walk classifies trees as `kSkinned` alongside actors. Measured: 892 skinned shapes in a forest with no NPCs. Anything that treats "skinned" as "actor" also catches trees; they sit on InstanceMask 0x04. Their pose is **not** stable within a frame: `BSLeafAnimNode::OnVisible` re-poses the branch bones for every camera that culls the tree (the view, the sun's shadow cascades, Skylighting's occlusion pass), so bones and `boneMatrices` hold the last culling camera's sway. Use the rest pose (`rootParent->world * inverse(NiSkinData::rootParentToSkin)`) for anything that must match the view.
- **Alpha test in `Lighting.hlsl`** (`DO_ALPHA_TEST`): discards when `alpha - AlphaTestRefRS < 0`. `alpha` is the diffuse alpha × `MaterialData.z` × vertex-colour alpha (except `TREE_ANIM` / LOD objects). The landscape shader (`LANDSCAPE` without `LOD_LAND_BLEND`) doesn't alpha-test. The test threshold is `NiAlphaProperty::alphaThreshold`; `GetAlphaTesting()` is `alphaFlags` bit 9 (CommonLib `NiAlphaProperty.cpp`).
- **UVs:** `VertexDesc::GetAttributeOffset(VA_TEXCOORD0)` returns a byte offset; UVs are 2 × half even when `VF_FULLPREC` is set (that flag only changes positions).
- **`NiSkinInstance::boneMatrices`** (CommonLib: `void*` at 0x48, with `numMatrices`, `numRegisters`, `allocatedSize`, `frameID`, `prevBoneMatrices`) is the renderer's copy of the skinning matrices, refreshed when the skin is drawn. Measured on 1.7.104: 3 registers per bone (48 B), rows of a row-major 3×4 in absolute world space, in skin-bone order, equal to `boneWorldTransforms[i] * skinToBone[i]` whenever the bones haven't moved since the draw. `frameID` is the frame of the last refresh (game frame + 4). Lighting.hlsl's `SKINNED` path reads the same data as `Bones[]` relative to `BonesPivot`.
