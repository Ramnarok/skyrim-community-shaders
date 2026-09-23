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
| M6 | 1-bounce diffuse GI + NRD | ☐ |
| M7 | Actors (skinning) + alpha-tested geometry | ☐ |
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

- ReSTIR DI for point lights (RTXDI), removing the per-object light limit.
- Reflections.
- Distant LOD in the TLAS.
- Water.
- Multi-bounce path tracing.
- One-frame-late async RT.
- The v2 texture path for all materials.
- True PBR material support.
