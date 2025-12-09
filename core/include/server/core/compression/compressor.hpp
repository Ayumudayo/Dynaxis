#pragma once

#include <vector>
#include <span>
#include <cstdint>
#include <stdexcept>

namespace server::core::compression {

class Compressor {
public:
    /**
     * @brief Compresses data using LZ4 default compression.
     * @param data Raw data to compress.
     * @return Compressed data.
     * @throws std::runtime_error if compression fails.
     */
    static std::vector<uint8_t> compress(std::span<const uint8_t> data);

    /**
     * @brief Decompresses LZ4 compressed data.
     * @param data Compressed data.
     * @param original_size Known size of the uncompressed data.
     * @return Decompressed data.
     * @throws std::runtime_error if decompression fails.
     */
    static std::vector<uint8_t> decompress(std::span<const uint8_t> data, size_t original_size);
    
    /**
     * @brief Returns the maximum compressed size for a given input size.
     * Useful for pre-allocating buffers.
     */
    static size_t get_max_compressed_size(size_t input_size);
};

} // namespace server::core::compression
