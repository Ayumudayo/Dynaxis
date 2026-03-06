#include "plugin_test_api.hpp"

namespace {

int transform(int value) {
    return value + 99;
}

const knights::tests::plugin::TestPluginApi kApi{
    999,
    "plugin_invalid",
    &transform,
};

} // namespace

extern "C" KNIGHTS_TEST_PLUGIN_EXPORT const knights::tests::plugin::TestPluginApi* knights_test_plugin_api_v1() {
    return &kApi;
}
