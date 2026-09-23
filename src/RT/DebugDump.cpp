#include "DebugDump.h"

#include <DirectXTex.h>

namespace RT
{
	namespace
	{
		json TimingJson(const TimingSeries& a_series)
		{
			return { { "avg", a_series.Average() }, { "max", a_series.Max() }, { "samples", a_series.count } };
		}

		std::string VertexFlagNames(uint16_t a_flags)
		{
			using V = RE::BSGraphics::Vertex;
			constexpr std::pair<uint16_t, const char*> kNames[] = {
				{ V::VF_VERTEX, "VERTEX" }, { V::VF_UV, "UV" }, { V::VF_UV_2, "UV2" }, { V::VF_NORMAL, "NORMAL" },
				{ V::VF_TANGENT, "TANGENT" }, { V::VF_COLORS, "COLORS" }, { V::VF_SKINNED, "SKINNED" }, { V::VF_LANDDATA, "LANDDATA" },
				{ V::VF_EYEDATA, "EYEDATA" }, { V::VF_INSTANCEDATA, "INSTANCEDATA" }, { V::VF_FULLPREC, "FULLPREC" }
			};
			std::string names;
			for (const auto& [bit, name] : kNames) {
				if (a_flags & bit)
					names += names.empty() ? name : std::string("|") + name;
			}
			return names;
		}

		json SceneJson(const DebugDumpData& a_data)
		{
			const auto& s = a_data.scene;
			json instances = json::object();
			for (size_t i = 0; i < s.instances.size(); i++)
				instances[std::string(GetCategoryName(static_cast<GeometryCategory>(i)))] = s.instances[i];
			return {
				{ "in_world", a_data.inWorld },
				{ "cells", s.cells },
				{ "hidden_subtrees_skipped", s.hiddenSubtrees },
				{ "geometry_instances", instances },
				{ "alpha_tested_instances", s.alphaTestedInstances },
				{ "unique_meshes", { { "static_mesh", s.uniqueStaticMeshes }, { "terrain", s.uniqueTerrainMeshes } } },
				{ "traversal_ms", TimingJson(a_data.sceneTraversalMs) },
			};
		}

		json CacheJson(const MeshCacheStats& c)
		{
			json formats = json::array();
			for (const auto& f : c.formats) {
				formats.push_back({ { "flags", std::format("{:#06x}", f.flags) },
					{ "flag_names", VertexFlagNames(f.flags) },
					{ "stride_from_desc", f.strideFromDesc },
					{ "stride_from_getsize", f.strideFromGetSize },
					{ "vb_bytes_per_vertex", f.vbBytesPerVertex },
					{ "meshes", f.meshes } });
			}
			constexpr double kMB = 1024.0 * 1024.0;
			return {
				{ "entries", c.entries },
				{ "resident", c.resident },
				{ "pending", c.pending },
				{ "resident_vb_mb", c.residentVertexBytes / kMB },
				{ "resident_ib_mb", c.residentIndexBytes / kMB },
				{ "pool_reserved_mb", c.poolBytes / kMB },
				{ "pool_pages", c.poolPages },
				{ "per_frame",
					{ { "last_frame", { { "uploads", c.uploadsLastFrame }, { "evictions", c.evictionsLastFrame }, { "readbacks_started", c.readbacksLastFrame }, { "upload_mb", c.uploadBytesLastFrame / kMB } } },
						{ "uploads", TimingJson(c.uploadsPerFrame) },
						{ "evictions", TimingJson(c.evictionsPerFrame) },
						{ "upload_mb", TimingJson(c.uploadMBPerFrame) },
						{ "update_ms", TimingJson(c.updateMs) } } },
				{ "totals", { { "uploads", c.totalUploads }, { "evictions", c.totalEvictions }, { "readbacks", c.totalReadbacks }, { "failed", c.totalFailed }, { "deferred", c.totalDeferred } } },
				{ "upload_source", { { "raw_cpu", c.sourceRawCpu }, { "d3d11_readback", c.sourceD3D11Readback } } },
				{ "raw_pointers_per_new_mesh", { { "both", c.rawBoth }, { "vertex_only", c.rawVertexOnly }, { "index_only", c.rawIndexOnly }, { "neither", c.rawNeither } } },
				{ "raw_vs_gpu_check", { { "compared", c.rawCompared }, { "matched", c.rawMatched }, { "mismatched", c.rawMismatched }, { "copy_faults", c.rawCopyFaults } } },
				{ "stride_check", { { "vb_size_matches", c.strideMatchesVB }, { "vb_size_mismatches", c.strideMismatchesVB }, { "ib_size_matches", c.indexBytesMatchIB }, { "ib_size_mismatches", c.indexBytesMismatchIB } } },
				{ "vertex_formats", formats },
			};
		}

