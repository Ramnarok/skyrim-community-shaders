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

		std::string WideToUtf8(const wchar_t* a_text)
		{
			return stl::utf16_to_utf8(a_text).value_or("<unicode conversion error>"s);
		}
	}

	void EnableDebugLayer()
	{
#ifndef NDEBUG
		// Enabling the debug layer after a D3D12 device exists removes that device, so this must run
		// before CS's frame generation (or anything else) creates one.
		winrt::com_ptr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())))) {
			debug->EnableDebugLayer();
			logger::info("[SkyrimRT] D3D12 debug layer enabled (debug build)");
		}
		winrt::com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dred;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dred.put())))) {
			dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			logger::info("[SkyrimRT] DRED breadcrumbs and page-fault reporting enabled (debug build)");
		}
#endif
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

		winrt::com_ptr<ID3D12Device> device;
		if (HRESULT hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.put())); FAILED(hr))
			return fail(std::format("D3D12CreateDevice failed on {} ({})", capabilities.adapterName, FormatHResult(hr)));

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
			capabilities.raytracingTier = options5.RaytracingTier;

		capabilities.probed = true;

		logger::info("[SkyrimRT] Adapter: {} | LUID {} | DXR tier {} (required {})",
			capabilities.adapterName,
			FormatLuid(capabilities.adapterLuid),
			GetTierName(capabilities.raytracingTier),
			GetTierName(kRequiredTier));

		if (!IsSupported())
			return false;  // the device is released here; nothing D3D12 stays resident

		// Keep the device: it becomes the sidecar.
		auto newSidecar = std::make_unique<Sidecar>();
		if (!newSidecar->Init(std::move(device), a_device, a_context)) {
			capabilities.failureReason = newSidecar->GetFailureReason();
			return false;
		}
		sidecar = std::move(newSidecar);
		return true;
	}

	void OnFrame()
	{
		if (sidecar)
			sidecar->OnFrame();
	}

	void RequestDebugDump()
	{
		if (sidecar) {
			sidecar->RequestDebugDump(globals::state->frameCount);
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
