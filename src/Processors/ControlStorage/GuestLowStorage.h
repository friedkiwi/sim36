// The guest low-storage map the control processor builds before it starts
// the main storage processor.
//
// Every address here is one the Advanced/36 control processor's own IPL
// routine writes at a literal displacement from the guest storage base,
// rather than one inferred from SSP's reads.  Only what that routine
// demonstrably writes is written; fields whose contents are not established
// are left zero and named in the trace rather than guessed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"

namespace sim36::processors::controlstorage {

class GuestLowStorage {
public:
    // Two-letter EBCDIC eyecatchers.  The machine validates these itself - a
    // task post refuses a task block whose first halfword is not "TB" - so an
    // emulator that builds them can be checked the same way the real machine
    // checks itself.
    static constexpr uint16_t kEyeAce = 0xC1C3;           // "AC" action control element
    static constexpr uint16_t kEyeTaskBlock = 0xE3C2;     // "TB" task block
    static constexpr uint16_t kEyeDiskIob = 0xC9C6;       // "IF" disk I/O block
    static constexpr uint16_t kEyeSystemBlock = 0xE2C2;   // "SB" system block
    static constexpr uint16_t kEyeRequestBlock = 0xD9C2;  // "RB" request block
    static constexpr uint16_t kEyeProgramBlock = 0xD7C2;  // "PB" program block

    // Ten ACEs, 32 bytes apart; "AC" is written at each of these.
    static constexpr int kAceCount = 10;
    static const int kAceAddresses[kAceCount];

    // Six disk IOBs, 48 bytes apart.  0x560 is the one to know: the IPL disk
    // read substitutes it when its IOB argument is null.
    static constexpr int kDiskIobCount = 6;
    static const int kDiskIobAddresses[kDiskIobCount];

    static constexpr int kTaskBlock = 0xF00;
    static constexpr int kSystemBlock = 0x6000;
    static constexpr int kDefaultDiskIob = 0x560;

    // Low storage proper: the base every one of the IPL routine's byte stores
    // reaches guest 0x08nn through.  Guest 0x0800..0801 is the system's own
    // MIC field.
    static constexpr int kLowStorage = 0x800;

    // Queue header table base.  Header n occupies the 4 bytes at 0xB00 + 4n,
    // with the 3-byte value in the last three of them.
    static constexpr int kQueueHeaderTable = 0xB00;

    // ace+19, decimal: the 24-bit task block address.
    static constexpr int kAceTaskBlockPointer = 19;

    // Queue headers the IPL initialises with the task block's guest address
    // (37 and 39).  Queue 39 is corroborated from the other side: the task
    // block carries a chain link for it at tb+25.
    static constexpr int kTaskBlockQueueHeaderCount = 2;
    static const int kTaskBlockQueueHeaders[kTaskBlockQueueHeaderCount];

    // Guest 0x092A..0x092C and 0x092D..0x092F: where the IPL records the guest
    // address of the system CONSOLE terminal unit block and the system PRINTER
    // unit block (the System/34's SCADMTUB / SCADPTUB pair, LY21-0049-7 figure
    // 2-251, widened from two bytes to three).  Only the console pointer is
    // written at IPL; the printer pointer stays zero.
    static constexpr int kSystemConsoleUnitBlockPointer = 0x092A;
    static constexpr int kSystemPrinterUnitBlockPointer = 0x092D;

    // 192 bytes, out of the system queue space.
    static constexpr int kSystemUnitBlockBytes = 192;

    // Queue headers the unit block is queued onto: 49 and 50 for a terminal
    // block, 51 and 50 for a printer one.
    static constexpr int kConsoleUnitBlockQueue = 49;
    static constexpr int kPrinterUnitBlockQueue = 51;
    static constexpr int kSharedUnitBlockQueue = 50;

    // Station/session activation blocks consumed by the work-station display
    // control task; its activation scan is seeded from the header at guest
    // 0x0CC1.
    static constexpr int kWorkStationActivationQueue = 112;

    // The last sector of the IPL / transient region (guest 0x0A78..0x0A7A) and
    // the fixed disk's exclusive 1-based end (0x0A84..0x0A86).  Both are
    // SECTORS, not addresses.  Phase 1 relocates its own transfer control
    // table entry by adding 0x0A7A, so an unbuilt region end hands SVC 10 the
    // shipped 00 00 01.
    static constexpr int kTransientRegionEnd = 0x0A78;
    static constexpr int kFixedDiskEnd = 0x0A84;
    static constexpr int kTransientRegionLastSector = 8191;

