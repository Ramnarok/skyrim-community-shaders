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
		kGIReflectionTraced,      ///< M8 reflections: pixels with a reflection term (within the roughness limit)
		kGIReflectionHits,        ///< ... rays that hit geometry (the rest see the sky)
		kGIReflectionDeeperHits,  ///< ... hits of the reflected surfaces' continuation rays
		kGIReflectionRays,        ///< ... reflection rays traced (one per 2x2 block at half resolution)
		kGIWaterPixels,           ///< M8 water: pixels whose reflecting surface is a water plane (counted in kGIReflectionTraced too)
		// M8 water diagnostic: StaticMotionVector (the water's) against the game's kMOTION_VECTOR on the G-buffer (non-water
		// pixels with depth): sums in 1/10 pixel, each pixel clamped to 100 pixels.
		kGIMotionChecked,
		kGIMotionGameSum,        ///< |game MV|
		kGIMotionErrorSum,       ///< |ours - game|
		kGIMotionErrorFlipYSum,  ///< |ours with y negated - game|
		kGIMotionErrorNegatedSum,  ///< |-ours - game|
		kGIEmissiveVertices,  ///< M9 phase 4: path vertices (GI and reflection hits, any bounce) on glowing surfaces
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

		// M8 reflections (set up the first time they're wanted).
		bool reflectionsAvailable = false;
		std::string reflectionsFailure;  ///< why reflections couldn't be set up, if they couldn't
		bool reflectionsLastSlot = false;  ///< the last collected frame traced reflections (counters and timings are theirs)
		uint64_t reflectionFramesTraced = 0;
		uint64_t reflectionHistoryResets = 0;
		uint32_t reflectionDispatches = 0;
		TimingSeries reflectionTraceMs;
		TimingSeries reflectionDenoiseMs;
		// M8 water diagnostic: mean StaticMotionVector error vs the game's motion vectors (pixels), over frames with camera motion.
		TimingSeries motionGamePx, motionErrorPx, motionErrorFlipYPx, motionErrorNegatedPx;
		// ... and whether projUnjittered x view (either order) and our previous matrix (NRD's) match the game's (relative).
		float cameraCheckProjTimesView = 0.0f, cameraCheckViewTimesProj = 0.0f, cameraCheckPrevious = 0.0f;
		std::array<float, 4> cameraCheckVariants{};  ///< P^T x V, P x V^T, P^T x V^T, V^T x P^T against the game's
		std::array<std::array<float, 16>, 6> cameraMatrices{};  ///< view, projUnjittered, viewProjUnjittered, viewProj, viewInverse, prevViewProjUnjittered
		RE::NiPoint3 cameraPosAdjust, cameraPrevPosAdjust;

		float HitPercent() const { return counters[kGITraced] ? 100.0f * counters[kGIHits] / counters[kGITraced] : 0.0f; }
		float ReflectionHitPercent() const { return counters[kGIReflectionRays] ? 100.0f * counters[kGIReflectionHits] / counters[kGIReflectionRays] : 0.0f; }
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
	 * M8 reflections ride the same hand-off: a glossy trace from the pixels with a reflection term, NRD REBLUR_SPECULAR
	 * (a second instance), and a resolve into the texture the composite reads at t17.
	 */
	class GlobalIllumination
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kMeshPageSlots = 64;
		static constexpr float kUnitsPerMeter = 70.0f;           ///< Skyrim units; NRD's defaults are in meters
		static constexpr float kReflectionHitDistanceA = 50000.0f;  ///< reflections' hit-distance normalization: their rays' reach, so no real hit saturates
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
		 * With a_reflections, also sets reflections up the first time (textures, NRD REBLUR_SPECULAR, pipelines) and
		 * copies Deferred's REFLECTANCE target; Record traces reflections only when that succeeded.
		 */
		bool CopyInputs(bool a_reflections);

		/** @brief Records trace, REBLUR and resolve. The TLAS and instance data must be this frame's (Prepass round trip). */
		void Record(ID3D12GraphicsCommandList4* a_list, uint32_t a_slot, D3D12_GPU_VIRTUAL_ADDRESS a_tlas, D3D12_GPU_VIRTUAL_ADDRESS a_instances,
			const BufferPool& a_meshPool, const SkinnedMeshes* a_skinned, const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight,
			const GIParams& a_params, bool a_captureDump);

		void CollectResults(uint32_t a_slot);
		void ReadDumpImages(std::vector<DumpImage>& a_out);

		/** @brief This frame's composite inputs; reflections only when Record traced them this frame. */
		GIOutputs GetOutputs() const
		{
			GIOutputs outputs{ ao.srv11.get(), y.srv11.get(), coCg.srv11.get() };
			outputs.reflections = reflectionsRecorded ? reflections.srv11.get() : nullptr;
			outputs.waterReflections = reflectionsRecorded && waterRecorded ? waterReflections.srv11.get() : nullptr;
			return outputs;
		}
		ID3D11ShaderResourceView* GetViewSRV() const { return view.srv11.get(); }
		GIStats& GetStats() { return stats; }
		const GIStats& GetStats() const { return stats; }

	private:
		bool Fail(std::string a_reason);
		bool CreateTexture(DXGI_FORMAT a_format, const wchar_t* a_name, winrt::com_ptr<ID3D12Resource>& a_out);
		bool CreatePipeline(const char* a_file, const wchar_t* a_name, winrt::com_ptr<ID3D12PipelineState>& a_out, std::string& a_error);
		bool CreatePipelines();
		bool InitReflections();
		void WriteDescriptors();
		void FillNrdSettings(const FrameCamera& a_camera, uint32_t a_renderWidth, uint32_t a_renderHeight, const GIParams& a_params,
			bool a_historyValid, bool a_everCleared, bool a_specular, nrd::CommonSettings& a_common, nrd::ReblurSettings& a_reblur) const;

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

		// M8 reflections, created the first time they're wanted (InitReflections).
		SharedTexture reflectance;  // Deferred's REFLECTANCE target, copied
		winrt::com_ptr<ID3D12Resource> specularViewZ;  // REBLUR_SPECULAR inputs/outputs (D3D12 only, resting in NON_PIXEL_SHADER_RESOURCE)
		winrt::com_ptr<ID3D12Resource> specularNormalRoughness;
		winrt::com_ptr<ID3D12Resource> specularNoisy;
		winrt::com_ptr<ID3D12Resource> specularDenoised;
		SharedTexture reflections;  // composite input (t17), resting in COMMON
		// M8 water: the traced water surface's view Z per pixel (0 = not water; D3D12 only, resting like the specular
		// textures), and Water.hlsl's input (t47): the water pixels' reflections, resting in COMMON.
		winrt::com_ptr<ID3D12Resource> waterViewZ;
		winrt::com_ptr<ID3D12Resource> specularMotionVectors;  // REBLUR_SPECULAR's IN_MV: GI's, with the water surface's own at water pixels
		SharedTexture waterReflections;
		bool waterRecorded = false;  // this frame's Record traced water too
		NrdDenoiser specularDenoiser;
		winrt::com_ptr<ID3D12PipelineState> reflectionTracePipeline;
		winrt::com_ptr<ID3D12PipelineState> reflectionResolvePipeline;
		bool reflectionsInitTried = false;
		bool reflectionsReady = false;
		bool reflectionsRecorded = false;  // this frame's Record traced reflections
		bool specularHaveHistory = false;
		bool specularEverCleared = false;
		uint32_t specularHistoryGameFrame = 0;

		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> tracePipeline;
		winrt::com_ptr<ID3D12PipelineState> resolvePipeline;
		winrt::com_ptr<ID3D12DescriptorHeap> heap;  // [0..63] mesh pages, then the trace and resolve tables, atlases, reflection tables
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
		bool slotReflections[kFramesInFlight]{};
		uint32_t slotReflectionDispatches[kFramesInFlight]{};

		// Dump images: GI noisy, denoised, AO, then (M8) reflections noisy and resolved when traced.
		static constexpr uint32_t kDumpImages = 5;
		winrt::com_ptr<ID3D12Resource> dumpReadback;
		uint64_t dumpReadbackBytes = 0;
		std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kDumpImages> dumpFootprints{};
		uint32_t dumpImageCount = 0;
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
