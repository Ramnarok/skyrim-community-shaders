# Architecture

Status: design draft. In M0 the CommonLib names were checked against the pinned headers (CommonLibSSE-NG v6.7.1); ✅ = verified, with file references in `docs/CS_NOTES.md`. Lines marked ⚠ are still-open uncertainties that need a spike. New CommonLib names added here must be verified the same way.

## 1. Frame overview

**Correction (M0, see `docs/CS_NOTES.md` "Frame order").** CS's `Lighting.hlsl` computes direct lighting, *including the sun shadow term*, in the same pass that writes the G-buffer. Only ambient/GI/reflections are added afterwards, in `DeferredCompositeCS`. So the RT sun-shadow mask must exist **before** the opaque pass, when only the depth pre-pass is available. RT GI runs after the full G-buffer, as originally assumed. That means two sync points:

```
D3D11 (game immediate context)                    D3D12 sidecar (our queue)
────────────────────────────────                  ──────────────────────────────
Shadow maps → Feature::EarlyPrepass
Copy new mesh VB/IB → shared buffers (budgeted)
Depth pre-pass (game)
StartDeferred → Feature::Prepass:
  copy pre-pass depth → shared R32 texture
  ctx4->Signal(fence, N)  ─────────────────────►  queue->Wait(fence, N)
                                                  Build/refit BLAS (budgeted)
                                                  Rebuild TLAS (camera-relative)
                                                  RayQuery: sun shadows from depth only
                                                  Denoise shadow mask
  ctx4->Wait(fence, N+1)  ◄────────────────────  queue->Signal(fence, N+1)
  bind RT shadow mask at PS t45 (SSS slot)
Opaque pass: Lighting.hlsl (direct light × shadow, writes G-buffer)
EndDeferred → DeferredPasses:
  copy normals/albedo → shared textures
  ctx4->Signal(fence, N+2) ────────────────────►  queue->Wait(fence, N+2)
                                                  RayQuery: 1-bounce GI (TLAS reused)
                                                  Denoise (NRD)
  ctx4->Wait(fence, N+3)  ◄────────────────────  queue->Signal(fence, N+3)
  DeferredCompositeCS reads RT GI at t10–t13 (SSGI slots)
Water / transparents / post / UI / Present
```

v1 serializes the GPU at both sync points. Once it works, consider running the RT work one frame behind, using reprojection, to overlap the two queues. For shadows that also means we could trace from last frame's full G-buffer (with normals) instead of depth only.

## 2. D3D12 sidecar & interop

**Getting the game's device.** ✅ Verified (M0): CS already does this in `globals::ReInit()` and exposes `globals::d3d::device` / `context` / `swapChain`. Underneath, `GetRuntimeData().forwarder` is the `ID3D11Device`, `.context` is the immediate context, and the swapchain is `.renderWindows[0].swapChain`. Use the globals and create the sidecar in `SetupResources()`; by then the device and all render targets exist.

**Creating the D3D12 device:**
- Query `IDXGIDevice → IDXGIAdapter` from the D3D11 device and read its LUID.
- Create the D3D12 device on the adapter with the same LUID. Require `D3D12_RAYTRACING_TIER_1_1`; otherwise disable the feature cleanly.
- Create one direct command queue, plus later an optional compute queue.

**Shared fence:**
- `ID3D12Device::CreateFence(..., D3D12_FENCE_FLAG_SHARED)` → `CreateSharedHandle`.
- On the D3D11 side: `ID3D11Device5::OpenSharedFence`.
- Signal and wait via `ID3D11DeviceContext4::Signal/Wait` and `ID3D12CommandQueue::Signal/Wait`.

**Shared textures (depth copy, normals, RT outputs):**
- Create them in D3D12 with `D3D12_HEAP_FLAG_SHARED` and `CreateSharedHandle`.
- Open them in D3D11 with `ID3D11Device1::OpenSharedResource1`.
- Depth can't be shared as a depth-stencil. Copy it to an `R32_FLOAT` texture first, with a small D3D11 pass or `CopyResource` from a typeless view.

**Shared buffers (geometry):**
- ⚠ Spike: confirm that a D3D12 buffer created with a shared heap opens in D3D11 via `OpenSharedResource1`.
- Fallback: copy through a shared texture, or upload from CPU readback of a staging buffer, done once per mesh.

