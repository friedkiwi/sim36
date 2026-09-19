#include "Processors/ControlStorage/FortranAssist.h"

#include <fmt/format.h>

#include "Monitor/Tracer.h"

namespace sim36::processors::controlstorage {

AssistResult FortranAssist::execute(AssistContext& context)
{
    context.trace().csp("XFER 01,{:02X} at {:04X}: dispatch to {} assist", context.r(),
                        context.sourceIar(), name());
    return AssistResult::notImplemented(
        fmt::format("XFER 01,{:02X} at {:04X}: the {} extended-control-storage assist is not implemented",
                    context.r(), context.sourceIar(), name()));
}

}  // namespace sim36::processors::controlstorage
