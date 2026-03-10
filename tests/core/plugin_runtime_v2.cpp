#include "plugin_test_api.hpp"

namespace {

int transform(int value) {
    return value + 2;
}

const tests::plugin::TestPluginApi kApi{
    tests::plugin::kExpectedAbiVersion,
    "plugin_v2",
    &transform,
};

} // namespace

extern "C" TEST_PLUGIN_EXPORT const tests::plugin::TestPluginApi* test_plugin_api_v1() {
    return &kApi;
}
