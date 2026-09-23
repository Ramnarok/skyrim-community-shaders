# References

## Skyrim modding / base code
- Community Shaders (base fork): https://github.com/community-shaders/skyrim-community-shaders (the old doodlum URL redirects here). Jake's fork: https://github.com/Ramnarok/skyrim-community-shaders
  - Developer wiki, covering the debugging setup (Steamless, ASLR, RenderDoc): https://github.com/community-shaders/skyrim-community-shaders/wiki/Developers
- PIXL Renderer (CS-derived; reference for material classification and its DX11/DX12 sidecar): https://github.com/pixlmusic/PIXL-Renderer
- CommonLibSSE-NG: use the exact submodule version CS pins. Don't mix versions.
  - Source is alandtse's fork, https://github.com/alandtse/CommonLibVR (branch `ng`), at `extern/CommonLibSSE-NG`.
  - As of 2026-09-23 CS pins v6.7.1 (`70c1acd`, knows 1.7.99). The `ng` branch head adds `RUNTIME_SSE_1_7_104`.
- Skyrim Upscaler: a prior DX11/DX12 hybrid in Skyrim, useful for its interop code.
- Address Library for SKSE (required at runtime).

## D3D12 / DXR
- DXR functional spec (RayQuery, AS formats, instance descs): https://github.com/microsoft/DirectX-Specs/blob/master/d3d/Raytracing.md
- Microsoft D3D12 raytracing samples: https://github.com/microsoft/DirectX-Graphics-Samples
- Sharing resources between D3D11 and D3D12 ("Surface sharing between Windows graphics APIs"): https://learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis
- MJP's DXRPathTracer (clean, small reference path tracer): https://github.com/TheRealMJP/DXRPathTracer

## Denoising / lighting
- NVIDIA NRD (ReBLUR, ReLAX, SIGMA shadow denoiser): https://github.com/NVIDIA-RTX/NRD
- NVIDIA RTXDI (ReSTIR DI/GI): https://github.com/NVIDIA-RTX/RTXDI
- Ray Tracing Gems I & II (free PDFs): https://www.realtimerendering.com/raytracinggems/

## Tools
- RenderDoc: capture Skyrim frames to learn the pass structure.
- PIX for Windows: for the D3D12 sidecar (RenderDoc can't see both devices well).
- Crash Logger SSE: readable crash logs.
