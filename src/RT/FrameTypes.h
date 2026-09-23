#pragma once

namespace RT
{
	/** @brief Camera of the frame being traced, captured from CS's per-frame buffer in SkyrimRT::Prepass(). */
	struct FrameCamera
	{
		bool valid = false;
		uint32_t gameFrame = 0;
		float viewProjInverse[16]{};  // FrameBuffer::CameraViewProjInverse, raw (HLSL row_major)
		float viewProj[16]{};         // FrameBuffer::CameraViewProj, raw (HLSL row_major): reprojection next frame
		RE::NiPoint3 posAdjust;       // FrameBuffer::CameraPosAdjust: TLAS origin
		uint32_t renderWidth = 0;
		uint32_t renderHeight = 0;
	};

	/** @brief One image captured for the debug dump (tightly packed rows). */
	struct DumpImage
	{
		std::string name;
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;  ///< converted to RGBA8 by the dump writer if different
		std::vector<uint8_t> pixels;
	};
}
