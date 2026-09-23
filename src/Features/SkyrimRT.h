#pragma once

#include "Feature.h"

/**
 * @brief Hybrid ray-traced lighting on a D3D12 sidecar device (see docs/ROADMAP.md).
 *
 * M1 skeleton: registers the feature, probes the game's adapter for DXR 1.1 once the
 * D3D11 device exists, and disables itself when inline raytracing is unavailable.
 * It has no shader defines and no per-frame work yet, so it does not change rendering.
 */
struct SkyrimRT : Feature
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
				T("feature.skyrim_rt.key_feature_2", "Work in progress: currently only detects raytracing support") } };
	}

	struct Settings
	{
		bool Enabled = true;
	};

	Settings settings;

	/** @brief Probes the game's adapter for DXR support; disables the feature when unsupported. */
	virtual void SetupResources() override;

	/** @brief Draws the enable toggle and the detected adapter / raytracing tier. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
};
