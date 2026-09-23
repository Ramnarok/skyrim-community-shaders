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
	};

	struct SceneStats
	{
		uint32_t cells = 0;
		uint32_t hiddenSubtrees = 0;  ///< Subtrees skipped because a node was flagged kHidden.
		std::array<uint32_t, static_cast<size_t>(GeometryCategory::kCount)> instances{};
		uint32_t alphaTestedInstances = 0;  ///< Subset of static + terrain instances.
		uint32_t uniqueStaticMeshes = 0;
		uint32_t uniqueTerrainMeshes = 0;
		float traversalMs = 0.0f;
	};

	/**
	 * @brief Collects static and terrain geometry from every loaded cell into a_out (cleared first).
	 * @return False when there is no world (main menu, loading), in which case a_out is empty.
	 */
	bool CollectScene(std::vector<GeometryCandidate>& a_out, SceneStats& a_stats);
}
