# Skyrim RT — hybrid ray/path-traced lighting for Skyrim SE/AE

Read this file first, then `docs/ROADMAP.md` to find the current milestone, then `docs/ARCHITECTURE.md` for the design.

## Goal

Replace Skyrim's lighting with ray-traced (and eventually path-traced) lighting. The work is built as a **feature inside a fork of Community Shaders (CS)**, an SKSE plugin built on CommonLibSSE-NG.

The approach is **hybrid**:
- Skyrim still rasterizes the G-buffer.
- We trace rays *from* that G-buffer on a D3D12 "sidecar" device.
- We feed the results back into the game's D3D11 lighting shaders.

We do not trace primary visibility for final output.

## Non-goals (for now)

- VR support.
- Full replacement of the raster pipeline.
- Replacing ENB.
- Supporting GPUs without DXR 1.1 (inline `RayQuery`).

## Hard facts — do not "fix" these

- **Skyrim SE/AE renders with D3D11. DXR needs D3D12.** All ray tracing runs on a separate D3D12 device created on the same adapter (match the adapter LUID). The two devices share resources and fences. See ARCHITECTURE §2.
- **Skyrim has no lightmaps or baked lighting pass.** Lighting is computed per frame from three sources: the sun, a limited set of point lights, and a directional ambient term. We replace those terms.
- **Acceleration structures:** one BLAS per unique mesh, and one TLAS of instances rebuilt every frame. Never one BVH per cell.
- **Game-owned D3D11 buffers and textures are not shareable.** They must be copied into resources we create as shared.
  - Measured in M2 (2026-09-23, RTX 4080 SUPER): shared **textures must be created in D3D11** (`MISC_SHARED | MISC_SHARED_NTHANDLE`) and opened in D3D12. The reverse (D3D12 shared heap → `OpenSharedResource1`) fails with `E_INVALIDARG`.
  - **Buffers cannot be shared in either direction** (D3D11 rejects shared buffers with `E_INVALIDARG`; a D3D12 shared-heap buffer won't open in D3D11). Geometry reaches D3D12 via CPU upload instead (ARCHITECTURE §2).
- **World coordinates are large.** Build the TLAS camera-relative to avoid float precision loss.

## Rules for working in this codebase

1. **Verify every CommonLibSSE-NG type, member and function in the headers before using it.** Use the headers in the CommonLib submodule that CS pins. ARCHITECTURE.md names types from memory and may be wrong; if so, fix the doc.
2. **Never invent offsets, `REL::ID`s or `REL::RelocationID`s.** If a hook needs an address that isn't already in CS or CommonLib, stop and ask Jake.
3. **Keep all D3D12 code under `src/RT/`** (or the equivalent folder inside the CS feature). The CS side only sees a small interface: `RT::Init`, `RT::OnGBufferReady`, `RT::GetShadowMaskSRV`, etc.
4. **No CPU waits on the GPU on the frame path.** Synchronize with shared fences only.
5. **Log with the CS/SKSE logger.** Every milestone adds a hotkey that writes a debug dump (see Verification loop).
6. **Match CS's existing conventions** for formatting, feature registration, the ImGui settings panel and shader defines. Look before writing.
7. **Keep changes small and per milestone.** Update `docs/ROADMAP.md` status and `docs/CS_NOTES.md` as you learn things.

## Build & deploy

- Windows, Visual Studio 2026 (Desktop C++; 2022 is also installed but don't build with it), CMake, vcpkg with `VCPKG_ROOT` set. The vcpkg commit must match `builtin-baseline` in `vcpkg.json`.
  - CS requires **CMake 4.2+** (VS 2022's bundled CMake is too old; a standalone CMake is installed).
  - vcpkg: `G:\DEV\vcpkg`, checked out at the baseline `dddca6f`; `VCPKG_ROOT` is set as a user env var.
  - Both VS 2022 (MSVC 14.44) and **VS 2026 (MSVC 14.51)** are installed. vcpkg always picks the newest MSVC, so use CS's default VS 2026 presets (`ALL`, `Dev`, …). **Don't use `ALL-VS2022`**: it links the 14.51-built vcpkg libs with 14.44 and fails with `unresolved external __std_search_4` in `efsw.lib`.
  - CS's own agent guide is `.claude/CLAUDE.md` (build wrappers, feature pattern, i18n, commit style). Follow it too.
- Build: follow the CS README (`BuildRelease.bat` or the CMake presets). Record the exact working commands here once confirmed:
  - Shell setup (the agent's PowerShell may hold a stale PATH and has `NoDefaultCurrentDirectoryInExePath=1`, so call `.bat` files by full path):
    `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User'); $env:VCPKG_ROOT = 'G:\DEV\vcpkg'`
  - Configure (once, or after adding/removing files): `cmake -S . --preset ALL` → `build/ALL` (confirmed 2026-09-23)
  - Build for testing: `cmake --build --preset Dev` → DLL + ready-to-copy folder in `build/ALL/aio` (confirmed 2026-09-23; ~3 min warm)
  - Don't use `BuildRelease.bat ALL`/`Package` locally: the packaging step picks up a broken `7z.exe` app alias in `%LOCALAPPDATA%\Microsoft\WindowsApps` and fails (the DLL is still built).
  - First cold build (vcpkg deps from source) took ~7 min; vcpkg binary cache is `%LOCALAPPDATA%\vcpkg\archives`.
  - Deploy target: Skyrim `Data` directly — `G:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data` (no MO2).
    Deploy: `robocopy build\ALL\aio "G:\SteamLibrary\steamapps\common\Skyrim Special Edition\Data" /E /NFL /NDL /NJH /NP` (exit codes 0–7 = success; confirmed 2026-09-23). The game must be closed or the DLL copy fails.
- Game runtime installed: **1.7.104** (Steam, fresh install 2026-09-23). Tested with CS: ✅ unmodified fork (CS v1.9.0) loads, hooks install cleanly and renders in world on 1.7.104 (2026-09-23, with SKSE 2.3.1, Address Library, Engine Fixes, Crash Logger). First launch compiles ~3,200 shader permutations, which takes several minutes.
  - CS pins CommonLibSSE-NG v6.7.1, which defines `RUNTIME_SSE_1_7_99` but not 1.7.104. That's fine: 1.7.104 takes CS's "1.7.99 or newer" code path, confirmed working in M0.
  - Launch with `skse64_loader.exe` (Steam's Play button starts the vanilla launcher and loads no plugins).
- Git: `origin` = Jake's fork, `upstream` = community-shaders/skyrim-community-shaders. RT work lives on the `skyrim-rt` branch; `dev` tracks upstream.

## Verification loop (important — you cannot see the game)

Claude Code can build but cannot play Skyrim. Each milestone therefore needs machine-readable output.

1. **Debug dump hotkey.** The feature writes files to `<Skyrim Documents>/SKSE/SkyrimRT/`:
   - `frame_<n>.json`: stats, timings and error metrics.
   - `debug_<view>_<n>.png`: images of the debug views.

   Claude reads these directly with its file tools.
2. **Log file.** The SKSE log for the plugin lives in `Documents/My Games/Skyrim Special Edition/SKSE/`. Read it after each test run.
3. **Test runs.** When a test run is needed, give Jake exact steps, e.g. "load save X, stand in Whiterun market, press F10, quit". Wait for him to confirm, then read the dump.
4. **Prefer numeric checks over eyeballing.** Example: RT-traced depth vs. raster depth mismatch percentage (ROADMAP M4).

If the device is removed or the game crashes:
- Read the SKSE log and any crash log (e.g. from Crash Logger SSE) before changing code.
- Enable the D3D12 debug layer and DRED in debug builds.

## Key references

See `docs/REFERENCES.md`.
