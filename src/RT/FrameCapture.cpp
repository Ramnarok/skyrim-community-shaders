#include "FrameCapture.h"

#include "RT.h"

#include <DirectXTex.h>

namespace RT
{
	namespace
	{
		// kFRAMEBUFFER's texture pointer can be null when it aliases the swap-chain buffer (as ScreenshotFeature notes);
		// resolve it from its views then.
		winrt::com_ptr<ID3D11Texture2D> ResolveFramebuffer()
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return texture;
			const auto& slot = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
			if (slot.texture) {
				texture.copy_from(slot.texture);
				return texture;
			}
			ID3D11View* view = slot.SRV ? static_cast<ID3D11View*>(slot.SRV) : static_cast<ID3D11View*>(slot.RTV);
			if (view) {
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				if (resource)
					resource.try_as(texture);
			}
			return texture;
		}

		// sRGB variants hold sRGB-encoded bytes already; reading them as UNORM keeps those bytes in the PNG.
		DXGI_FORMAT StripSrgb(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return a_format;
			}
		}
	}

	void FrameCapture::Begin(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string a_name)
	{
		image = {};
		image.name = std::move(a_name);
		pending = false;
		finished = true;  // until the copy is queued, a failure means "done, no image"

		auto source = ResolveFramebuffer();
		if (!source) {
			logger::warn("[SkyrimRT] Debug dump: kFRAMEBUFFER not available for {}", image.name);
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		if (desc.SampleDesc.Count != 1) {
			logger::warn("[SkyrimRT] Debug dump: kFRAMEBUFFER is multisampled, {} skipped", image.name);
			return;
		}

		if (!staging || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height || stagingDesc.Format != desc.Format) {
			staging = nullptr;
			D3D11_TEXTURE2D_DESC stagingDescNew{};
			stagingDescNew.Width = desc.Width;
			stagingDescNew.Height = desc.Height;
			stagingDescNew.MipLevels = 1;
			stagingDescNew.ArraySize = 1;
			stagingDescNew.Format = desc.Format;
			stagingDescNew.SampleDesc = { 1, 0 };
			stagingDescNew.Usage = D3D11_USAGE_STAGING;
			stagingDescNew.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(a_device->CreateTexture2D(&stagingDescNew, nullptr, staging.put()))) {
				logger::warn("[SkyrimRT] Debug dump: cannot create a staging copy of kFRAMEBUFFER (format {})", static_cast<uint32_t>(desc.Format));
				return;
			}
			Util::SetResourceName(staging.get(), "SkyrimRT::FrameCaptureStaging");
			stagingDesc = stagingDescNew;
		}

		a_context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, source.get(), 0, nullptr);
		pending = true;
		finished = false;
	}

	bool FrameCapture::Poll(ID3D11DeviceContext* a_context)
	{
		if (finished)
			return true;
		if (!pending)
			return false;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = a_context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			return false;
		pending = false;
		finished = true;
		if (FAILED(hr)) {
			logger::warn("[SkyrimRT] Debug dump: mapping the {} capture failed ({})", image.name, FormatHResult(hr));
			return true;
		}

		const DXGI_FORMAT format = StripSrgb(stagingDesc.Format);
		const size_t rowBytes = static_cast<size_t>(stagingDesc.Width) * (DirectX::BitsPerPixel(format) / 8);
		image.width = stagingDesc.Width;
		image.height = stagingDesc.Height;
		image.format = format;
		image.pixels.resize(rowBytes * stagingDesc.Height);
		for (uint32_t y = 0; y < stagingDesc.Height; y++)
			std::memcpy(image.pixels.data() + y * rowBytes, static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
		a_context->Unmap(staging.get(), 0);
		return true;
	}

	DumpImage FrameCapture::Take()
	{
		pending = false;
		finished = false;
		return std::move(image);
	}
}
