#pragma once

#include <vector>
#include <span>
#include <cstdint>

namespace server::core::compression {

class Compressor {
public:
    /**
     * @brief LZ4 기본 압축으로 바이트 배열을 압축합니다.
     * @param data 압축할 원본 데이터
     * @return 압축된 바이트 배열
     * @throws std::runtime_error 압축 실패 시 발생
     *
     * 왜 LZ4인가?
     * - 실시간 채팅 경로에서는 압축률보다 지연(latency)과 CPU 비용이 더 중요합니다.
     * - LZ4는 낮은 압축 오버헤드로 네트워크 대역폭을 절약하는 균형점으로 사용됩니다.
     */
    static std::vector<uint8_t> compress(std::span<const uint8_t> data);

    /**
     * @brief LZ4 압축 데이터를 원본으로 복원합니다.
     * @param data 압축된 데이터
     * @param original_size 원본(복원 후) 데이터 크기
     * @return 복원된 원본 바이트 배열
     * @throws std::runtime_error 복원 실패 시 발생
     */
    static std::vector<uint8_t> decompress(std::span<const uint8_t> data, size_t original_size);
    
    /**
     * @brief 입력 크기에 대한 최대 압축 결과 크기를 반환합니다.
     *
     * 전송 버퍼를 미리 할당할 때 사용해 재할당 비용을 줄일 수 있습니다.
     */
    static size_t get_max_compressed_size(size_t input_size);
};

} // namespace server::core::compression
