#pragma once

#include "MeshCache.h"
#include "RT.h"
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
	};

	/**
	 * @brief Writes frame_<n>.json and debug_testpattern_<n>.png to GetDumpDirectory() on a worker thread,
	 * so PNG encoding and disk I/O never stall the render thread.
	 */
	void WriteDebugDumpAsync(DebugDumpData a_data);
}
