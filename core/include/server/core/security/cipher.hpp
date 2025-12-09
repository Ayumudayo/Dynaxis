#pragma once

#include <vector>
#include <span>
#include <string>
#include <cstdint>
#include <stdexcept>

namespace server::core::security {

class Cipher {
public:
    static constexpr size_t KEY_SIZE = 32; // AES-256
    static constexpr size_t IV_SIZE = 12;  // GCM standard IV size
    static constexpr size_t TAG_SIZE = 16; // GCM standard tag size

    /**
     * @brief Encrypts plaintext using AES-256-GCM.
     * @param plaintext Data to encrypt.
     * @param key 32-byte key.
     * @param iv 12-byte initialization vector.
     * @return Ciphertext with appended authentication tag.
     * @throws std::runtime_error if encryption fails.
     */
    static std::vector<uint8_t> encrypt(std::span<const uint8_t> plaintext, 
                                      std::span<const uint8_t> key, 
                                      std::span<const uint8_t> iv);

    /**
     * @brief Decrypts ciphertext using AES-256-GCM.
     * @param ciphertext Data to decrypt (must include appended tag).
     * @param key 32-byte key.
     * @param iv 12-byte initialization vector.
     * @return Decrypted plaintext.
     * @throws std::runtime_error if decryption or authentication fails.
     */
    static std::vector<uint8_t> decrypt(std::span<const uint8_t> ciphertext, 
                                      std::span<const uint8_t> key, 
                                      std::span<const uint8_t> iv);
    
    /**
     * @brief Generates a random sequence of bytes using OpenSSL RAND_bytes.
     */
    static std::vector<uint8_t> generate_random_bytes(size_t size);
};

} // namespace server::core::security
