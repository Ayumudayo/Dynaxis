#include <gtest/gtest.h>
#include <server/core/memory/memory_pool.hpp>
#include <vector>
#include <thread>
#include <limits>
#include <stdexcept>

/**
 * @brief MemoryPool/BufferManager의 기본 할당 및 RAII 반환 동작을 검증합니다.
 */
using namespace server::core;

// MemoryPool 기본 할당 테스트
TEST(MemoryPoolTest, BasicAllocation) {
    // 1KB 블록 10개를 관리하는 메모리 풀 생성
    MemoryPool pool(1024, 10); 

    // 첫 번째 블록 할당 요청
    void* ptr1 = pool.Acquire();
    EXPECT_NE(ptr1, nullptr); // 할당 성공 여부 확인

    // 두 번째 블록 할당 요청
    void* ptr2 = pool.Acquire();
    EXPECT_NE(ptr2, nullptr); // 할당 성공 여부 확인
    EXPECT_NE(ptr1, ptr2);    // 두 포인터가 서로 다른 주소인지 확인

    // 사용한 메모리 반환
    pool.Release(ptr1);
    pool.Release(ptr2);
}

// 메모리 풀 고갈 테스트
TEST(MemoryPoolTest, Exhaustion) {
    // 100바이트 블록 2개만 관리하는 작은 풀 생성
    MemoryPool pool(100, 2); 

    void* ptr1 = pool.Acquire();
    void* ptr2 = pool.Acquire();
    
    EXPECT_NE(ptr1, nullptr);
    EXPECT_NE(ptr2, nullptr);

    EXPECT_EQ(pool.Acquire(), nullptr);
    
    pool.Release(ptr1);
    pool.Release(ptr2);
}

TEST(MemoryPoolTest, DoubleReleaseDoesNotAliasLiveAllocations) {
    MemoryPool pool(16, 2);
    void* first = pool.Acquire();
    void* second = pool.Acquire();
    pool.Release(first);
    pool.Release(first);
    void* reused = pool.Acquire();
    EXPECT_EQ(reused, first);
    EXPECT_EQ(pool.Acquire(), nullptr);
    pool.Release(second);
    void* other = pool.Acquire();
    EXPECT_NE(other, reused);
    pool.Release(other);
    pool.Release(reused);
}

TEST(MemoryPoolTest, RejectsOverflowBeforeAllocating) {
    EXPECT_THROW(MemoryPool(std::numeric_limits<std::size_t>::max() / 2 + 1, 2), std::length_error);
}

// BufferManager RAII(자원 획득 즉시 초기화) 패턴 테스트
TEST(BufferManagerTest, RAII) {
    BufferManager manager(1024, 5);

    {
        // 버퍼 획득 (스마트 포인터로 반환됨)
        auto buf = manager.Acquire();
        EXPECT_NE(buf, nullptr);
        // 이 블록({})을 벗어나면 buf는 자동으로 소멸되며 메모리 풀로 반환됨
    }

    // 반환된 메모리를 다시 할당받을 수 있는지 확인
    auto buf2 = manager.Acquire();
    EXPECT_NE(buf2, nullptr);
}

TEST(BufferManagerTest, OutstandingBufferOwnsPoolAfterManagerDestruction) {
    BufferManager::PooledBuffer buffer;
    {
        BufferManager manager(32, 1);
        buffer = manager.Acquire();
        ASSERT_TRUE(buffer);
        buffer[0] = std::byte{42};
    }
    EXPECT_EQ(buffer[0], std::byte{42});
    buffer.reset();
}
