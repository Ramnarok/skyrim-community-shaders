#include "SkyrimRT.h"

#include "Features/InverseSquareLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "I18n/I18n.h"
#include "RT/AlphaAtlas.h"
#include "RT/GlobalIllumination.h"
#include "RT/MaterialTable.h"
#include "Utils/SphericalHarmonics.h"
#include "RT/MeshCache.h"
#include "RT/RT.h"
#include "RT/Raytracer.h"
#include "RT/Scene.h"
#include "RT/SkinnedMeshes.h"
#include "RT/SunShadows.h"
#include "State.h"

#define I18N_KEY_PREFIX "feature.skyrim_rt."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkyrimRT::Settings,
	Enabled,
	ShowTestPattern,
	TraceDebugView,
	DebugView,
	AlphaTest,
	TreeRestPose,
	SunShadows,
	SunAngularRadius,
	AlphaTestedShadows,
	ShadowNormalBias,
	ShadowDistanceBias,
	ShadowHistory,
	ShadowSpatialRadius,
	ShadowView,
	GlobalIllumination,
	GIIntensity,
	GIAOStrength,
	GIRayLength,
	GIAlphaTested,
	GIInteriors,
	GIPointLights,
	GIPointLightShadows,
	GIHistory,
	GIView)

namespace
{
	// Same light Screen-Space Shadows uses: the shadow scene node's directional light (the sun, or the moon at night).
	// Its world direction points away from the light, so the direction towards it is the negation.
	bool GetDirectionToSun(float (&a_out)[3])
	{
		auto** accumulatorSlot = globals::game::currentAccumulator.get();
		auto* accumulator = accumulatorSlot ? *accumulatorSlot : nullptr;
		auto* shadowSceneNode = accumulator ? accumulator->GetRuntimeData().activeShadowSceneNode : nullptr;
		auto* sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunLight : nullptr;
		auto* light = sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get()) : nullptr;
		if (!light)
			return false;
		const auto& direction = light->GetWorldDirection();
		const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
		if (!(length > 1e-6f))
			return false;
		a_out[0] = -direction.x / length;
		a_out[1] = -direction.y / length;
		a_out[2] = -direction.z / length;
		return true;
	}

	// The lighting inputs of SharedData, computed the way State::UpdateSharedData does, so the bounce light matches the
	// direct light and ambient CS's Lighting.hlsl applies (the light Screen-Space GI gathers from the screen).
	bool GatherLighting(RT::GIParams& a_params)
	{
		const auto* smState = globals::game::smState;
		auto* shadowSceneNode = smState ? smState->shadowSceneNode[0] : nullptr;
		auto* sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunLight : nullptr;
		auto* light = sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get()) : nullptr;
		if (!light)
			return false;

		const auto& runtime = light->GetLightRuntimeData();
		float scale = runtime.fade;
		if (auto* imageSpaceManager = globals::game::imageSpaceManager)
			scale *= imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale;
		a_params.sunColor[0] = runtime.diffuse.red * scale;
		a_params.sunColor[1] = runtime.diffuse.green * scale;
		a_params.sunColor[2] = runtime.diffuse.blue * scale;

		const auto& direction = light->GetWorldDirection();
		const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
		if (!(length > 1e-6f))
			return false;
		a_params.toSun[0] = -direction.x / length;
		a_params.toSun[1] = -direction.y / length;
		a_params.toSun[2] = -direction.z / length;

		const auto& m = smState->directionalAmbientTransform.rotate;
		const auto& t = smState->directionalAmbientTransform.translate;
		const float3 dalcColors[6] = {
			{ m.entry[0][0] + t.x, m.entry[1][0] + t.y, m.entry[2][0] + t.z }, { -m.entry[0][0] + t.x, -m.entry[1][0] + t.y, -m.entry[2][0] + t.z },
			{ m.entry[0][1] + t.x, m.entry[1][1] + t.y, m.entry[2][1] + t.z }, { -m.entry[0][1] + t.x, -m.entry[1][1] + t.y, -m.entry[2][1] + t.z },
			{ m.entry[0][2] + t.x, m.entry[1][2] + t.y, m.entry[2][2] + t.z }, { -m.entry[0][2] + t.x, -m.entry[1][2] + t.y, -m.entry[2][2] + t.z }
		};
		const auto sh = SphericalHarmonics::DALCToSH(dalcColors);
		const SphericalHarmonics::SH2* channels[3] = { &sh.r, &sh.g, &sh.b };
		for (uint32_t i = 0; i < 3; i++) {
			a_params.ambientSH[i][0] = channels[i]->c0;
			a_params.ambientSH[i][1] = channels[i]->c1[0];
			a_params.ambientSH[i][2] = channels[i]->c1[1];
			a_params.ambientSH[i][3] = channels[i]->c1[2];
		}

		const auto& linearLighting = globals::features::linearLighting;
		a_params.linearLighting = linearLighting.loaded && linearLighting.settings.enableLinearLighting;
		a_params.colorGamma = linearLighting.settings.colorGamma;
		a_params.lightGamma = linearLighting.settings.lightGamma;
		a_params.ambientGamma = linearLighting.settings.ambientGamma;
		a_params.ambientMult = linearLighting.settings.ambientMult;
		a_params.directionalLightMult = linearLighting.settings.directionalLightMult;
		return true;
	}

	// Light Limit Fix's lights for this frame (built in its Prepass, earlier in the feature list), with the colour
	// Lighting.hlsl ends up multiplying by attenuation and N.L: Color::PointLight(color) x Color::VanillaNormalization
	// x fade. Positions are already relative to FrameBuffer::CameraPosAdjust, the TLAS origin.
	std::span<const RT::GIPointLight> GatherPointLights(bool a_linearLighting)
	{
		static std::vector<RT::GIPointLight> lights;
		lights.clear();
		const auto& lightLimitFix = globals::features::lightLimitFix;
		if (!lightLimitFix.loaded)
			return {};
		const auto& linearLighting = globals::features::linearLighting.settings;
		const uint32_t count = std::min<uint32_t>(lightLimitFix.lightCount, static_cast<uint32_t>(lightLimitFix.lightsData.size()));
		lights.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			const auto& source = lightLimitFix.lightsData[i];
			if (!(source.radius > 0.0f))
				continue;
			const bool isLinear = source.lightFlags.any(LightLimitFix::LightFlags::Linear);
			RT::GIPointLight& light = lights.emplace_back();
			light.position[0] = source.positionWS.data.x;
			light.position[1] = source.positionWS.data.y;
			light.position[2] = source.positionWS.data.z;
			light.radius = source.radius;
			light.invRadius = 1.0f / source.radius;  // Light Limit Fix fills it only under Inverse Square Lighting
			light.fadeZone = source.fadeZone;
			light.sizeBias = source.sizeBias;
			light.flags = source.lightFlags.underlying();
			const float color[3] = { source.color.x, source.color.y, source.color.z };
			for (uint32_t c = 0; c < 3; c++) {
				float value = std::max(color[c], 0.0f);
				// Under Linear Lighting, Color::PointLight's pi x pointLightMult (skipped for linear lights) meets the
				// diffuse term's 1/pi.
				if (a_linearLighting)
					value = isLinear ? value / std::numbers::pi_v<float> : std::pow(value, linearLighting.lightGamma) * linearLighting.pointLightMult;
				light.color[c] = value * source.fade;
			}
		}
		return lights;
	}

	bool IsGameWindowFocused()
	{
		auto renderer = globals::game::renderer;
		return renderer && GetForegroundWindow() == reinterpret_cast<HWND>(renderer->GetRuntimeData().renderWindows[0].hWnd);
	}

	void DrawResult(const char* a_label, HRESULT a_hr)
	{
		ImGui::Text("%s: %s", a_label, RT::FormatHResult(a_hr).c_str());
	}
}

