#include "server/auth/session_token.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include "server/core/protocol/packet.hpp"

namespace server::auth {
namespace {
constexpr std::string_view prefix = "chat1.";

std::string hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::optional<std::string> unhex(std::string_view bytes) {
    if (bytes.size() % 2) { return std::nullopt; }
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::string result;
    result.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const int a = digit(bytes[i]), b = digit(bytes[i + 1]);
        if (a < 0 || b < 0) { return std::nullopt; }
        result.push_back(static_cast<char>((a << 4) | b));
    }
    return result;
}

std::string mac(std::string_view data, std::string_view secret) {
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int size = 0;
    if (secret.size() < 32 || secret.size() > 4096
        || !HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
                 reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &size)) {
        return {};
    }
    return hex({reinterpret_cast<const char*>(digest), size});
}

bool identifier(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

bool valid(const SessionClaims& claims) {
    return identifier(claims.subject) && claims.subject != "guest" && claims.subject != "anonymous" && identifier(claims.session_id)
        && !claims.display_name.empty() && claims.display_name.size() <= 128
        && server::core::protocol::is_valid_utf8(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(claims.display_name.data()), claims.display_name.size()))
        && std::none_of(claims.display_name.begin(), claims.display_name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })
        && claims.expires_at > claims.issued_at && claims.expires_at - claims.issued_at <= 86400;
}
} // namespace

Config load_config_from_env() {
    const char* mode = std::getenv("CHAT_AUTH_MODE");
    if (mode && std::string_view(mode) == "development") { return {}; }
    if (mode && std::string_view(mode) != "signed") { throw std::invalid_argument("invalid CHAT_AUTH_MODE"); }
    const char* secret = std::getenv("CHAT_AUTH_SIGNING_SECRET");
    if (!secret || std::string_view(secret).size() < 32 || std::string_view(secret).size() > 4096) {
        throw std::invalid_argument("signed chat authentication requires a 32..4096 byte signing secret");
    }
    return {true, secret};
}

std::uint64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::optional<Credentials> split_credentials(std::string_view token) {
    if (token.size() > 8192) { return std::nullopt; }
    const auto split = token.find("|resume:");
    Credentials result{std::string(token.substr(0, split)), {}};
    if (split != std::string_view::npos) {
        result.resume_token = token.substr(split + 8);
        if (result.resume_token.empty() || result.resume_token.size() > 512
            || result.resume_token.find('|') != std::string::npos) { return std::nullopt; }
    }
    return result;
}

std::string issue_token(const SessionClaims& claims, std::string_view secret) {
    if (!valid(claims)) { throw std::invalid_argument("invalid session claims"); }
    const nlohmann::json payload{{"v", 1}, {"iss", "dynaxis-auth"}, {"aud", "dynaxis-chat"},
        {"sub", claims.subject}, {"sid", claims.session_id}, {"name", claims.display_name},
        {"iat", claims.issued_at}, {"exp", claims.expires_at}};
    const std::string data = std::string(prefix) + hex(payload.dump());
    const auto signature = mac(data, secret);
    if (signature.empty()) { throw std::invalid_argument("invalid signing key"); }
    return data + "." + signature;
}

std::optional<SessionClaims> verify_token(std::string_view token, std::string_view secret, std::uint64_t now) {
    if (token.size() > 4096 || !token.starts_with(prefix)) { return std::nullopt; }
    const auto separator = token.rfind('.');
    if (separator == std::string_view::npos) { return std::nullopt; }
    const auto signature = mac(token.substr(0, separator), secret);
    const auto supplied = token.substr(separator + 1);
    if (signature.empty() || supplied.size() != signature.size()
        || CRYPTO_memcmp(supplied.data(), signature.data(), signature.size()) != 0) { return std::nullopt; }
    const auto raw = unhex(token.substr(prefix.size(), separator - prefix.size()));
    if (!raw) { return std::nullopt; }
    try {
        const auto parsed = nlohmann::json::parse(*raw);
        if (!parsed.is_object() || !parsed.at("v").is_number_unsigned() || parsed.at("v") != 1 || parsed.at("iss") != "dynaxis-auth" || parsed.at("aud") != "dynaxis-chat"
            || !parsed.at("iat").is_number_unsigned() || !parsed.at("exp").is_number_unsigned()) { return std::nullopt; }
        SessionClaims claims{parsed.at("sub").get<std::string>(), parsed.at("sid").get<std::string>(),
            parsed.at("name").get<std::string>(), parsed.at("iat").get<std::uint64_t>(), parsed.at("exp").get<std::uint64_t>()};
        if (!valid(claims) || claims.issued_at > now || claims.expires_at <= now) { return std::nullopt; }
        return claims;
    } catch (...) { return std::nullopt; }
}
} // namespace server::auth
