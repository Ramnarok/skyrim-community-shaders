#include "RT.h"

#include "Sidecar.h"
#include "State.h"

#include <dxgi.h>
#include <winrt/base.h>

namespace RT
{
	namespace
	{
		Capabilities capabilities;
		std::unique_ptr<Sidecar> sidecar;
		FrameCamera camera;

		bool hangDiagnostics = false;  // EnableDebugLayer's DRED choice, applied to the sidecar's own device factory too

		std::string WideToUtf8(const wchar_t* a_text)
		{
			return stl::utf16_to_utf8(a_text).value_or("<unicode conversion error>"s);
		}

		/**
		 * D3D12 devices are per-adapter singletons: D3D12CreateDevice hands back the device CS's frame generation
		 * already made (DX12SwapChain), the one the game presents through. Measured 2026-09-24 on the RTX 4080 SUPER:
		 * the two calls return one object, and a removed device sets all its fences to UINT64_MAX. So a hang in our
		 * passes removed the presenting device and froze the game. A device factory that doesn't store its device as
		 * the singleton gives the sidecar its own: a hang then removes only ours, which releases every D3D11 wait on
		 * our fence, and the game carries on without RT.
		 */
		winrt::com_ptr<ID3D12Device> CreateSidecarDevice(IDXGIAdapter* a_adapter, bool& a_independent, HRESULT& a_hr)
		{
			a_independent = false;
			winrt::com_ptr<ID3D12DeviceFactory> factory;
			a_hr = D3D12GetInterface(CLSID_D3D12DeviceFactory, IID_PPV_ARGS(factory.put()));
			if (SUCCEEDED(a_hr))
				a_hr = factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
			if (SUCCEEDED(a_hr)) {
				// The global debug/DRED settings (EnableDebugLayer) don't reach a factory's devices; configure it too.
#ifndef NDEBUG
				winrt::com_ptr<ID3D12Debug> debug;
				if (SUCCEEDED(factory->GetConfigurationInterface(CLSID_D3D12Debug, IID_PPV_ARGS(debug.put()))))
					debug->EnableDebugLayer();
#endif
				if (hangDiagnostics) {
					winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
					if (SUCCEEDED(factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(dred.put())))) {
						dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
						dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
						dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
					} else {
						logger::warn("[SkyrimRT] DRED can't be enabled on the sidecar's device factory");
					}
				}
				winrt::com_ptr<ID3D12Device> device;
				a_hr = factory->CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put()));
				if (SUCCEEDED(a_hr)) {
					a_independent = true;
					return device;
				}
			}
			logger::warn("[SkyrimRT] No independent D3D12 device ({}): sharing the process-wide device, so a ray tracing hang would also stop the frame generation device", FormatHResult(a_hr));
			winrt::com_ptr<ID3D12Device> device;
			a_hr = D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put()));
			return SUCCEEDED(a_hr) ? device : nullptr;
		}
	}

	void EnableDebugLayer(bool a_hangDiagnostics)
	{
		// The process-wide debug layer must be enabled before CS's frame generation (or anything else) creates a D3D12
		// device: enabling it afterwards removes existing devices. The sidecar's own device gets its debug layer and
		// DRED from its device factory (CreateSidecarDevice), so DRED doesn't touch frame generation's device.
#ifndef NDEBUG
		winrt::com_ptr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
			debug->EnableDebugLayer();
			logger::info("[SkyrimRT] D3D12 debug layer enabled (debug build)");
		}
		a_hangDiagnostics = true;