void SkyrimRT::Load()
{
	RT::EnableDebugLayer();
}

void SkyrimRT::SetupResources()
{
	// The D3D11 device exists by now, so the adapter the game renders on is known.
	if (!RT::Init(globals::d3d::device, globals::d3d::context)) {
		const auto& caps = RT::GetCapabilities();
		loaded = false;
		if (!caps.probed)
			failedLoadedMessage = std::format("Disabled: raytracing support could not be detected ({}).", caps.failureReason);
		else if (!RT::IsSupported())
			failedLoadedMessage = std::format("Disabled: {} supports DirectX Raytracing tier {}, tier {} is required.", caps.adapterName, RT::GetTierName(caps.raytracingTier), RT::GetTierName(RT::kRequiredTier));
		else
			failedLoadedMessage = std::format("Disabled: the DirectX 12 device could not be set up ({}).", caps.failureReason);
		logger::warn("[SkyrimRT] {}", failedLoadedMessage);
		return;
	}

	logger::info("[SkyrimRT] DXR {} available, sidecar active (enabled setting: {})", RT::GetTierName(RT::kRequiredTier), settings.Enabled);
}

bool SkyrimRT::ProvidesSunShadowMask()
{
	// Cached per frame: Screen-Space Shadows asks first (its Prepass runs earlier in the feature list), and both must
	// agree even if the device state changes during this frame's round trip.
	const uint32_t frame = globals::state->frameCount;
	if (providesMaskFrame != frame) {
		providesMaskFrame = frame;
		// Lighting.hlsl samples t45 only when compiled with SCREEN_SPACE_SHADOWS, i.e. when that feature is loaded,
		// and only in exteriors; kFull is the sky mode Screen-Space Shadows traces in too.
		const auto* sky = globals::game::sky;
		providesMask = loaded && settings.Enabled && settings.SunShadows && globals::features::screenSpaceShadows.loaded &&
		               sky && sky->mode.get() == RE::Sky::Mode::kFull && RT::CanTraceSunShadows() && !RT::IsSunShadowSuppressed();
	}
	return providesMask;
}

