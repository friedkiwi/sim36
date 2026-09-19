#pragma once

#include "Processors/ControlStorage/ExtendedControlStoreAssist.h"

namespace sim36::processors::controlstorage {

// System/36 FORTRAN/scientific macroinstruction engine (XFER 01,R).
class FortranAssist final : public IExtendedControlStoreAssist {
public:
    const char* name() const override { return "NuFortran"; }
    AssistResult execute(AssistContext& context) override;
};

}  // namespace sim36::processors::controlstorage
