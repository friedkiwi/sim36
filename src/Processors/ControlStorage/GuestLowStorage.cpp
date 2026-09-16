#include "Processors/ControlStorage/GuestLowStorage.h"

#include <fmt/format.h>

#include "Devices/WorkStationController.h"
#include "Storage/Ebcdic.h"

namespace sim36::processors::controlstorage {

const int GuestLowStorage::kAceAddresses[GuestLowStorage::kAceCount] =
    { 0x2C0, 0x2E0, 0x3C0, 0x3E0, 0x400, 0x420, 0x440, 0x4A0, 0x4C0, 0x4E0 };

const int GuestLowStorage::kDiskIobAddresses[GuestLowStorage::kDiskIobCount] =
    { 0x300, 0x330, 0x360, 0x500, 0x530, 0x560 };

const int GuestLowStorage::kTaskBlockQueueHeaders[GuestLowStorage::kTaskBlockQueueHeaderCount] = { 37, 39 };

namespace {

// The four ACEs the IPL gives a task-block pointer at ace+19.
const int kAcesPointingAtTaskBlock[] = { 0x3C0, 0x400, 0x440, 0x4E0 };

// Two halfword constants the IPL sets to -1 from one register.  0904 is
// verified against a storage dump of a running Advanced/36.
constexpr int kMinusOne0904 = 0x0904;
constexpr int kMinusOne3FFE = 0x3FFE;

// Host identity bytes; see GuestLowStorage::HostInfo.
constexpr int kHostModel08E2 = 0x08E2;         // ..08E4, three EBCDIC characters
constexpr int kHostFeatureHigh08E6 = 0x08E6;   // packed BCD, two digits
constexpr int kHostFeatureLow08E7 = 0x08E7;    // packed BCD, two digits
constexpr int kHostProcModel0A88 = 0x0A88;     // ..0A8A, three EBCDIC characters

// Unit definition table record header.
constexpr int kUdtL1 = 8;
constexpr int kUdtL2 = 9;
constexpr int kUdtL3 = 10;
constexpr int kUdtHeaderBytes = 11;

// Low-storage bytes the walk touches.  Every one is a displacement from the
// guest storage base + 0x800.
constexpr int kDeviceFlags080B = 0x080B;    // class 0x40 sets 0x9F here
constexpr int kCommFlags0820 = 0x0820;      // three bytes the comm arm ORs into
constexpr int kWsControllerFlags = 0x0849;  // |= the wkstn controller's configure[0]
constexpr int kDiskStorageSize = 0x084A;    // the system entry's configure[7]
constexpr int kSystemCustomize1 = 0x0850;   // ...and its customize[0]
constexpr int kDiskUnitCount = 0x0851;      // one per device id A7
constexpr int kCommUnitTable = 0x086B;      // + 5n, five bytes of configure per unit
constexpr int kTapeUnitFlags = 0x086C;      // device id B1: 01 for unit 0, 02 for unit 1
constexpr int kSerialDigits = 0x0898;       // name[9..12] and name[13..14]
constexpr int kLanUnitTable = 0x0899;       // + 5n, five bytes of configure per unit
constexpr int kDeviceFlags08B0 = 0x08B0;    // 04 diskette absent, 08 LAN, 40 see below
constexpr int kDeviceFlags08B1 = 0x08B1;    // 10 class 0x40, 20 device id B2
constexpr int kDeviceFlags08B2 = 0x08B2;    // 80 class 0x40, 40 class 0xC0
constexpr int kDeviceFlags08B6 = 0x08B6;    // 08 when BOTH id-B1 units are present
constexpr int kDeviceFlags08BC = 0x08BC;    // 02 and 08
constexpr int kSystemCustomize1Copy = 0x08BD;
constexpr int kSerialPrefix = 0x08BF;       // name[5..8]
constexpr int kMaxDevices08C3 = 0x08C3;     // ..08C6, the controllers' device counts
constexpr int kLanUnit7Configure = 0x08DB;  // ..08DF
constexpr int kPersonality08E0 = 0x08E0;    // bit 0x80 when the machine is NOT a /36
constexpr int kLanguageId = 0x097F;         // ..0980
constexpr int kDiskAddressSlots = 0x0A7B;   // 0A7B / 0A7E / 0A81, three bytes each

}  // namespace

// One record, in the three-pointers-and-a-length form the IPL sets up.  Each
// area pointer is left absent (-1) when its length field is zero, which is
// why an arm that reads a field of an area a record does not have would
// fault on the real machine as well.
struct GuestLowStorage::UdtRecord {
    const uint8_t* table = nullptr;
    int tableLength = 0;
    int offset = 0, length = 0;
    uint8_t id = 0, cls = 0, unit = 0;
    int customize = -1, configure = -1, name = -1;   // absolute, or -1 when absent

