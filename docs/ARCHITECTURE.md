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

**As built in M5:** the first sync point is implemented. `SkyrimRT::Prepass()` → `Sidecar::Submit` does, in order: scene walk, `CopyInputs` (depth → shared R32, plus `kSHADOW_MASK` on dump frames), then `Signal`. D3D12 then runs: test pattern, BLAS builds, TLAS, the optional M4 debug trace, and the sun-shadow trace/temporal/spatial passes, then `Signal` back. D3D11 `Wait`s, and the mask is bound at t45. Mesh uploads go in a second command list after that signal, so they don't lengthen the wait. Everything D3D12 does for the frame sits inside this single round trip, whose D3D11 GPU-timeline length (`timings_ms.d3d11_round_trip`) is the frame-time cost.

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

**Shared textures, measured (M2).** Creating in D3D12 with `D3D12_HEAP_FLAG_SHARED` and opening in D3D11 with `OpenSharedResource1` **fails with `E_INVALIDARG`** on Jake's RTX 4080 SUPER. Creating in D3D11 with `MISC_SHARED | MISC_SHARED_NTHANDLE`, then `IDXGIResource1::CreateSharedHandle` → `ID3D12Device::OpenSharedHandle`, works (as in CS's `WrappedResource`). **All shared textures are created on the D3D11 side.** The cause of the D3D12→D3D11 failure wasn't investigated (maybe resource flags); it's not needed.

**Shared buffers (geometry), measured (M2 spike, resolved):**
- D3D12 shared-heap buffer → `ID3D11Device1::OpenSharedResource1` as `ID3D11Buffer`: **`E_INVALIDARG`**.
- D3D11 buffer with `MISC_SHARED | MISC_SHARED_NTHANDLE`: **`CreateBuffer` itself fails with `E_INVALIDARG`**.
- ⇒ **Buffers can't be shared in either direction.** Geometry must reach D3D12 through a CPU upload, done once per mesh:
  1. Preferred: `BSGraphics::TriShape::rawVertexData` / `rawIndexData` (CPU copies) → D3D12 upload heap → default buffer. M3 must count how often these are non-null.
  2. Otherwise: D3D11 `CopyResource` into a staging buffer, polled with a D3D11 query (no CPU wait on the frame path), then `Map` → D3D12 upload.
  3. (Not planned) A compute pass copying buffer → shared texture → D3D12 copy back to a buffer.

