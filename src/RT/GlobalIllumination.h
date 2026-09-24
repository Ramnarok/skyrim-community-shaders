#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "FrameTypes.h"
#include "RT.h"
#include "SharedTexture.h"

namespace RT
{
	/** @brief Counter slots written by GITraceCS.hlsl (keep in sync with GICommon.hlsli). */
	enum GICounter : uint32_t
	{
		kGITraced,
		kGIHits,
		kGISunLitHits,
		kGILightSampled,   ///< hits that picked a point light (one within range, facing the hit)
		kGILightOccluded,  ///< ... whose visibility ray was blocked
		kGIOccluderNear32,   ///< ... by something within 32 units of the light (its own fixture?)
		kGIOccluderNear64,   ///< ... 32-64 units from it
		kGIOccluderNear128,  ///< ... 64-128 units from it (the rest is farther: walls)
		kGISkyVisible,       ///< M8: misses whose continuation to 50,000 units reached the sky (sky light on)
		kGITexturedHits,     ///< M8: hits shaded with their texture from the albedo atlas (else the average albedo)
		kGIDeeperHits,       ///< M8 multi-bounce: hits of continuation rays (second bounce and deeper)
		kGICounterCount
	};

	struct GIStats
	{
		bool haveResult = false;
		uint64_t framesTraced = 0;
		uint64_t historyResets = 0;
		std::array<uint32_t, kGICounterCount> counters{};
		uint32_t nrdDispatches = 0;
		uint32_t textureWidth = 0;
		uint32_t textureHeight = 0;
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
		GIParams params;           ///< pointLights is cleared (the span doesn't outlive SubmitGI)
		uint32_t pointLights = 0;  ///< point lights uploaded for the bounce last frame
		uint32_t pointLightsDropped = 0;  ///< beyond kMaxPointLights
		std::vector<PointLight> lastPointLights;  ///< last frame's uploaded lights, for the dump
		TimingSeries traceMs;
		TimingSeries denoiseMs;
		TimingSeries resolveMs;
		TimingSeries totalMs;
		TimingSeries roundTripMs;  ///< D3D11 GPU timeline of the GI hand-off: its frame-time cost

		float HitPercent() const { return counters[kGITraced] ? 100.0f * counters[kGIHits] / counters[kGITraced] : 0.0f; }
		float SunLitHitPercent() const { return counters[kGIHits] ? 100.0f * counters[kGISunLitHits] / counters[kGIHits] : 0.0f; }
		float LightSampledHitPercent() const { return counters[kGIHits] ? 100.0f * counters[kGILightSampled] / counters[kGIHits] : 0.0f; }
		float LightOccludedPercent() const { return counters[kGILightSampled] ? 100.0f * counters[kGILightOccluded] / counters[kGILightSampled] : 0.0f; }
	};
}

#if defined(SKYRIMRT_NRD)

#	include "BufferPool.h"
#	include "SkinnedMeshes.h"
#	include "NrdDenoiser.h"

namespace RT
{
	/**
	 * @brief M6 one-bounce diffuse GI: a RayQuery trace from the finished G-buffer (reusing this frame's TLAS), NRD
	 * REBLUR_DIFFUSE, and a resolve into the three textures DeferredCompositeCS reads from Screen-Space GI.
	 * Recorded into the frame's second D3D11 -> D3D12 hand-off, from Deferred::DeferredPasses.
	 */
	class GlobalIllumination
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kMeshPageSlots = 64;
		static constexpr float kUnitsPerMeter = 70.0f;           ///< Skyrim units; NRD's defaults are in meters
		static constexpr float kDenoisingRange = 400000.0f;     ///< game units; farther pixels (and sky) are ignored
		static constexpr float kSkyViewZ = 10000000.0f;
		static constexpr uint32_t kMaxPointLights = 1024;  ///< LightLimitFix::MAX_LIGHTS

