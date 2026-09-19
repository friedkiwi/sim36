#pragma once

#include "Processors/ControlStorage/ExtendedControlStoreAssist.h"

namespace sim36::processors::controlstorage {

struct BasicDecodedOpcode {
    uint8_t byte = 0;
    uint8_t operandClass = 0;
    uint8_t operation = 0;
};

// System/36 BASIC's internal bytecode/evaluation engine (XFER 02,00).
class BasicAssist final : public IExtendedControlStoreAssist {
public:
    const char* name() const override { return "NuBasic"; }
    AssistResult execute(AssistContext& context) override;
    static BasicDecodedOpcode decodeOpcode(uint8_t byte)
    {
        return {byte, static_cast<uint8_t>(byte >> 4), static_cast<uint8_t>(byte & 0x0F)};
    }
};

}  // namespace sim36::processors::controlstorage
