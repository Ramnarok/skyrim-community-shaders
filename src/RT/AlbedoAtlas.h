#pragma once

#include "Scene.h"
#include "TextureAtlas.h"

namespace RT
{
	struct AlbedoAtlasStats
	{
		bool available = false;  ///< the shared atlas and its fill shader were created
		bool enabled = false;    ///< textured bounce light was on for the last update
		uint32_t capacity = 0;   ///< tiles
		uint32_t tilesUsed = 0;
		uint32_t filledLastFrame = 0;
		uint32_t evictedLastFrame = 0;
		uint64_t totalFills = 0;
		uint64_t totalEvictions = 0;
		// This frame's traced candidates (not blended, not decals).
		uint32_t candidates = 0;
		uint32_t candidatesTextured = 0;     ///< got a tile: GI hits sample the texture
		uint32_t candidatesVertexColors = 0;  ///< ... whose vertex colours multiply it, as Lighting.hlsl's albedo
		uint32_t candidatesNoTexture = 0;
		uint32_t candidatesNoUV = 0;
		uint32_t candidatesUnsupported = 0;
		uint32_t candidatesWaiting = 0;  ///< fill budget used up or atlas full: the average albedo this frame
	};

	/**
	 * @brief M8 materials at GI hits (path-tracing step 2, ARCHITECTURE §6 v2): each traced diffuse texture's colour, as the
	 * texture stores it, in one kTileSize² tile of an RGBA8 TextureAtlas. Candidates with a tile carry
	 * GeometryCandidate::albedoWord (InstanceData.Flags bits 8-31); the GI trace samples it at the hit UV and multiplies
	 * the vertex colour, as Lighting.hlsl builds the G-buffer albedo. Without a tile the average albedo (M6) stays.
	 */
	class AlbedoAtlas
	{
	public:
		static constexpr uint32_t kTileSize = 128;    // must match MeshData.hlsli
		static constexpr uint32_t kTilesPerRow = 32;  // must match MeshData.hlsli; 4096² RGBA8: 64 MB, 1,024 tiles

		bool Init(ID3D11Device5* a_d3d11Device, ID3D11DeviceContext4* a_d3d11Context, ID3D12Device* a_device);

		/**
		 * @brief Render thread, D3D11 immediate context, same frame as the scene walk and before the D3D11 -> D3D12 signal:
		 * fills tiles for new textures and sets each candidate's albedoWord (0 when a_enabled is false: average albedo).
		 */
		void Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame, bool a_enabled);

		/** @brief The shared atlas, resting in COMMON; nullptr if Init failed. */
		ID3D12Resource* GetResource() const { return atlas.GetResource(); }
		const AlbedoAtlasStats& GetStats() const { return stats; }

		void CaptureForDump() { atlas.CaptureForDump(); }
		void ReadDumpImage(std::vector<DumpImage>& a_out) { atlas.ReadDumpImage("albedo_atlas", a_out); }

	private:
		TextureAtlas atlas;
		AlbedoAtlasStats stats;
	};

	/** @brief Writes the albedo atlas SRV at t1, space2 (a null RGBA8 view when a_atlas is null: average albedo only). */
	void WriteAlbedoAtlasDescriptor(ID3D12Device* a_device, ID3D12Resource* a_atlas, D3D12_CPU_DESCRIPTOR_HANDLE a_handle);
}
