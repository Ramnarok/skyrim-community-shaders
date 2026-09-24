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

	/** @brief How trees (skinned shapes under a BSTreeNode) are traced. */
	enum class TreeMode : uint8_t
	{
		kLiveBones,    // skinned from their bones' current pose (the last culling camera's sway)
		kRestSkinned,  // M7: skinned, every bone at the rest pose
		kRestStatic,   // M8: the rest pose as one rigid transform: a static instance of the bind-pose mesh, no skinning or refit
	};

	/** @brief What CollectScene extracts. */
	struct SceneOptions
	{
		TreeMode treeMode = TreeMode::kRestStatic;
		bool skipMeshLOD = false;  // M8 diagnostic: leave out every kMeshLOD shape, not only the alternates (see CollectScene)
		bool distantLOD = false;   // M8: walk TES::lodLandRoot too (distant terrain and object LOD), exteriors only
	};

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
		uint32_t albedoWord = 0;     // M8: albedo-atlas tile, UV and vertex-colour offsets (InstanceData.Flags >> 8), by AlbedoAtlas; 0 = average
		bool windAnimated = false;   // M7c: kTreeAnim; the game's vertex shader sways it along its normals (Lighting.hlsl TREE_ANIM)
		// M8: nearest BSMultiBoundRoom / BSPortalSharedNode ancestor (LightLimitFix GetParentRoomNode), valid this frame only,
		// and its Light Limit Fix room index + 1 (0 = none or no light uses it), filled by the sidecar.
		const RE::NiNode* room = nullptr;
		uint32_t roomWord = 0;
		bool tree = false;  // M8: a tree partition traced as a static instance (TreeMode::kRestStatic)
		RE::BSGeometry* geometry = nullptr;  // the scene-graph shape (skinned: shared by its partitions), valid this frame only
		const RE::NiNode* objectRoot = nullptr;  // outermost BSFadeNode above the shape (the object's root), valid this frame only
		bool meshLOD = false;  // a pure lower-detail copy: kMeshLOD and named "L<digit>_..." (merged "X - L2_..." shapes are not)
		// M8: kDecal / kDynamicDecal: an overlay drawn just above another surface without writing depth (road dirt and grass
		// strips). Traced, it shadowed the surface under it (Whiterun gate), so it stays out of every trace.
		bool decal = false;
		// M8 distant LOD: a shape under TES::lodLandRoot. It's traced only outside the loaded cells, where the game draws it;
		// lodClip marks one whose bound reaches into them, so the traces reject its hits inside (InstanceData LOD clip).
		bool distantLOD = false;
		bool lodClip = false;
		bool treeLOD = false;  // M8: one distant tree (a BGSDistantTreeBlock instance of its group's crossed-quad card)
	};

	/** @brief Debug dump: one TLAS candidate near the camera, described while its game pointers are valid. */
	struct NearbyObject
	{
		std::string name;         // the shape's NiAVObject name
		std::string parents;      // up to four ancestor names, nearest first, " < "-separated
		uint32_t refFormID = 0;   // owning TESObjectREFR (NiAVObject user data), 0 if none found
		uint32_t baseFormID = 0;
		std::string baseName;     // TESForm::GetName of the base object
		uint32_t flags = 0;          // the shape's NiAVObject flags
		uint32_t ancestorFlags = 0;  // OR of every ancestor's flags (kHidden subtrees are never walked)
		float minFade = 1.0f;        // lowest BSFadeNode::currentFade above the shape
		float distance = 0.0f;       // from the camera to the shape's world bound (0 = inside it)
		float offset[3]{};           // bound centre - camera
		float boundRadius = 0.0f;
		uint32_t triangles = 0;
		bool alphaTested = false, alphaBlended = false, windAnimated = false, skinned = false, terrain = false, tree = false;
		// What decides whether the raster draws the shape at all (M8, invisible-occluder hunt).
		float materialAlpha = 1.0f;    // BSShaderProperty::alpha
		uint32_t renderPasses = 0;     // BSShaderProperty::renderPassList length (capped at 16): passes built for this frame
		int32_t lastRenderPassState = 0;
		bool vertexAlpha = false;      // kVertexAlpha: the game multiplies alpha by the vertex colour's (our alpha test doesn't)
		bool vertexColors = false;     // the vertex format has colours (VF_COLORS)
		bool decal = false;            // kDecal or kDynamicDecal
	};

	/**
	 * @brief Describes this frame's candidates whose world bound comes within a_radius of a_center, nearest first, at most
	 * a_maxCount (one entry per shape). Must run in the frame CollectScene filled a_candidates.
	 */
	void DescribeNearby(const std::vector<GeometryCandidate>& a_candidates, const RE::NiPoint3& a_center, float a_radius, size_t a_maxCount,
		std::vector<NearbyObject>& a_out);

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
	};

	struct SkinnedScene
	{
		std::vector<SkinnedPartition> partitions;
		std::vector<float> palettes;
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
		uint32_t decalInstances = 0;         ///< M8: subset of static + terrain instances that are decals (not traced)
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
		uint32_t skinnedInvalidPoses = 0;  ///< partitions skipped for a non-finite or absurd bone this frame (refit safety)
		uint32_t instancesInRooms = 0;  ///< M8: candidates whose room/portal ancestor has a Light Limit Fix room index
		uint32_t roomNodes = 0;         ///< M8: rooms/portals Light Limit Fix indexed this frame (referenced by its lights)
		uint32_t dynamicShapes = 0;          ///< M7b: dynamic (FaceGen) shapes extracted as skinned
		uint32_t dynamicRejectedShapes = 0;  ///< dynamic shapes without usable skin or position data: excluded
		uint32_t treeShapes = 0;          ///< skinned shapes under a BSLeafAnimNode, BSTreeNode included (they sway on bones)
		uint32_t leafAnimShapes = 0;      ///< M8: the subset under a plain BSLeafAnimNode (not a BSTreeNode)
		uint32_t meshLODShapes = 0;       ///< M8: geometry flagged kMeshLOD (walked, whether extracted or skipped)
		uint32_t meshLODSkipped = 0;      ///< M8: of those, left out by the diagnostic SceneOptions::skipMeshLOD
		uint32_t meshLODAlternates = 0;   ///< M8: of those, left out as alternates of an object that has plain geometry
		uint32_t treeRestPoseShapes = 0;  ///< trees traced in their rest pose
		uint32_t treeStaticShapes = 0;            ///< M8: rest-pose trees with at least one static partition
		uint32_t treeStaticPartitions = 0;        ///< M8: static tree instances (one per partition)
		uint32_t treeHalfPositionPartitions = 0;  ///< M8: rest-pose tree partitions with half positions, left on the skinned path
		uint32_t uniqueTreeMeshes = 0;            ///< M8: distinct bind-pose buffers among the static tree instances (BLASes needed)
		// M8 distant LOD (TES::lodLandRoot), exteriors with SceneOptions::distantLOD.
		struct DistantLOD
		{
			bool walked = false;
			uint32_t hiddenSubtrees = 0;      ///< kHidden (app-culled) nodes under the LOD root: blocks and cells the game isn't drawing
			uint32_t terrainShapes = 0;       ///< traced: LOD land (material kLODLand / kLODLandNoise)
			uint32_t objectShapes = 0;        ///< traced: other LOD geometry (object LOD)
			uint32_t clippedShapes = 0;       ///< of the traced, bound reaching into the loaded cells (hits there rejected)
			uint64_t triangles = 0;           ///< of the traced
			uint32_t skippedTrees = 0;        ///< tree LOD billboards (instanced), not traced
			uint32_t alphaTestedShapes = 0;   ///< of the traced, alpha-tested (object LOD; alpha-atlas tile when available)
			uint32_t skippedBlendedDecal = 0; ///< alpha-blended or decal LOD, not traced
			uint32_t skippedOther = 0;        ///< water, effects, no renderer data, skinned
			uint32_t skippedHalfPositions = 0;  ///< positions stored as 4 x half (the BLAS reads float3), not traced
			std::array<uint32_t, 32> byGeometryType{};  ///< every LOD shape seen, by BSGeometry::Type
		} lod;
		// M8 tree LOD census (diagnostic, before tracing it): the terrain quadtree's BGSDistantTreeBlocks, which aren't under
		// TES::lodLandRoot. Raw samples of the first groups, to learn the instance formats from real data.
		struct TreeLODSample
		{
			int32_t baseCellX = 0, baseCellY = 0;
			uint32_t lodLevel = 0;
			std::string geometryName, geometryRTTI, propertyRTTI, parents;
			uint32_t ancestorFlags = 0;  // OR of the geometry's and its ancestors' NiAVObject flags
			bool blockAttached = false, blockAllVisible = false;
			int32_t treeType = 0;
			uint32_t groupNum = 0, instanceArraySize = 0;
			float worldTranslate[3]{}, worldScale = 0.0f;
			float boundCenter[3]{}, boundRadius = 0.0f;
			uint64_t vertexDesc = 0;
			uint32_t vertexCount = 0, triangleCount = 0, stride = 0;
			bool rawVertices = false, rawIndices = false;
			std::vector<float> firstVertices;  // up to 4 vertices: the first 6 floats of each (raw, if present)
			// BSMultiStreamInstanceTriShape runtime data
			uint32_t instanceGroups = 0, meshTriCount = 0, maxInstancesPerGroup = 0, instanceCount = 0, instanceSize = 0, activeGroupCount = 0;
			float renderDistance = 0.0f;
			uint32_t group0TriCount = 0, group0InstanceCount = 0;
			bool group0Visible = false, group0CpuData = false;
			uint64_t group0ByteWidth = 0;
			std::string group0FirstBytes;  // hex of the first two instances (instanceSize bytes each), from the CPU copy
			std::vector<std::array<uint32_t, 7>> firstInstances;  // BGSDistantTreeBlock::InstanceData: id, x, y, z, rotZ, scale, hidden
		};
		struct TreeLOD
		{
			bool walked = false;
			bool haveManager = false;
			// The census faulted every frame in its first run (CommonLib's quadtree layouts vs 1.7.104). It now runs under its
			// own guard, stops for the session after a fault, and keeps what it had reached, to locate the mismatch.
			bool faulted = false;
			uint32_t stage = 0;  ///< 1 worldspace, 2 manager read, 3 root node read, 4 walking nodes, 5 sampling a group
			std::string managerHex, rootNodeHex;  ///< raw bytes: BGSTerrainManager (0xD0), root BGSTerrainNode (0x50)
			uint64_t managerAddress = 0, rootNodeAddress = 0;
			uint32_t childMismatches = 0;  ///< child nodes whose manager / parent didn't check out (skipped)
			// Traced trees (one candidate each) and why the others weren't.
			uint32_t traced = 0, clipped = 0, skippedHidden = 0, skippedInsideLoaded = 0, skippedFar = 0, skippedInvalid = 0, groupsWithoutTexture = 0;
			uint32_t texturesFromVisitor = 0;  ///< groups whose billboard texture came from ForEachTexture
			uint32_t groupsDecoded = 0;        ///< groups whose transforms were (re)decoded this frame (else cached)
			uint32_t cachedGroups = 0;         ///< groups in the transform cache after this walk
			std::string textureName;           ///< the first billboard texture found
			std::string atlasPath;             ///< the worldspace's tree billboard atlas, loaded by path
			bool atlasLoaded = false;
			uint32_t nodes = 0, treeLayers = 0, blocks = 0, blocksAttached = 0, groups = 0, groupsWithGeometry = 0;
			uint64_t instances = 0, hiddenInstances = 0;
			std::vector<TreeLODSample> samples;  // first attached groups, up to 4
		} treeLOD;
		float traversalMs = 0.0f;
	};

	/**
	 * @brief Collects static and terrain geometry, and (M7) skinned partitions with their bone palettes, from every
	 * loaded cell into a_out / a_skinned (cleared first).
	 * @return False when there is no world (main menu, loading), in which case a_out is empty.
	 */
	bool CollectScene(std::vector<GeometryCandidate>& a_out, SkinnedScene& a_skinned, std::vector<ExclusionBound>& a_exclusions, LoadedArea& a_area, SceneStats& a_stats,
		const SceneOptions& a_options);
}
