#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "RT.h"

namespace RT
{
	/**
	 * @brief The persistent D3D12 device that runs next to the game's D3D11 device.
	 *
	 * Owns the queue, the shared fence and the M2 interop test: every frame D3D11 signals the
	 * fence, the D3D12 queue waits on it, dispatches a test-pattern compute shader into a texture
	 * shared with D3D11, signals back, and D3D11 waits before the overlay samples the texture.
	 * Nothing here blocks the CPU on the GPU during a frame; per-frame slots are skipped instead.
	 */
	class Sidecar
	{
	public:
		static constexpr uint32_t kFramesInFlight = 3;
		static constexpr uint32_t kPatternSize = 256;

		/**
		 * @brief Creates the queue, fence, shared test texture and compute pipeline, then runs the shared-buffer spike.
		 * @param a_device The probe-verified D3D12 device on the game's adapter (ownership is taken).
		 * @return False if any required object could not be created; failureReason is set.
		 */
		bool Init(winrt::com_ptr<ID3D12Device> a_device, ID3D11Device* a_d3d11Device, ID3D11DeviceContext* a_d3d11Context);

		/** @brief One interop round trip: D3D11 → D3D12 test-pattern dispatch → D3D11. Call once per frame on the render thread. */
		void OnFrame();

		/** @brief Queues a dump; the pattern copy rides along with the next round trip and is written once it completes. */
		void RequestDebugDump(uint32_t a_gameFrame)
		{
			dumpRequested = true;
			pendingDumpGameFrame = a_gameFrame;
		}

		ID3D11ShaderResourceView* GetTestPatternSRV() const { return framesSubmitted > 0 ? patternSRV11.get() : nullptr; }
		const InteropStats& GetStats() const { return stats; }
		const SpikeResults& GetSpikeResults() const { return spike; }
		const std::string& GetFailureReason() const { return failureReason; }

	private:
		bool CreatePatternTexture();
		bool CreatePipeline();
		bool CreateTimingObjects();
		void RunSharedBufferSpike();
		bool WaitForFenceBlocking(uint64_t a_value);  // Init-time only, never on the frame path.
		void CheckDeviceRemoved();
		void CollectTimings(uint32_t a_slot);
		void FinishDumpIfReady();

		winrt::com_ptr<ID3D12Device> device;
		winrt::com_ptr<ID3D12CommandQueue> queue;
		winrt::com_ptr<ID3D12CommandAllocator> allocators[kFramesInFlight];
		winrt::com_ptr<ID3D12GraphicsCommandList> commandList;
		winrt::com_ptr<ID3D12Fence> fence;
		uint64_t fenceValue = 0;
		uint64_t slotFenceValues[kFramesInFlight]{};

		winrt::com_ptr<ID3D11Device5> d3d11Device;
		winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;
		winrt::com_ptr<ID3D11Fence> d3d11Fence;

		// Test pattern: created in D3D12 with a shared heap and opened in D3D11 (or the reverse, if that fails).
		winrt::com_ptr<ID3D12Resource> patternTexture;
		winrt::com_ptr<ID3D11Texture2D> patternTexture11;
		winrt::com_ptr<ID3D11ShaderResourceView> patternSRV11;
		winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap;
		winrt::com_ptr<ID3D12RootSignature> rootSignature;
		winrt::com_ptr<ID3D12PipelineState> pipeline;

		// GPU timing: D3D12 timestamps around the dispatch, D3D11 timestamps around the fence round trip.
		winrt::com_ptr<ID3D12QueryHeap> timestampHeap;
		winrt::com_ptr<ID3D12Resource> timestampReadback;
		const uint64_t* timestampData = nullptr;
		uint64_t d3d12TimestampFrequency = 0;
		winrt::com_ptr<ID3D11Query> d3d11Disjoint[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> d3d11Begin[kFramesInFlight];
		winrt::com_ptr<ID3D11Query> d3d11End[kFramesInFlight];
		bool slotHasTimings[kFramesInFlight]{};

		// Debug dump readback of the pattern texture.
		winrt::com_ptr<ID3D12Resource> patternReadback;
		uint32_t patternRowPitch = 0;
		bool dumpRequested = false;
		bool dumpInFlight = false;
		uint64_t dumpFenceValue = 0;
		uint32_t dumpPatternFrame = 0;
		uint32_t dumpGameFrame = 0;
		uint32_t pendingDumpGameFrame = 0;

		LARGE_INTEGER qpcFrequency{};
		LARGE_INTEGER startTime{};
		uint32_t framesSubmitted = 0;
		bool deviceRemoved = false;

		InteropStats stats;
		SpikeResults spike;
		std::string failureReason;
	};
}
