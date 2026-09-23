#pragma once

#include "OverlayFeature.h"

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
				T("feature.skyrim_rt.key_feature_3", "Work in progress: actors, foliage transparency and global illumination come later") } };
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
	};

	Settings settings;

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
	virtual bool IsOverlayVisible() const override { return settings.Enabled && (settings.ShowTestPattern || settings.TraceDebugView || settings.ShadowView != 0); }

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	void DrawSunShadowSettings();

	bool dumpKeyWasDown = false;
	uint32_t providesMaskFrame = UINT32_MAX;
	bool providesMask = false;
};
