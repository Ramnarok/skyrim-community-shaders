#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "BufferPool.h"
#include "FrameTypes.h"
#include "RT.h"
#include "SharedTexture.h"

namespace RT
{
	/** @brief Counter slots written by SunShadowTraceCS.hlsl (keep in sync with SunShadowCommon.hlsli). */
	enum ShadowCounter : uint32_t
	{
		kShadowTraced,
		kShadowShadowed,
		kCompareCompared,
		kCompareBothLit,
		kCompareBothShadowed,
		kCompareRtOnly,
		kCompareMapOnly,
		kShadowCounterCount
	};

	struct SunShadowStats
	{
		bool haveResult = false;
		uint64_t framesTraced = 0;
		uint64_t historyResets = 0;  ///< traced frames that started without usable history (first frame, gaps, resize)
		std::array<uint32_t, kShadowCounterCount> counters{};
		std::array<uint32_t, kShadowCounterCount> comparedCounters{};  ///< counters of the last frame that compared against the game's shadow mask
		bool haveComparison = false;
		float toSun[3]{};
		float coneHalfAngleDegrees = 0.0f;
		uint32_t textureWidth = 0;
		uint32_t textureHeight = 0;
		uint32_t renderWidth = 0;  ///< region of the mask written by the last traced frame
		uint32_t renderHeight = 0;
		TimingSeries traceMs;
		TimingSeries temporalMs;
		TimingSeries spatialMs;
		TimingSeries totalMs;
		TimingSeries shadowedPercent;

		float ShadowedPercent() const { return counters[kShadowTraced] ? 100.0f * counters[kShadowShadowed] / counters[kShadowTraced] : 0.0f; }
		float AgreementPercent() const
		{
			const auto& c = comparedCounters;
			return c[kCompareCompared] ? 100.0f * (c[kCompareBothLit] + c[kCompareBothShadowed]) / c[kCompareCompared] : 0.0f;
		}
	};

	/**
	 * @brief M5 ray-traced sun shadows: trace (1 cone-jittered ray per pixel from the depth pre-pass), temporal
	 * accumulation and a spatial filter, producing an R8 visibility mask shared with D3D11. The mask replaces the
	 * Screen-Space Shadows input at PS t45 and multiplies the game's shadow-map term in Lighting.hlsl.
	 */
	class SunShadows
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr float kMaxRayDistance = 50000.0f;
		static constexpr float kDepthTolerance = 0.02f;
		static constexpr float kPlaneTolerance = 0.01f;
		static constexpr float kCompareDistance = 3000.0f;  ///< game units; the game's shadow cascades cover at least this far

		/**
		 * @param a_rasterDepth The R32 copy of the scene depth the Raytracer shares with D3D11 (read by every pass).
		 * @param a_copyCS D3D11 copy shader (CopyDepthCS) reused to copy the game's shadow mask on comparison frames.
		 * @param a_alphaAtlas M7c alpha atlas, or nullptr (then nothing is alpha-tested).
		 */
		bool Init(ID3D12Device5* a_device, ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context,
			uint32_t a_width, uint32_t a_height, ID3D12Resource* a_rasterDepth, ID3D11ComputeShader* a_copyCS, ID3D12Resource* a_alphaAtlas);
		const std::string& GetFailureReason() const { return failureReason; }
		void SetTimestampFrequency(uint64_t a_frequency) { timestampFrequency = a_frequency; }

		/** @brief D3D11 side, before the fence signal: on comparison frames copy the game's kSHADOW_MASK into a shared texture. */
		void CopyInputs(bool a_compareShadowMap);

