#include "server/core/compression/compressor.hpp"
#include <lz4.h>
#include <iostream>

/**
 * @brief LZ4 압축/해제 구현입니다.
 *
 * 네트워크/스토리지 경로에서 페이로드 크기를 줄여 대역폭을 절감하되,
 * 잘못된 입력은 예외로 즉시 차단해 데이터 무결성을 보호합니다.
 */
namespace server::core::compression {

std::vector<uint8_t> Compressor::compress(std::span<const uint8_t> data) {
    if (data.empty()) return {};

    int max_dst_size = LZ4_compressBound(static_cast<int>(data.size()));
    if (max_dst_size <= 0) throw std::runtime_error("Input too large for LZ4");

    std::vector<uint8_t> compressed(max_dst_size);
    
    int compressed_size = LZ4_compress_default(
        reinterpret_cast<const char*>(data.data()),
        reinterpret_cast<char*>(compressed.data()),
        static_cast<int>(data.size()),
        max_dst_size
    );

    if (compressed_size <= 0) {
        throw std::runtime_error("LZ4 compression failed");
    }

    compressed.resize(compressed_size);
    return compressed;
}

std::vector<uint8_t> Compressor::decompress(std::span<const uint8_t> data, size_t original_size) {
    if (data.empty() && original_size == 0) return {};
    if (data.empty() || original_size == 0) throw std::invalid_argument("Empty input or zero size");

    std::vector<uint8_t> decompressed(original_size);

    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data.data()),
        reinterpret_cast<char*>(decompressed.data()),
        static_cast<int>(data.size()),
        static_cast<int>(original_size)
    );

    if (result < 0) {
        throw std::runtime_error("LZ4 decompression failed (malformed data)");
    }
    
    if (static_cast<size_t>(result) != original_size) {
        throw std::runtime_error("LZ4 decompression result size mismatch");
    }

    return decompressed;
}

size_t Compressor::get_max_compressed_size(size_t input_size) {
    return static_cast<size_t>(LZ4_compressBound(static_cast<int>(input_size)));
}

} // namespace server::core::compression