		json BuildJson(const DebugDumpData& a_data, const std::string& a_pngName, bool a_pngWritten)
		{
			const auto& s = a_data.stats;
			const auto& sp = a_data.spike;
			const float cost = s.roundTripMs.Average();
			const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

			return {
				{ "milestone", "M3" },
				{ "scene", SceneJson(a_data) },
				{ "mesh_cache", CacheJson(a_data.cache) },
				{ "frame", a_data.gameFrame },
				{ "written_utc", std::format("{:%FT%TZ}", now) },
				{ "adapter", { { "name", a_data.caps.adapterName }, { "luid", FormatLuid(a_data.caps.adapterLuid) }, { "dxr_tier", GetTierName(a_data.caps.raytracingTier) } } },
				{ "frames", { { "submitted", s.framesSubmitted }, { "skipped_slot_busy", s.framesSkipped }, { "last_signaled_fence", s.lastSignaledFenceValue }, { "last_completed_fence", s.lastCompletedFenceValue } } },
				{ "timings_ms", { { "d3d11_round_trip", TimingJson(s.roundTripMs) }, { "d3d12_dispatch", TimingJson(s.d3d12DispatchMs) }, { "cpu_submit", TimingJson(s.cpuSubmitMs) } } },
				{ "cost_ms", cost },
				{ "budget_ms", kInteropBudgetMs },
				{ "within_budget", s.roundTripMs.count > 0 && cost < kInteropBudgetMs },
				{ "device_removed", { { "any", s.deviceRemoved }, { "d3d12", FormatHResult(s.d3d12RemovedReason) }, { "d3d11", FormatHResult(s.d3d11RemovedReason) } } },
				{ "interop_spike",
					{ { "ran", sp.ran },
						{ "texture_d3d12_to_d3d11", FormatHResult(sp.textureD3D12ToD3D11) },
						{ "texture_d3d11_to_d3d12", FormatHResult(sp.textureD3D11ToD3D12) },
						{ "texture_created_in", sp.textureCreatedInD3D12 ? "D3D12" : "D3D11" },
						{ "buffer_d3d12_to_d3d11_open", FormatHResult(sp.bufferD3D12ToD3D11Open) },
						{ "buffer_d3d12_to_d3d11_data_verified", sp.bufferD3D12ToD3D11Verified },
						{ "buffer_d3d12_to_d3d11_mismatches", sp.bufferMismatches },
						{ "buffer_d3d11_create_shared", FormatHResult(sp.bufferD3D11Create) },
						{ "buffer_d3d11_to_d3d12_open", FormatHResult(sp.bufferD3D11ToD3D12Open) } } },
				{ "test_pattern",
					{ { "width", a_data.width },
						{ "height", a_data.height },
						{ "pattern_frame", a_data.patternFrame },
						{ "verified_this_frame", a_data.patternVerified },
						{ "png", a_pngWritten ? a_pngName : "" } } },
			};
		}
	}

	void WriteDebugDumpAsync(DebugDumpData a_data)
	{
		std::thread([data = std::move(a_data)]() {
			const auto dir = GetDumpDirectory();
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			if (ec) {
				logger::error("[SkyrimRT] Debug dump: cannot create {}: {}", dir.string(), ec.message());
				return;
			}

			// WIC needs COM on this thread.
			const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

			const auto pngName = std::format("debug_testpattern_{}.png", data.gameFrame);
			DirectX::Image image{};
			image.width = data.width;
			image.height = data.height;
			image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			image.rowPitch = static_cast<size_t>(data.width) * 4;
			image.slicePitch = image.rowPitch * data.height;
			image.pixels = const_cast<uint8_t*>(data.pixels.data());
			const auto pngPath = dir / pngName;
			const HRESULT pngHr = DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), pngPath.c_str());
			if (FAILED(pngHr))
				logger::error("[SkyrimRT] Debug dump: writing {} failed ({})", pngPath.string(), FormatHResult(pngHr));

			if (SUCCEEDED(comHr))
				CoUninitialize();

			const auto jsonPath = dir / std::format("frame_{}.json", data.gameFrame);
			std::ofstream out(jsonPath);
			if (!out) {
				logger::error("[SkyrimRT] Debug dump: cannot open {}", jsonPath.string());
				return;
			}
			out << BuildJson(data, pngName, SUCCEEDED(pngHr)).dump(2) << '\n';
			logger::info("[SkyrimRT] Debug dump written: {} (pattern verified: {})", jsonPath.string(), data.patternVerified);
		}).detach();
	}
}
