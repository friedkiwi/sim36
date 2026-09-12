#include "Devices/WorkStationActions.h"

#include <fmt/format.h>

namespace sim36::devices {

const char* wsActionName(WsAction action)
{
    switch (action) {
        case WsAction::None: return "None";
        case WsAction::Put: return "Put";
        case WsAction::PutWithInvite: return "PutWithInvite";
        case WsAction::Invite: return "Invite";
        case WsAction::ReadInputFields: return "ReadInputFields";
        case WsAction::ReadScreen: return "ReadScreen";
        case WsAction::ReadImmed: return "ReadImmed";
        case WsAction::SaveScreen: return "SaveScreen";
        case WsAction::RestoreScreen: return "RestoreScreen";
        case WsAction::SetOnMsgWaiting: return "SetOnMsgWaiting";
        case WsAction::SetOffMsgWaiting: return "SetOffMsgWaiting";
        case WsAction::CancelInvite: return "CancelInvite";
        case WsAction::Clear: return "Clear";
        case WsAction::GetPrinterStatus: return "GetPrinterStatus";
        case WsAction::Activate: return "Activate";
        case WsAction::HandleCmdReject: return "HandleCmdReject";
        case WsAction::PowerOn: return "PowerOn";
        case WsAction::VaryOn: return "VaryOn";
        case WsAction::VaryOff: return "VaryOff";
        case WsAction::EndM36Session: return "EndM36Session";
        case WsAction::AllowXpfToUseDevice: return "AllowXpfToUseDevice";
        case WsAction::ReturnDeviceToSSP: return "ReturnDeviceToSSP";
        case WsAction::CreateDevice: return "CreateDevice";
    }
    return "?";
}

namespace {

// The controller's command switch, read arm by arm.  Its range gate admits
// only 0x02 <= cmd <= 0xC3; everything else, and the gaps, only log.
struct CommandArm {
    int command;
    WorkStationActions::Arm arm;
};

const CommandArm kByCommand[] = {
    {0x02, {8, WsAction::SaveScreen, "c18beb08"}},
    {0x12, {9, WsAction::RestoreScreen, "c18becb4"}},
    {0x22, {6, WsAction::ReadInputFields, "c18beb58"}},
    {0x27, {4, WsAction::Put, "c18bec40"}},
    {0x32, {6, WsAction::ReadInputFields, "c18beba0"}},
    {0x33, {10, WsAction::SetOnMsgWaiting, "c18becd8"}},
    {0x40, {12, WsAction::Clear, "c18bed48"}},
    {0x42, {6, WsAction::ReadInputFields, "c18beabc"}},
    {0x43, {11, WsAction::SetOffMsgWaiting, "c18bed10"}},
    {0x47, {13, WsAction::GetPrinterStatus, "c18bed80"}},
    {0x62, {7, WsAction::ReadScreen, "c18bec8c"}},
    {0xA7, {5, WsAction::PutWithInvite, "c18bec68"}},
    {0xC3, {14, WsAction::CancelInvite, "c18bec1c"}},
};

// The action executor's own table, action code 0..23.  Codes 1, 2 and 17
// point at the arm that logs and calls no device method, the same target
// the out-of-range check uses.
const WsAction kByActionCode[] = {
    /*  0 */ WsAction::CreateDevice,
    /*  1 */ WsAction::None,
    /*  2 */ WsAction::None,
    /*  3 */ WsAction::Invite,
    /*  4 */ WsAction::Put,
    /*  5 */ WsAction::PutWithInvite,
    /*  6 */ WsAction::ReadInputFields,
    /*  7 */ WsAction::ReadScreen,
    /*  8 */ WsAction::SaveScreen,
    /*  9 */ WsAction::RestoreScreen,
    /* 10 */ WsAction::SetOnMsgWaiting,
    /* 11 */ WsAction::SetOffMsgWaiting,
    /* 12 */ WsAction::Clear,
    /* 13 */ WsAction::GetPrinterStatus,
    /* 14 */ WsAction::CancelInvite,
    /* 15 */ WsAction::VaryOff,
    /* 16 */ WsAction::VaryOn,
    /* 17 */ WsAction::None,
    /* 18 */ WsAction::Activate,
    /* 19 */ WsAction::HandleCmdReject,
    /* 20 */ WsAction::PowerOn,
    /* 21 */ WsAction::EndM36Session,
    /* 22 */ WsAction::AllowXpfToUseDevice,
    /* 23 */ WsAction::ReturnDeviceToSSP,
};

constexpr int kActionCodes = static_cast<int>(sizeof(kByActionCode) / sizeof(kByActionCode[0]));

}  // namespace

bool WorkStationActions::tryLookup(int command, Arm& arm)
{
    for (const auto& c : kByCommand)
        if (c.command == command) {
            arm = c.arm;
            return true;
        }
    return false;
}

WsAction WorkStationActions::actionOf(int code)
{
    if (code < 0 || code >= kActionCodes) return WsAction::None;
    return kByActionCode[code];
}

int WorkStationActions::actionCodeCount() { return kActionCodes; }

std::string WorkStationActions::knownCommands()
{
    // The table is already in ascending command order.
    std::string s;
    for (const auto& c : kByCommand) {
        if (!s.empty()) s += ' ';
        s += fmt::format("{:02X}", c.command);
    }
    return s;
}

}  // namespace sim36::devices
