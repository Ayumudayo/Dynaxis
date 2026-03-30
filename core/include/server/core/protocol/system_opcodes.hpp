#pragma once

/**
 * @file
 * @brief Configure/build 단계에서 생성되는 system opcode 헤더의 canonical forwarding entrypoint입니다.
 *
 * source tree에는 안정된 include 경로만 남기고, 실제 generated body는 build/generated include tree와
 * install tree 아래의 `server/generated/**` 경로에서 제공합니다.
 */
#include "server/generated/core/protocol/system_opcodes.generated.hpp"
