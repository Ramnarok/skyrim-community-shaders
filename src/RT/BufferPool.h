#pragma once

#include <d3d12.h>
#include <winrt/base.h>

namespace RT
{
	/** @brief A range inside one of a BufferPool's buffers. */
	struct PoolAllocation
	{
		static constexpr uint32_t kInvalidPage = UINT32_MAX;
		uint32_t page = kInvalidPage;
		uint64_t offset = 0;
		uint64_t size = 0;
		bool IsValid() const { return page != kInvalidPage; }
	};

	/**
	 * @brief First-fit suballocator over large DEFAULT-heap buffers; oversized requests get a dedicated buffer.
	 * Used for mesh vertex/index data and for acceleration structures (with different state/flags).
	 */
	class BufferPool
	{
	public:
		static constexpr uint64_t kAlignment = 256;  // also D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT

		void Init(ID3D12Device* a_device, uint64_t a_pageBytes, uint64_t a_budgetBytes, D3D12_RESOURCE_STATES a_initialState, D3D12_RESOURCE_FLAGS a_flags, const wchar_t* a_name);
		bool Allocate(uint64_t a_bytes, PoolAllocation& a_out);
		void Free(PoolAllocation& a_allocation);

		ID3D12Resource* GetResource(uint32_t a_page) const { return a_page < pages.size() ? pages[a_page].buffer.get() : nullptr; }
		D3D12_GPU_VIRTUAL_ADDRESS GetAddress(const PoolAllocation& a_allocation) const { return pages[a_allocation.page].buffer->GetGPUVirtualAddress() + a_allocation.offset; }
		uint32_t GetPageSlots() const { return static_cast<uint32_t>(pages.size()); }
		uint64_t GetPageSize(uint32_t a_page) const { return pages[a_page].size; }
		/** @brief Changes whenever the buffer in a page slot is replaced (descriptor caches key on it). */
		uint64_t GetPageSerial(uint32_t a_page) const { return a_page < pages.size() ? pages[a_page].serial : 0; }
		uint64_t GetReservedBytes() const { return reservedBytes; }
		uint32_t GetPageCount() const;

	private:
		struct Page
		{
			winrt::com_ptr<ID3D12Resource> buffer;
			uint64_t size = 0;
			std::map<uint64_t, uint64_t> freeRanges;  // offset -> size
			bool dedicated = false;
			uint64_t serial = 0;
		};
		bool CreatePage(uint64_t a_bytes, bool a_dedicated, uint32_t& a_index);
		uint64_t nextSerial = 1;

		ID3D12Device* device = nullptr;
		uint64_t pageBytes = 0;
		uint64_t budgetBytes = 0;
		D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
		const wchar_t* name = L"SkyrimRT::Pool";
		std::vector<Page> pages;
		uint64_t reservedBytes = 0;
	};

	/**
	 * @brief Rewrites raw-buffer SRVs for the pool's page slots whose buffer changed since a_describedSerials was
	 * recorded (null SRVs for empty slots). A slot's previous buffer is only freed after mesh eviction (120 frames
	 * unreferenced), so no in-flight frame reads a descriptor being overwritten.
	 */
	void UpdatePageDescriptors(ID3D12Device* a_device, const BufferPool& a_pool, D3D12_CPU_DESCRIPTOR_HANDLE a_first, uint32_t a_increment,
		uint64_t* a_describedSerials, uint32_t a_slots);

	/** @brief Creates a committed buffer; shared helper for the RT code. */
	HRESULT CreateBufferResource(ID3D12Device* a_device, D3D12_HEAP_TYPE a_heap, uint64_t a_bytes, D3D12_RESOURCE_STATES a_state, D3D12_RESOURCE_FLAGS a_flags, ID3D12Resource** a_out);
}