		/** @param a_alphaAtlas M7c alpha atlas, or nullptr (then nothing is alpha-tested). @param a_albedoAtlas M8 albedo atlas, or nullptr (average albedo). */
		bool Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context,
			uint32_t a_width, uint32_t a_height, ID3D12Resource* a_rasterDepth, ID3D12Resource* a_alphaAtlas, ID3D12Resource* a_albedoAtlas);
		const std::string& GetFailureReason() const { return failureReason; }
		void SetTimestampFrequency(uint64_t a_frequency) { timestampFrequency = a_frequency; }

		/**
		 * @brief D3D11 side, before the fence signal: copies the G-buffer normals and motion vectors into shared
		 * textures (created on first use with the game targets' formats). False if they aren't available.
		 */
		bool CopyInputs();

		/** @brief Records trace, REBLUR and resolve. The TLAS and instance data must be this frame's (Prepass round trip). */
		void Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
			const BufferPool& a_meshPool, const SkinnedMeshes* a_skinned, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight,
			const GIParams& a_params, bool a_captureDump);

		void CollectResults(uint32_t a_slot);
		void ReadDumpImages(std::vector<DumpImage>& a_out);

		GIOutputs GetOutputs() const { return { ao.srv11.get(), y.srv11.get(), coCg.srv11.get() }; }
		ID3D11ShaderResourceView* GetViewSRV() const { return view.srv11.get(); }
		GIStats& GetStats() { return stats; }
		const GIStats& GetStats() const { return stats; }

	private:
		bool Fail(std::string a_reason);
		bool CreateTexture(DXGI_FORMAT a_format, const wchar_t* a_name, winrt::com_ptr<ID3D12Resource>& a_out);
		bool CreatePipelines();
		void WriteDescriptors();
		void FillNrdSettings(const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const GIParams& a_params,
			bool a_historyValid, nrd::CommonSettings& a_common, nrd::ReblurSettings& a_reblur) const;

		ID3D12Device5* device = nullptr;
		ID3D11Device5* d3d11Device = nullptr;
		ID3D11DeviceContext4* d3d11Context = nullptr;
		ID3D12Resource* rasterDepth = nullptr;
		ID3D12Resource* alphaAtlas = nullptr;
		ID3D12Resource* albedoAtlas = nullptr;  // M8
		uint32_t width = 0;
		uint32_t height = 0;

		// Inputs copied from the game (created on first use to match the game targets).
		SharedTexture gbufferNormal;
		SharedTexture motionVectors;
		// NRD inputs/outputs (D3D12 only, resting in NON_PIXEL_SHADER_RESOURCE).
		winrt::com_ptr<ID3D12Resource> viewZ;
		winrt::com_ptr<ID3D12Resource> normalRoughness;
		winrt::com_ptr<ID3D12Resource> nrdMotionVectors;
		winrt::com_ptr<ID3D12Resource> noisy;
		winrt::com_ptr<ID3D12Resource> denoised;
		// Composite inputs (shared, resting in COMMON).
		SharedTexture ao;
		SharedTexture y;
		SharedTexture coCg;
		SharedTexture view;

		NrdDenoiser denoiser;

		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> tracePipeline;
		winrt::com_ptr<ID3D12PipelineState> resolvePipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;  // [0..63] mesh pages, then the trace and resolve tables
		uint32_t descriptorSize = 0;
		std::array<uint64_t, kMeshPageSlots> describedPageSerials{};

		winrt::com_ptr<ID3D12Resource> uploads[kFramesInFlight];  // constants + zeros
		uint8_t* uploadCpu[kFramesInFlight]{};
		winrt::com_ptr<ID3D12Resource> counters;
		winrt::com_ptr<ID3D12Resource> countersReadback;
		const uint32_t* countersCpu = nullptr;
		winrt::com_ptr<ID3D12QueryHeap> timestamps;
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampCpu = nullptr;
		uint64_t timestampFrequency = 0;
		bool slotPending[kFramesInFlight]{};
		uint32_t slotDispatches[kFramesInFlight]{};

		winrt::com_ptr<ID3D12Resource> dumpReadback;
		std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 3> dumpFootprints{};
		uint32_t dumpWidth = 0;
		uint32_t dumpHeight = 0;
		bool dumpCaptured = false;

		// Previous traced frame, for NRD's reprojection.
		bool haveHistory = false;
		bool everCleared = false;
		uint32_t historyGameFrame = 0;
		float prevView[16]{};
		float prevProj[16]{};
		RE::NiPoint3 prevPosAdjust;
		uint32_t prevRenderWidth = 0;
		uint32_t prevRenderHeight = 0;
		uint32_t frameIndex = 0;

		GIStats stats;
		std::string failureReason;
	};
}

#endif
