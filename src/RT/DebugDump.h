#pragma once

#include "AlbedoAtlas.h"
#include "AlphaAtlas.h"
#include "GlobalIllumination.h"
#include "MaterialTable.h"
#include "MeshCache.h"
#include "RT.h"
#include "Raytracer.h"
#include "Scene.h"

namespace RT
{
	/** @brief Everything one debug dump needs, captured on the render thread. */
	struct DebugDumpData
	{
		uint32_t gameFrame = 0;     ///< CS frame counter when the dump was requested; names the files.
		uint32_t patternFrame = 0;  ///< Sidecar frame index the pattern was dispatched with.
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;  ///< RGBA8, tightly packed.
		bool patternVerified = false;
		Capabilities caps;
		InteropStats stats;
		SpikeResults spike;
		bool inWorld = false;
		SceneStats scene;
		TimingSeries sceneTraversalMs;
		MeshCacheStats cache;
		bool haveTrace = false;  ///< the dump frame ran the M4 debug trace (debug view images are this frame's)
		TraceStats trace;
		bool haveShadows = false;  ///< the dump frame traced M5 sun shadows (mask images and the RT on/off pair)
		SunShadowStats shadows;
		bool pointShadowsAvailable = false;  ///< M8 point-light shadows set up
		bool havePointShadows = false;       ///< the dump frame traced them
		SunShadowStats pointShadows;         ///< counters per PointShadowCounter
		bool giCompiledIn = false;  ///< built with SKYRIMRT_NRD
		bool giAvailable = false;   ///< GI set up successfully
		bool haveGI = false;        ///< the dump frame traced M6 GI
		GIStats gi;
		bool haveSkinned = false;
		SkinnedStats skinned;
		MaterialTableStats materials;
		AlphaAtlasStats alphaAtlas;
		AlbedoAtlasStats albedoAtlas;  // M8
		std::vector<DumpImage> images;  ///< debug_<name>_<frame>.png
		std::vector<NearbyObject> nearby;  ///< TLAS candidates near the camera, nearest first
	};

	/**
	 * @brief Writes frame_<n>.json and debug_testpattern_<n>.png to GetDumpDirectory() on a worker thread,
	 * so PNG encoding and disk I/O never stall the render thread.
	 */
	void WriteDebugDumpAsync(DebugDumpData a_data);
}
