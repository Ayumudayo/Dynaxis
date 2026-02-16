#include "server/core/security/cipher.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <stdexcept>
#include <memory>

/**
 * @brief AES-256-GCM 암복호화 구현입니다.
 *
 * 인증 태그 검증 실패를 즉시 오류로 처리해 위변조 데이터를 차단하고,
 * OpenSSL C 자원은 RAII로 감싸 예외 경로에서도 누수를 방지합니다.
 */
namespace server::core::security {

namespace {
    // OpenSSL C API 자원을 RAII로 감싸 예외 경로에서도 누수를 막는다.
    struct EVP_CIPHER_CTX_Deleter {
        void operator()(EVP_CIPHER_CTX* ctx) const {
            EVP_CIPHER_CTX_free(ctx);
        }
    };
    using ScopedCtx = std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_Deleter>;

    void handle_openssl_error(const std::string& msg) {
        // 상위 호출자가 "어느 단계에서 실패했는지" 즉시 알 수 있도록
        // 단계별 메시지를 보존해 예외로 전달한다.
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

    // AES-GCM은 인증 태그(tag)가 필수다.
    // 복호화 시 이 태그가 일치하지 않으면 데이터 변조/키 불일치로 판단해 실패한다.
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, TAG_SIZE, ciphertext.data() + out_len + final_len))
        handle_openssl_error("Failed to get tag");

    // 최종 출력은 "암호문 + 인증 태그" 구조다.
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

    // 입력 버퍼 끝(TAG_SIZE 바이트)를 "검증용 예상 태그"로 설정한다.
    // 이후 EVP_DecryptFinal_ex가 인증 검증을 수행한다.
    void* tag_ptr = const_cast<uint8_t*>(ciphertext.data() + data_len);
    if (1 != EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, TAG_SIZE, tag_ptr))
        handle_openssl_error("Failed to set tag");

    int final_len = 0;
    if (1 != EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + out_len, &final_len)) {
        // 여기서 실패하면 "복호화는 되었더라도 인증이 실패"한 상태다.
        // 즉, 손상/위변조 가능성이 있으므로 평문을 절대 신뢰하면 안 된다.
        throw std::runtime_error("Decryption failed: authentication tag mismatch");
    }

    plaintext.resize(out_len + final_len);
    return plaintext;
}

std::vector<uint8_t> Cipher::generate_random_bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    if (1 != RAND_bytes(bytes.data(), static_cast<int>(size))) {
        // 키/IV 생성 실패는 암호 안전성 전제를 깨므로 즉시 치명 오류로 처리한다.
        throw std::runtime_error("Failed to generate random bytes");
    }
    return bytes;
}

} // namespace server::core::security
