#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include "FrameTypes.h"

namespace RT
{
	/**
	 * @brief Captures the game's final frame (kFRAMEBUFFER) for the debug dump: a D3D11 copy into a staging texture,
	 * read back later with D3D11_MAP_FLAG_DO_NOT_WAIT so the render thread never waits on the GPU.
	 */
	class FrameCapture
	{
	public:
		/** @brief Queues the copy on the immediate context. On failure the capture finishes immediately with no image. */
		void Begin(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string a_name);

		/** @brief True once the capture has finished (with or without an image); never waits. */
		bool Poll(ID3D11DeviceContext* a_context);

		bool IsIdle() const { return !pending && !finished; }

		/** @brief Moves the image out (empty pixels if the capture failed) and returns to idle. */
		DumpImage Take();

	private:
		winrt::com_ptr<ID3D11Texture2D> staging;
		D3D11_TEXTURE2D_DESC stagingDesc{};
		DumpImage image;
		bool pending = false;
		bool finished = false;
	};
}
