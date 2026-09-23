#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include "Scene.h"

namespace RT
{
	/**
	 * @brief What a game texture SRV points at. The game's textures unload and their SRV addresses get reused, so
	 * tables keyed by SRV compare this each frame; a different resource or size means a different texture.
	 */
	struct TextureIdentity
	{
		ID3D11Resource* resource = nullptr;  // not referenced: identity only
		uint32_t width = 0;
		uint32_t height = 0;
		bool supported = false;  ///< a plain single-sample 2D texture viewed as TEXTURE2D
		bool operator==(const TextureIdentity&) const = default;
	};

	TextureIdentity GetTextureIdentity(ID3D11ShaderResourceView* a_srv);

	struct MaterialTableStats
	{
		uint32_t textures = 0;         ///< diffuse textures with a computed average
		uint32_t pending = 0;          ///< queued or in flight
		uint32_t unsupported = 0;      ///< not a plain 2D texture: default albedo
		uint32_t computedLastFrame = 0;
		uint32_t candidatesWithoutTexture = 0;
		uint32_t candidatesDefaulted = 0;  ///< this frame: no average yet, default albedo used
	};

	/**
	 * @brief M6 average-albedo material table (ARCHITECTURE §6, v1). The game's textures are D3D11-only, so each new
	 * diffuse texture is averaged once by a D3D11 compute pass (budgeted per frame) and read back without CPU waits.
	 * Candidates then carry the average as GeometryCandidate::albedo, which reaches the GI shader through the instance data.
	 */
	class MaterialTable
	{
	public:
		static constexpr uint32_t kSlotsPerBatch = 64;
		static constexpr uint32_t kBatchesInFlight = 3;
		/// Used until a texture's average is known, or when it has none: a neutral mid grey (texture values).
		static constexpr uint32_t kDefaultAlbedo = 0xFF737373u;

		bool Init(ID3D11Device* a_device, ID3D11DeviceContext* a_context);

		/**
		 * @brief Render thread, D3D11 immediate context, while the candidates' SRVs are valid (same frame as the scene
		 * walk): collects finished readbacks, queues averaging passes for new textures, and fills each candidate's albedo.
		 */
		void Update(std::vector<GeometryCandidate>& a_candidates, uint32_t a_frame);

		const MaterialTableStats& GetStats() const { return stats; }

	private:
		struct Entry
		{
			ID3D11Resource* resource = nullptr;  // guards against SRV address reuse after a texture unloads
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t plain = kDefaultAlbedo;     // RGBA8 plain average
			uint32_t weighted = kDefaultAlbedo;  // RGBA8 alpha-weighted average (alpha-tested meshes)
			uint32_t validatedFrame = UINT32_MAX;
			bool ready = false;
			bool supported = true;
		};

		struct Batch
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::vector<ID3D11ShaderResourceView*> keys;
			bool inFlight = false;
		};

		bool Validate(ID3D11ShaderResourceView* a_srv, Entry& a_entry);
		void CollectBatches();

		ID3D11Device* device = nullptr;
		ID3D11DeviceContext* context = nullptr;
		winrt::com_ptr<ID3D11ComputeShader> shader;
		winrt::com_ptr<ID3D11Buffer> output;
		winrt::com_ptr<ID3D11UnorderedAccessView> outputUAV;
		winrt::com_ptr<ID3D11Buffer> params;
		std::array<Batch, kBatchesInFlight> batches;
		uint32_t nextBatch = 0;

		std::unordered_map<ID3D11ShaderResourceView*, Entry> entries;
		std::vector<ID3D11ShaderResourceView*> queue;
		MaterialTableStats stats;
	};
}
