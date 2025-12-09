#include "server/core/security/cipher.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <stdexcept>
#include <memory>
#include <iostream>

namespace server::core::security {

namespace {
    struct EVP_CIPHER_CTX_Deleter {
        void operator()(EVP_CIPHER_CTX* ctx) const {
            EVP_CIPHER_CTX_free(ctx);
        }
    };
    using ScopedCtx = std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_Deleter>;

    void handle_openssl_error(const std::string& msg) {
        // In a real app, might want to retrieve the actual error from ERR_get_error()
        throw std::runtime_error(msg);
    }
}

std::vector<uint8_t> Cipher::encrypt(std::span<const uint8_t> plaintext, 
                                   std::span<const uint8_t> key, 
                                   std::span<const uint8_t> iv) {
    if (key.size() != KEY_SIZE) throw std::invalid_argument("Invalid key size");
    if (iv.size() != IV_SIZE) throw std::invalid_argument("Invalid IV size");

    ScopedCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) handle_openssl_error("Failed to create cipher context");

    if (1 != EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
        handle_openssl_error("Failed to init encryption");

    if (1 != EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()))
        handle_openssl_error("Failed to set key/iv");

    std::vector<uint8_t> ciphertext(plaintext.size() + TAG_SIZE);
    int out_len = 0;

    if (1 != EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &out_len, plaintext.data(), static_cast<int>(plaintext.size())))
        handle_openssl_error("Failed to encrypt update");

    int final_len = 0;
    if (1 != EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + out_len, &final_len))
        handle_openssl_error("Failed to encrypt final");

    // Get the tag
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, TAG_SIZE, ciphertext.data() + out_len + final_len))
        handle_openssl_error("Failed to get tag");

    // Actual size is plaintext + tag
    ciphertext.resize(out_len + final_len + TAG_SIZE);
    
    return ciphertext;
}

std::vector<uint8_t> Cipher::decrypt(std::span<const uint8_t> ciphertext, 
                                   std::span<const uint8_t> key, 
                                   std::span<const uint8_t> iv) {
    if (key.size() != KEY_SIZE) throw std::invalid_argument("Invalid key size");
    if (iv.size() != IV_SIZE) throw std::invalid_argument("Invalid IV size");
    if (ciphertext.size() < TAG_SIZE) throw std::invalid_argument("Ciphertext too short (missing tag)");

    ScopedCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) handle_openssl_error("Failed to create cipher context");

    if (1 != EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr))
        handle_openssl_error("Failed to init decryption");

    if (1 != EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()))
        handle_openssl_error("Failed to set key/iv");

    const size_t data_len = ciphertext.size() - TAG_SIZE;
    std::vector<uint8_t> plaintext(data_len);
    int out_len = 0;

    if (1 != EVP_DecryptUpdate(ctx.get(), plaintext.data(), &out_len, ciphertext.data(), static_cast<int>(data_len)))
        handle_openssl_error("Failed to decrypt update");

    // Set expected tag
    void* tag_ptr = const_cast<uint8_t*>(ciphertext.data() + data_len);
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag_ptr))
        handle_openssl_error("Failed to set tag");

    int final_len = 0;
    if (1 != EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + out_len, &final_len)) {
        // Verify failed
        throw std::runtime_error("Decryption failed: authentication tag mismatch");
    }

    plaintext.resize(out_len + final_len);
    return plaintext;
}

std::vector<uint8_t> Cipher::generate_random_bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    if (1 != RAND_bytes(bytes.data(), static_cast<int>(size))) {
        throw std::runtime_error("Failed to generate random bytes");
    }
    return bytes;
}

} // namespace server::core::security
