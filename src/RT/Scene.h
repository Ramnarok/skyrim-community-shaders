#pragma once

/**
 * @brief Scene extraction: walks the loaded cells' 3D each frame and classifies geometry (ARCHITECTURE §3).
 *
 * Runs on the main/render thread from the Present hook, where the scene graph is not being modified.
 * Game pointers in GeometryCandidate are valid for the current frame only; the mesh cache uses them
 * as keys and reads through them only in the same frame.
 */
namespace RT
{
	/** @brief ARCHITECTURE §3 categories. Only kStaticMesh and kTerrain are extracted in M3. */
	enum class GeometryCategory : uint8_t
	{
		kStaticMesh,
		kTerrain,
		kSkinned,     // M7
		kDynamic,     // BSDynamicTriShape, M7
		kInstanced,   // grass / multi-stream instances, backlog
		kParticles,   // backlog
		kEffectOrWater,  // effect / water / sky / grass / blood shader properties, backlog
		kLOD,         // LOD tri shapes inside loaded cells, M8
		kOther,       // lines, unknown shader, non-tri-shape geometry
		kNoRendererData,  // tri shape without a renderer buffer or with zero counts
		kCount
	};

	std::string_view GetCategoryName(GeometryCategory a_category);

	/** @brief One extractable (static or terrain) geometry instance this frame. */
	struct GeometryCandidate
	{
		RE::BSGraphics::TriShape* rendererData = nullptr;  // mesh identity; shared by instances of the same mesh
		uint64_t vertexDesc = 0;                           // raw BSGraphics::VertexDesc bits
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		bool terrain = false;
		bool alphaTested = false;
		bool alphaBlended = false;  // drawn in the transparent pass: not in the pre-water depth
		RE::NiTransform world;  // absolute world transform, copied this frame
		ID3D11ShaderResourceView* diffuseSRV = nullptr;  // game-owned, valid this frame only (terrain: first layer)
		uint32_t albedo = 0;  // M6: average diffuse colour, RGBA8 as the texture stores it (filled by MaterialTable)
	};

	/** @brief World-space bounding sphere of geometry the TLAS does not contain (actors, grass, alpha-tested, ...). */
	struct ExclusionBound
	{
		RE::NiPoint3 center;
		float radius = 0.0f;
	};

	/** @brief Axis-aligned world bounds of the loaded cells (absolute coordinates). */
	struct LoadedArea
	{
		bool bounded = false;  ///< false in interiors: everything visible is loaded
		RE::NiPoint3 min;
		RE::NiPoint3 max;
	};

	struct SceneStats
	{
		uint32_t cells = 0;
		uint32_t exclusionBounds = 0;
		std::array<uint32_t, static_cast<size_t>(GeometryCategory::kCount)> exclusionsByCategory{};  ///< indexed by GeometryCategory
		std::array<float, static_cast<size_t>(GeometryCategory::kCount)> exclusionMaxRadius{};       ///< largest bound radius per category
		uint32_t exclusionsRejectedTooLarge = 0;
		uint32_t hiddenSubtrees = 0;  ///< Subtrees skipped because a node was flagged kHidden.
		std::array<uint32_t, static_cast<size_t>(GeometryCategory::kCount)> instances{};
		uint32_t alphaTestedInstances = 0;  ///< Subset of static + terrain instances.
		uint32_t alphaBlendedInstances = 0;  ///< Subset of static + terrain instances.
		bool grassWalked = false;           ///< BGSGrassManager::grassNode was found and walked
		uint32_t grassBounds = 0;           ///< exclusion bounds contributed by grass
		uint32_t uniqueStaticMeshes = 0;
		uint32_t uniqueTerrainMeshes = 0;
		float traversalMs = 0.0f;
	};

	/**
	 * @brief Collects static and terrain geometry from every loaded cell into a_out (cleared first).
	 * @return False when there is no world (main menu, loading), in which case a_out is empty.
	 */
	bool CollectScene(std::vector<GeometryCandidate>& a_out, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats);
}
