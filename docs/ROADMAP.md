# Roadmap

Each milestone lists its **acceptance criteria** and a **kickoff prompt**; paste the prompt into Claude Code to start that milestone. Only start a milestone after the previous one's criteria pass. Update `Status` as you go.

| # | Milestone | Status |
|---|---|---|
| M0 | Environment + codebase exploration | ✅ 2026-09-23 |
| M1 | RT feature skeleton + DXR capability check | ✅ 2026-09-23 |
| M2 | D3D12 sidecar + interop proof | ✅ 2026-09-23 (16-min run, 52,160 round trips, 0 skipped, no device removal, 0.30 ms avg) |
| M3 | Scene extraction + stats dump | ✅ 2026-09-23 (5,428 uploads, 0 failed, stride verified 5,428/5,428; see ARCHITECTURE §3) |
| M4 | BLAS/TLAS + traced debug view (depth match) | ✅ 2026-09-23 (mismatch 0.02–0.14% in Whiterun, Bleak Falls Barrow, Solitude; see ARCHITECTURE §4) |
| M5 | RT sun shadows | ✅ 2026-09-23 (98% agreement with the game's shadow mask at Whiterun; round trip 1.0–1.2 ms at 1280×720 on RTX 4080 SUPER, shadow passes 0.13–0.29 ms; 1440p / 3070-class cost not measured; see ARCHITECTURE §5) |
| M6 | 1-bounce diffuse GI + NRD | ✅ 2026-09-24 (GI hand-off 0.70 ms, passes 0.42–0.51 ms at 1280×720 on RTX 4080 SUPER; frame time unchanged within noise; NRD in private builds only, see CLAUDE.md and ARCHITECTURE §5) |
| M7 | Actors (skinning) + alpha-tested geometry | ✅ 2026-09-24. M7a skinned actors + M7b FaceGen heads (depth mismatch incl. actors 0.001–0.03%). M7c foliage alpha test, trees and hair included (forest shadow agreement 68% → 85–91%; depth mismatch 0.4–1.1% in the forest with wind sway bucketed; alpha-tested hits 0.3–0.7%). Trees are traced in their rest pose: every culling camera (view, shadow cascades, Skylighting) re-poses their swaying bones, which made trunks jump and flicker (up to 12.8% mismatch, a dark GI band); rest pose: 0.87%, band gone. **Open:** a fireplace card in the TLAS but not in the depth (interior, 2%). Interiors now use RT GI too, since the point-light bounce landed (M8, 2026-09-24). See ARCHITECTURE §4 "M7". |
| M8 | Many lights (ReSTIR DI), reflections, LOD, multi-bounce path tracing | ☐ |

---

## M0 — Environment + exploration

**Accept when:**
- Jake confirms the unmodified CS fork builds, deploys and runs in game.
- The exact build commands are recorded in CLAUDE.md.
- `docs/CS_NOTES.md` exists and covers:
  - the feature registration API and lifecycle hooks;
  - how CS gets the D3D11 device and context;
  - where the lighting shaders are hooked;
  - how the screen-space shadows and screen-space GI features produce and consume their outputs (file names and functions);
  - how shader defines per feature work;
  - where the depth and normal buffers are obtained;
  - the pinned CommonLib version.

**Kickoff prompt:**
> Read CLAUDE.md and docs/ARCHITECTURE.md. This repo is a fork of Community Shaders. Don't write feature code yet. First, get the build working and record the exact commands in CLAUDE.md. Then explore the codebase and write docs/CS_NOTES.md covering the items listed under M0 in docs/ROADMAP.md, with file paths and function names. While exploring, check every CommonLib type named in ARCHITECTURE.md against the headers and correct the doc where it's wrong.

## M1 — Feature skeleton

**Accept when:**
- A new CS feature, `SkyrimRT`, appears in the CS menu with an enable toggle.
- On load, it logs the adapter name, LUID and DXR tier.
- It disables itself gracefully when the tier is below 1.1.
- There are no other behavior changes.

**Kickoff prompt:**
> Implement M1 from docs/ROADMAP.md, following the conventions in docs/CS_NOTES.md. Put the D3D12 code under src/RT/ behind a small interface, as described in CLAUDE.md. When it builds, give me exact test steps, then read the SKSE log after I run them.

## M2 — Sidecar + interop proof

**Accept when:**
- The D3D12 device is created on the same adapter as the game.
- The shared fence works in both directions.
- Every frame, a D3D12 compute shader writes an animated test pattern into a shared texture, and D3D11 composites it into a screen corner.
- The frame-time cost logged in the JSON dump is under 0.5 ms.
- A 10-minute play session produces no device-removed errors.
- The ⚠ shared-buffer spike from ARCHITECTURE §2 is resolved and the doc updated.

**Kickoff prompt:**
> Implement M2. Also do the shared-buffer spike from ARCHITECTURE §2 and record the result in the doc. Add the debug-dump hotkey that writes frame_<n>.json and a PNG of the shared texture to Documents/My Games/Skyrim Special Edition/SKSE/SkyrimRT/, then verify from those files after my test run. Enable the D3D12 debug layer and DRED in debug builds.

## M3 — Scene extraction

**Accept when:**
- The dump JSON lists:
  - geometry counts by type (per the ARCHITECTURE §3 table);
  - the vertex formats and strides seen;
  - total VB/IB bytes;
  - mesh-cache size, uploads and evictions per frame.
- Numbers are plausible and stable across three test locations: Whiterun exterior, a dungeon interior, and Solitude.
- Walking between cells shows evictions happening, with no crashes.

**Kickoff prompt:**
> Implement M3: static non-skinned geometry and terrain only, with the mesh cache and budgeted uploads into D3D12 buffers. Buffers can't be shared between the devices (M2 spike, ARCHITECTURE §2), so upload from `TriShape::rawVertexData`/`rawIndexData` when present, else from a D3D11 staging readback polled without CPU waits; report how often each path is used. Decode the vertex format from BSGraphics::VertexDesc, verifying the bits in the headers. No BLAS yet. Give me a test route through three locations and check the dumps.

## M4 — Acceleration structures + debug view

**Accept when:**
- The BLAS and camera-relative TLAS are built as in ARCHITECTURE §4.
- A RayQuery compute pass renders traced depth, InstanceID and normal debug views, selectable in the menu.
- The dump includes the fraction of pixels where traced depth differs from raster depth by more than 1% (over static-geometry pixels, excluding actors/foliage/sky).
- Target: **under 2%** in all three test locations.
- TLAS build time and trace time are logged.

**Kickoff prompt:**
> Implement M4. The depth-mismatch metric is our correctness test. If it fails, diagnose from the dumped PNGs (write a diff image) before changing anything. Common causes are transform convention (row- vs column-major, scale), camera-relative offset, or vertex format decoding.

## M5 — RT sun shadows

**Accept when:**
- The RT shadow mask replaces CS's screen-space shadow input in the lighting shader, behind a toggle.
- Soft shadows from cone jitter, plus temporal and spatial denoise.
- A dump PNG pair (RT on/off) at a fixed spot and time of day.
- Cost is logged; the target is under 2 ms at 1440p on an RTX 3070-class GPU.

## M6 — Diffuse GI

**Accept when:**
- 1-bounce GI (using the average-albedo material table) is denoised with NRD and feeds the ambient term through the same path as CS's screen-space GI.
- Toggleable, with timings logged.

## M7 — Actors + alpha

**Accept when:**
- Skinned meshes get compute skinning plus a BLAS refit each frame. (Buffers can't be shared, so skinning runs on the D3D12 side from uploaded bind-pose data and bone matrices, or bone data travels through a shared texture; decide in M7.)
- `BSDynamicTriShape` is updated.
- Foliage alpha-tests using the v2 texture path, for foliage only.
- The depth-mismatch metric now includes actors and still stays under 2%.

## M8 — Backlog

- ReSTIR DI for point lights. **Phase 1 built 2026-09-24. Works in the Riverwood Trader; in Bleak Falls 32–44 of 35–47 lights are portal-strict; per-instance rooms now trace them where Lighting.hlsl applies them (run 2: 62–77% of pixels lit by traced lights, crisp root/pillar shadows). Combat GPU hangs (skinned BLAS builds, per DRED) froze the game until the sidecar got its own D3D12 device (run 3: hang survived). Likely cause found in run 4: combat leaves skinned partitions listed twice, which raced two BLAS builds in one list; guarded, no hang since.** Planned 2026-09-24 (ARCHITECTURE §5 "M8 ray-traced point-light shadows"). Light Limit Fix already removed the per-object light limit; what's missing is visibility for the unshadowed lights. RTXDI shares NRD's licence, so it's our own ReSTIR in HLSL, which stays in the GPL build. Phase 1 = ray-traced shadows for unshadowed, non-portal-strict point lights via a ratio mask at PS t46. Accept phase 1 when: in the Riverwood Trader and a dungeon, candle/torch light no longer reaches surfaces behind walls and counters (final RT on/off pair), directly lit surfaces don't darken, counters are plausible, the Prepass hand-off grows by under 0.3 ms at 1280×720, and the mask has a toggle. Phase 2 (reservoir reuse) only if many-light scenes need it.
- ✅ Point lights in the GI bounce (2026-09-24): Light Limit Fix's light list, one sampled light + one visibility ray per bounce hit (ARCHITECTURE §5). Riverwood Trader: 89.5% of bounce hits sample a light, 54% occluded (mostly hearth masonry and walls; under 2.5% by a light's own fixture), GI hand-off 1.33–1.42 ms. "Ray-traced GI in interiors" is now on by default. Not yet measured: exteriors or cities with many particle lights (the light loop is O(lights) per hit), and dungeons.
- Wind sway (`TREE_ANIM`) replicated on D3D12, so foliage matches the raster exactly.
- Trees as static BLASes (they're traced in rest pose, so per-frame skinning and refit are wasted work). **✅ 2026-09-24** (ARCHITECTURE §4 "Static trees"): depth mismatch and shadow agreement unchanged in three runs, the tree BLASes are built once, and the setting can be switched live. The saving is small because the measured forests had only 10–12 skinned trees (20–24 partitions, about 0.01 ms); the CPU-submit drop is within noise. Accept when, in a forest with the setting on vs off: the depth mismatch and shadow agreement are unchanged (within noise of 0.87% / 85–91%), skinned partitions and `skin_and_blas` time drop by roughly the trees' share, static tree BLASes stay built (no per-frame rebuilds), and CPU submit time drops.
- Reflections.
- Distant LOD in the TLAS.
- Water.
- ✅ Ray-traced sky light (2026-09-24, on by default; path-tracing step 1): GI misses that reach the sky carry its radiance, and the composite scales the game's ambient by the traced / open-sky ratio (ARCHITECTURE §5). Market: sunlit open surfaces −1 to −6%, under awnings −13 to −17%; GI hand-off +0.03 ms. Found on the way: a ratio is needed rather than a replacement, because the game adds its ambient to direct light in gamma space; and decals must stay out of the traces (Whiterun gate).
- Multi-bounce path tracing.
- One-frame-late async RT.
- The v2 texture path for all materials. **✅ Albedo at GI hits (2026-09-24, path-tracing step 2)** (ARCHITECTURE §6: `TextureAtlas`, `AlbedoAtlas`, texture × vertex colour at the hit): 100% of GI hits textured in the market, GI trace +0.02–0.04 ms, no evictions. The visual effect on diffuse bounce is small (frame −2%, local −6 to −15% from vertex colours; no visible tint), as expected; it's the base for reflections and multi-bounce. Still open: terrain layer blending, normal maps and specular at hits.
- True PBR material support.
