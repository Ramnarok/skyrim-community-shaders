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
		bool skinned = false;  // M7: one skin partition; rendererData is the partition's bind-pose buffers
		uint8_t alphaThreshold = 0;  // M7c: NiAlphaProperty::alphaThreshold (the game discards alpha below threshold / 255)
		uint32_t alphaWord = 0;      // M7c: InstanceData::Alpha (atlas tile, threshold, UV offset), filled by AlphaAtlas; 0 = opaque
		bool windAnimated = false;   // M7c: kTreeAnim; the game's vertex shader sways it along its normals (Lighting.hlsl TREE_ANIM)
	};

	/** @brief M7: one skin partition of a skinned shape this frame, with its bone palette. */
	struct SkinnedPartition
	{
		const void* skinInstance = nullptr;  // identity of the animated instance (with partitionIndex)
		uint32_t partitionIndex = 0;
		uint32_t candidateIndex = 0;  // into the frame's candidates (bind-pose mesh + material)
		uint32_t paletteOffset = 0;   // first float in SkinnedScene::palettes (12 per bone: rows of a 3x4, absolute world)
		uint32_t boneCount = 0;
		uint32_t skinningOffset = 0;  // byte offset of the weights (4 x half) and bone indices (4 x uint8) in a vertex
		bool halfPositions = false;   // positions are 4 x half instead of float3 + pad
		// M7b BSDynamicTriShape (FaceGen heads): morphed bind-pose positions the game keeps on the CPU. Valid this frame only.
		const uint8_t* dynamicData = nullptr;
		uint32_t dynamicStride = 0;      // bytes per vertex (float3 at offset 0)
		uint32_t dynamicVertexCount = 0;
		uint32_t dynamicVersion = 0;     // BSDynamicTriShape frameCount: changes when the game rewrites the data
		bool tree = false;               // under a BSTreeNode
		uint32_t rendererFrameID = 0;    // NiSkinInstance::frameID when its renderer matrices were read, else 0
	};

	/**
	 * @brief M7 tree-pose diagnostic: what one skin instance looked like at the scene walk (and, filled by the sidecar,
	 * again at Present of the same frame). Compares our palette source (boneWorldTransforms x skinToBone) with the
	 * renderer's own NiSkinInstance::boneMatrices cache, whose layout this is meant to reveal.
	 */
	struct SkinPoseSample
	{
		static constexpr uint32_t kRawFloats = 36;  // three bones' worth if the cache holds 3 x float4 per bone

		const RE::NiSkinInstance* skin = nullptr;  // identity only; re-read at Present under an SEH guard
		bool tree = false;                         // under a BSTreeNode (branches sway on bones)
		uint32_t boneCount = 0;
		// At the scene walk.
		uint32_t frameID = 0;
		uint32_t numMatrices = 0;
		uint32_t numRegisters = 0;
		uint32_t allocatedSize = 0;
		std::array<float, 12> bone0World{};  // boneWorldTransforms[0] as row-major 3x4 (rotate * scale | translate)
		std::array<float, 12> palette0{};    // our palette entry for skin bone 0 (boneWorld * skinToBone)
		std::array<float, kRawFloats> boneMatrices{};
		std::array<float, kRawFloats> prevBoneMatrices{};
		bool boneMatricesRead = false;
		// At Present of the same frame.
		bool presentRead = false;
		uint32_t presentFrameID = 0;
		std::array<float, 12> presentBone0World{};
		std::array<float, kRawFloats> presentBoneMatrices{};
	};

	struct SkinnedScene
	{
		std::vector<SkinnedPartition> partitions;
		std::vector<float> palettes;
		std::vector<float> rendererPalettes;  ///< same layout: the renderer's matrices for each palette bone (else a copy)
		std::vector<SkinPoseSample> poseSamples;  ///< up to 4 trees and 4 other skins per frame (M7 diagnostic)
	};

	/** @brief Fills a sample's walk-time fields from its skin instance (call during the scene walk). */
	void ReadSkinPose(const RE::NiSkinInstance* a_skin, SkinPoseSample& a_sample);
	/** @brief Re-reads a sample's Present-time fields; false if the skin can't be read (SEH-guarded). */
	bool ReadSkinPoseAtPresent(SkinPoseSample& a_sample);

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

	/** @brief M7: our bone palettes vs the renderer's NiSkinInstance::boneMatrices, per kind of skin. */
	struct SkinPoseStats
	{
		uint32_t shapes = 0;
		uint32_t compared = 0;   ///< shapes whose renderer matrices could be read
		uint32_t differ = 0;     ///< compared shapes with any bone translation more than 1 unit off
		float maxDelta = 0.0f;   ///< largest bone translation difference (game units)
		uint32_t minFrameID = UINT32_MAX;  ///< NiSkinInstance::frameID range (when the renderer last refreshed the matrices)
		uint32_t maxFrameID = 0;
		uint32_t fromCache = 0;  ///< partitions posed from the renderer's matrices
		std::array<uint32_t, 4> lagPartitions{};  ///< trees: partitions whose matrices are 0, 1-2, 3-8, >8 frames older than the newest
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
		// M7 skinned geometry.
		uint32_t skinnedShapes = 0;           ///< skinned shapes extracted (at least one partition)
		uint32_t skinnedPartitions = 0;
		uint32_t skinnedBones = 0;            ///< palette entries this frame
		uint32_t skinnedHalfPositions = 0;    ///< partitions whose positions are 4 x half
		uint32_t skinnedRejectedShapes = 0;   ///< no usable skin data: excluded as before
		uint32_t skinnedRejectedPartitions = 0;
		uint32_t dynamicShapes = 0;          ///< M7b: dynamic (FaceGen) shapes extracted as skinned
		uint32_t dynamicRejectedShapes = 0;  ///< dynamic shapes without usable skin or position data: excluded
		SkinPoseStats treePose;   ///< skinned shapes under a BSTreeNode
		SkinPoseStats otherPose;  ///< actors and everything else skinned
		float traversalMs = 0.0f;
	};

	/**
	 * @brief Collects static and terrain geometry, and (M7) skinned partitions with their bone palettes, from every
	 * loaded cell into a_out / a_skinned (cleared first).
	 * @return False when there is no world (main menu, loading), in which case a_out is empty.
	 */
	bool CollectScene(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats,
		bool a_poseFromCache);
}
