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

	/** @brief M8: Lighting.hlsl reads the point-light shadow mask at PS t46 (SkyrimRT/PointLightShadows.hlsli). */
	virtual inline std::string_view GetShaderDefineName() override { return "SKYRIM_RT"; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.skyrim_rt.description", "Experimental hybrid ray-traced lighting using a DirectX 12 raytracing device alongside the game's renderer."),
			{ T("feature.skyrim_rt.key_feature_1", "Requires a GPU with DirectX Raytracing tier 1.1"),
				T("feature.skyrim_rt.key_feature_2", "Ray-traced sun and moon shadows from the scene, characters and foliage"),
				T("feature.skyrim_rt.key_feature_3", "Ray-traced one-bounce global illumination and ambient occlusion (private builds with NVIDIA NRD)"),
				T("feature.skyrim_rt.key_feature_4", "Work in progress: many lights, reflections and distant land come later") } };
	}

	/** @brief Debug dump hotkey (ROADMAP verification loop). */
	static constexpr int kDumpHotkey = VK_F10;

	struct Settings
	{
		bool Enabled = true;
		bool ShowTestPattern = false;
		bool TraceDebugView = false;  ///< M4: trace the depth / instance / normal / mismatch debug views every frame
		uint32_t DebugView = 3;       ///< 0 depth, 1 instance, 2 normal, 3 depth-mismatch diff
		bool AlphaTest = true;        ///< M7c: alpha-test foliage in every trace (else solid cards)
		bool TreeRestPose = true;        ///< M7: trace trees without sway (their bones hold the last culling camera's pose)
		bool StaticTrees = true;         ///< M8: rest-pose trees as static instances (one BLAS per mesh, no per-frame skinning)
		bool SkipMeshLOD = false;        ///< M8 diagnostic: leave kMeshLOD shapes (L1_/L2_ detail levels) out of the traced scene
		bool GPUHangDiagnostics = true;  ///< DRED breadcrumbs + page faults in every build (read at startup)
		bool SunShadows = true;       ///< M5: ray-traced sun shadows in place of Screen-Space Shadows
		float SunAngularRadius = 0.5f;  ///< degrees; penumbra width
		bool AlphaTestedShadows = true;
		float ShadowNormalBias = 1.0f;
		float ShadowDistanceBias = 0.002f;
		uint32_t ShadowHistory = 24;
		float ShadowSpatialRadius = 3.0f;
		uint32_t ShadowView = 0;  ///< overlay: 0 off, 1 raw, 2 denoised
		bool PointLightShadows = true;  ///< M8: ray-traced shadows for the game's unshadowed point lights (PS t46)
		uint32_t PointShadowView = 0;   ///< overlay: 0 off, 1 raw, 2 denoised
		bool GlobalIllumination = true;  ///< M6: ray-traced GI in place of Screen-Space GI (builds with NRD only)
		float GIIntensity = 1.0f;
		float GIAOStrength = 1.0f;
		float GIRayLength = 3000.0f;  ///< game units
		bool GIAlphaTested = true;
		bool GIInteriors = true;          ///< ray-traced GI in interiors (else Screen-Space GI there)
		bool GIPointLights = true;        ///< bounce Light Limit Fix's point lights
		bool GIPointLightShadows = true;  ///< trace visibility to the sampled point light
		uint32_t GIBouncesInterior = 2;   ///< M8 multi-bounce indoors: path vertices per GI ray (1 = single bounce, at most 3)
		uint32_t GIBouncesExterior = 1;   ///< M8 multi-bounce outdoors (measured: no visible gain under the sky, +0.5 ms per bounce)
		bool GIAlbedoTextures = true;     ///< M8: GI hits sample their diffuse texture x vertex colour (else the texture's average)
		bool GISkyLight = true;           ///< M8: rays that reach the sky carry its light; the game's ambient is scaled by the traced / open-sky ratio (exteriors)
		uint32_t GIHistory = 30;  ///< REBLUR accumulated frames
		uint32_t GIView = 0;      ///< overlay: 0 off, 1 noisy, 2 denoised, 3 ambient occlusion, 4/5 reflections noisy/denoised
		bool GIReflections = false;             ///< M8: ray-traced reflections in place of the cubemaps (needs Dynamic Cubemaps; off until verified)
		float GIReflectionMaxRoughness = 1.0f;  ///< rougher surfaces keep the cubemap reflection
		bool GIReflectionHalfResolution = true;  ///< one reflection ray per 2x2 block (measured +1.2-1.5 ms at full resolution in rain)
	};

	Settings settings;

	/** @brief Whether this frame should build the scene for, and trace, ray-traced GI (before any tracing happens). */
	bool WantsGlobalIllumination();

	/**
	 * @brief Called by Deferred::DeferredPasses in place of Screen-Space GI's pass. Runs the GI hand-off and returns
	 * true with the composite's t10-t12 inputs when ray-traced GI ran this frame; false to let Screen-Space GI run.
	 * M8: a_outputs.reflections (composite t17) is set when ray-traced reflections ran too.
	 */
	bool DrawGlobalIllumination(RT::GIOutputs& a_outputs);

	/**
	 * @brief Whether Skyrim RT binds its sun-shadow mask at PS t45 this frame, in which case Screen-Space Shadows skips
	 * its own pass. Evaluated once per frame, so both features see the same answer.
	 */
	bool ProvidesSunShadowMask();

	/** @brief M8: whether this frame traces the point-light shadow mask Lighting.hlsl reads at PS t46. */
	bool ProvidesPointLightShadowMask();

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
	virtual bool IsOverlayVisible() const override { return settings.Enabled && (settings.ShowTestPattern || settings.TraceDebugView || settings.ShadowView != 0 || settings.PointShadowView != 0 || settings.GIView != 0); }

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

private:
	void DrawSunShadowSettings();
	void DrawPointLightShadowSettings();
	void DrawGlobalIlluminationSettings();

	bool dumpKeyWasDown = false;
	uint32_t providesMaskFrame = UINT32_MAX;
	bool providesMask = false;
};
