#include "BufferPool.h"

namespace RT
{
	HRESULT CreateBufferResource(ID3D12Device* a_device, D3D12_HEAP_TYPE a_heap, uint64_t a_bytes, D3D12_RESOURCE_STATES a_state, D3D12_RESOURCE_FLAGS a_flags, ID3D12Resource** a_out)
	{
		D3D12_HEAP_PROPERTIES heap{ .Type = a_heap };
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = a_bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc = { 1, 0 };
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = a_flags;
		return a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr, IID_PPV_ARGS(a_out));
	}

	void UpdatePageDescriptors(ID3D12Device* a_device, const BufferPool& a_pool, D3D12_CPU_DESCRIPTOR_HANDLE a_first, uint32_t a_increment,
		uint64_t* a_describedSerials, uint32_t a_slots)
	{
		for (uint32_t i = 0; i < a_slots; i++) {
			const uint64_t serial = a_pool.GetPageSerial(i);
			if (serial == a_describedSerials[i])
				continue;
			a_describedSerials[i] = serial;

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R32_TYPELESS;
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			auto* resource = serial ? a_pool.GetResource(i) : nullptr;
			srv.Buffer.NumElements = resource ? static_cast<UINT>(a_pool.GetPageSize(i) / 4) : 1;

			auto handle = a_first;
			handle.ptr += static_cast<SIZE_T>(i) * a_increment;
			a_device->CreateShaderResourceView(resource, &srv, handle);
		}
	}

	void BufferPool::Init(ID3D12Device* a_device, uint64_t a_pageBytes, uint64_t a_budgetBytes, D3D12_RESOURCE_STATES a_initialState, D3D12_RESOURCE_FLAGS a_flags, const wchar_t* a_name)
	{
		device = a_device;
		pageBytes = a_pageBytes;
		budgetBytes = a_budgetBytes;
		initialState = a_initialState;
		flags = a_flags;
		name = a_name;
	}

	bool BufferPool::CreatePage(uint64_t a_bytes, bool a_dedicated, uint32_t& a_index)
	{
		winrt::com_ptr<ID3D12Resource> buffer;
		if (FAILED(CreateBufferResource(device, D3D12_HEAP_TYPE_DEFAULT, a_bytes, initialState, flags, buffer.put())))
			return false;
		buffer->SetName(name);

		a_index = static_cast<uint32_t>(pages.size());
		for (uint32_t i = 0; i < pages.size(); i++) {
			if (!pages[i].buffer) {
				a_index = i;
				break;
			}
		}
		if (a_index == pages.size())
			pages.emplace_back();

		auto& page = pages[a_index];
		page.buffer = std::move(buffer);
		page.size = a_bytes;
		page.dedicated = a_dedicated;
		page.serial = nextSerial++;
		page.freeRanges.clear();
		if (!a_dedicated)
			page.freeRanges.emplace(0, a_bytes);
		reservedBytes += a_bytes;
		return true;
	}

	bool BufferPool::Allocate(uint64_t a_bytes, PoolAllocation& a_out)
	{
		const uint64_t bytes = (std::max<uint64_t>(a_bytes, 1) + kAlignment - 1) & ~(kAlignment - 1);

		if (bytes > pageBytes) {
			uint32_t index;
			if (reservedBytes + bytes > budgetBytes || !CreatePage(bytes, true, index))
				return false;
			a_out = { index, 0, bytes };
			return true;
		}

		auto tryPage = [&](uint32_t a_index) {
			auto& page = pages[a_index];
			for (auto it = page.freeRanges.begin(); it != page.freeRanges.end(); ++it) {
				if (it->second < bytes)
					continue;
				const uint64_t offset = it->first;
				const uint64_t remaining = it->second - bytes;
				page.freeRanges.erase(it);
				if (remaining)
					page.freeRanges.emplace(offset + bytes, remaining);
				a_out = { a_index, offset, bytes };
				return true;
			}
			return false;
		};

		for (uint32_t i = 0; i < pages.size(); i++) {
			if (pages[i].buffer && !pages[i].dedicated && tryPage(i))
				return true;
		}

		uint32_t index;
		if (reservedBytes + pageBytes > budgetBytes || !CreatePage(pageBytes, false, index))
			return false;
		return tryPage(index);
	}

	void BufferPool::Free(PoolAllocation& a_allocation)
	{
		if (!a_allocation.IsValid())
			return;
		auto& page = pages[a_allocation.page];
		if (page.dedicated) {
			reservedBytes -= page.size;
			page.buffer = nullptr;
			page.size = 0;
			page.serial = 0;
		} else {
			auto& ranges = page.freeRanges;
			auto [it, inserted] = ranges.emplace(a_allocation.offset, a_allocation.size);
			if (auto next = std::next(it); next != ranges.end() && it->first + it->second == next->first) {
				it->second += next->second;
				ranges.erase(next);
			}
			if (it != ranges.begin()) {
				if (auto prev = std::prev(it); prev->first + prev->second == it->first) {
					prev->second += it->second;
					ranges.erase(it);
				}
			}
		}
		a_allocation = {};
	}

	uint32_t BufferPool::GetPageCount() const
	{
		uint32_t count = 0;
		for (const auto& page : pages)
			count += page.buffer != nullptr;
		return count;
	}
}
