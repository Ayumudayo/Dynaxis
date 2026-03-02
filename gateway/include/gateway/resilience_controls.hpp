#pragma once

#include "server/core/net/resilience_controls.hpp"

namespace gateway {

using TokenBucket = server::core::net::TokenBucket;
using RetryBudget = server::core::net::RetryBudget;
using CircuitBreaker = server::core::net::CircuitBreaker;

} // namespace gateway