    uint8_t head(int i) const { return table[offset + i]; }

    // A byte of one of the three areas.  The IPL does not length-check these -
    // it reads straight on into the next record's bytes, because the whole
    // table is one 4 KB buffer - so the only bound applied here is the
    // buffer's.
    uint8_t field(int area, int i) const
    {
        if (area < 0) return 0;
        int at = area + i;
        return at >= 0 && at < tableLength ? table[at] : static_cast<uint8_t>(0);
    }
};

// The IPL decides its record length as 11 + L1 + L2 + L3 and terminates on a
// zero device id.
int GuestLowStorage::recordLength(const uint8_t* udt, int off)
{
    return kUdtHeaderBytes + udt[off + kUdtL1] + udt[off + kUdtL2] + udt[off + kUdtL3];
}

void GuestLowStorage::orByte(machine::MachineState& m, int addr, uint8_t bits)
{
    m.writeByte(addr, static_cast<uint8_t>(m.readByte(addr) | bits));
}

void GuestLowStorage::andNot(machine::MachineState& m, int addr, uint8_t bits)
{
    m.writeByte(addr, static_cast<uint8_t>(m.readByte(addr) & ~bits));
}

// Three EBCDIC characters, space-padded, as the hardware holds them.
// F1 F5 F0 is "150" in code page 500.
void GuestLowStorage::writeEbcdic3(machine::MachineState& m, int at, const std::string& s)
{
    std::string padded = s;
    while (padded.size() < 3) padded.push_back(' ');
    padded = padded.substr(0, 3);
    std::vector<uint8_t> b = storage::Ebcdic::fromAscii(padded, storage::Ebcdic::CodePage::Cp500);
    for (int i = 0; i < 3; i++) m.writeByte(at + i, i < static_cast<int>(b.size()) ? b[i] : 0x40);
}

// Seven bytes of host machine identity.  Two of the three fields are EBCDIC
// text; only 08E6/08E7 is packed BCD.  Nothing in the IPL reads any of the
// seven, so they change no behaviour; 08E5 is not written because nothing
// stores it.
void GuestLowStorage::setHostProcessorInfo(machine::MachineState& m, monitor::Tracer& trace, const HostInfo& host)
{
    writeEbcdic3(m, kHostModel08E2, host.model);
    m.writeByte(kHostFeatureHigh08E6, static_cast<uint8_t>((host.processorFeature >> 8) & 0xFF));
    m.writeByte(kHostFeatureLow08E7, static_cast<uint8_t>(host.processorFeature & 0xFF));
    writeEbcdic3(m, kHostProcModel0A88, host.processorModel);
    trace.csp("host processor info: 08E2..08E4 = \"{}\", 08E6/08E7 = {:04X} packed BCD, "
              "0A88..0A8A = \"{}\" (setHostProcessorInfo c180fc90; measured by DUMP MAIN "
              "on a running A/36). 08E5 is not written - nothing stores it.",
              host.model, host.processorFeature, host.processorModel);
}

bool GuestLowStorage::build(machine::MachineState& m, monitor::Tracer& trace, int volumeSectors,
                            const HostInfo& host, uint8_t systemCustomize1, std::string& error)
{
    error.clear();
    setHostProcessorInfo(m, trace, host);

    // This is pre-UDT configuration, not a reload workaround.  Real hardware
    // obtains the byte during system configuration before SSP generation can
    // run.  buildFromUnitDefinitionTable() follows this call and overwrites
    // the seed from an installed volume (for example, SSP 7.5 supplies 89).
    m.writeByte(kSystemCustomize1, systemCustomize1);
    m.writeByte(kSystemCustomize1Copy, systemCustomize1);
    trace.csp("low storage: 0850 and 08BD = {:02X}, model-derived system customize seed; a disk UDT system entry "
              "overrides both",
              systemCustomize1);

    // This is a capability of the live emulator controller, not an inventory
    // of stations in the machine definition.  Publish it before the UDT walk:
    // some SSP-generated UDTs contain only the C2 system-console record and
    // no id-61/class-C0 record, but CNFIGSSP still asks how many devices the
    // controller can support.  Leaving the byte at zero makes it reject every
    // configuration as exceeding a fictitious zero-device maximum.
    m.writeByte(kMaxDevices08C3,
                static_cast<uint8_t>(devices::WorkStationController::kMaxDevices));
    trace.csp("low storage: {:04X} = {} - maximum supported by the emulator work-station controller, "
              "independent of the number of configured stations",
              kMaxDevices08C3, devices::WorkStationController::kMaxDevices);

    // One register holding -1 is stored as a halfword to BOTH guest 0x0904
    // and guest 0x3FFE.  A storage dump of a running Advanced/36 reads FF FF
    // at 0904; a zero field looks like an absent one, which is exactly the
    // failure this whole exercise exists to stop.
    m.writeHalf(kMinusOne0904, 0xFFFF);
    m.writeHalf(kMinusOne3FFE, 0xFFFF);
    trace.csp("low storage: 0904 and 3FFE = FFFF (csipl c18316f8/c18316fc, r20 = -1 "
              "from c18316d0; 0904 verified against a DUMP MAIN on real hardware)");

    for (int ace : kAceAddresses) m.writeHalf(ace, kEyeAce);

    for (int ace : kAcesPointingAtTaskBlock) m.writeAddr24(ace + kAceTaskBlockPointer, kTaskBlock);

    for (int iob : kDiskIobAddresses) m.writeHalf(iob, kEyeDiskIob);

    m.writeHalf(kSystemBlock, kEyeSystemBlock);
    m.writeHalf(kTaskBlock, kEyeTaskBlock);

    for (int n : kTaskBlockQueueHeaders) m.writeAddr24(queueHeader(n), kTaskBlock);

    // The main-storage size block: literals, and the derived field is written
    // as the derivation rather than as its answer, so that if 0843 is ever
    // found to be configuration-dependent this still follows it.
    m.writeByte(0x0840, 0x10);
    m.writeHalf(0x0843, 0x1000);
    m.writeHalf(0x0841, m.readHalf(0x0843));
    m.writeHalf(0x0852, m.readHalf(0x0841));
    m.writeHalf(0x0845, static_cast<uint16_t>(m.readHalf(0x0843) - 32));
    m.writeByte(0x084B, 0x07);
    m.writeByte(0x084C, 0x05);
    trace.csp("low storage: main-storage size block 0840..0846, 084B, 084C, 0852 "
              "(csipl c1831764) - 0841/0843 = {:04X}, 0845 = {:04X}",
              m.readHalf(0x0841), m.readHalf(0x0845));

    // The IPL region's end is a literal 8191; the fixed disk's end is the
    // EXCLUSIVE 1-based end, not the count or last physical sector: a real
    // machine publishes 0C8001 for a 0C8000-sector medium.  SSP proves the
    // convention when it subtracts the IPL-region end 001FFF: the resulting
    // sector count must be divisible into ten-sector blocks, and publishing
    // the raw image count instead yields the guest's own SYS-2848 "Number of
    // blocks on drive is not a multiple of 10".
    m.writeAddr24(kTransientRegionEnd, kTransientRegionLastSector);
    // Only three bytes of the end are stored.  Refuse silent wrap at the
    // representable boundary; ordinary images are much smaller than this.
    if (volumeSectors < 0 || volumeSectors >= 0xFFFFFF) {
        error = fmt::format("fixed-disk exclusive end must fit in 24 bits\nParameter name: volumeSectors\n"
                            "Actual value was {}.", volumeSectors);
        return false;
    }
    int fixedDiskExclusiveEnd = volumeSectors + 1;
    m.writeAddr24(kFixedDiskEnd, fixedDiskExclusiveEnd);
    trace.csp("low storage: setfd - {:04X} = {} (IPL region end, a literal), "
              "{:04X} = {} (fixed disk exclusive 1-based end = "
              "{} image sectors + 1)",
              kTransientRegionEnd, kTransientRegionLastSector,
              kFixedDiskEnd, fixedDiskExclusiveEnd, volumeSectors);

    trace.csp("low storage: {} ACEs {:03X}-{:03X}, {} disk IOBs {:03X}-{:03X}, "
              "system block {:04X}, task block {:03X}",
              kAceCount, kAceAddresses[0], kAceAddresses[kAceCount - 1],
              kDiskIobCount, kDiskIobAddresses[0], kDiskIobAddresses[kDiskIobCount - 1],
              kSystemBlock, kTaskBlock);

    trace.csp("low storage: queue headers {} and {} = task block {:03X}",
              kTaskBlockQueueHeaders[0], kTaskBlockQueueHeaders[1], kTaskBlock);

    // Say what is NOT built, so a failure downstream is attributable.
    //
    // Queue headers 50, 34 and 53 - the ones phase 1 reads through SVC 0F -
    // are deliberately NOT built: the IPL initialises 37, 39 and 46 and
    // nothing else, so a zero header is what phase 1 is supposed to see at
    // this point.  Queue 50 is the program-block hash chain a transfer
    // searches, legitimately empty before the first transfer.
    //
    // Queue header 46's "QH" blocks are the task work area queue and are
    // built on demand by the control storage processor's task work area
    // code, so that a caller which has already put its own chain at 0x0BB9
    // keeps it - exactly the condition the IPL's own build is under.
    trace.csp("low storage: the 0x2000/0x4000 region sentinels, ace+5 and the "
              "error recovery blocks are NOT built - csipl's writes for them are "
              "mapped but their contents are only partly read. Queue header 46's "
              "\"QH\" blocks are the task work area queue and are built on first "
              "use (docs/s36/csipl-low-storage.md)");
    return true;
}

// ====================================================================
// The unit definition table walk
// ====================================================================
//
// The IPL reads the UDT into guest 0x1000 (16 sectors from 1-based 27) and
// walks it, dispatching on the device id and - for one id - on the device
// class.  What follows is that walk, transcribed.

void GuestLowStorage::walkUnitDefinitionTable(machine::MachineState& m, monitor::Tracer& trace,
                                              const uint8_t* udt, int udtLength)
{
    // Immediately before the loop: assume no diskette.  The device id D2 arm
    // is the only thing that clears it again.
    orByte(m, kDeviceFlags08B0, 0x04);
    trace.csp("UDT: 08B0 |= 04 before the walk (c1831dd0) - \"no diskette\", "
              "cleared again by the device id D2 arm");

    int off = 0, n = 0;
    while (off + kUdtHeaderBytes <= udtLength && udt[off + kUdtDeviceId] != 0) {
        UdtRecord r;
        r.table = udt;
        r.tableLength = udtLength;
        r.offset = off;
        r.id = udt[off + kUdtDeviceId];
        r.cls = udt[off + kUdtClass];
        r.unit = udt[off + kUdtUnit];
        r.length = recordLength(udt, off);
        r.customize = udt[off + kUdtL1] != 0 ? off + kUdtHeaderBytes : -1;
        r.configure = udt[off + kUdtL2] != 0 ? off + kUdtHeaderBytes + udt[off + kUdtL1] : -1;
        r.name = udt[off + kUdtL3] != 0
            ? off + kUdtHeaderBytes + udt[off + kUdtL1] + udt[off + kUdtL2] : -1;

        n++;
        dispatch(m, trace, r, n);

        if (r.length <= 0) break;             // a malformed record; the IPL would spin
        off += r.length;
    }

    trace.csp("UDT: {} record(s) walked, {} bytes, terminator at +{}", n, off, off);
}

// The device id dispatch, which is a comparison tree and not a table until it
// reaches ids A7..B2.  Transcribed branch for branch, including the ids that
// fall through to "advance and ignore" - those are as much a finding as the
// arms are.
void GuestLowStorage::dispatch(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    // Ids outside 1..210 are ignored.
    if (static_cast<unsigned>(r.id - 1) > 209) { ignore(trace, r, n, "device id is outside 1..210"); return; }

    if (r.id == 0x75) { lanArm(m, trace, r, n); return; }

    if (r.id > 0x75) {
        if (r.id >= 0xA7) {
            // Ids A7..B2 index a jump table.
            if (r.id <= 0xB2) { jumpTable(m, trace, r, n); return; }
            if (r.id < 0xD2) { ignore(trace, r, n, "B3..D1 have no arm"); return; }
            // Id D2: a diskette is present after all.
            andNot(m, kDeviceFlags08B0, 0x04);
            arm(trace, r, n, "diskette (c1832008)", "08B0 &= ~04");
            return;
        }
        if (r.id < 0x80 || r.id > 0x82) {
            ignore(trace, r, n, "76..7F and 83..A6 have no arm");
            return;
        }
        commArm(m, trace, r, n);
        return;
    }

    if (r.id == 0x61) { classDispatch(m, trace, r, n); return; }

    if (r.id > 0x61) {
        // Only id 71 does anything; 72, 73 and 74 are tested and rejected one
        // at a time, which is why they are worth recording.
        if (r.id != 0x71) { ignore(trace, r, n, "62..70 and 72..74 have no arm"); return; }
        orByte(m, kDeviceFlags08B0, 0x08);
        arm(trace, r, n, "device id 71 (c1831f14)", "08B0 |= 08");
        return;
    }

    if (r.id > 1) { ignore(trace, r, n, "02..60 have no arm"); return; }

    systemEntryArm(m, trace, r, n);
}

// The system entry, device id 01.  Six fields out of the record and one
// conditional flag.
void GuestLowStorage::systemEntryArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    uint8_t customize1 = r.field(r.customize, 0);
    m.writeByte(kSystemCustomize1, customize1);
    m.writeByte(kSystemCustomize1Copy, customize1);