    // The current number of pages in the user area of main storage, a
    // halfword - what SVC 13 returns in work register 6.
    static constexpr int kUserAreaPages = 0x0845;

    // The main-storage size block, 0840..0846, 084B, 084C and 0852: seven
    // fields, all literals with no configuration dependency.
    static constexpr int kMainStorageBlockStart = 0x0840;

    // Record header, decimal offsets: +0 device id, +1 class, +2..7 addresses
    // and unit, +8 L1 customize, +9 L2 configure, +10 L3 name, then the three
    // variable areas in that order.
    static constexpr int kUdtDeviceId = 0;
    static constexpr int kUdtClass = 1;
    static constexpr int kUdtUnit = 4;

    // Header n occupies the 4 bytes at 0xB00 + 4n; the 3-byte value is the
    // last three of them, so it ends at 0xB03 + 4n - the System/36's
    // rightmost-byte field convention.
    static constexpr int queueHeader(int n) { return kQueueHeaderTable + 4 * n + 1; }

    // Seven bytes of HOST machine identity written into MSP low storage: the
    // system underneath, not the System/36 image, which is why nothing on the
    // volume supplies them.  Measured on a running Advanced/36: "150", packed
    // BCD 2270, "440".
    struct HostInfo {
        std::string model = "150";           // 08E2..08E4, three EBCDIC characters
        int processorFeature = 0x2270;       // 08E6/08E7, packed BCD
        std::string processorModel = "440";  // 0A88..0A8A, three EBCDIC characters
    };

    // Build it.  volumeSectors is the number of sectors in the host image;
    // the fixed disk end published to the guest is the exclusive 1-based end,
    // volumeSectors + 1.  Only three bytes of it are stored, so a value that
    // does not fit 24 bits is refused: false is returned and `error` carries
    // the reason.
    static bool build(machine::MachineState& m, monitor::Tracer& trace, int volumeSectors,
                      const HostInfo& host, uint8_t systemCustomize1, std::string& error);

    // Build the hardware description which Advanced/36 power-on places in
    // the disk UDT area before starting the SSP IPL.  The returned buffer has
    // the same 4 KB shape read by csipl; its meaningful, persisted prefix is
    // four sectors.
    static std::vector<uint8_t> synthesizeUnitDefinitionTable(uint8_t systemCustomize1);

    // Walk the unit definition table exactly as the IPL does.  `udt` is the
    // 4 KB the IPL disk read fetches (16 sectors from 1-based 27), which the
    // real machine leaves at guest 0x1000 until phase 1 is loaded over it.
    // Reading it from a buffer instead is the only deviation: nothing in the
    // walk depends on the table being addressable by the guest.
    static void walkUnitDefinitionTable(machine::MachineState& m, monitor::Tracer& trace,
                                        const uint8_t* udt, int udtLength);

private:
    struct UdtRecord;
    static int recordLength(const uint8_t* udt, int off);
    static void setHostProcessorInfo(machine::MachineState& m, monitor::Tracer& trace, const HostInfo& host);
    static void writeEbcdic3(machine::MachineState& m, int at, const std::string& s);
    static void orByte(machine::MachineState& m, int addr, uint8_t bits);
    static void andNot(machine::MachineState& m, int addr, uint8_t bits);
    static void dispatch(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void systemEntryArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void classDispatch(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void controllerClassArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static uint8_t mapLanguageId(uint8_t id);
    static void lanArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void commArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void jumpTable(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void diskArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void tapeUnitArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n);
    static void skipMaxDevices(monitor::Tracer& trace, int addr);
    static void skipStackByte(monitor::Tracer& trace, const std::string& what, uint8_t unit, const char* site);
    static std::string hex(const UdtRecord& r, int area, int at, int len);
    static void arm(monitor::Tracer& trace, const UdtRecord& r, int n, const char* arm, const std::string& what);
    static void ignore(monitor::Tracer& trace, const UdtRecord& r, int n, const std::string& why);
};

}  // namespace sim36::processors::controlstorage