**Precedent:** CS itself already does D3D11/D3D12 interop in `src/Features/Upscaling/DX12SwapChain.{h,cpp}` (frame generation). It uses a same-adapter D3D12 device, a shared fence opened via `ID3D11Device5::OpenSharedFence`, and `WrappedResource` textures. Note that CS **creates shared textures in D3D11** (`MISC_SHARED | SHARED_NTHANDLE`) and opens them in D3D12, which is the reverse of the direction above; that direction is proven in this codebase. When frame generation is active a D3D12 device already exists, so decide whether to share it. Skyrim Upscaler and PIXL's sidecar are further references.

## 3. Scene extraction

**Traversal.** Walk the world scene graph each frame, or incrementally on attach/detach:
- `RE::Main::WorldRootNode()`, or the loaded cells via `RE::TES`.
- Use `RE::BSVisit::TraverseScenegraphGeometries(NiAVObject*, std::function<BSVisitControl(BSGeometry*)>)` (✅ `RE/B/BSVisit.h:19`) or a manual `NiNode` child walk.
- Skip culled/hidden objects (`NiAVObject` flags) only after we have off-screen coverage policy — RT needs off-screen geometry, so do **not** reuse the raster cull result.

**Geometry types (scope by milestone):**

| Type | What it is | Milestone |
|---|---|---|
| `BSTriShape`, static, no `skinInstance` | architecture, rocks, clutter | M3 |
| Landscape `BSTriShape` under cell 3D | terrain | M3 |
| Distant LOD (`BSLODTriShape` is **not** in CommonLib; LOD uses `BSMultiIndexTriShape`/`BSSubIndexTriShape`/`BSMultiStreamInstanceTriShape` — identify by RTTI in M8) | far objects | M8 |
| Skinned (`skinInstance` set) | actors, creatures | M7 (compute skinning + BLAS refit) |
| `BSDynamicTriShape` | faces, morphs (CPU-updated verts) | M7 |
| Alpha-tested (foliage, hair) | needs alpha test | M7 |
| Grass (instanced) | huge counts | backlog, maybe never |
| Water, particles, effect shaders | special | backlog |

**Reading mesh data:**
- ✅ `BSGeometry::GetGeometryRuntimeData().rendererData` is a `BSGraphics::TriShape*` (struct defined in `RE/N/NiSkinPartition.h`). It holds `vertexBuffer`, `indexBuffer` (`ID3D11Buffer*`), `vertexDesc`, **and `rawVertexData` / `rawIndexData` CPU pointers.** ⚠ If the game keeps the raw pointers alive, M3 can upload from CPU memory and skip copying game-owned D3D11 buffers entirely. Count non-null raw pointers in the M3 dump.
- ✅ Vertex and triangle counts: `BSTriShape::GetTrishapeRuntimeData().vertexCount` / `.triangleCount` (both `uint16_t`).
- Vertices are **interleaved and packed**. Position is at offset 0:
  - When `VF_FULLPREC` is set, it's `float3`.
  - Otherwise it's a half4.
- DXR accepts the matching BLAS vertex format directly:
  - `R32G32B32_FLOAT` or `R16G16B16A16_FLOAT` (w ignored).
  - Set the stride to the full vertex size.
- So we can build the BLAS from the copied interleaved buffer without repacking. Partly verified: `Vertex::VF_FULLPREC = 0x400`; flags sit at bits 44+ (`VertexDesc::HasFlag`); per-attribute offsets come from `GetAttributeOffset(attr)`. ⚠ **Don't use `VertexDesc::GetSize()` for the stride**: it always counts position as 16 bytes, even for half4 positions. Use `(desc & 0xF) * 4` (nifskope layout) and confirm against real buffers in M3.
- Indices are 16-bit.

**Mesh cache:**
- Key: `rendererData` pointer (or the VB pointer).
- Value: the D3D12 buffer copies, BLAS, geometry-table index and material info.
- ⚠ Lifetime: meshes unload with cells. Detect removal with a generation sweep: a mesh not seen for K frames is evicted after a fence confirms the GPU is idle on it. Never dereference a stale game pointer; key only, don't hold raw pointers across frames without validation.
- Upload budget: at most N MB and M BLAS builds per frame. The rest waits, which is acceptable because it streams.

