#include "plugin_test_api.hpp"

namespace {

int transform(int value) {
    return value + 2;
}

const knights::tests::plugin::TestPluginApi kApi{
    knights::tests::plugin::kExpectedAbiVersion,
    "plugin_v2",
    &transform,
};

} // namespace

extern "C" KNIGHTS_TEST_PLUGIN_EXPORT const knights::tests::plugin::TestPluginApi* knights_test_plugin_api_v1() {
    return &kApi;
}
