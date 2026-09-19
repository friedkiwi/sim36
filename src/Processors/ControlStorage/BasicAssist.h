#pragma once

#include "Processors/ControlStorage/ExtendedControlStoreAssist.h"

namespace sim36::processors::controlstorage {

// System/36 BASIC's internal bytecode/evaluation engine (XFER 02,00).
// The stub is intentionally a real dispatch target so implementation can be
// added without changing the MSP/CSP hand-off again.
class BasicAssist final : public IExtendedControlStoreAssist {
public:
    const char* name() const override { return "NuBasic"; }
    AssistResult execute(AssistContext& context) override;
};

}  // namespace sim36::processors::controlstorage
