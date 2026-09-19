#include "Processors/ControlStorage/FortranAssist.h"

#include <fmt/format.h>

#include "Monitor/Tracer.h"

namespace sim36::processors::controlstorage {

AssistResult FortranAssist::execute(AssistContext& context)
{
    context.trace().fortranAssist("FORTRAN entry XFER 01,{:02X} at {:04X}: task={:06X} request={:06X}",
                                  context.r(), context.sourceIar(), context.taskBlock() & 0xFFFFFF,
                                  context.requestBlock() & 0xFFFFFF);
    return AssistResult::notImplemented(
        fmt::format("XFER 01,{:02X} at {:04X}: the {} extended-control-storage assist is not implemented",
                    context.r(), context.sourceIar(), name()));
}

}  // namespace sim36::processors::controlstorage
