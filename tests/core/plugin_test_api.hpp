#pragma once

#include <cstdint>

#if defined(_WIN32)
#  define KNIGHTS_TEST_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define KNIGHTS_TEST_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace knights::tests::plugin {

struct TestPluginApi {
    std::uint32_t abi_version;
    const char* name;
    int (*transform)(int value);
};

constexpr std::uint32_t kExpectedAbiVersion = 1;
constexpr const char* kEntrypointSymbol = "knights_test_plugin_api_v1";

using GetApiFn = const TestPluginApi* (*)();

} // namespace knights::tests::plugin
