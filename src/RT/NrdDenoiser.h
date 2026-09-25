#pragma once

#if defined(SKYRIMRT_NRD)

#	include <d3d12.h>
#	include <winrt/base.h>

#	include <NRD.h>

namespace RT
{
	/**
	 * @brief Drives one NVIDIA NRD denoiser (REBLUR_DIFFUSE for GI, M8 REBLUR_SPECULAR for reflections, M9 REBLUR_DIFFUSE_OCCLUSION
	 * for the outdoor sky signal) directly on the
	 * sidecar's D3D12 device (no NRI): one shared root signature, NRD's texture pools, a per-frame-slot descriptor and
	 * constant ring, and resource-state tracking. One instance per denoiser, so each keeps its own history and can be
	 * skipped on frames it isn't needed. Built only with SKYRIMRT_NRD (NVIDIA RTX SDKs License: private builds only, see
	 * cmake/SkyrimRTNRD.cmake).
	 */
	class NrdDenoiser
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kMaxDispatchesPerFrame = 64;
		static constexpr nrd::Identifier kIdentifier = 0;

		/** @brief The application's NRD inputs/outputs. They must be in NON_PIXEL_SHADER_RESOURCE on entry and are left there. */
		struct Resources
		{
			ID3D12Resource* motionVectors = nullptr;       // IN_MV
			ID3D12Resource* normalRoughness = nullptr;     // IN_NORMAL_ROUGHNESS
			ID3D12Resource* viewZ = nullptr;               // IN_VIEWZ
			ID3D12Resource* radianceHitDist = nullptr;     // IN_DIFF_RADIANCE_HITDIST, IN_SPEC_RADIANCE_HITDIST or IN_DIFF_HITDIST
			ID3D12Resource* outRadianceHitDist = nullptr;  // OUT_DIFF_RADIANCE_HITDIST, OUT_SPEC_RADIANCE_HITDIST or OUT_DIFF_HITDIST
		};

		NrdDenoiser() = default;
		NrdDenoiser(const NrdDenoiser&) = delete;
		NrdDenoiser& operator=(const NrdDenoiser&) = delete;
		~NrdDenoiser();

		/**
		 * @param a_width, a_height NRD's resource size (the full texture size; the render region may be smaller).
		 * @param a_denoiser REBLUR_DIFFUSE, REBLUR_SPECULAR or REBLUR_DIFFUSE_OCCLUSION.
		 */
		bool Init(ID3D12Device* a_device, uint32_t a_width, uint32_t a_height, nrd::Denoiser a_denoiser);
		bool IsReady() const { return ready; }
		const std::string& GetFailureReason() const { return failureReason; }

		/** @brief Records every dispatch of the denoiser for this frame into a_list. */
		void Record(ID3D12GraphicsCommandList* a_list, uint32_t a_slot, const nrd::CommonSettings& a_common,
			const nrd::ReblurSettings& a_settings, const Resources& a_resources);

		uint32_t GetLastDispatchCount() const { return lastDispatchCount; }

	private:
		bool Fail(std::string a_reason);
		ID3D12Resource* ResolveResource(const nrd::ResourceDesc& a_desc, const Resources& a_resources) const;

		ID3D12Device* device = nullptr;
		nrd::Instance* instance = nullptr;
		nrd::Denoiser denoiser = nrd::Denoiser::REBLUR_DIFFUSE;
		uint32_t width = 0;
		uint32_t height = 0;

		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		std::vector<winrt::com_ptr<ID3D12PipelineState>> pipelines;
		std::vector<winrt::com_ptr<ID3D12Resource>> permanentPool;
		std::vector<winrt::com_ptr<ID3D12Resource>> transientPool;
		std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> states;

		winrt::com_ptr<ID3D12DescriptorHeap> heap;  // kFramesInFlight x kMaxDispatchesPerFrame tables
		uint32_t descriptorSize = 0;
		uint32_t tableSrvs = 0;  // perSetTexturesMaxNum
		uint32_t tableUavs = 0;  // perSetStorageTexturesMaxNum

		winrt::com_ptr<ID3D12Resource> constants;  // upload ring: kFramesInFlight x kMaxDispatchesPerFrame slices
		uint8_t* constantsCpu = nullptr;
		uint64_t constantSliceBytes = 0;

		uint32_t lastDispatchCount = 0;
		bool ready = false;
		std::string failureReason;
	};
}

#endif