		/**
		 * @brief Records trace, temporal and spatial passes (the TLAS must be built and barriered, and the alpha atlas
		 * in NON_PIXEL_SHADER_RESOURCE). a_instances / a_meshPool: this frame's instance data and static mesh pages,
		 * read by the M7c alpha test.
		 */
		void Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
			const BufferPool& a_meshPool, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const SunShadowParams& a_params,
			bool a_compareShadowMap, bool a_captureDump);

		/** @brief Reads the slot's counters and timestamps once its fence value has completed (never waits). */
		void CollectResults(uint32_t a_slot);

		/** @brief Copies the captured dump images out of the readback buffer (call once the dump's fence completed). */
		void ReadDumpImages(std::vector<DumpImage>& a_out);

		/** @brief Fills the mask with 1 (fully lit) on the D3D11 queue; used when it would otherwise be stale. */
		void ClearMask();

		ID3D11ShaderResourceView* GetMaskSRV() const { return mask.srv11.get(); }
		ID3D11ShaderResourceView* GetViewSRV() const { return view.srv11.get(); }
		const SunShadowStats& GetStats() const { return stats; }

	private:
		bool CreateTexture(DXGI_FORMAT a_format, const wchar_t* a_name, winrt::com_ptr<ID3D12Resource>& a_out);
		bool CreatePipelines();
		bool CreateDescriptors();
		bool Fail(std::string a_reason);

		ID3D12Device5* device = nullptr;
		ID3D11Device5* d3d11Device = nullptr;
		ID3D11DeviceContext4* d3d11Context = nullptr;
		ID3D11ComputeShader* copyCS = nullptr;
		ID3D12Resource* rasterDepth = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;

		SharedTexture mask;             // R8 final visibility, bound at PS t45 by the feature
		SharedTexture view;             // RGBA8 debug view for the overlay
		SharedTexture gameShadowMask;   // R8 copy of the game's kSHADOW_MASK (comparison frames only)
		winrt::com_ptr<ID3D12Resource> rawVisibility;  // R8, D3D12 only
		winrt::com_ptr<ID3D12Resource> geometry;       // RGBA16F reconstructed normal, D3D12 only
		winrt::com_ptr<ID3D12Resource> history[2];     // RGBA16F (mean, length, view depth), ping-pong

		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> tracePipeline;
		winrt::com_ptr<ID3D12PipelineState> temporalPipeline;
		winrt::com_ptr<ID3D12PipelineState> spatialPipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;  // 5 tables: trace, temporal[2], spatial[2]; then mesh pages + alpha atlas
		uint32_t descriptorSize = 0;
		ID3D12Resource* alphaAtlas = nullptr;
		std::array<uint64_t, 64> describedPageSerials{};  // BufferPool page serial each mesh-page descriptor describes

		winrt::com_ptr<ID3D12Resource> uploads[kFramesInFlight];  // constants + zeros for the counter clear
		uint8_t* uploadCpu[kFramesInFlight]{};
		winrt::com_ptr<ID3D12Resource> counters;
		winrt::com_ptr<ID3D12Resource> countersReadback;
		const uint32_t* countersCpu = nullptr;
		winrt::com_ptr<ID3D12QueryHeap> timestamps;
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampCpu = nullptr;
		uint64_t timestampFrequency = 0;
		bool slotPending[kFramesInFlight]{};
		bool slotCompared[kFramesInFlight]{};

		winrt::com_ptr<ID3D12Resource> dumpReadback;
		uint64_t dumpImageBytes = 0;
		uint32_t dumpRowPitch = 0;
		uint32_t dumpWidth = 0;
		uint32_t dumpHeight = 0;
		bool dumpHasComparison = false;
		bool dumpCaptured = false;

		// History of the previous traced frame, for reprojection.
		bool haveHistory = false;
		uint32_t historyGameFrame = 0;
		float prevViewProj[16]{};
		RE::NiPoint3 prevPosAdjust;
		uint32_t prevRenderWidth = 0;
		uint32_t prevRenderHeight = 0;
		uint32_t parity = 0;
		uint32_t frameIndex = 0;

		SunShadowStats stats;
		std::string failureReason;
	};
}
