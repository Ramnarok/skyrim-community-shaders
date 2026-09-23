#include "SkyrimRT.h"

#include "I18n/I18n.h"
#include "RT/RT.h"

#define I18N_KEY_PREFIX "feature.skyrim_rt."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SkyrimRT::Settings,
	Enabled,
	ShowTestPattern)

namespace
{
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
	auto* srv = RT::GetTestPatternSRV();
	if (!srv)
		return;

	constexpr float kMargin = 16.0f;
	const auto& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, kMargin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
	                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
	if (ImGui::Begin("##SkyrimRTTestPattern", nullptr, kFlags)) {
		ImGui::TextUnformatted(T(TKEY("overlay_title"), "Skyrim RT: DirectX 12 interop test"));
		ImGui::Image((void*)srv, ImVec2(256.0f, 256.0f));
		if (const auto* stats = RT::GetInteropStats())
			ImGui::Text("%s: %.3f ms", T(TKEY("round_trip_cost"), "Round trip cost"), stats->roundTripMs.Average());
	}
	ImGui::End();
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

		ImGui::BeginDisabled(!settings.Enabled || !RT::IsRunning());
		if (ImGui::Button(T(TKEY("write_dump"), "Write debug dump (F10)")))
			RT::RequestDebugDump();
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("write_dump_tooltip"), "Writes frame_<n>.json and a PNG of the test pattern to Documents\\My Games\\Skyrim Special Edition\\SKSE\\SkyrimRT."));

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
