#pragma once

#include "OverlayFeature.h"

/**
 * @brief Hybrid ray-traced lighting on a D3D12 sidecar device (see docs/ROADMAP.md).
 *
 * M2: keeps a D3D12 device on the game's adapter and proves interop every frame. D3D11 signals a
 * shared fence, D3D12 writes an animated test pattern into a shared texture and signals back, and
 * the overlay composites that texture into a screen corner. A hotkey writes a JSON + PNG debug dump.
 * No shader defines yet, so the game's own rendering is unchanged.
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
				T("feature.skyrim_rt.key_feature_2", "Work in progress: currently runs a DirectX 12 interop test") } };
	}

	/** @brief Debug dump hotkey (ROADMAP verification loop). */
	static constexpr int kDumpHotkey = VK_F10;

	struct Settings
	{
		bool Enabled = true;
		bool ShowTestPattern = true;
	};

	Settings settings;

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
	virtual bool IsOverlayVisible() const override { return settings.Enabled && settings.ShowTestPattern; }

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	bool dumpKeyWasDown = false;
};
