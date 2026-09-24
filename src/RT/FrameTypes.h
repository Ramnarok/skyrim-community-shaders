#pragma once

#include <d3d12.h>

namespace RT
{
	/** @brief The pass markers recorded into one command list, in order (the names are string literals). */
	struct PassMarkerLog
	{
		const ID3D12GraphicsCommandList* list = nullptr;
		std::vector<const wchar_t*> names;
	};

	/** @brief The log the render thread's SetPassMarker calls append to while the sidecar records a list, or nullptr. */
	inline PassMarkerLog*& CurrentPassMarkerLog()
	{
		static thread_local PassMarkerLog* log = nullptr;
		return log;
	}

	/**
	 * @brief Names the next commands in a D3D12 list. DRED records each SetMarker as a breadcrumb op but, on the
	 * RTX 4080 SUPER, not its string (measured 2026-09-24), so the names are also kept on the CPU (PassMarkerLog):
	 * Sidecar::LogDeviceRemovedDetails matches the nth SetMarker op with the nth name.
	 */
	inline void SetPassMarker(ID3D12GraphicsCommandList* a_list, const wchar_t* a_name)
	{
		a_list->SetMarker(0, a_name, static_cast<UINT>((std::wcslen(a_name) + 1) * sizeof(wchar_t)));
		if (auto* log = CurrentPassMarkerLog(); log && log->list == a_list)
			log->names.push_back(a_name);
	}

	/** @brief Camera of the frame being traced, captured from CS's per-frame buffer in SkyrimRT::Prepass(). */
	struct FrameCamera
	{
		bool valid = false;
		uint32_t gameFrame = 0;
		float viewProjInverse[16]{};  // FrameBuffer::CameraViewProjInverse, raw (HLSL row_major)
		float viewProj[16]{};         // FrameBuffer::CameraViewProj, raw (HLSL row_major): reprojection next frame
		float view[16]{};             // FrameBuffer::CameraView: camera-relative world -> view (M6)
		float viewInverse[16]{};      // FrameBuffer::CameraViewInverse (M6)
		float projUnjittered[16]{};   // the unjittered projection (M6, NRD): CameraViewProjUnjittered x CameraViewInverse, set by CaptureCameraMotion (FrameBuffer::CameraProjUnjittered holds its inverse)
		// M8 water motion vectors: the matrices the game builds its own motion vectors with (MotionBlur::GetSSMotionVector).
		float viewProjUnjittered[16]{};      // FrameBuffer::CameraViewProjUnjittered
		float prevViewProjUnjittered[16]{};  // FrameBuffer::CameraPreviousViewProjUnjittered (relative to prevPosAdjust)
		RE::NiPoint3 prevPosAdjust;          // FrameBuffer::CameraPreviousPosAdjust
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
