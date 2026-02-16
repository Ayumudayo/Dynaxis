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
    static constexpr size_t IV_SIZE = 12;  // GCM 표준 IV 크기
    static constexpr size_t TAG_SIZE = 16; // GCM 표준 인증 태그 크기

    /**
     * @brief AES-256-GCM으로 평문을 암호화합니다.
     * @param plaintext 암호화할 데이터
     * @param key 32바이트 키
     * @param iv 12바이트 초기화 벡터(Nonce)
     * @return 인증 태그(TAG)가 뒤에 붙은 암호문
     * @throws std::runtime_error 암호화 실패 시 발생
     *
     * GCM은 기밀성(암호화)과 무결성(인증)을 함께 제공하므로,
     * 네트워크 페이로드 변조를 검출해야 하는 실시간 시스템에 적합합니다.
     */
    static std::vector<uint8_t> encrypt(std::span<const uint8_t> plaintext, 
                                      std::span<const uint8_t> key, 
                                      std::span<const uint8_t> iv);

    /**
     * @brief AES-256-GCM 암호문을 복호화하고 인증을 검증합니다.
     * @param ciphertext 복호화할 데이터(TAG 포함)
     * @param key 32바이트 키
     * @param iv 12바이트 초기화 벡터(Nonce)
     * @return 복호화된 평문
     * @throws std::runtime_error 복호화 또는 인증 실패 시 발생
     */
    static std::vector<uint8_t> decrypt(std::span<const uint8_t> ciphertext, 
                                      std::span<const uint8_t> key, 
                                      std::span<const uint8_t> iv);
    
    /**
     * @brief OpenSSL `RAND_bytes`로 암호학적으로 안전한 난수를 생성합니다.
     */
    static std::vector<uint8_t> generate_random_bytes(size_t size);
};

} // namespace server::core::security