    // 08E0 bit 80 is set when the machine is NOT a plain physical System/36
    // personality but the hosted one; it does not ask what instruction set
    // the guest executes.  This model is the hosted personality, and every
    // capture of a running Advanced/36 has the bit set.
    m.writeByte(kPersonality08E0, static_cast<uint8_t>(m.readByte(kPersonality08E0) | 0x80));
    trace.csp("UDT: 08E0 bit 80 set - Advanced/36 hosted-M36 personality "
              "(csipl / NuMach::is36Pers)");

    // The serial number, out of the name field.  "S/36 SN/10590MG" splits as
    // name[5..8] -> 08BF, name[9..12] -> 0898, name[13..14] -> 089C.
    for (int i = 0; i < 4; i++) m.writeByte(kSerialPrefix + i, r.field(r.name, 5 + i));
    for (int i = 0; i < 4; i++) m.writeByte(kSerialDigits + i, r.field(r.name, 9 + i));
    for (int i = 0; i < 2; i++) m.writeByte(kSerialDigits + 4 + i, r.field(r.name, 13 + i));

    uint8_t diskSize = r.field(r.configure, 7);
    m.writeByte(kDiskStorageSize, diskSize);

    // Configure byte 8 bit 0x20 - NOT the class byte - gates 08B0 bit 40,
    // which phase 1 branches on.
    uint8_t configure8 = r.field(r.configure, 8);
    bool bit40 = (configure8 & 0x20) != 0;
    if (bit40) orByte(m, kDeviceFlags08B0, 0x40);

