#include "server/core/memory/memory_pool.hpp"
#include "server/core/runtime_metrics.hpp"

/**
 * @brief 고정 블록 메모리 풀/버퍼 매니저 구현입니다.
 *
 * 잦은 new/delete를 줄여 단편화와 할당 지연을 완화하고,
 * 풀 고갈 시 실패를 명시적으로 반환해 메모리 상한을 제어합니다.
 */
namespace server::core {

// --- MemoryPool 구현 ---

MemoryPool::MemoryPool(size_t blockSize, size_t blockCount)
    : blockSize_(blockSize), blockCount_(blockCount) {
    if (blockSize == 0 || blockCount == 0) {
        blockCount_ = 0;
        runtime_metrics::register_memory_pool_capacity(0);
        return;
    }
    // 고정 길이 blockCount 개를 한 번에 할당하면 운영체제 할당/해제 비용을 숨길 수 있습니다.
    // 이는 잦은 new/delete 호출로 인한 단편화와 오버헤드를 줄여줍니다.
    // 메모리 풀의 핵심 원리: 미리 할당하고 재사용한다.
    memoryChunk_.resize(blockSize * blockCount);
    for (size_t i = 0; i < blockCount; ++i) {
        freeList_.push(memoryChunk_.data() + i * blockSize);
    }
    runtime_metrics::register_memory_pool_capacity(blockCount_);
}

MemoryPool::~MemoryPool() {}

void* MemoryPool::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (freeList_.empty()) {
        // 실제 서비스라면 추가 블록을 확장하는 등의 처리가 필요할 수 있지만,
        // 현재 구현에서는 nullptr 을 반환하여 호출자가 처리하도록 합니다.
        // 이는 메모리 사용량의 상한을 강제하는 효과도 있습니다.
        return nullptr;
    }
    void* ptr = freeList_.top();
    freeList_.pop();
    runtime_metrics::record_memory_pool_acquire();
    return ptr;
}

void MemoryPool::Release(void* ptr) {
    if (ptr == nullptr) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (memoryChunk_.empty() || blockSize_ == 0 || blockCount_ == 0) {
        return;
    }

    auto* begin = memoryChunk_.data();
    auto* end = begin + memoryChunk_.size();
    auto* byte_ptr = static_cast<std::byte*>(ptr);
    if (byte_ptr < begin || byte_ptr >= end) {
        return;
    }

    const auto offset = static_cast<std::size_t>(byte_ptr - begin);
    if ((offset % blockSize_) != 0) {
        return;
    }

    if (freeList_.size() >= blockCount_) {
        return;
    }

    freeList_.push(ptr);
    runtime_metrics::record_memory_pool_release();
}

// --- BufferManager 구현 ---

BufferManager::BufferManager(size_t blockSize, size_t blockCount)
    : pool_(blockSize, blockCount), blockSize_(blockSize) {}

BufferManager::PooledBuffer BufferManager::Acquire() {
    std::byte* raw_ptr = static_cast<std::byte*>(pool_.Acquire());
    if (!raw_ptr) {
        // Pool 이 고갈되었습니다. 비어 있는 포인터를 반환합니다.
        return nullptr;
    }

    // pool 로 복귀시키는 custom deleter 를 가진 unique_ptr 을 만듭니다.
    // unique_ptr가 scope를 벗어나면 Release()가 자동으로 호출되므로 호출자는 생명주기만 관리하면 됩니다.
    // 이는 C++의 RAII(Resource Acquisition Is Initialization) 패턴을 활용한 것입니다.
    return PooledBuffer(raw_ptr, [this](std::byte* p) {
        pool_.Release(p);
    });
}

} // namespace server::core
