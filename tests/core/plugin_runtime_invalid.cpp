#include "plugin_test_api.hpp"

namespace {

int transform(int value) {
    return value + 99;
}

const tests::plugin::TestPluginApi kApi{
    999,
    "plugin_invalid",
    &transform,
};

} // namespace

extern "C" TEST_PLUGIN_EXPORT const tests::plugin::TestPluginApi* test_plugin_api_v1() {
    return &kApi;
}