#endif
		hangDiagnostics = a_hangDiagnostics;
		if (a_hangDiagnostics)
			logger::info("[SkyrimRT] GPU hang diagnostics on: DRED breadcrumbs, pass markers and page faults for the sidecar's device");
	}

	bool Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		capabilities = {};
		sidecar.reset();

		auto fail = [](std::string reason) {
			capabilities.failureReason = std::move(reason);
			logger::error("[SkyrimRT] Capability probe failed: {}", capabilities.failureReason);
			return false;
		};

		if (!a_device || !a_context)
			return fail("no D3D11 device");

		// The D3D12 device must live on the same adapter the game renders with, so resolve
		// that adapter from the game's own device rather than enumerating adapters.
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
			return fail("D3D11 device does not expose IDXGIDevice");

		winrt::com_ptr<IDXGIAdapter> adapter;
		if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
			return fail("IDXGIDevice::GetAdapter failed");

		DXGI_ADAPTER_DESC adapterDesc{};
		if (FAILED(adapter->GetDesc(&adapterDesc)))
			return fail("IDXGIAdapter::GetDesc failed");

		capabilities.adapterName = WideToUtf8(adapterDesc.Description);
		capabilities.adapterLuid = adapterDesc.AdapterLuid;

		bool independent = false;
		HRESULT createHr = S_OK;
		winrt::com_ptr<ID3D12Device> device = CreateSidecarDevice(adapter.get(), independent, createHr);
		if (!device)
			return fail(std::format("D3D12CreateDevice failed on {} ({})", capabilities.adapterName, FormatHResult(createHr)));
		capabilities.independentDevice = independent;

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
			capabilities.raytracingTier = options5.RaytracingTier;

		capabilities.probed = true;

		logger::info("[SkyrimRT] Adapter: {} | LUID {} | DXR tier {} (required {}) | {} D3D12 device",
			capabilities.adapterName,
			FormatLuid(capabilities.adapterLuid),
			GetTierName(capabilities.raytracingTier),
			GetTierName(kRequiredTier),
			capabilities.independentDevice ? "independent" : "shared (process-wide singleton)");

		if (!IsSupported())
			return false;  // the device is released here; nothing D3D12 stays resident

		// Keep the device: it becomes the sidecar.
		auto newSidecar = std::make_unique<Sidecar>();
		const auto* graphicsState = globals::game::graphicsState;
		if (!newSidecar->Init(std::move(device), a_device, a_context, graphicsState->screenWidth, graphicsState->screenHeight)) {
			capabilities.failureReason = newSidecar->GetFailureReason();
			return false;
		}
		sidecar = std::move(newSidecar);
		return true;
	}

	void CaptureCamera(const float* a_viewProjInverse, const float* a_viewProj, const float* a_view, const float* a_viewInverse, const float* a_projUnjittered,
		const float* a_posAdjust, uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_gameFrame)
	{
		camera.valid = true;
		camera.gameFrame = a_gameFrame;
		std::memcpy(camera.viewProjInverse, a_viewProjInverse, sizeof(camera.viewProjInverse));
		std::memcpy(camera.viewProj, a_viewProj, sizeof(camera.viewProj));
		std::memcpy(camera.view, a_view, sizeof(camera.view));
		std::memcpy(camera.viewInverse, a_viewInverse, sizeof(camera.viewInverse));
		std::memcpy(camera.projUnjittered, a_projUnjittered, sizeof(camera.projUnjittered));
		camera.posAdjust = { a_posAdjust[0], a_posAdjust[1], a_posAdjust[2] };
		camera.renderWidth = a_renderWidth;
		camera.renderHeight = a_renderHeight;
	}

	void OnPrepass(bool a_debugTrace, const SunShadowParams* a_shadows, const PointShadowParams* a_pointShadows, bool a_buildForGI)
	{
		if (sidecar)
			sidecar->Submit(globals::state->frameCount, camera, a_debugTrace, a_shadows, a_pointShadows, a_buildForGI);
	}

	bool IsGICompiledIn()
	{
#if defined(SKYRIMRT_NRD)
		return true;
#else
		return false;
#endif
	}

	bool IsGIAvailable()
	{
		return sidecar && sidecar->GetGIStats() != nullptr;
	}

	bool CanTraceGI()
	{
		return sidecar && sidecar->CanTraceGI(globals::state->frameCount);
	}

	GIOutputs SubmitGI(const GIParams& a_params)
	{
		return sidecar ? sidecar->SubmitGI(globals::state->frameCount, a_params) : GIOutputs{};
	}

	const SkinnedStats* GetRaytracerSkinnedStats()
	{
		const auto* raytracer = sidecar ? sidecar->GetRaytracer() : nullptr;
		const auto* skinned = raytracer ? raytracer->GetSkinned() : nullptr;
		return skinned ? &skinned->GetStats() : nullptr;
	}

	const GIStats* GetGIStats()
	{
		return sidecar ? sidecar->GetGIStats() : nullptr;
	}

	void SetAlphaTest(bool a_enabled)
	{
		if (sidecar)
			sidecar->SetAlphaTest(a_enabled);
	}

	void SetTreeRestPose(bool a_enabled)
	{
		if (sidecar)
			sidecar->SetTreeRestPose(a_enabled);
	}

	void SetStaticTrees(bool a_enabled)
	{
		if (sidecar)
			sidecar->SetStaticTrees(a_enabled);
	}

	void SetSkipMeshLOD(bool a_enabled)
	{
		if (sidecar)
			sidecar->SetSkipMeshLOD(a_enabled);
	}

	const AlphaAtlasStats* GetAlphaAtlasStats()
	{
		return sidecar ? &sidecar->GetAlphaAtlasStats() : nullptr;
	}

	ID3D11ShaderResourceView* GetGIViewSRV()
	{
		const auto* stats = GetGIStats();
		return (stats && stats->haveResult) ? sidecar->GetGIViewSRV() : nullptr;
	}

	void OnFrame()
	{
		if (sidecar)
			sidecar->OnPresent(globals::state->frameCount);
	}

	bool CanTraceSunShadows()
	{
		return sidecar && sidecar->CanTraceSunShadows();
	}

	bool IsSunShadowSuppressed()
	{
		return sidecar && sidecar->IsSunShadowSuppressed();
	}

	ID3D11ShaderResourceView* AcquireSunShadowMask()
	{
		return sidecar ? sidecar->AcquireSunShadowMask(globals::state->frameCount) : nullptr;
	}

	ID3D11ShaderResourceView* GetSunShadowViewSRV()
	{
		const auto* shadows = sidecar ? sidecar->GetSunShadows() : nullptr;
		return (shadows && shadows->GetStats().haveResult) ? shadows->GetViewSRV() : nullptr;
	}

	const SunShadowStats* GetSunShadowStats()
	{
		const auto* shadows = sidecar ? sidecar->GetSunShadows() : nullptr;
		return shadows ? &shadows->GetStats() : nullptr;
	}

	void SetRoomIndices(std::span<const RoomIndex> a_rooms)
	{
		if (sidecar)
			sidecar->SetRoomIndices(a_rooms);
	}

	void SimulateGpuHang()
	{
		if (sidecar)
			sidecar->SimulateHang();
	}

	bool CanTracePointLightShadows()
	{
		return sidecar && sidecar->CanTracePointLightShadows();
	}

	ID3D11ShaderResourceView* AcquirePointLightShadowMask()
	{
		return sidecar ? sidecar->AcquirePointLightShadowMask(globals::state->frameCount) : nullptr;
	}

	ID3D11ShaderResourceView* GetPointLightShadowViewSRV()
	{
		const auto* shadows = sidecar ? sidecar->GetPointShadows() : nullptr;
		return (shadows && shadows->GetStats().haveResult) ? shadows->GetViewSRV() : nullptr;
	}

	const SunShadowStats* GetPointLightShadowStats()
	{
		const auto* shadows = sidecar ? sidecar->GetPointShadows() : nullptr;
		return shadows ? &shadows->GetStats() : nullptr;
	}

	ID3D11ShaderResourceView* GetDebugViewSRV(uint32_t a_view)
	{
		const auto* raytracer = sidecar ? sidecar->GetRaytracer() : nullptr;
		if (!raytracer || a_view >= static_cast<uint32_t>(DebugView::kCount) || !raytracer->GetStats().haveResult)
			return nullptr;
		return raytracer->GetViewSRV(static_cast<DebugView>(a_view));
	}

	bool GetDebugViewSize(uint32_t& a_textureWidth, uint32_t& a_textureHeight, uint32_t& a_renderWidth, uint32_t& a_renderHeight)
	{
		const auto* raytracer = sidecar ? sidecar->GetRaytracer() : nullptr;
		if (!raytracer || !raytracer->GetStats().haveResult)
			return false;
		a_textureWidth = raytracer->GetTextureWidth();
		a_textureHeight = raytracer->GetTextureHeight();
		a_renderWidth = raytracer->GetStats().renderWidth;
		a_renderHeight = raytracer->GetStats().renderHeight;
		return a_renderWidth > 0 && a_renderHeight > 0;
	}

	const TraceStats* GetTraceStats()
	{
		const auto* raytracer = sidecar ? sidecar->GetRaytracer() : nullptr;
		return raytracer ? &raytracer->GetStats() : nullptr;
	}

	void RequestDebugDump()
	{
		if (sidecar) {
			sidecar->RequestDebugDump();
			logger::info("[SkyrimRT] Debug dump requested at frame {}", globals::state->frameCount);
		}
	}

	ID3D11ShaderResourceView* GetTestPatternSRV()
	{
		return sidecar ? sidecar->GetTestPatternSRV() : nullptr;
	}

	const Capabilities& GetCapabilities()
	{
		return capabilities;
	}

	bool IsSupported()
	{
		return capabilities.probed && capabilities.raytracingTier >= kRequiredTier;
	}

	bool IsRunning()
	{
		return sidecar != nullptr;
	}

	const InteropStats* GetInteropStats()
	{
		return sidecar ? &sidecar->GetStats() : nullptr;
	}

	const SpikeResults* GetSpikeResults()
	{
		return sidecar ? &sidecar->GetSpikeResults() : nullptr;
	}

	const SceneStats* GetSceneStats()
	{
		return sidecar ? &sidecar->GetSceneStats() : nullptr;
	}

	const TimingSeries* GetSceneTraversalMs()
	{
		return sidecar ? &sidecar->GetSceneTraversalMs() : nullptr;
	}

	const MeshCacheStats* GetMeshCacheStats()
	{
		return sidecar ? &sidecar->GetMeshCacheStats() : nullptr;
	}

	std::string GetTierName(D3D12_RAYTRACING_TIER a_tier)
	{
		// Tier enum values encode major/minor as major * 10 + minor (1_0 = 10, 1_1 = 11). Deriving
		// the name keeps tiers newer than the Windows SDK we build against readable (e.g. 1.2).
		if (a_tier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
			return "not supported";
		const auto value = static_cast<int>(a_tier);
		return std::format("{}.{}", value / 10, value % 10);
	}

	std::string FormatLuid(const LUID& a_luid)
	{
		return std::format("{:08X}:{:08X}", static_cast<uint32_t>(a_luid.HighPart), a_luid.LowPart);
	}

	std::string FormatHResult(HRESULT a_hr)
	{
		if (a_hr == S_OK)
			return "S_OK";
		return std::format("{:#010x}", static_cast<uint32_t>(a_hr));
	}

	std::filesystem::path GetDumpDirectory()
	{
		auto path = logger::log_directory();  // <Documents>/My Games/Skyrim Special Edition/SKSE
		if (!path)
			return {};
		return *path / "SkyrimRT";
	}
}
