#include "RT.h"

#include <dxgi.h>
#include <winrt/base.h>

namespace RT
{
	namespace
	{
		Capabilities capabilities;

		std::string WideToUtf8(const wchar_t* a_text)
		{
			return stl::utf16_to_utf8(a_text).value_or("<unicode conversion error>"s);
		}
	}

	bool Init(ID3D11Device* a_device)
	{
		capabilities = {};

		auto fail = [](std::string reason) {
			capabilities.failureReason = std::move(reason);
			logger::error("[SkyrimRT] Capability probe failed: {}", capabilities.failureReason);
			return false;
		};

		if (!a_device)
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

		// Probe device only: released when this function returns.
		winrt::com_ptr<ID3D12Device> probeDevice;
		if (HRESULT hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(probeDevice.put())); FAILED(hr))
			return fail(std::format("D3D12CreateDevice failed on {} (hr {:#010x})", capabilities.adapterName, static_cast<uint32_t>(hr)));

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
		if (SUCCEEDED(probeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
			capabilities.raytracingTier = options5.RaytracingTier;

		capabilities.probed = true;

		logger::info("[SkyrimRT] Adapter: {} | LUID {} | DXR tier {} (required {})",
			capabilities.adapterName,
			FormatLuid(capabilities.adapterLuid),
			GetTierName(capabilities.raytracingTier),
			GetTierName(kRequiredTier));

		return IsSupported();
	}

	const Capabilities& GetCapabilities()
	{
		return capabilities;
	}

	bool IsSupported()
	{
		return capabilities.probed && capabilities.raytracingTier >= kRequiredTier;
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
}