M2 measurements (RTX 4080 SUPER, ~57 fps, 13,207 frames): the D3D11→D3D12→D3D11 fence round trip costs **0.30 ms average / 0.75 ms max** on the D3D11 GPU timeline. The D3D12 dispatch itself is 0.005 ms, so the cost is almost entirely the queue handoff; that argues for batching all RT work into as few sync points as possible (§1). CPU submit is 0.2–0.27 ms, likely dominated by the `ID3D11DeviceContext::Flush()` after the signal. That's a candidate to remove later.

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
- Vertices are **interleaved and packed**. Position is at offset 0.
  - ⚠ **Measured in M3 (5,428 meshes, 3 locations): `VF_FULLPREC` (0x400) was never set, yet every stride only adds up with a 16-byte position** (e.g. flags 0x3b = VERTEX|UV|NORMAL|TANGENT|COLORS → 32 = 16 + 4 + 4 + 4 + 4). So the runtime layout is `float3` position + one float (bitangent X), not `half4`. Treat positions as `R32G32B32_FLOAT` and confirm with the M4 depth-match test. The original "half4 unless VF_FULLPREC" assumption (nifskope's on-disk layout) does not hold for the uploaded buffers.
  - Formats seen: 0x3b (stride 32, 88% of meshes), 0x1b VERTEX|UV|NORMAL|TANGENT (28), 0xbb … |LANDDATA (40, all terrain).
- DXR accepts the matching BLAS vertex format directly:
  - `R32G32B32_FLOAT` or `R16G16B16A16_FLOAT` (w ignored).
  - Set the stride to the full vertex size.
- So we can build the BLAS from the copied interleaved buffer without repacking. Flags sit at bits 44+ (`VertexDesc::HasFlag`); per-attribute offsets come from `GetAttributeOffset(attr)`.
- ✅ **Stride = `(desc & 0xF) * 4`**, verified in M3: it matched the game's VB `ByteWidth / vertexCount` for 5,428/5,428 meshes. **Never use `VertexDesc::GetSize()`**: it ignores `VF_LANDDATA` and reports 32 for terrain, whose real stride is 40.
- ✅ Indices are 16-bit, `triangleCount * 3 * 2` bytes; matched the IB `ByteWidth` for 5,428/5,428 meshes.
- Indices are 16-bit.

**Mesh cache:**
- Key: `rendererData` pointer (or the VB pointer).
- Value: the D3D12 buffer copies, BLAS, geometry-table index and material info.
- ✅ Upload source (M3): `TriShape::rawVertexData`/`rawIndexData` were both present for 93% of new meshes (5,044) and **byte-identical to the GPU buffers in 16/16 sampled meshes**, with 0 access faults. Terrain (384 meshes) keeps `rawVertexData` but not `rawIndexData`, so it goes through the D3D11 staging readback. Key = `TriShape*` + VB pointer + desc + counts, which guards against address reuse.
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

### M4 results and lessons (2026-09-23)

- ✅ Transform convention: `NiTransform` is `rotate * p * scale + translate` with `rotate.entry[row][col]` (CommonLib `NiMatrix3::operator*`), so `D3D12_RAYTRACING_INSTANCE_DESC::Transform[r][c] = entry[r][c] * scale`, `Transform[r][3] = translate[r] - CameraPosAdjust[r]`. Verified: static geometry matches raster depth to 0.001–0.14% in three locations.
- ✅ Rays: unproject the render-region pixel centre with `FrameBuffer::CameraViewProjInverse` (jittered, `row_major`, `mul(M, v)`, exactly as CS's `FrameBuffer.hlsli`) at depth 0 and 1. Render size = `Util::ConvertToDynamic(screen)`, captured with the matrices in `Feature::Prepass()`.
- ✅ Raster depth to compare against: `Util::GetCurrentSceneDepthSRV(false)` at Present time (TerrainBlending's R32 depth when active), copied into a D3D11-created shared R32 texture.
- **Never walk the scene graph while a loading screen is up.** Present keeps firing while cells are torn down; the first M4 run crashed on a garbage child pointer after `coc`. `CollectScene` skips frames with `LoadingMenu`/`MainMenu` open and runs under an SEH guard.
- **Grass is not under the cells' 3D.** It lives under `BGSGrassManager::grassNode`; the manager pointer is captured from CS's `GrassOptimizations::LoadGrassType` hook, not `BGSGrassManager::GetSingleton()`, since that CommonLib ID has no 1.7.99-specific entry and a missing Address Library ID is fatal. The grass shapes' bounds did **not** cover the drawn grass, so grass is identified geometrically instead (below).
- **Alpha-tested meshes are ~20% of static instances** and their bounds often cover whole rooms. They stay in the TLAS (opaque until M7) on mask 0x08; mismatches where they're hit are attributed to alpha, not to geometry errors. **Alpha-blended** meshes (glass, light rays) are drawn after the depth copy, so they sit on mask 0x20, outside the depth trace.
- **Depth-mismatch metric definition** (`RayQueryDebugCS.hlsl`): relative distance difference > 1%. Pixels are counted unless sky, outside the loaded cells, or a *mismatch* that is (a) an alpha-tested hit, (b) raster-nearer inside the bounds of skinned/dynamic/LOD geometry, or (c) raster-nearer within 150 units above the terrain directly below (grass / ground clutter; this could also hide a missing object under 150 units, so it's counted separately). Matches are always counted. Coverage (counted / non-sky) was 45–95%.
- Costs at 1280×720 (RTX 4080 SUPER): BLAS builds 0.15–0.29 ms (steady state; bursts on cell load), exclusion BLAS + TLAS 0.23–0.29 ms, trace + counters 0.23–0.50 ms. The whole M4 frame runs inside the M2 D3D11↔D3D12 round trip.

### M7 as built (2026-09-24)

- **M7a, skinned actors** (`src/RT/SkinnedMeshes.{h,cpp}`, `src/RT/Shaders/SkinCS.hlsl`):
  - The scene walk (`Scene.cpp` `CollectSkinned`) emits one candidate per `NiSkinPartition` partition. Its `rendererData` is `partition.buffData`, and the vertex count comes from that buffer's D3D11 `ByteWidth / stride`.
  - The bone palette (`*skin->boneWorldTransforms[bone] * skinData->GetBoneDataSkinToBone(bone)`, one row-major 3×4 per palette bone) is computed during the walk.
  - The mesh cache uploads bind-pose VB/IB as usual but skips BLAS/TLAS for `skinned` entries (`FindResident` exposes their location).
  - Per frame, per (skinInstance, partition): `SkinCS` does linear-blend skinning (4 half weights + 4 byte indices at `GetAttributeOffset(VA_SKINNING)`, partition-local palette indices) into float3 camera-relative positions in `outputPool`.
  - The BLAS is built with `ALLOW_UPDATE | PREFER_FAST_BUILD`, refit every frame, and fully rebuilt every 60 frames. The TLAS instance uses an identity transform with mask 0x04; instance flag 8 = actor.
  - Skinned output pages occupy mesh-page descriptor slots 48–63 (the mesh pool uses 0–47) in both the Raytracer and GI heaps, so `GeometricNormal` works on actors.
  - Actors now cast RT sun shadows, occlude and bounce GI, and count in the M4 depth metric.
  - Measured in the Whiterun market: 800–890 skinned shapes, 1,650–1,810 partitions, ~830k vertices, 0 rejected, 0 failed. Skinning + refit costs 0.16–0.19 ms GPU. Depth mismatch including actors: 0.001–0.03%. Positions were float3 (no half-position partitions seen).
- **M7b, `BSDynamicTriShape` (FaceGen heads):** these are skinned too. `dynamicData` (the CPU-side morphed positions, `dataSize / vertexCount` stride) overrides the bind-pose positions in `SkinCS`. It's re-uploaded only when `DYNAMIC_TRISHAPE_RUNTIME_DATA::frameCount` changes (8 MB/frame budget). Measured: 49 heads, 0 rejected; pixels excluded as "occluder not in TLAS" dropped to 0; no re-uploads while standing (frameCount stable).
- **Hand-off fix:** both hand-offs now record the D3D12 list **before** D3D11 signals. Recording after the signal left the D3D11 GPU waiting on CPU recording; with M7's ~3,600 commands that measured +1.1 ms. After the fix the pre-opaque hand-off is back to 0.92 ms (shadows, skinning, BLAS/TLAS all included) and GI to 0.79 ms.
- **Open:** CPU submit is now ~3.0 ms per frame (scene walk + palettes); candidate for walking every N frames or caching per-reference. **M7c** (alpha-tested foliage via a D3D11-decoded alpha atlas + non-opaque geometry in RayQuery) is next.

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

**M5 as built** (`src/RT/SunShadows.cpp`, shaders `src/RT/Shaders/SunShadow*.hlsl`):
- **Trace:** one ray per render pixel. The normal is reconstructed from depth by picking, per axis, the neighbour nearer to the centre point, then oriented towards the viewer. The origin is `P + N * (normalBias + distance * distanceBias)`, with defaults of 1 unit + 0.2% of distance. The direction is jittered inside the sun's cone (`SunAngularRadius`, default 0.5°) by a concentric-disk sample: a per-pixel hash plus the R2 sequence over frames. Flags: `FORCE_OPAQUE | SKIP_PROCEDURAL | ACCEPT_FIRST_HIT_AND_END_SEARCH`, no culling. The caster mask is static | terrain, plus alpha-tested when enabled (opaque until M7; off by default because leaves would cast solid quads). Rays run to 50,000 units, beyond which there's no geometry in the loaded cells. Output: R8 raw visibility and RGBA16F normals.
- **Temporal:** each pixel is reprojected with the previous traced frame's jittered `CameraViewProj` and the `CameraPosAdjust` delta. The 2×2 bilinear taps are accepted when their stored view depth (`clip.w`) matches within 2% / max(N·V, 0.1). The accumulation is `lerp(history, raw, 1/len)` with `len` capped at `ShadowHistory` (default 24). History is RGBA16F (mean, length, view depth), ping-ponged, and invalidated after any untraced frame.
- **Spatial:** a 12-tap rotated Poisson disk of radius `ShadowSpatialRadius × lerp(1, 0.35, len/max)` pixels (default 3). Weights: plane distance (tolerance 1% of distance + 1 unit) × normal^8 × Gaussian. It writes the D3D11-created shared R8 mask (1 = lit) and an optional RGBA debug view.
- If the mask wasn't traced this frame or the previous one, it's cleared to 1 on D3D11 before being bound, so a stale mask is never used.

**M5 results (2026-09-23, RTX 4080 SUPER, 1280×720 internal, DLSS to 1080p; Whiterun exterior → Dragonsreach → back out, 6 dumps):**
- **Correctness vs the game's shadow mask** (raw RT visibility vs `kSHADOW_MASK < 0.5`, pixels within 3,000 units): 98.0% agreement on the sunset courtyard frame (921,600 pixels, 88% shadowed), with matching silhouettes. That confirms the sun-direction sign, the transforms and the bias. In back-lit forest frames agreement was 76–82%. Almost all of the gap is "map-only" shadow from casters the TLAS doesn't have yet: the player, and bushes and conifers (alpha-tested, excluded from casters by default). Of the pixels RT shadows, 51–99% are also shadowed in the map. The rest are surfaces facing away from the sun, which RT self-shadows and the biased shadow map doesn't; the player's back-mounted weapons are one example (they're rigid shapes, so they're in the TLAS). N·L already darkens these, so they barely change the final image.
- **RT on vs off, final frame:** mean |ΔLuma| 0.6–1.0 / 255; 2–3.5% of pixels darker by more than 2 levels, 1–5.5% brighter. Most of the brighter pixels are Screen-Space Shadows' contact darkening on foliage and character outlines, which goes away with RT on.
- **Pre-pass depth confirmed:** the M4 depth trace, now run at `Prepass()` against the pre-pass depth, matches 334,296 of 334,298 counted pixels (0.0006%; coverage 38.6% in dense foliage).
- **Cost:** the whole round trip (the frame-time cost: BLAS/exclusion 0.16 + TLAS 0.22 + shadow passes + handoff) is **0.97–1.19 ms** with the debug trace off. The shadow passes alone are 0.13 ms at midday and 0.28 ms at a low sun (trace 0.07 → 0.21 ms, since low rays cross more of the BVH; temporal 0.02, spatial 0.05). CPU: 1.3 ms per frame, mostly the scene walk. The ROADMAP target (under 2 ms at 1440p on a 3070-class GPU) wasn't measured. Scaling the per-pixel passes by 4× pixels and ~2.5× GPU suggests roughly 1.5–3 ms, so it may need the one-frame-late overlap from §1 or a half-resolution trace.
- 0 skipped frames, no device removal. History resets happen only at dump suppressions, interior transitions and loads.

**M6 diffuse GI (1 bounce):**
- Use cosine-weighted rays from the G-buffer.
- At each hit, fetch the hit's material (see §6) and evaluate the sun (with a shadow ray) plus sky.
- Denoise with NVIDIA NRD (ReBLUR/ReLAX).
- **Integration point:** ✅ confirmed (CS_NOTES "Screen-Space GI"). `ScreenSpaceGI::DrawSSGI()` runs in `Deferred::DeferredPasses()`, and `DeferredCompositeCS.hlsl` reads its four outputs at t10–t13: AO, luminance as SH (`float4`), CoCg chroma, HQ specular. RT GI can write the same four textures. The composite's defines are a hand-written list in `Deferred::GetComputeMainComposite[Interior]()`, so a new define must be added to both.

**M6 as built** (`src/RT/GlobalIllumination.cpp`, `src/RT/NrdDenoiser.cpp`, shaders `src/RT/Shaders/NRD/GI*.hlsl`; only compiled with `SKYRIMRT_NRD`):
- **Second hand-off:** `Deferred::DeferredPasses` calls `SkyrimRT::DrawGlobalIllumination` in place of `ScreenSpaceGI::DrawSSGI`. D3D11 copies the G-buffer normals (`NORMALROUGHNESS`) and `kMOTION_VECTOR` into shared textures, then signals. D3D12 runs trace → REBLUR → resolve, reusing the TLAS, the instance data (same frame slot) and the Prepass depth copy, then signals back. D3D11 waits, and the composite reads our AO / Y / CoCg at t10–t12. That needs the Screen Space GI feature loaded, since the composite's `SSGI` define depends on it.
- **Trace:** one cosine-weighted ray per pixel around the G-buffer normal, kept above the depth-reconstructed geometric plane; 3,000 units by default; static + terrain casters. At a hit, the radiance follows the rule `kMAIN` (which SSGI gathers) is lit by: `SkyrimGammaToLinear(albedo × (DirLightColor × N·L × sunVisibility + DALC ambient SH(N)))`. The sun visibility is a second ray, up to 50,000 units. Misses carry 0: sky light stays the game's ambient, multiplied by AO. Linear Lighting uses an approximate conversion (untested; it's off by default and off on Jake's install).
- **NRD REBLUR_DIFFUSE**, driven through raw D3D12 via `nrd::GetComputeDispatches`, no NRI. It uses one root signature (root CBV, static samplers, a table with an SRV range then a UAV range), NRD's texture pools, a per-frame-slot descriptor and constant ring, and tracked resource states. Inputs: viewZ = `(CameraView·P).z`, octahedral R10G10B10A2 normals with roughness 1, and the game's motion vectors, which are already NRD's `prevUV − currUV`. Matrices: `CameraProjUnjittered` / `CameraView`, converted to column-major, with the previous view matrix re-based by the `CameraPosAdjust` delta. `hitDistanceParameters.A` = 3 m = 210 units. Camera jitter is passed as 0 (not yet derived).
- **Resolve:** REBLUR's output is the cosine-weighted mean incoming radiance at the pixel normal. Y goes into SH L0 as `luma / 0.886` (the composite's cosine-lobe L0 weight), plus a 1% L1 term along N, because an all-zero L1 would make `SHHallucinateZH3Irradiance` normalize a zero vector. CoCg is written directly. AO = 1 − REBLUR's normalized hit distance.

**M6 results (2026-09-24, RTX 4080 SUPER, 1280×720 internal; Whiterun forest, 6 dumps):**
- **Output:** the AO and denoised bounce images are plausible and stable: contact darkening in crevices, under ledges and around the feet, and bounce brightest next to sunlit rock. 27–38% of bounce rays hit (3,000-unit rays) and 70–86% of hits are sunlit. The material table averaged 160 textures, with 0 unsupported and 0 candidates left on the default.
- **Final frame, RT on vs off** (SSS + SSGI in the off frame): mean |ΔLuma| 1.7–2.4 / 255; 13–22% of pixels brighter by more than 2 levels (shaded rock and the player lit by ground bounce), 5–8% darker (RT AO is stronger than SSGI's in crevices).
- **Cost:** GI passes 0.42–0.51 ms on D3D12 (trace 0.15–0.25, REBLUR 0.30 in 7 dispatches, resolve 0.01). The D3D11 GI hand-off is **0.70 ms** (max 0.81). The first measurement read 3–11 ms: without a `Flush()` after the D3D11 `Wait`, the closing timestamp sat in the command buffer until the game's next flush, so the span included idle time of a CPU-bound frame. Present-to-present frame time was 17.36 ms with GI on and 17.25–17.50 ms off, identical within noise at this frame-limited ~57 fps. Total RT GPU cost per frame is therefore about 0.9 ms (Prepass hand-off, including shadows) + 0.7 ms (GI hand-off).
- Camera jitter is still passed to NRD as 0, and Linear Lighting is untested.

## 6. Materials & textures — the hard problem

Game textures are D3D11 resources we can't read from D3D12.

- **Primary surfaces:** use the G-buffer. It's already correct.
- **Secondary hits** (GI bounce, reflections):
  - v1: a per-mesh average albedo, computed once at load by sampling a low mip on the D3D11 side and stored in the material table.
    **As built (M6, `src/RT/MaterialTable.cpp`):** keyed by the diffuse texture's SRV, re-validated against its resource and size once per frame. Terrain uses its first landscape layer only. At most 64 new textures per frame go through `AverageAlbedoCS` (a 16×16 grid read from the mip nearest 16×16), read back polled with `DO_NOT_WAIT`. Values are stored as the texture holds them (Skyrim gamma). Alpha-tested meshes use the alpha-weighted average. Until known, mid grey (0x73). The value goes into `InstanceGpu::albedo`.
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
