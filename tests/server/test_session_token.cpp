#include <gtest/gtest.h>
#include <server/auth/session_token.hpp>
#include "signed_authenticator.hpp"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>

namespace {
const std::string key(32, 'k');
server::auth::SessionClaims claims() { return {"account-1", "session-1", "표시 이름", 1000, 2000}; }
std::string sign_json(const nlohmann::json& payload) {
    constexpr char hex[] = "0123456789abcdef";
    auto encode = [&](std::string_view value) {
        std::string out;
        for (unsigned char c : value) { out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        return out;
    };
    const auto data = "chat1." + encode(payload.dump());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &length);
    return data + "." + encode({reinterpret_cast<char*>(digest), length});
}
}

TEST(SessionTokenTest, RoundTripRejectsTamperWrongKeyAndTimeBoundaries) {
    const auto token = server::auth::issue_token(claims(), key);
    const auto result = server::auth::verify_token(token, key, 1500);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->subject, "account-1");
    EXPECT_EQ(result->display_name, "표시 이름");
    EXPECT_FALSE(server::auth::verify_token(token + "x", key, 1500));
    EXPECT_FALSE(server::auth::verify_token(token, std::string(32, 'x'), 1500));
    EXPECT_FALSE(server::auth::verify_token(token, "short", 1500));
    EXPECT_FALSE(server::auth::verify_token(token, key, 999));
    EXPECT_FALSE(server::auth::verify_token(token, key, 2000));
    EXPECT_TRUE(server::auth::verify_token(token, key, 1000));
}

TEST(SessionTokenTest, SignedButInvalidIssuerAudienceTypesAndIdentityAreRejected) {
    const nlohmann::json good{{"v", 1}, {"iss", "dynaxis-auth"}, {"aud", "dynaxis-chat"},
        {"sub", "account-1"}, {"sid", "session-1"}, {"name", "name"}, {"iat", 1000}, {"exp", 2000}};
    for (const auto& [field, value] : std::vector<std::pair<std::string, nlohmann::json>>{
        {"aud", "admin"}, {"iss", "client"}, {"v", 1.0}, {"iat", -1}, {"exp", "2000"},
        {"sub", "guest"}, {"sub", "Account-1"}, {"sub", ""}, {"sid", ""}, {"name", "line\nbreak"}, {"exp", 1000000}}) {
        auto payload = good;
        payload[field] = value;
        EXPECT_FALSE(server::auth::verify_token(sign_json(payload), key, 1500)) << field;
    }
    auto missing = good;
    missing.erase("sid");
    EXPECT_FALSE(server::auth::verify_token(sign_json(missing), key, 1500));
    EXPECT_THROW(server::auth::issue_token(claims(), "short"), std::invalid_argument);
}

TEST(SessionTokenTest, AccessAndResumeCredentialsRemainDistinct) {
    const auto token = server::auth::issue_token(claims(), key);
    const auto credentials = server::auth::split_credentials(token + "|resume:opaque-token");
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->access_token, token);
    EXPECT_EQ(credentials->resume_token, "opaque-token");
    EXPECT_FALSE(server::auth::verify_token("resume:opaque-token", key, 1500));
    EXPECT_FALSE(server::auth::split_credentials(token + "|resume:"));
    EXPECT_FALSE(server::auth::split_credentials(std::string(8193, 'x')));
}

TEST(SessionTokenTest, GatewayDerivesIdentityFromClaimsAndRejectsInvalidCredential) {
    auto value = claims();
    value.issued_at = server::auth::unix_seconds();
    value.expires_at = value.issued_at + 60;
    gateway::auth::SignedAuthenticator authenticator({true, key});
    const auto token = server::auth::issue_token(value, key);
    const auto result = authenticator.authenticate({token, "impersonated-account", "127.0.0.1"});
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.subject, value.subject);
    EXPECT_FALSE(authenticator.authenticate({"", "account-1", "127.0.0.1"}).success);
    EXPECT_FALSE(authenticator.authenticate({token + "x", "account-1", "127.0.0.1"}).success);
}
