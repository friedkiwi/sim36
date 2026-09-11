#include "Processors/MainStorageProcessor.h"

namespace sim36::processors {

void MainStorageProcessor::reset()
{
    m_.msp.reset();
    stopped_ = false;
    stopReason_.clear();
    instructions_ = 0;
    atPreemptionPoint_ = true;
}

}  // namespace sim36::processors
