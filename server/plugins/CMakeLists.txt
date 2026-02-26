add_library(chat_hook_sample MODULE
    chat_hook_sample.cpp
)

target_include_directories(chat_hook_sample PRIVATE
    ${CMAKE_SOURCE_DIR}/server/include
)

target_compile_definitions(chat_hook_sample PRIVATE
    CHAT_HOOK_PLUGIN_BUILD
    CHAT_HOOK_SAMPLE_V1
)

set_target_properties(chat_hook_sample PROPERTIES
    PREFIX ""
    OUTPUT_NAME "10_chat_hook_sample"
)

add_library(chat_hook_sample_v2 MODULE
    chat_hook_sample.cpp
)

target_include_directories(chat_hook_sample_v2 PRIVATE
    ${CMAKE_SOURCE_DIR}/server/include
)

target_compile_definitions(chat_hook_sample_v2 PRIVATE
    CHAT_HOOK_PLUGIN_BUILD
    CHAT_HOOK_SAMPLE_V2
)

set_target_properties(chat_hook_sample_v2 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "10_chat_hook_sample_v2"
)

add_library(chat_hook_tag MODULE
    chat_hook_tag.cpp
)

target_include_directories(chat_hook_tag PRIVATE
    ${CMAKE_SOURCE_DIR}/server/include
)

target_compile_definitions(chat_hook_tag PRIVATE
    CHAT_HOOK_PLUGIN_BUILD
)

set_target_properties(chat_hook_tag PROPERTIES
    PREFIX ""
    OUTPUT_NAME "20_chat_hook_tag"
)
