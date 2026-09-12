// The work station IOB command byte to device-action mapping.
//
// Three decoded tables in a row: the controller routes a unit address of FF
// to the all-stations invite before the command byte is read; the command
// byte selects an action code 0..23; and the action code selects a device
// operation.  The command byte the controller switches on lives in its own
// work station control field, and the IOB's +0x0B carries the same code set
// (strong evidence, not a decode), so the model says so in its trace and
// refuses any byte outside the decoded set rather than inventing an arm.
//
// SA21-9436 chapter 11's `21` Output Data and `41` Get Printer Status are
// not in the decoded set; the operations appear as 0x27 put and 0x47
// getPrinterStatus.  `40` Clear Printer is in both under the same code.
// `FF` Invite is not a command byte at all: it is a unit address.
#pragma once

#include <cstdint>
#include <string>

namespace sim36::devices {

// The device operation an action code selects: the device's own virtual
// functions, in vtable order.
enum class WsAction {
    None = 0,            // action code 1, and the two arms that only log
    Put,                 // vtable +0x18
    PutWithInvite,       // +0x20
    Invite,              // +0x28
    ReadInputFields,     // +0x30
    ReadScreen,          // +0x38
    ReadImmed,           // +0x40   no action code reaches it
    SaveScreen,          // +0x48
    RestoreScreen,       // +0x50
    SetOnMsgWaiting,     // +0x58
    SetOffMsgWaiting,    // +0x60
    CancelInvite,        // +0x68
    Clear,               // +0x70
    GetPrinterStatus,    // +0x78
    Activate,            // +0x80
    HandleCmdReject,     // +0x88
    PowerOn,             // +0x90
    VaryOn,              // +0x98
    VaryOff,             // +0xA0
    EndM36Session,       // +0xA8
    AllowXpfToUseDevice, // +0xB0
    ReturnDeviceToSSP,   // +0xB8
    // Action code 0: the arm that constructs the device object rather than
    // calling a method on one.
    CreateDevice
};

// The enumerator's name, as the reference prints the enum.
const char* wsActionName(WsAction action);

class WorkStationActions {
public:
    // The unit address that means "invite", before the command byte is
    // looked at at all.
    static constexpr int kInviteUnitAddress = 0xFF;

    struct Arm {
        // The action code the controller passes to the action's constructor.
        int code = 0;
        WsAction action = WsAction::None;
        // The controller switch address that selects this arm.
        const char* site = "";
    };

    static bool tryLookup(int command, Arm& arm);

    // What the action executor would do with an action code.  Public because
    // the two tables are independent decodes and the model asserts they
    // agree.
    static WsAction actionOf(int code);

    static int actionCodeCount();

    // The decoded command bytes, low to high, for a trace or a refusal that
    // has to say what IS understood.
    static std::string knownCommands();
};

}  // namespace sim36::devices
