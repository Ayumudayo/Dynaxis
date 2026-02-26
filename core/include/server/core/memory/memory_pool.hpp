#pragma once

#include <vector>
#include <cstddef>
#include <mutex>
#include <memory>
#include <stack>
#include <functional>

namespace server::core {

/**
 * @brief 고정 크기 메모리 블록을 관리하는 스레드 안전 메모리 풀
 * 
 * 작은 객체를 빈번하게 생성/삭제할 때 발생하는 메모리 파편화와 오버헤드를 줄이기 위해 사용합니다.
 * 미리 큰 메모리 덩어리(Chunk)를 할당해두고, 이를 고정 크기 블록으로 나누어 관리합니다.
 */
class MemoryPool {
public:
    /**
     * @brief 고정 블록 메모리 풀을 생성합니다.
     * @param blockSize 블록당 바이트 크기
     * @param blockCount 블록 개수
     *
     * 계약:
     * - `blockSize == 0` 또는 `blockCount == 0`이면 비활성 풀로 생성됩니다.
     * - 비활성 풀의 `Acquire()`는 항상 `nullptr`를 반환합니다.
     */
    MemoryPool(size_t blockSize, size_t blockCount);
    ~MemoryPool();

    // 복사 불가
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    /**
     * @brief 블록 1개를 할당합니다.
     * @return 성공 시 블록 포인터, 풀이 비활성/고갈 상태면 `nullptr`
     */
    void* Acquire();

    /**
     * @brief 블록을 풀로 반환합니다.
     * @param ptr `Acquire()`로 획득한 블록 포인터
     *
     * 계약:
     * - `nullptr`는 무시됩니다.
     * - 풀 소유 범위를 벗어난 포인터/블록 경계가 아닌 포인터는 무시됩니다.
     */
    void Release(void* ptr);

    size_t block_size() const noexcept { return blockSize_; }
    size_t capacity() const noexcept { return blockCount_; }

private:
    size_t blockSize_;
    size_t blockCount_{0};
    std::vector<std::byte> memoryChunk_;
    std::stack<void*> freeList_;
    std::mutex mutex_;
};


/**
 * @brief MemoryPool을 사용하는 RAII 기반 버퍼 관리자
 * 
 * 메모리 풀에서 버퍼를 할당받고, 사용이 끝나면 자동으로 반환하는 기능을 제공합니다.
 * std::unique_ptr와 커스텀 삭제자(Deleter)를 사용하여 자원 관리를 자동화합니다.
 */
class BufferManager {
public:
    // 소멸 시 자동으로 버퍼를 pool 로 반환하는 스마트 포인터
    using PooledBuffer = std::unique_ptr<std::byte[], std::function<void(std::byte*)>>;

    BufferManager(size_t blockSize, size_t blockCount);
    
    PooledBuffer Acquire();
    size_t GetBlockSize() const { return blockSize_; }

private:
    MemoryPool pool_;
    size_t blockSize_;
};

} // namespace server::core