    arm(trace, r, n, "system entry (c1831e68)",
        fmt::format("0850=08BD={:02X}, 084A={:02X}, 08BF..08C2={}, "
                    "0898..089D={}, configure[8]={:02X} -> 08B0 bit 40 {}",
                    customize1, diskSize,
                    hex(r, r.name, 5, 4), hex(r, r.name, 9, 6), configure8,
                    bit40 ? "SET" : "clear"));
}

// The class dispatch, reached for device id 61 and for no other id.  The
// 3487 SYSTEM CONSOLE on a typical volume carries class C0 but device id C2,
// and C2 is rejected before this - so it never reaches this dispatch.
void GuestLowStorage::classDispatch(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    switch (r.cls) {
    case 0x40:
        // Three flag bytes and nothing out of the record.
        orByte(m, kDeviceFlags08B2, 0x80);
        orByte(m, kDeviceFlags08B1, 0x10);
        orByte(m, kDeviceFlags080B, 0x9F);
        arm(trace, r, n, "class 40 (c18321c4)", "08B2 |= 80, 08B1 |= 10, 080B |= 9F");
        return;

    case 0xC0: controllerClassArm(m, trace, r, n); return;

    // Three arms whose whole content is a controller lookup and its device
    // count into 08C4, 08C5 and 08C6, plus one flag bit on the first.
    case 0x90:
        orByte(m, kWsControllerFlags, 0x20);
        arm(trace, r, n, "class 90 (c183232c)", "0849 |= 20");
        skipMaxDevices(trace, kMaxDevices08C3 + 1);
        return;
    case 0xA0:
        arm(trace, r, n, "class A0 (c183236c)", "nothing but the byte below");
        skipMaxDevices(trace, kMaxDevices08C3 + 2);
        return;
    case 0xB0:
        arm(trace, r, n, "class B0 (c183239c)", "nothing but the byte below");
        skipMaxDevices(trace, kMaxDevices08C3 + 3);
        return;

    default:
        ignore(trace, r, n, fmt::format("device id 61 class {:02X} matches none of 40/C0/90/A0/B0", r.cls));
        return;
    }
}