void SkyrimRT::Prepass()
{
	if (!settings.Enabled)
		return;
	const bool shadows = ProvidesSunShadowMask();
	const bool gi = WantsGlobalIllumination();
	if (!shadows && !gi && !settings.TraceDebugView)
		return;

	// Same sources ScreenSpaceShadows uses in its Prepass: CS's cached per-frame buffer and the
	// dynamic-resolution render size (DLSS/FSR render below output resolution).
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto* graphicsState = globals::game::graphicsState;
	const float2 renderSize = Util::ConvertToDynamic(float2{ static_cast<float>(graphicsState->screenWidth), static_cast<float>(graphicsState->screenHeight) });
	auto matrix = [](const auto& a_matrix) { return reinterpret_cast<const float*>(&a_matrix); };
	RT::CaptureCamera(matrix(frameBuffer.GetCameraViewProjInverse()), matrix(frameBuffer.GetCameraViewProj()), matrix(frameBuffer.GetCameraView()),
		matrix(frameBuffer.GetCameraViewInverse()), matrix(frameBuffer.GetCameraProjUnjittered()), &frameBuffer.GetCameraPosAdjust().x, static_cast<uint32_t>(std::lround(renderSize.x)), static_cast<uint32_t>(std::lround(renderSize.y)), globals::state->frameCount);

	RT::SunShadowParams params;
	const bool traceShadows = shadows && GetDirectionToSun(params.toSun);
	params.coneHalfAngleDegrees = settings.SunAngularRadius;
	params.alphaTestedCasters = settings.AlphaTestedShadows;
	params.normalBias = settings.ShadowNormalBias;
	params.distanceBias = settings.ShadowDistanceBias;
	params.maxHistory = settings.ShadowHistory;
	params.spatialRadius = settings.ShadowSpatialRadius;
	params.viewMode = settings.ShadowView;
	RT::SetAlphaTest(settings.AlphaTest);
	RT::SetTreeRestPose(settings.TreeRestPose);
	RT::OnPrepass(settings.TraceDebugView, traceShadows ? &params : nullptr, gi);

	// Screen-Space Shadows skipped its pass for this frame, so the slot is ours. A mask that couldn't be traced
	// recently comes back cleared to lit.
	if (shadows) {
		if (auto* mask = RT::AcquireSunShadowMask())
			globals::d3d::context->PSSetShaderResources(45, 1, &mask);
	}
}

bool SkyrimRT::WantsGlobalIllumination()
{
	// DeferredCompositeCS reads t10-t12 only when compiled with SSGI, i.e. when Screen-Space GI is loaded.
	// Interiors stay with Screen-Space GI unless GIInteriors asks otherwise.
	return loaded && settings.Enabled && settings.GlobalIllumination && globals::features::screenSpaceGI.loaded &&
	       RT::IsGIAvailable() && !RT::IsSunShadowSuppressed() && (settings.GIInteriors || !Util::IsInterior());
}

bool SkyrimRT::DrawGlobalIllumination(RT::GIOutputs& a_outputs)
{
	if (!WantsGlobalIllumination() || !RT::CanTraceGI())
		return false;
	RT::GIParams params;
	if (!GatherLighting(params))
		return false;
	params.intensity = settings.GIIntensity;
	params.aoStrength = settings.GIAOStrength;
	params.rayLength = settings.GIRayLength;
	params.alphaTestedCasters = settings.GIAlphaTested;
	params.maxAccumulatedFrames = settings.GIHistory;
	params.viewMode = settings.GIView;
	params.interior = Util::IsInterior();
	if (settings.GIPointLights)
		params.pointLights = GatherPointLights(params.linearLighting);
	params.pointLightShadows = settings.GIPointLightShadows;
	params.inverseSquare = globals::features::inverseSquareLighting.loaded;
	a_outputs = RT::SubmitGI(params);
	return a_outputs.ao && a_outputs.y && a_outputs.coCg;
}

void SkyrimRT::Reset()
{
	// Disabled means no D3D12 work and no fence waits at all.
	if (!settings.Enabled)
		return;

	RT::OnFrame();

	const bool keyDown = (GetAsyncKeyState(kDumpHotkey) & 0x8000) != 0;
	if (keyDown && !dumpKeyWasDown && IsGameWindowFocused())
		RT::RequestDebugDump();
	dumpKeyWasDown = keyDown;
}

