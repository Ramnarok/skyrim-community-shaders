#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

namespace RT
{
	/** @brief A 2D texture visible to both devices: D3D11 SRV/UAV plus the opened D3D12 resource. */
	struct SharedTexture
	{
		winrt::com_ptr<ID3D11Texture2D> texture11;
		winrt::com_ptr<ID3D11ShaderResourceView> srv11;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav11;
		winrt::com_ptr<ID3D12Resource> resource12;
	};

	/**
	 * @brief Creates the texture in D3D11 (MISC_SHARED | MISC_SHARED_NTHANDLE) and opens it in D3D12, the only
	 * direction that works (M2, ARCHITECTURE §2). The D3D12 resource starts in COMMON.
	 * @return False with a_error set on failure.
	 */
	bool CreateSharedTexture(ID3D11Device5* a_d3d11Device, ID3D12Device* a_device, uint32_t a_width, uint32_t a_height,
		DXGI_FORMAT a_format, const char* a_name, SharedTexture& a_out, std::string& a_error);
}