// Class C0: the work station controller.  Its customize byte 1 is a language
// id, put through a substitution table before it lands at guest 0x097F and
// is tested for a right-to-left language.
void GuestLowStorage::controllerClassArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    uint8_t customize1 = r.field(r.customize, 1);
    uint8_t language = mapLanguageId(customize1);
    m.writeByte(kLanguageId, language);
    m.writeByte(kLanguageId + 1, r.field(r.customize, 2));

    uint8_t configure0 = r.field(r.configure, 0);
    orByte(m, kWsControllerFlags, configure0);

    // The right-to-left test is four compares and nothing else: 36, 37, 38
    // and 98.
    bool rtl = language == 36 || language == 37 || language == 38 || language == 98;
    if (rtl) orByte(m, kWsControllerFlags, 0x02);

    orByte(m, kDeviceFlags08B2, 0x40);

    arm(trace, r, n, "class C0, work station controller (c18321f8)",
        fmt::format("097F={:02X} (customize[1]={:02X}), 0980={:02X}, "
                    "0849 |= {:02X}{}, 08B2 |= 40",
                    language, customize1, r.field(r.customize, 2), configure0,
                    rtl ? " | 02 (right-to-left language)" : ""));

    // Refresh 0x08C3 with the work-station controller's capacity.  build()
    // already seeds it so the capacity does not depend on this UDT record
    // being present.  MSIPL phase 2 tests 08C3, 08C4, 08C5 and 08C6 in turn
    // to pick the controller base it subtracts from a unit address; a
    // NON-zero 08C3 leaves the base at zero, while a zero one falls through
    // to 0x08 and then 0x88, which makes every station's Configure New Work
    // Stations record fail with reason 3.  08C4/08C5/08C6 stay unwritten:
    // their arms are classes 90, A0 and B0, controllers this emulator has no
    // device count for.
    m.writeByte(kMaxDevices08C3,
                static_cast<uint8_t>(devices::WorkStationController::kMaxDevices));
    trace.csp("UDT: {:04X} = {} - NuController::getMaxDevices for the work station "
              "controller (c1832318). Phase 2 tests it at logical 1DCB/1DF3 to choose "
              "the controller base it subtracts from a unit address; zero here selects "
              "0x88 and makes every station's Configure record fail cnfws reason 3.",
              kMaxDevices08C3, devices::WorkStationController::kMaxDevices);
}

