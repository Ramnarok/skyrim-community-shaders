#pragma once

#include <d3d11.h>
#include <d3d12.h>

/**
 * @brief SkyrimRT's D3D12 side. All D3D12 code lives under src/RT/; CS code only sees this interface.
 *
 * M1 scope: identify the adapter the game renders on and probe its DXR support. The probe device is
 * released again, so nothing D3D12 stays resident yet (the persistent sidecar arrives in M2).
 */
namespace RT
{
	/** @brief What the capability probe found on the game's adapter. */
	struct Capabilities
	{
		std::string adapterName;
		LUID adapterLuid{};
		D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
		bool probed = false;       ///< The probe ran to completion (adapter found, D3D12 device created).
		std::string failureReason;  ///< Why the probe failed, when probed is false.
	};

	/** @brief Minimum tier we require: DXR 1.1 for inline RayQuery in compute shaders. */
	inline constexpr D3D12_RAYTRACING_TIER kRequiredTier = D3D12_RAYTRACING_TIER_1_1;

	/**
	 * @brief Probes the adapter behind the game's D3D11 device for DXR support and logs the result.
	 * @param a_device The game's D3D11 device (globals::d3d::device).
	 * @return True when the adapter supports kRequiredTier.
	 */
	bool Init(ID3D11Device* a_device);

	/** @brief Result of the last Init() call. */
	const Capabilities& GetCapabilities();

	/** @brief True when Init() found an adapter that supports kRequiredTier. */
	bool IsSupported();

	/** @brief Human-readable name for a raytracing tier, e.g. "1.1"; derived from the enum value so tiers newer than the SDK still print. */
	std::string GetTierName(D3D12_RAYTRACING_TIER a_tier);

	/** @brief Formats a LUID as "HighPart:LowPart" in hex. */
	std::string FormatLuid(const LUID& a_luid);
}