**Transforms:**
- Read the world transform from `NiAVObject::world` (rotate 3×3, translate, scale). ✅ It's a direct member at 0x7C on all runtimes, and `previousWorld` (0xB0) is there too for motion and refit.
- Convert it to the 3×4 row-major matrix used by `D3D12_RAYTRACING_INSTANCE_DESC`.
- **Subtract `posAdjust` from the translation.** ✅ `posAdjust` lives on `RendererShadowState` runtime data, not the renderer; CS mirrors it as `globals::game::frameBufferCached.GetCameraPosAdjust()` / HLSL `FrameBuffer::CameraPosAdjust`. CS's G-buffer world positions are already relative to it (`Common/SharedData.hlsli:411`), so the TLAS must use exactly this origin, not the NiCamera position.

## 4. Acceleration structures

- **BLAS:** one per mesh. Flags: `PREFER_FAST_TRACE`, adding `ALLOW_COMPACTION` later. Geometry flag `OPAQUE`, except alpha-tested geometry in M7.
- **TLAS:** rebuilt every frame with `PREFER_FAST_BUILD`.
  - `InstanceID` indexes a per-instance data buffer (geometry-table index, material index, flags).
  - `InstanceMask` bits: 0x01 static, 0x02 terrain, 0x04 actor, 0x08 alpha-tested. Lets shaders choose what casts/receives.
- Use scratch buffers from a ring allocator, and keep the build count per frame bounded.

## 5. Tracing: inline RayQuery first

Start with **DXR 1.1 inline `RayQuery` in compute shaders**. It avoids state objects and shader binding tables, and it's a much smaller surface area to debug. Move to a full `DispatchRays` pipeline only if we need many hit shaders.

**M4 debug view:** trace from the camera, then output:
- hit `t` (converted to depth),
- `InstanceID` as a color,
- geometric normal.

Compare the traced depth to the raster depth.

**M5 RT sun shadows:**
- Reconstruct the camera-relative world position from depth, and read the normal.
- Offset along the normal and trace toward the sun direction, with a cone jitter for soft shadows.
- Output a single-channel mask, then denoise: temporal accumulation plus a small spatial filter.
- **Integration point:** ✅ confirmed (CS_NOTES "Screen-Space Shadows"). `ScreenSpaceShadows::Prepass()` writes an `R8_UNORM` mask (same size as `kSHADOW_MASK`) and binds it at **PS t45**; `Lighting.hlsl:2223` multiplies the sun's `dirDetailedShadow` by it under `SCREEN_SPACE_SHADOWS && DEFERRED`, exteriors only. Feed the RT mask through the same slot and path.
- ⚠ At `Prepass()` time only the depth pre-pass exists (no G-buffer normals). Reconstruct the normal for the ray-origin offset from depth derivatives, or trace one frame late from the previous G-buffer (see §1).

**M6 diffuse GI (1 bounce):**
- Use cosine-weighted rays from the G-buffer.
- At each hit, fetch the hit's material (see §6) and evaluate the sun (with a shadow ray) plus sky.
- Denoise with NVIDIA NRD (ReBLUR/ReLAX).
- **Integration point:** ✅ confirmed (CS_NOTES "Screen-Space GI"). `ScreenSpaceGI::DrawSSGI()` runs in `Deferred::DeferredPasses()`, and `DeferredCompositeCS.hlsl` reads its four outputs at t10–t13: AO, luminance as SH (`float4`), CoCg chroma, HQ specular. RT GI can write the same four textures. The composite's defines are a hand-written list in `Deferred::GetComputeMainComposite[Interior]()`, so a new define must be added to both.

## 6. Materials & textures — the hard problem

Game textures are D3D11 resources we can't read from D3D12.

- **Primary surfaces:** use the G-buffer. It's already correct.
- **Secondary hits** (GI bounce, reflections):
  - v1: a per-mesh average albedo, computed once at load by sampling a low mip on the D3D11 side and stored in the material table.
  - v2: copy low-res mips of each unique texture into a shared D3D12 texture array or a bindless heap, within a memory budget.
- **Alpha test (M7):** needs real alpha textures in D3D12, which means the v2 texture path, at least for foliage.
- **Material model:** Skyrim uses diffuse + normal + specular/envmap. Convert to PBR heuristically, or read CS "True PBR" material data when present.

## 7. Open questions / risks

- ⚠ D3D12→D3D11 shared *buffer* support; the fallback path is described in §2.
- Performance of a full TLAS rebuild in dense cities (thousands of instances). May need to split into a static TLAS plus dynamic instances.
- Interaction with CS features we replace: disable their screen-space shadows/GI when RT is active.
- ENB is incompatible; enforce at startup.
- Proton/vkd3d users: shared fences are known to be flaky there. Not a v1 concern.
- Licensing: CS is GPL-3.0 (with modding exceptions); our fork inherits that.