void SkyrimRT::DrawOverlay()
{
	if (!IsOverlayVisible())
		return;

	constexpr float kMargin = 16.0f;
	const auto& io = ImGui::GetIO();
	constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
	                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

	if (auto* pattern = settings.ShowTestPattern ? RT::GetTestPatternSRV() : nullptr) {
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, kMargin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##SkyrimRTTestPattern", nullptr, kFlags)) {
			ImGui::TextUnformatted(T(TKEY("overlay_title"), "Skyrim RT: DirectX 12 interop test"));
			ImGui::Image((void*)pattern, ImVec2(256.0f, 256.0f));
			if (const auto* stats = RT::GetInteropStats())
				ImGui::Text("%s: %.3f ms", T(TKEY("round_trip_cost"), "Round trip cost"), stats->roundTripMs.Average());
		}
		ImGui::End();
	}

	uint32_t textureWidth, textureHeight, renderWidth, renderHeight;
	auto* view = settings.TraceDebugView ? RT::GetDebugViewSRV(settings.DebugView) : nullptr;
	const auto* trace = RT::GetTraceStats();
	if (view && trace && RT::GetDebugViewSize(textureWidth, textureHeight, renderWidth, renderHeight)) {
		// Show only the render region (dynamic resolution renders into the top-left of the texture).
		constexpr float kWidth = 640.0f;
		const ImVec2 size(kWidth, kWidth * renderHeight / renderWidth);
		const ImVec2 uvMax(static_cast<float>(renderWidth) / textureWidth, static_cast<float>(renderHeight) / textureHeight);
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, io.DisplaySize.y - kMargin), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##SkyrimRTDebugView", nullptr, kFlags)) {
			ImGui::Text("%s: %.2f%% (%s %.1f%%)", T(TKEY("depth_mismatch"), "Depth mismatch"), trace->MismatchPercent(),
				T(TKEY("coverage"), "coverage"), trace->CoveragePercent());
			ImGui::Image((void*)view, size, ImVec2(0.0f, 0.0f), uvMax);
		}
		ImGui::End();
	}

	// Sun-shadow view, bottom-left.
	auto* shadowView = settings.ShadowView != 0 && ProvidesSunShadowMask() ? RT::GetSunShadowViewSRV() : nullptr;
	const auto* shadowStats = RT::GetSunShadowStats();
	if (shadowView && shadowStats && shadowStats->renderWidth > 0 && shadowStats->renderHeight > 0) {
		constexpr float kWidth = 640.0f;
		const ImVec2 size(kWidth, kWidth * shadowStats->renderHeight / shadowStats->renderWidth);
		const ImVec2 uvMax(static_cast<float>(shadowStats->renderWidth) / shadowStats->textureWidth, static_cast<float>(shadowStats->renderHeight) / shadowStats->textureHeight);
		ImGui::SetNextWindowPos(ImVec2(kMargin, io.DisplaySize.y - kMargin), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##SkyrimRTShadowView", nullptr, kFlags)) {
			ImGui::Text("%s: %.1f%% (%.3f ms)", settings.ShadowView == 1 ? T(TKEY("shadow_view_raw"), "RT sun shadow, raw") : T(TKEY("shadow_view_denoised"), "RT sun shadow, denoised"),
				shadowStats->ShadowedPercent(), shadowStats->totalMs.Average());
			ImGui::Image((void*)shadowView, size, ImVec2(0.0f, 0.0f), uvMax);
		}
		ImGui::End();
	}

	// GI view, top-left.
	auto* giView = settings.GIView != 0 ? RT::GetGIViewSRV() : nullptr;
	const auto* giStats = RT::GetGIStats();
	if (giView && giStats && giStats->renderWidth > 0 && giStats->renderHeight > 0) {
		constexpr float kWidth = 640.0f;
		const ImVec2 size(kWidth, kWidth * giStats->renderHeight / giStats->renderWidth);
		const ImVec2 uvMax(static_cast<float>(giStats->renderWidth) / giStats->textureWidth, static_cast<float>(giStats->renderHeight) / giStats->textureHeight);
		ImGui::SetNextWindowPos(ImVec2(kMargin, kMargin), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##SkyrimRTGIView", nullptr, kFlags)) {
			ImGui::Text("%s: %.1f%% %s (%.3f ms)", T(TKEY("gi_section"), "Global illumination"), giStats->HitPercent(), T(TKEY("gi_hit_short"), "hit"), giStats->totalMs.Average());
			ImGui::Image((void*)giView, size, ImVec2(0.0f, 0.0f), uvMax);
		}
		ImGui::End();
	}
}

