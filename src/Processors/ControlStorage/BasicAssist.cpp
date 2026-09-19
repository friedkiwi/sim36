#include "Processors/ControlStorage/BasicAssist.h"

#include <fmt/format.h>

#include "Monitor/Tracer.h"

namespace sim36::processors::controlstorage {

AssistResult BasicAssist::execute(AssistContext& context)
{
    context.trace().csp("XFER 02,00 at {:04X}: dispatch to {} assist", context.sourceIar(), name());
    return AssistResult::notImplemented(
        fmt::format("XFER 02,00 at {:04X}: the {} extended-control-storage assist is not implemented",
                    context.sourceIar(), name()));
}

}  // namespace sim36::processors::controlstorage
