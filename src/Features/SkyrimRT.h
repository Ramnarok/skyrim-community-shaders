#pragma once

#include "OverlayFeature.h"

namespace RT
{
	struct GIOutputs;
}

/**
 * @brief Hybrid ray-traced lighting on a D3D12 sidecar device (see docs/ROADMAP.md).
 *
 * Keeps a D3D12 device on the game's adapter; each frame, before the opaque pass, D3D11 hands the depth pre-pass to
 * D3D12, which builds the acceleration structures and traces. M5: the ray-traced sun-shadow mask replaces the
 * Screen-Space Shadows input (PS t45), so it multiplies the game's shadow-map term in Lighting.hlsl, RunGrass.hlsl
 * and DistantTree.hlsl. A hotkey writes a JSON + PNG debug dump.
 */
struct SkyrimRT : OverlayFeature
{
	virtual inline std::string GetName() override { return "Skyrim RT"; }
	virtual std::string GetDisplayName() override { return T("feature.skyrim_rt.name", "Skyrim RT"); }
	virtual inline std::string GetShortName() override { return "SkyrimRT"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.skyrim_rt.description", "Experimental hybrid ray-traced lighting using a DirectX 12 raytracing device alongside the game's renderer."),
			{ T("feature.skyrim_rt.key_feature_1", "Requires a GPU with DirectX Raytracing tier 1.1"),
				T("feature.skyrim_rt.key_feature_2", "Ray-traced sun and moon shadows from the static scene and terrain"),
				T("feature.skyrim_rt.key_feature_3", "Ray-traced one-bounce global illumination and ambient occlusion (private builds with NVIDIA NRD)"),
				T("feature.skyrim_rt.key_feature_4", "Work in progress: actors and foliage transparency come later") } };
	}

	/** @brief Debug dump hotkey (ROADMAP verification loop). */
	static constexpr int kDumpHotkey = VK_F10;

	struct Settings
	{
		bool Enabled = true;
		bool ShowTestPattern = false;
		bool TraceDebugView = false;  ///< M4: trace the depth / instance / normal / mismatch debug views every frame
		uint32_t DebugView = 3;       ///< 0 depth, 1 instance, 2 normal, 3 depth-mismatch diff
		bool SunShadows = true;       ///< M5: ray-traced sun shadows in place of Screen-Space Shadows
		float SunAngularRadius = 0.5f;  ///< degrees; penumbra width
		bool AlphaTestedShadows = false;
		float ShadowNormalBias = 1.0f;
		float ShadowDistanceBias = 0.002f;
		uint32_t ShadowHistory = 24;
		float ShadowSpatialRadius = 3.0f;
		uint32_t ShadowView = 0;  ///< overlay: 0 off, 1 raw, 2 denoised
		bool GlobalIllumination = true;  ///< M6: ray-traced GI in place of Screen-Space GI (builds with NRD only)
		float GIIntensity = 1.0f;
		float GIAOStrength = 1.0f;
		float GIRayLength = 3000.0f;  ///< game units
		bool GIAlphaTested = false;
		uint32_t GIHistory = 30;  ///< REBLUR accumulated frames
		uint32_t GIView = 0;      ///< overlay: 0 off, 1 noisy, 2 denoised, 3 ambient occlusion
	};

	Settings settings;

	/** @brief Whether this frame should build the scene for, and trace, ray-traced GI (before any tracing happens). */
	bool WantsGlobalIllumination();

	/**
	 * @brief Called by Deferred::DeferredPasses in place of Screen-Space GI's pass. Runs the GI hand-off and returns
	 * true with the composite's t10-t12 inputs when ray-traced GI ran this frame; false to let Screen-Space GI run.
	 */
	bool DrawGlobalIllumination(RT::GIOutputs& a_outputs);

	/**
	 * @brief Whether Skyrim RT binds its sun-shadow mask at PS t45 this frame, in which case Screen-Space Shadows skips
	 * its own pass. Evaluated once per frame, so both features see the same answer.
	 */
	bool ProvidesSunShadowMask();

	/**
	 * @brief Before the opaque pass: captures the main camera, runs the RT round trip (debug trace and/or sun
	 * shadows) and binds the sun-shadow mask at PS t45.
	 */
	virtual void Prepass() override;

	/** @brief Enables the D3D12 debug layer and DRED in debug builds, before any D3D12 device exists. */
	virtual void Load() override;

	/** @brief Probes the adapter and starts the D3D12 sidecar; disables the feature when unsupported. */
	virtual void SetupResources() override;

	/** @brief Per-frame (from the Present hook): runs the interop round trip and polls the dump hotkey. */
	virtual void Reset() override;

	/** @brief Draws the enable toggles, device info, interop results and live timings. */
	virtual void DrawSettings() override;

	/** @brief Composites the D3D12-written test pattern into the top-right corner. */
	virtual void DrawOverlay() override;
	virtual bool IsOverlayVisible() const override { return settings.Enabled && (settings.ShowTestPattern || settings.TraceDebugView || settings.ShadowView != 0 || settings.GIView != 0); }

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	void DrawSunShadowSettings();
	void DrawGlobalIlluminationSettings();

	bool dumpKeyWasDown = false;
	uint32_t providesMaskFrame = UINT32_MAX;
	bool providesMask = false;
};