// Six substitutions and a default.
uint8_t GuestLowStorage::mapLanguageId(uint8_t id)
{
    switch (id) {
    case 23: return 32;
    case 64: return 16;
    case 65: case 71: return 34;
    case 66: case 72: return 54;
    case 70: return 147;
    case 151: return 160;
    default: return id;
    }
}

// The LAN / token ring arm, device id 75.  Units 1 and 2 get a five-byte slot
// in the table at 0x0899 + 5n; unit 7 gets a fixed one at 0x08DB; every other
// unit address falls through and is ignored.
void GuestLowStorage::lanArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    if (r.unit == 7) {
        for (int i = 0; i < 5; i++) m.writeByte(kLanUnit7Configure + i, r.field(r.configure, i));
        orByte(m, kCommUnitTable, 0x01);
        orByte(m, kDeviceFlags08B0, 0x08);
        arm(trace, r, n, "LAN unit 7 (c1832450)",
            fmt::format("08DB..08DF={}, 086B |= 01, 08B0 |= 08", hex(r, r.configure, 0, 5)));
        return;
    }

    if (r.unit != 1 && r.unit != 2) {
        ignore(trace, r, n, fmt::format("LAN unit address {:02X} is none of 1, 2 or 7", r.unit));
        return;
    }

    int slot = kLanUnitTable + 5 * r.unit;
    for (int i = 0; i < 5; i++) m.writeByte(slot + i, r.field(r.configure, i));
    orByte(m, kDeviceFlags08B0, 0x08);
    arm(trace, r, n, "LAN unit 1/2 (c18323fc)",
        fmt::format("{:04X}..{:04X}={}, 08B0 |= 08", slot, slot + 4, hex(r, r.configure, 0, 5)));
    skipStackByte(trace, "086B", r.unit, "c1832428");
}

