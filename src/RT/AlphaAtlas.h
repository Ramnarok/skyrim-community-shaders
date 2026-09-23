#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "FrameTypes.h"
#include "MaterialTable.h"
#include "Scene.h"
#include "SharedTexture.h"

namespace RT
{
	struct AlphaAtlasStats
	{
		bool available = false;       ///< the shared atlas and its fill shader were created
		bool enabled = false;         ///< alpha testing was on for the last update
		uint32_t capacity = 0;        ///< tiles
		uint32_t tilesUsed = 0;
		uint32_t filledLastFrame = 0;
		uint32_t evictedLastFrame = 0;
		uint64_t totalFills = 0;
		uint64_t totalEvictions = 0;
		// This frame's alpha-tested (not blended) candidates: static meshes and skinned partitions (trees, hair).
		uint32_t candidates = 0;
		uint32_t candidatesTested = 0;      ///< got an atlas tile: alpha-tested in the traces
		uint32_t candidatesNoTexture = 0;   ///< no diffuse texture
		uint32_t candidatesNoUV = 0;        ///< vertex format without texture coordinates
		uint32_t candidatesZeroThreshold = 0;  ///< threshold 0 never discards: kept opaque
		uint32_t candidatesUnsupported = 0;    ///< not a plain 2D texture
		uint32_t candidatesWaiting = 0;        ///< fill budget used up or atlas full: opaque this frame
	};

	/**
	 * @brief M7c alpha test, the start of the v2 texture path (ARCHITECTURE §6). The game's textures are D3D11-only, so
	 * each alpha-tested diffuse texture's alpha is copied (budgeted per frame, from the mip nearest the tile size) into
	 * one kTileSize² tile of an R8 atlas created in D3D11 and shared with D3D12. Candidates with a tile carry
	 * GeometryCandidate::alphaWord; their instances are traced as non-opaque and the RayQuery passes alpha-test candidate
	 * hits against the atlas (MeshData.hlsli). Tiles of textures not seen this frame are recycled, oldest first.
	 */
	class AlphaAtlas
	{
	public:
		static constexpr uint32_t kTileSize = 512;       // must match AlphaAtlasFillCS.hlsl and MeshData.hlsli
		static constexpr uint32_t kTilesPerRow = 16;     // must match MeshData.hlsli
		static constexpr uint32_t kAtlasSize = kTileSize * kTilesPerRow;  // 8192² R8: 64 MB, 256 tiles
		static constexpr uint32_t kCapacity = kTilesPerRow * kTilesPerRow;
		static constexpr uint32_t kMaxFillsPerFrame = 64;
		static constexpr uint32_t kDumpRows = 2;  // tile rows copied for the debug dump

		bool Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device);

		/**
		 * @brief Render thread, D3D11 immediate context, same frame as the scene walk and before the D3D11 -> D3D12
		 * signal: assigns and fills tiles for new alpha-tested textures and sets each candidate's alphaWord (0 when
		 * a_enabled is false, so everything is traced opaque as before M7c).
		 */
		void Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame, bool a_enabled);

		/** @brief The shared atlas, resting in COMMON; nullptr if Init failed. */
		ID3D12Resource* GetResource() const { return atlas.resource12.get(); }
		const AlphaAtlasStats& GetStats() const { return stats; }

		/** @brief Copies the first kDumpRows tile rows to a staging texture (D3D11 queue). */
		void CaptureForDump();
		/** @brief Adds the captured rows (half size, grey RGBA8) as "alpha_atlas" if the copy has completed; never waits. */
		void ReadDumpImage(std::vector<DumpImage>& a_out);

	private:
		struct Entry
		{
			TextureIdentity identity;
			uint32_t validatedFrame = UINT32_MAX;
			uint32_t lastSeenFrame = 0;
			int32_t tile = -1;
		};

		int32_t AllocateTile(uint32_t a_frame);
		void Fill(ID3D11ShaderResourceView* a_srv, uint32_t a_tile);

		ID3D11Device5* d3d11Device = nullptr;
		ID3D11DeviceContext4* d3d11Context = nullptr;
		SharedTexture atlas;
		winrt::com_ptr<ID3D11ComputeShader> fillCS;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11Buffer> params;
		winrt::com_ptr<ID3D11Texture2D> dumpStaging;
		bool dumpPending = false;

		std::unordered_map<ID3D11ShaderResourceView*, Entry> entries;
		std::vector<ID3D11ShaderResourceView*> tileOwners;  // per tile, nullptr when free
		std::vector<uint32_t> freeTiles;
		AlphaAtlasStats stats;
	};

	/** @brief Descriptor-table range for the atlas: one SRV at t0, space2 (MeshData.hlsli). */
	D3D12_DESCRIPTOR_RANGE GetAlphaAtlasRange(uint32_t a_offsetInTable);
	/** @brief Static sampler s0 the RayQuery passes sample the atlas with (bilinear, clamp). */
	D3D12_STATIC_SAMPLER_DESC GetAlphaAtlasSampler();
	/** @brief Writes the atlas SRV (a null R8 view when a_atlas is null: nothing is alpha-tested then). */
	void WriteAlphaAtlasDescriptor(ID3D12Device* a_device, ID3D12Resource* a_atlas, D3D12_CPU_DESCRIPTOR_HANDLE a_handle);
}
