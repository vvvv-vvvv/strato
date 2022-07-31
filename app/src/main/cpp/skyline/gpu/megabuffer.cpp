// SPDX-License-Identifier: MPL-2.0
// Copyright © 2021 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <gpu.h>
#include "megabuffer.h"

namespace skyline::gpu {
    constexpr static vk::DeviceSize MegaBufferChunkSize{25 * 1024 * 1024}; //!< Size in bytes of a single megabuffer chunk (25MiB)

    MegaBufferChunk::MegaBufferChunk(GPU &gpu) : backing{gpu.memory.AllocateBuffer(MegaBufferChunkSize)}, freeRegion{backing.subspan(PAGE_SIZE)} {}

    bool MegaBufferChunk::TryReset() {
        if (cycle && cycle->Poll(true)) {
            freeRegion = backing.subspan(PAGE_SIZE);
            cycle = nullptr;
            return true;
        }

        return cycle == nullptr;
    }

    vk::Buffer MegaBufferChunk::GetBacking() const {
        return backing.vkBuffer;
    }

    vk::DeviceSize MegaBufferChunk::Push(const std::shared_ptr<FenceCycle> &newCycle, span<u8> data, bool pageAlign) {
        if (pageAlign) {
            // If page aligned data was requested then align the free
            auto alignedFreeBase{util::AlignUp(static_cast<size_t>(freeRegion.data() - backing.data()), PAGE_SIZE)};
            freeRegion = backing.subspan(alignedFreeBase);
        }

        if (data.size() > freeRegion.size())
            return 0;

        if (cycle != newCycle) {
            newCycle->ChainCycle(cycle);
            cycle = newCycle;
        }

        // Allocate space for data from the free region
        auto resultSpan{freeRegion.subspan(0, data.size())};
        resultSpan.copy_from(data);

        // Move the free region along
        freeRegion = freeRegion.subspan(data.size());
        return static_cast<vk::DeviceSize>(resultSpan.data() - backing.data());
    }

    MegaBufferAllocator::MegaBufferAllocator(GPU &gpu) : gpu{gpu}, activeChunk{chunks.emplace(chunks.end(), gpu)} {}

    void MegaBufferAllocator::lock() {
        mutex.lock();
    }

    void MegaBufferAllocator::unlock() {
        mutex.unlock();
    }

    bool MegaBufferAllocator::try_lock() {
        return mutex.try_lock();
    }

    MegaBufferAllocator::Allocation MegaBufferAllocator::Push(const std::shared_ptr<FenceCycle> &cycle, span<u8> data, bool pageAlign) {
        if (vk::DeviceSize offset{activeChunk->Push(cycle, data, pageAlign)}; offset)
            return {activeChunk->GetBacking(), offset};

        activeChunk = ranges::find_if(chunks, [&](auto &chunk) { return chunk.TryReset(); });
        if (activeChunk == chunks.end()) // If there are no chunks available, allocate a new one
            activeChunk = chunks.emplace(chunks.end(), gpu);

        if (vk::DeviceSize offset{activeChunk->Push(cycle, data, pageAlign)}; offset)
            return {activeChunk->GetBacking(), offset};
        else
            throw exception("Failed to to allocate megabuffer space for size: 0x{:X}", data.size());
    }
}