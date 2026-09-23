#include "SharedTexture.h"

#include "RT.h"

#include <dxgi1_2.h>

namespace RT
{
	bool CreateSharedTexture(ID3D11Device5* a_d3d11Device, ID3D12Device* a_device, uint32_t a_width, uint32_t a_height,
		DXGI_FORMAT a_format, const char* a_name, SharedTexture& a_out, std::string& a_error)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT hr = a_d3d11Device->CreateTexture2D(&desc, nullptr, a_out.texture11.put());
		if (FAILED(hr)) {
			a_error = std::format("CreateTexture2D({}) failed ({})", a_name, FormatHResult(hr));
			return false;
		}
		Util::SetResourceName(a_out.texture11.get(), "SkyrimRT::%s", a_name);

		winrt::com_ptr<IDXGIResource1> dxgiResource;
		HANDLE sharedHandle = nullptr;
		hr = a_out.texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
		if (SUCCEEDED(hr))
			hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
		if (SUCCEEDED(hr)) {
			hr = a_device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(a_out.resource12.put()));
			CloseHandle(sharedHandle);
		}
		if (FAILED(hr)) {
			a_error = std::format("sharing {} with D3D12 failed ({})", a_name, FormatHResult(hr));
			return false;
		}

		if (FAILED(hr = a_d3d11Device->CreateShaderResourceView(a_out.texture11.get(), nullptr, a_out.srv11.put())) ||
			FAILED(hr = a_d3d11Device->CreateUnorderedAccessView(a_out.texture11.get(), nullptr, a_out.uav11.put()))) {
			a_error = std::format("D3D11 views for {} failed ({})", a_name, FormatHResult(hr));
			return false;
		}
		Util::SetResourceName(a_out.srv11.get(), "SkyrimRT::%s SRV", a_name);
		Util::SetResourceName(a_out.uav11.get(), "SkyrimRT::%s UAV", a_name);
		return true;
	}
}
