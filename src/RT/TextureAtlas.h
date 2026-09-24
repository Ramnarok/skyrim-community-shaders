#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

#include "FrameTypes.h"
#include "MaterialTable.h"
#include "SharedTexture.h"

namespace RT
{
	/** @brief How one atlas is laid out and filled. */
	struct TextureAtlasConfig
	{
		const char* name = "";          ///< resource names and log lines
		uint32_t tileSize = 0;          ///< tiles are tileSize², sampled from the game texture's mip nearest that size
		uint32_t tilesPerRow = 0;       ///< the atlas is square: tilesPerRow² tiles
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  ///< R8_UNORM (alpha) or R8G8B8A8_UNORM (colour)
		bool albedo = false;            ///< the fill shader writes colour (ALBEDO define), else alpha
		uint32_t maxFillsPerFrame = 64;
		uint32_t dumpRows = 2;          ///< tile rows the debug dump copies
	};

	/** @brief Tile bookkeeping shared by the atlases, reported in their stats. */
	struct TextureAtlasStats
	{
		bool available = false;  ///< the shared atlas and its fill shader were created
		uint32_t capacity = 0;   ///< tiles
		uint32_t tilesUsed = 0;
		uint32_t filledLastFrame = 0;
		uint32_t evictedLastFrame = 0;
		uint64_t totalFills = 0;
		uint64_t totalEvictions = 0;
	};

	/**
	 * @brief The v2 texture path (ARCHITECTURE §6): the game's textures are D3D11-only, so one tile per texture is copied
	 * (budgeted per frame) into an atlas created in D3D11 and shared with D3D12. Keyed by the texture's SRV, re-validated
	 * once per frame against its resource and size. Tiles of textures not seen this frame are recycled, oldest first. The
	 * alpha atlas (M7c alpha test) and the albedo atlas (M8 materials at GI hits) are two of these.
	 */
	class TextureAtlas
	{
	public:
		enum class Result
		{
			kTile,         ///< a_tile holds the texture (filled this frame or earlier)
			kUnsupported,  ///< not a plain 2D texture
			kWaiting,      ///< fill budget used up, or the atlas is full of textures seen this frame
		};

		bool Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device, const TextureAtlasConfig& a_config);

		/** @brief Render thread, D3D11 immediate context, before the frame's D3D11 -> D3D12 signal. */
		void BeginFrame();
		/** @brief The tile of a_srv's texture this frame, filling it first if needed (dispatches on the immediate context). */
		Result Acquire(ID3D11ShaderResourceView* a_srv, uint32_t a_frame, uint32_t& a_tile);
		/** @brief Unbinds the fill state if anything was filled, and updates the tile count. */
		void EndFrame();

		/** @brief The shared atlas, resting in COMMON; nullptr if Init failed. */
		ID3D12Resource* GetResource() const { return atlas.resource12.get(); }
		const TextureAtlasStats& GetStats() const { return stats; }
		const TextureAtlasConfig& GetConfig() const { return config; }

		/** @brief Copies the first dumpRows tile rows to a staging texture (D3D11 queue). */
		void CaptureForDump();
		/** @brief Adds the captured rows (half size, RGBA8) as a_name if the copy has completed; never waits. */
		void ReadDumpImage(const char* a_name, std::vector<DumpImage>& a_out);

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

		TextureAtlasConfig config;
		ID3D11Device5* d3d11Device = nullptr;
		ID3D11DeviceContext4* d3d11Context = nullptr;
		SharedTexture atlas;
		winrt::com_ptr<ID3D11ComputeShader> fillCS;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11Buffer> params;
		winrt::com_ptr<ID3D11Texture2D> dumpStaging;
		bool dumpPending = false;
		bool bound = false;

		std::unordered_map<ID3D11ShaderResourceView*, Entry> entries;
		std::vector<ID3D11ShaderResourceView*> tileOwners;  // per tile, nullptr when free
		std::vector<uint32_t> freeTiles;
		TextureAtlasStats stats;
	};
}
