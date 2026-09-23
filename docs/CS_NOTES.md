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
3. `src/Feature.cpp:222` — add `&globals::features::x` to the static list in `Feature::GetFeatureList()`. List order = call order for every lifecycle hook.
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
- [ ] Confirm in RenderDoc that `kPOST_ZPREPASS_COPY` at `Prepass()` time holds this frame's depth pre-pass, and which geometry the pre-pass skips (alpha-tested, first person?).