void SkyrimRT::DrawGlobalIlluminationSettings()
{
	ImGui::Checkbox(T(TKEY("gi"), "Ray-traced global illumination"), &settings.GlobalIllumination);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_tooltip"), "Trace one bounce of sunlight, point lights and ambient light off the scene, with ray-traced ambient occlusion, in place of Screen-Space GI."));

	if (!RT::IsGICompiledIn()) {
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("gi_not_compiled"), "Not in this build: ray-traced GI needs a private build with NVIDIA NRD (SKYRIMRT_NRD)."));
		return;
	}
	if (!globals::features::screenSpaceGI.loaded)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("gi_needs_ssgi"), "Requires the Screen Space GI feature to be installed: the deferred composite reads the result through it."));
	else if (settings.GlobalIllumination && settings.Enabled && !RT::IsGIAvailable())
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("gi_unavailable"), "Unavailable: GI could not be set up. See CommunityShaders.log."));

	ImGui::SliderFloat(T(TKEY("gi_intensity"), "Bounce intensity"), &settings.GIIntensity, 0.0f, 4.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_intensity_tooltip"), "Scales the bounced light. 1 matches the direct lighting."));

	ImGui::SliderFloat(T(TKEY("gi_ao_strength"), "Ambient occlusion strength"), &settings.GIAOStrength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_ao_strength_tooltip"), "How much nearby geometry darkens the game's ambient light."));

	ImGui::SliderFloat(T(TKEY("gi_ray_length"), "Ray length"), &settings.GIRayLength, 250.0f, 10000.0f, "%.0f units");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_ray_length_tooltip"), "How far bounce rays look for surfaces. Longer rays find more distant light but cost more."));

	ImGui::Checkbox(T(TKEY("gi_alpha_tested"), "Alpha-tested meshes bounce and occlude"), &settings.GIAlphaTested);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_alpha_tested_tooltip"), "Include foliage and other alpha-tested meshes. With Alpha-test foliage off, leaves act as solid cards."));

	ImGui::Checkbox(T(TKEY("gi_point_lights"), "Bounce point lights"), &settings.GIPointLights);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_point_lights_tooltip"), "Bounce the light of torches, candles, fires and spells, as Light Limit Fix passes them to the lighting shaders. Interiors are lit mostly by these."));

	ImGui::Checkbox(T(TKEY("gi_point_light_shadows"), "Point lights are occluded"), &settings.GIPointLightShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_point_light_shadows_tooltip"), "Trace a ray to each sampled point light, so walls stop its bounce light. When off, point lights bounce through walls, as the game lights surfaces through them."));

	ImGui::Checkbox(T(TKEY("gi_interiors"), "Ray-traced GI in interiors"), &settings.GIInteriors);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_interiors_tooltip"), "Use ray-traced GI inside buildings and dungeons too. Interiors are lit mostly by point lights, so keep Bounce point lights on. When off, interiors use Screen-Space GI."));

	ImGui::SliderInt(T(TKEY("gi_history"), "Denoiser history (frames)"), reinterpret_cast<int*>(&settings.GIHistory), 1, 63);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_history_tooltip"), "How many frames the denoiser blends. Higher is smoother but reacts more slowly to change."));

	const char* giViewNames[] = { T(TKEY("shadow_view_off"), "Off"), T(TKEY("gi_view_noisy"), "Bounce light, noisy"), T(TKEY("gi_view_denoised"), "Bounce light, denoised"), T(TKEY("gi_view_ao"), "Ambient occlusion") };
	int giView = static_cast<int>(std::min<uint32_t>(settings.GIView, 3));
	if (ImGui::Combo(T(TKEY("gi_view"), "GI debug view"), &giView, giViewNames, IM_ARRAYSIZE(giViewNames)))
		settings.GIView = static_cast<uint32_t>(giView);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("gi_view_tooltip"), "Show the ray-traced GI in the top-left corner."));

	if (const auto* stats = RT::GetGIStats(); stats && stats->haveResult) {
		ImGui::Text("%s: %.1f%% (%s %.1f%%)", T(TKEY("gi_hits"), "Rays hitting geometry"), stats->HitPercent(), T(TKEY("gi_sunlit"), "sunlit"), stats->SunLitHitPercent());
		ImGui::Text("%s: %u (%s %.1f%%, %s %.1f%%)", T(TKEY("gi_point_light_count"), "Point lights"), stats->pointLights, T(TKEY("gi_point_light_sampled"), "hits in range"),
			stats->LightSampledHitPercent(), T(TKEY("gi_point_light_occluded"), "occluded"), stats->LightOccludedPercent());
		ImGui::Text("%s: %.3f / %.3f / %.3f ms", T(TKEY("gi_timings"), "Trace / denoise / resolve"), stats->traceMs.Average(), stats->denoiseMs.Average(), stats->resolveMs.Average());
		ImGui::Text("%s: %.3f ms", T(TKEY("gi_frame_cost"), "Frame cost (GI hand-off)"), stats->roundTripMs.Average());
		if (const auto* interop = RT::GetInteropStats())
			ImGui::Text("%s: %.2f ms", T(TKEY("frame_time"), "Frame time (toggle a feature to compare)"), interop->frameMs.Average());
	}
}

