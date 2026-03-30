/**
 * @file gateway_app.cpp
 * @brief GatewayApp의 최소 구현 소유자(Impl ctor/dtor)만 남긴 TU입니다.
 */
#include "gateway/gateway_app.hpp"

#include <memory>

#include "gateway/auth/authenticator.hpp"
#include "gateway_app_state.hpp"
#include "server/core/app/engine_builder.hpp"

namespace gateway {

GatewayApp::Impl::Impl()
    : hive_(std::make_shared<server::core::net::Hive>(io_))
    , engine_(server::core::app::EngineBuilder("gateway_app").build())
    , app_host_(engine_.host())
    , authenticator_(std::make_shared<auth::NoopAuthenticator>())
    , rudp_config_(std::make_unique<server::core::net::rudp::RudpConfig>()) {}

GatewayApp::Impl::~Impl() = default;

} // namespace gateway