// The communications arm, device ids 80..82.  Five bytes of the configure
// area into a per-unit slot at 0x086B + 5n.
void GuestLowStorage::commArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    int slot = kCommUnitTable + 5 * r.unit;
    for (int i = 0; i < 5; i++) m.writeByte(slot + i, r.field(r.configure, i));
    arm(trace, r, n, "communications (c1831f40)",
        fmt::format("{:04X}..{:04X}={}", slot, slot + 4, hex(r, r.configure, 0, 5)));
    skipStackByte(trace, fmt::format("{:04X}/{:04X}/{:04X}", kCommFlags0820, kCommFlags0820 + 1, kCommFlags0820 + 2),
                  r.unit, "c1831f68");
}

// Device ids A7..B2, the one part of the dispatch that IS a jump table.  Its
// twelve entries: A7 disk; A8..B0 the record-advance tail (nine ids with no
// arm); B1 the two-unit device; B2 flags only.
void GuestLowStorage::jumpTable(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    if (r.id == 0xA7) { diskArm(m, trace, r, n); return; }
    if (r.id == 0xB1) { tapeUnitArm(m, trace, r, n); return; }
    if (r.id == 0xB2) {
        orByte(m, kDeviceFlags08B1, 0x20);
        orByte(m, kDeviceFlags08BC, 0x08);
        orByte(m, kDeviceFlags08BC, 0x02);
        arm(trace, r, n, "device id B2 (c18325e8)", "08B1 |= 20, 08BC |= 08 | 02");
        return;
    }
    ignore(trace, r, n, fmt::format("jump table entry {} for device id {:02X} is c1832028, the advance tail",
                                    r.id - 0xA7, r.id));
}