void SkyrimRT::DrawSunShadowSettings()
{
	ImGui::Checkbox(T(TKEY("sun_shadows"), "Ray-traced sun shadows"), &settings.SunShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("sun_shadows_tooltip"), "Trace sun and moon shadows from the scene, characters and foliage in exteriors. They take the place of Screen-Space Shadows and combine with the game's shadow maps, which still provide shadows from grass and distant land."));

	if (!globals::features::screenSpaceShadows.loaded)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("sun_shadows_needs_sss"), "Requires the Screen-Space Shadows feature to be installed: the lighting shaders read the shadow mask through it."));
	else if (settings.SunShadows && settings.Enabled && !RT::CanTraceSunShadows())
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", T(TKEY("sun_shadows_unavailable"), "Unavailable: the ray tracing pipeline could not be set up. See CommunityShaders.log."));

	ImGui::SliderFloat(T(TKEY("sun_angular_radius"), "Sun angular radius"), &settings.SunAngularRadius, 0.0f, 3.0f, "%.2f deg");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("sun_angular_radius_tooltip"), "Apparent size of the sun. Larger values give wider, softer shadow edges. The real sun is about 0.27 degrees."));

	ImGui::Checkbox(T(TKEY("alpha_tested_shadows"), "Alpha-tested meshes cast shadows"), &settings.AlphaTestedShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("alpha_tested_shadows_tooltip"), "Let foliage and other alpha-tested meshes cast ray-traced shadows. With Alpha-test foliage off, leaves cast solid shadows."));

	ImGui::SliderInt(T(TKEY("shadow_history"), "Temporal history (frames)"), reinterpret_cast<int*>(&settings.ShadowHistory), 1, 64);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shadow_history_tooltip"), "How many frames are blended to remove noise. Higher is smoother but reacts more slowly. 1 turns temporal filtering off."));

	ImGui::SliderFloat(T(TKEY("shadow_spatial_radius"), "Spatial filter radius"), &settings.ShadowSpatialRadius, 0.0f, 8.0f, "%.1f px");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shadow_spatial_radius_tooltip"), "Blur radius for the shadow edges, reduced as more history accumulates. 0 turns spatial filtering off."));

	ImGui::SliderFloat(T(TKEY("shadow_normal_bias"), "Normal bias"), &settings.ShadowNormalBias, 0.0f, 8.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shadow_normal_bias_tooltip"), "Offsets each shadow ray away from its surface, in game units, to prevent surfaces from shadowing themselves."));

	ImGui::SliderFloat(T(TKEY("shadow_distance_bias"), "Distance bias"), &settings.ShadowDistanceBias, 0.0f, 0.01f, "%.4f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shadow_distance_bias_tooltip"), "Extra ray offset that grows with distance from the camera, where depth precision is lower."));

	const char* shadowViewNames[] = { T(TKEY("shadow_view_off"), "Off"), T(TKEY("shadow_view_raw"), "RT sun shadow, raw"), T(TKEY("shadow_view_denoised"), "RT sun shadow, denoised") };
	int shadowView = static_cast<int>(std::min<uint32_t>(settings.ShadowView, 2));
	if (ImGui::Combo(T(TKEY("shadow_view"), "Shadow debug view"), &shadowView, shadowViewNames, IM_ARRAYSIZE(shadowViewNames)))
		settings.ShadowView = static_cast<uint32_t>(shadowView);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("%s", T(TKEY("shadow_view_tooltip"), "Show the ray-traced shadow mask in the bottom-left corner: white is lit, black is shadowed."));

	if (const auto* stats = RT::GetSunShadowStats(); stats && stats->haveResult) {
		ImGui::Text("%s: %s", T(TKEY("shadow_active"), "Active this frame"), ProvidesSunShadowMask() ? T(TKEY("yes"), "yes") : T(TKEY("no"), "no"));
		ImGui::Text("%s: %.1f%%", T(TKEY("shadowed_pixels"), "Shadowed pixels (raw)"), stats->ShadowedPercent());
		ImGui::Text("%s: %.3f / %.3f / %.3f ms", T(TKEY("shadow_timings"), "Trace / temporal / spatial"), stats->traceMs.Average(), stats->temporalMs.Average(), stats->spatialMs.Average());
		if (const auto* interop = RT::GetInteropStats())
			ImGui::Text("%s: %.3f ms", T(TKEY("frame_cost"), "Frame cost (whole ray tracing round trip)"), interop->roundTripMs.Average());
	}
}

