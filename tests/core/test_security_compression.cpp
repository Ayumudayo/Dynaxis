#include <gtest/gtest.h>
#include "server/core/security/cipher.hpp"
#include "server/core/compression/compressor.hpp"
#include <string>
#include <vector>

/**
 * @brief 암복호화(AES-GCM)와 압축(LZ4) 왕복 무결성 동작을 검증합니다.
 */
using namespace server::core::security;
using namespace server::core::compression;

class SecurityCompressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }
};

TEST_F(SecurityCompressionTest, CipherRoundTrip) {
    auto key = Cipher::generate_random_bytes(Cipher::KEY_SIZE);
    auto iv = Cipher::generate_random_bytes(Cipher::IV_SIZE);
    
    std::string msg = "Secret Message! Do not steal.";
    std::vector<uint8_t> plaintext(msg.begin(), msg.end());

    // Encrypt
    auto ciphertext = Cipher::encrypt(plaintext, key, iv);
    
    // GCM appends a 16-byte tag
    EXPECT_EQ(ciphertext.size(), plaintext.size() + Cipher::TAG_SIZE);

    // Decrypt
    auto decrypted = Cipher::decrypt(ciphertext, key, iv);
    EXPECT_EQ(plaintext, decrypted);
}

TEST_F(SecurityCompressionTest, CipherDecryptFailsWithWrongKey) {
    auto key = Cipher::generate_random_bytes(Cipher::KEY_SIZE);
    auto wrong_key = Cipher::generate_random_bytes(Cipher::KEY_SIZE);
    auto iv = Cipher::generate_random_bytes(Cipher::IV_SIZE);
    
    std::vector<uint8_t> plaintext = {0x1, 0x2, 0x3, 0x4};
    auto ciphertext = Cipher::encrypt(plaintext, key, iv);

    // Should fail authentication/decryption
    EXPECT_THROW({
        Cipher::decrypt(ciphertext, wrong_key, iv);
    }, std::runtime_error);
}

TEST_F(SecurityCompressionTest, CipherDecryptFailsWithModifiedCiphertext) {
    auto key = Cipher::generate_random_bytes(Cipher::KEY_SIZE);
    auto iv = Cipher::generate_random_bytes(Cipher::IV_SIZE);
    
    std::vector<uint8_t> plaintext = {0xA, 0xB, 0xC, 0xD};
    auto ciphertext = Cipher::encrypt(plaintext, key, iv);

    // Tamper with the ciphertext (flip a bit)
    if (!ciphertext.empty()) {
        ciphertext[0] ^= 0xFF;
    }

    EXPECT_THROW({
        Cipher::decrypt(ciphertext, key, iv);
    }, std::runtime_error);
}

TEST_F(SecurityCompressionTest, CompressorRoundTrip) {
    // Generate compressible data (repeating pattern)
    std::vector<uint8_t> data;
    for (int i = 0; i < 100; ++i) {
        data.push_back(static_cast<uint8_t>(i % 5));
    }

    auto compressed = Compressor::compress(data);
    // Should be smaller than original
    EXPECT_LT(compressed.size(), data.size());

    auto decompressed = Compressor::decompress(compressed, data.size());
    EXPECT_EQ(data, decompressed);
}

TEST_F(SecurityCompressionTest, CompressorHandlesUncompressible) {
    // Random data usually doesn't compress well or might slightly expand
    auto data = Cipher::generate_random_bytes(100);

    auto compressed = Compressor::compress(data);
    // It shouldn't crash
    EXPECT_FALSE(compressed.empty());

    auto decompressed = Compressor::decompress(compressed, data.size());
    EXPECT_EQ(data, decompressed);
}
