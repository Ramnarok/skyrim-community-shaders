#pragma once

#if defined(SKYRIMRT_NRD)

#	include <d3d12.h>
#	include <winrt/base.h>

#	include <NRD.h>

namespace RT
{
	/**
	 * @brief Drives NVIDIA NRD's REBLUR_DIFFUSE directly on the sidecar's D3D12 device (no NRI): one shared root
	 * signature, NRD's texture pools, a per-frame-slot descriptor and constant ring, and resource-state tracking.
	 * Built only with SKYRIMRT_NRD (NVIDIA RTX SDKs License: private builds only, see cmake/SkyrimRTNRD.cmake).
	 */
	class NrdDenoiser
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kMaxDispatchesPerFrame = 64;
		static constexpr nrd::Identifier kDiffuse = 0;

		/** @brief The application's NRD inputs/outputs. They must be in NON_PIXEL_SHADER_RESOURCE on entry and are left there. */
		struct Resources
		{
			ID3D12Resource* motionVectors = nullptr;       // IN_MV
			ID3D12Resource* normalRoughness = nullptr;     // IN_NORMAL_ROUGHNESS
			ID3D12Resource* viewZ = nullptr;               // IN_VIEWZ
			ID3D12Resource* radianceHitDist = nullptr;     // IN_DIFF_RADIANCE_HITDIST
			ID3D12Resource* outRadianceHitDist = nullptr;  // OUT_DIFF_RADIANCE_HITDIST
		};

		NrdDenoiser() = default;
		NrdDenoiser(const NrdDenoiser&) = delete;
		NrdDenoiser& operator=(const NrdDenoiser&) = delete;
		~NrdDenoiser();

		/** @param a_width, a_height NRD's resource size (the full texture size; the render region may be smaller). */
		bool Init(ID3D12Device* a_device, uint32_t a_width, uint32_t a_height);
		const std::string& GetFailureReason() const { return failureReason; }

		/** @brief Records every REBLUR dispatch for this frame into a_list. */
		void Record(ID3D12GraphicsCommandList* a_list, uint32_t a_slot, const nrd::CommonSettings& a_common,
			const nrd::ReblurSettings& a_settings, const Resources& a_resources);

		uint32_t GetLastDispatchCount() const { return lastDispatchCount; }

	private:
		bool Fail(std::string a_reason);
		ID3D12Resource* ResolveResource(const nrd::ResourceDesc& a_desc, const Resources& a_resources) const;

		ID3D12Device* device = nullptr;
		nrd::Instance* instance = nullptr;
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
		std::string failureReason;
	};
}

#endif