void SkyrimRT::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Run the DirectX 12 device each frame. When off, Skyrim RT does no work at all."));

		ImGui::Checkbox(T(TKEY("show_test_pattern"), "Show interop test pattern"), &settings.ShowTestPattern);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("show_test_pattern_tooltip"), "Show the animated image written by DirectX 12 in the top-right corner."));

		ImGui::Checkbox(T(TKEY("trace_debug_view"), "Trace debug view"), &settings.TraceDebugView);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("trace_debug_view_tooltip"), "Build ray tracing acceleration structures and trace a debug view of the static scene every frame, shown in the bottom-right corner."));

		const char* viewNames[] = { T(TKEY("view_depth"), "Traced depth"), T(TKEY("view_instance"), "Instance ID"), T(TKEY("view_normal"), "Geometric normal"), T(TKEY("view_diff"), "Depth mismatch vs raster") };
		int view = static_cast<int>(std::min<uint32_t>(settings.DebugView, 3));
		if (ImGui::Combo(T(TKEY("debug_view"), "Debug view"), &view, viewNames, IM_ARRAYSIZE(viewNames)))
			settings.DebugView = static_cast<uint32_t>(view);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("debug_view_tooltip"), "Mismatch colours: green match, red traced nearer, blue traced farther, yellow traced miss, grey excluded (geometry the ray tracing scene doesn't contain), pink excluded (foliage not alpha-tested yet), ochre excluded (wind-animated foliage), teal excluded (grass and ground clutter), dark grey outside the loaded cells."));

		ImGui::Checkbox(T(TKEY("alpha_test"), "Alpha-test foliage"), &settings.AlphaTest);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("alpha_test_tooltip"), "Trace foliage and other alpha-tested meshes with their transparency, from a low-resolution copy of each texture's alpha. When off, their leaves act as solid cards."));
		if (const auto* atlas = RT::GetAlphaAtlasStats(); atlas && atlas->available && settings.AlphaTest)
			ImGui::Text("%s: %u / %u (%u / %u)", T(TKEY("alpha_atlas_status"), "Alpha atlas tiles (alpha-tested meshes)"), atlas->tilesUsed, atlas->capacity, atlas->candidatesTested, atlas->candidates);

		ImGui::Checkbox(T(TKEY("tree_rest_pose"), "Trace trees in their rest pose"), &settings.TreeRestPose);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("tree_rest_pose_tooltip"), "Trace trees without their sway. Every camera that culls a tree (the view, the shadow maps, Skylighting) re-poses its branches, so the swaying pose at tracing time can belong to another camera and jump away from the drawn tree."));

		ImGui::BeginDisabled(!settings.Enabled || !RT::IsRunning());
		if (ImGui::Button(T(TKEY("write_dump"), "Write debug dump (F10)")))
			RT::RequestDebugDump();
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("write_dump_tooltip"), "Writes frame_<n>.json and PNGs of the debug views, the shadow masks and the final frame with ray-traced shadows on and off to Documents\\My Games\\Skyrim Special Edition\\SKSE\\SkyrimRT."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("sun_shadows_section"), "Sun shadows"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawSunShadowSettings();
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("gi_section"), "Global illumination"), ImGuiTreeNodeFlags_DefaultOpen)) {
		DrawGlobalIlluminationSettings();
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("device"), "Device"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto& caps = RT::GetCapabilities();
		ImGui::Text("%s: %s", T(TKEY("adapter"), "Adapter"), caps.adapterName.c_str());
		ImGui::Text("%s: %s", T(TKEY("adapter_luid"), "Adapter LUID"), RT::FormatLuid(caps.adapterLuid).c_str());
		ImGui::Text("%s: %s", T(TKEY("raytracing_tier"), "DirectX Raytracing tier"), RT::GetTierName(caps.raytracingTier).c_str());

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("interop"), "Interop"), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (const auto* stats = RT::GetInteropStats()) {
			ImGui::Text("%s: %.3f ms (%s %.3f ms)", T(TKEY("round_trip_cost"), "Round trip cost"), stats->roundTripMs.Average(), T(TKEY("max"), "max"), stats->roundTripMs.Max());
			ImGui::Text("%s: %.3f ms", T(TKEY("d3d12_dispatch"), "DirectX 12 dispatch"), stats->d3d12DispatchMs.Average());
			ImGui::Text("%s: %.3f ms", T(TKEY("cpu_submit"), "CPU submit"), stats->cpuSubmitMs.Average());
			ImGui::Text("%s: %u / %u", T(TKEY("frames_submitted_skipped"), "Frames submitted / skipped"), stats->framesSubmitted, stats->framesSkipped);
			if (stats->deviceRemoved)
				ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", T(TKEY("device_removed"), "Device removed: interop stopped. See CommunityShaders.log."));
		}
		if (const auto* spike = RT::GetSpikeResults()) {
			ImGui::SeparatorText(T(TKEY("sharing_spike"), "Resource sharing"));
			ImGui::Text("%s: %s", T(TKEY("texture_created_in"), "Test texture created in"), spike->textureCreatedInD3D12 ? "DirectX 12" : "DirectX 11");
			DrawResult(T(TKEY("buffer_12_to_11"), "Buffer DirectX 12 -> 11"), spike->bufferD3D12ToD3D11Open);
			ImGui::Text("%s: %s", T(TKEY("buffer_12_to_11_data"), "Buffer DirectX 12 -> 11 data"), spike->bufferD3D12ToD3D11Verified ? T(TKEY("verified"), "verified") : T(TKEY("not_verified"), "not verified"));
			DrawResult(T(TKEY("buffer_11_to_12"), "Buffer DirectX 11 -> 12"), spike->bufferD3D11ToD3D12Open);
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("scene"), "Scene and mesh cache"), ImGuiTreeNodeFlags_DefaultOpen)) {
		const auto* scene = RT::GetSceneStats();
		const auto* traversal = RT::GetSceneTraversalMs();
		const auto* cache = RT::GetMeshCacheStats();
		if (scene && traversal && cache) {
			using C = RT::GeometryCategory;
			auto count = [&](C a_category) { return scene->instances[static_cast<size_t>(a_category)]; };
			ImGui::Text("%s: %u", T(TKEY("cells"), "Loaded cells"), scene->cells);
			ImGui::Text("%s: %u (%u %s)", T(TKEY("static_meshes"), "Static mesh instances"), count(C::kStaticMesh), scene->uniqueStaticMeshes, T(TKEY("unique"), "unique"));
			ImGui::Text("%s: %u (%u %s)", T(TKEY("terrain"), "Terrain instances"), count(C::kTerrain), scene->uniqueTerrainMeshes, T(TKEY("unique"), "unique"));
			ImGui::Text("%s: %u / %u / %u / %u", T(TKEY("skipped_types"), "Skipped: skinned / dynamic / instanced / LOD"), count(C::kSkinned), count(C::kDynamic), count(C::kInstanced), count(C::kLOD));
			ImGui::Text("%s: %u (%u %s, %u %s)", T(TKEY("skinned_shapes"), "Skinned shapes traced"), scene->skinnedShapes, scene->skinnedPartitions, T(TKEY("partitions"), "partitions"), scene->skinnedRejectedShapes, T(TKEY("rejected"), "rejected"));
			if (const auto* raytracer = RT::GetRaytracerSkinnedStats())
				ImGui::Text("%s: %u / %.3f ms", T(TKEY("skinned_instances"), "Skinned TLAS instances / skinning + refit"), raytracer->instances, raytracer->skinMs.Average());
			ImGui::Text("%s: %.3f ms", T(TKEY("traversal"), "Scene traversal"), traversal->Average());
			ImGui::Text("%s: %u / %u (%u %s)", T(TKEY("cache_entries"), "Cache resident / entries"), cache->resident, cache->entries, cache->pending, T(TKEY("pending"), "pending"));
			ImGui::Text("%s: %.1f MB", T(TKEY("cache_gpu"), "Cache GPU memory"), (cache->residentVertexBytes + cache->residentIndexBytes) / (1024.0 * 1024.0));
			ImGui::Text("%s: %u / %u", T(TKEY("cache_frame"), "Uploads / evictions last frame"), cache->uploadsLastFrame, cache->evictionsLastFrame);
			ImGui::Text("%s: %u / %u", T(TKEY("cache_sources"), "Uploaded from CPU copy / GPU readback"), cache->sourceRawCpu, cache->sourceD3D11Readback);
			ImGui::Text("%s: %u (%u %s)", T(TKEY("blas_built"), "Acceleration structures built"), cache->blasBuilt, cache->blasPending, T(TKEY("pending"), "pending"));
		}
		if (const auto* trace = RT::GetTraceStats(); trace && trace->haveResult) {
			ImGui::Text("%s: %.2f%% (%s %.1f%%)", T(TKEY("depth_mismatch"), "Depth mismatch"), trace->MismatchPercent(), T(TKEY("coverage"), "coverage"), trace->CoveragePercent());
			ImGui::Text("%s: %u / %u", T(TKEY("tlas_instances"), "TLAS instances / exclusion bounds"), trace->instances, trace->exclusions);
			ImGui::Text("%s: %.3f / %.3f / %.3f ms", T(TKEY("rt_timings"), "BLAS / TLAS / trace"), trace->blasBuildMs.Average(), trace->tlasBuildMs.Average(), trace->traceMs.Average());
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void SkyrimRT::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SkyrimRT::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SkyrimRT::RestoreDefaultSettings()
{
	settings = {};
}
#undef I18N_KEY_PREFIX
