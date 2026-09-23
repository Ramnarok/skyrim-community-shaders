#include "SkyrimRT.h"

#include "I18n/I18n.h"
#include "RT/RT.h"

#define I18N_KEY_PREFIX "feature.skyrim_rt."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkyrimRT::Settings,
	Enabled)

void SkyrimRT::SetupResources()
{
	// The D3D11 device exists by now, so the adapter the game renders on is known.
	if (!RT::Init(globals::d3d::device)) {
		const auto& caps = RT::GetCapabilities();
		loaded = false;
		failedLoadedMessage = caps.probed ?
		                          std::format("Disabled: {} supports DirectX Raytracing tier {}, tier {} is required.", caps.adapterName, RT::GetTierName(caps.raytracingTier), RT::GetTierName(RT::kRequiredTier)) :
		                          std::format("Disabled: raytracing support could not be detected ({}).", caps.failureReason);
		logger::warn("[SkyrimRT] {}", failedLoadedMessage);
		return;
	}

	logger::info("[SkyrimRT] DXR {} available, feature active (enabled setting: {})", RT::GetTierName(RT::kRequiredTier), settings.Enabled);
}

void SkyrimRT::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable ray-traced lighting. Nothing is ray traced yet in this build."));

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
