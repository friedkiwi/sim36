#include "Devices/WorkStationIob.h"

#include "Devices/WorkStationActions.h"

namespace sim36::devices {

std::string WorkStationIob::commandName(int cmd)
{
    switch (cmd) {
        case kCmdReadCurrentConfiguration: return "Read Current Configuration";
        case kCmdConfigureNewWorkStations: return "Configure New Work Stations";
        default: {
            WorkStationActions::Arm arm;
            if (WorkStationActions::tryLookup(cmd, arm)) return wsActionName(arm.action);
            return "unknown";
        }
    }
}

}  // namespace sim36::devices