// Disk, device id A7.  Counts the unit and files three bytes of its configure
// area in one of three slots chosen by record byte 3.  Record byte 3 equal to
// 1 matches no slot, so the count is then the arm's whole effect.
void GuestLowStorage::diskArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    m.writeByte(kDiskUnitCount, static_cast<uint8_t>(m.readByte(kDiskUnitCount) + 1));

    uint8_t selector = r.head(3);
    int slot = selector == 2 ? kDiskAddressSlots
             : selector == 3 ? kDiskAddressSlots + 3
             : selector == 4 ? kDiskAddressSlots + 6 : 0;

    if (slot != 0)
        for (int i = 0; i < 3; i++) m.writeByte(slot + i, r.field(r.configure, 1 + i));

    arm(trace, r, n, "disk (c18324e4)",
        fmt::format("0851 -> {}; record+3 = {:02X} -> {}",
                    m.readByte(kDiskUnitCount), selector,
                    slot == 0
                        ? std::string("no address slot (only 2, 3 and 4 have one)")
                        : fmt::format("{:04X}..{:04X} = {}", slot, slot + 2, hex(r, r.configure, 1, 3))));
}

// Device id B1.  A two-unit device: unit 0 and unit 1 each own a bit of
// 0x086C, and when both are present 0x08B6 gets bit 08.  Every unit address,
// including the ones with no bit, sets 0x08BC bit 02.
void GuestLowStorage::tapeUnitArm(machine::MachineState& m, monitor::Tracer& trace, const UdtRecord& r, int n)
{
    uint8_t bit = r.unit == 0 ? 0x01 : r.unit == 1 ? 0x02 : 0x00;
    if (bit != 0) orByte(m, kTapeUnitFlags, bit);
    orByte(m, kDeviceFlags08BC, 0x02);

    bool both = (m.readByte(kTapeUnitFlags) & 0x03) == 0x03;
    if (both) orByte(m, kDeviceFlags08B6, 0x08);

    arm(trace, r, n, "device id B1 (c183256c)",
        fmt::format("unit {:02X} -> {}, 08BC |= 02{}", r.unit,
                    bit == 0 ? std::string("no 086C bit") : fmt::format("086C |= {:02X}", bit),
                    both ? ", both units present -> 08B6 |= 08" : ""));
}

// ---- what the walk does NOT build, and why -----------------------------

// The device count of the controller a class arm just found, stored at
// 0x08C3..0x08C6 by the four class arms.  Nothing in the unit definition
// table carries the answer, and inventing a device count is a guess.
void GuestLowStorage::skipMaxDevices(monitor::Tracer& trace, int addr)
{
    trace.csp("UDT: {:04X} NOT written - it is NuController::getMaxDevices for the "
              "controller NuControllerMap::findController returns, a live host "
              "object with nothing on the volume behind it", addr);
}

// The comm and LAN arms OR a byte of the IPL routine's own stack frame into a
// low-storage flag byte.  For every unit address but 0 the source is
// uninitialised stack, and even for unit 0 it is a lookup input byte rather
// than a result.  The value is not established, so it is not written.
void GuestLowStorage::skipStackByte(monitor::Tracer& trace, const std::string& what, uint8_t unit, const char* site)
{
    trace.csp("UDT: {} NOT OR-ed at {} - csipl's source is its own stack at "
              "r1+{} ({}+unit {}), which nothing in the walk initialises",
              what, site, 103 + unit, 103, unit);
}

// ---- tracing -----------------------------------------------------------

std::string GuestLowStorage::hex(const UdtRecord& r, int area, int at, int len)
{
    std::string s;
    for (int i = 0; i < len; i++) {
        if (i != 0) s.push_back(' ');
        s += fmt::format("{:02X}", r.field(area, at + i));
    }
    return s;
}

void GuestLowStorage::arm(monitor::Tracer& trace, const UdtRecord& r, int n, const char* arm, const std::string& what)
{
    trace.csp("UDT: record {} at +{}, id {:02X} class {:02X} unit {:02X}, {} bytes "
              "-> {}: {}", n, r.offset, r.id, r.cls, r.unit, r.length, arm, what);
}

void GuestLowStorage::ignore(monitor::Tracer& trace, const UdtRecord& r, int n, const std::string& why)
{
    trace.csp("UDT: record {} at +{}, id {:02X} class {:02X}, {} bytes -> ignored "
              "({})", n, r.offset, r.id, r.cls, r.length, why);
}

}  // namespace sim36::processors::controlstorage
