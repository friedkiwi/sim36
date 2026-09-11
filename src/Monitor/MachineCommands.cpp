// Monitor commands that reach into the control storage processor: the
// device path (diskread), the guest structure decoders (ace, iob, tu), the
// scheduler view and the control-storage IPL conformance vectors.
#include "Monitor/MonitorCli.h"

#include <algorithm>

#include <fmt/format.h>

#include "Devices/IoBlock.h"
#include "Devices/UnitBlock.h"
#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Processors/ControlStorage/ActionControlElementQueue.h"
#include "Processors/ControlStorage/GuestLowStorage.h"

namespace sim36::monitor {

using processors::controlstorage::ActionControlElementQueue;
using processors::controlstorage::As36ControlStorageProcessor;
using processors::controlstorage::GuestLowStorage;
using processors::controlstorage::RequestBlock;
using processors::controlstorage::SvcRequest;
using storage::DiskBackend;

namespace {

const char* dispatchClassName(processors::controlstorage::DispatchClass c)
{
    switch (c) {
        case processors::controlstorage::DispatchClass::Immediate: return "Immediate";
        case processors::controlstorage::DispatchClass::Overlapped: return "Overlapped";
        case processors::controlstorage::DispatchClass::Delayed: return "Delayed";
    }
    return "?";
}

// Convert.ToInt32(s, 16) / Convert.ToByte(s, 16) as the reference parses
// hex arguments: optional 0x prefix, hex digits only.
int parseHexArg(const std::string& s, int maxDigits)
{
    std::string h = s;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.empty()) throw MonitorError("Could not find any recognizable digits.");
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw MonitorError("Could not find any recognizable digits.");
    if (static_cast<int>(h.size()) > maxDigits)
        throw MonitorError(maxDigits == 2 ? "Value was either too large or too small for an unsigned byte."
                                          : "Value was either too large or too small for an Int32.");
    unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    if (maxDigits == 8 && v > 0x7FFFFFFFUL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

int parseInt(const std::string& s)
{
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    std::size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) throw MonitorError("Input string was not in a correct format.");
    for (std::size_t k = i; k < s.size(); k++)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) throw MonitorError("Input string was not in a correct format.");
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    if (v > 0x7FFFFFFFLL || v < -0x80000000LL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

struct Score {
    int pass = 0, fail = 0;
};

void report(const std::string& what, bool ok, Score& s)
{
    fmt::print("  {:<42} {}\n", what, ok ? "PASS" : "FAIL");
    if (ok) ++s.pass; else ++s.fail;
}

}  // namespace

// Build a disk IOB in guest storage and drive it through the real SVC 40
// path, so the ACE, the device and the completion mask are all exercised
// exactly as the guest would exercise them.  The landing area is high
// enough to be clear of phase 1 and low storage.
void MonitorCli::diskRead(const std::vector<std::string>& a)
{
    constexpr int kDiskReadBuffer = 0x20000;
    if (a.size() < 2) { fmt::print("diskread <sector> [count]\n"); return; }
    int sector = parseInt(a[1]);
    int count = a.size() > 2 ? parseInt(a[2]) : 1;

    constexpr int iob = 0x0600;
    auto& st = m_.state;
    for (int i = 0; i < 48; i++) st.writeByte(iob + i, 0);

    // The command is the HIGH byte, +0x0A; the sector field is 1-based on
    // the wire while the command takes a 0-based sector.
    st.writeByte(iob + devices::IoBlock::kOffCommand, devices::VirtualFixedDisk::kCommandRead);
    st.writeAddr24(iob + devices::IoBlock::kOffDiskSector, sector + 1);
    st.writeAddr24(iob + devices::IoBlock::kOffDiskCount, count - 1);
    st.writeAddr24(iob + devices::IoBlock::kOffDataBuffer, kDiskReadBuffer);

    // A rejected call is still reported: only "no request block" returns
    // early, exactly as the reference does.
    if (issueDeviceSvc(0x40, iob) < 0) return;
    fmt::print("{}", devices::IoBlock::dump(st, iob));

    auto& disk = m_.devices().disk;
    if (disk.hasLastRead()) {
        fmt::print("-- first 64 bytes read from sector {} --\n", disk.lastReadSector());
        fmt::print("{}", hexDump(disk.lastRead().data(), std::min<int>(64, static_cast<int>(disk.lastRead().size())), 0));
    }
}

// Issue a device SVC through the control processor with XR1 naming the
// IOB, exactly as an SVC instruction would, and put the live and saved XR1
// back afterwards.  -1 when the machine has no current request block
// (nothing has been booted), 0 when the call was rejected, 1 when it
// completed.
int MonitorCli::issueDeviceSvc(uint8_t r, int iob)
{
    auto& csp = m_.nativeControlStorage();
    int rb = csp.currentRequestBlock();
    if (rb == 0) {
        fmt::print("no current request block - `boot` the machine first\n");
        return -1;
    }

    auto& st = m_.state;
    uint8_t livePrefix = st.msp.pactXr1;
    uint16_t liveXr1 = st.msp.xr1;
    uint8_t savedPrefix = st.readByte(rb + RequestBlock::kOffXr1High);
    uint16_t savedXr1 = st.readHalf(rb + RequestBlock::kOffXr1Low);

    st.msp.pactXr1 = static_cast<uint8_t>(iob >> 16);
    st.msp.xr1 = static_cast<uint16_t>(iob);

    SvcRequest req;
    req.r = r;
    req.q = 0;
    req.dispatch = m_.controlStorage().classify(r);
    bool ok = m_.controlStorage().svc(req);
    fmt::print("SVC {:02X} ({}) iob {:06X} -> {}\n", r, dispatchClassName(req.dispatch), iob, ok ? "completed" : "rejected");

    st.msp.pactXr1 = livePrefix;
    st.msp.xr1 = liveXr1;
    st.writeByte(rb + RequestBlock::kOffXr1High, savedPrefix);
    st.writeHalf(rb + RequestBlock::kOffXr1Low, savedXr1);
    return ok ? 1 : 0;
}

void MonitorCli::showAce(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() > 2 && equalsIgnoreCase(a[1], "queue")) {
        int h = parseHexArg(a[2], 2);
        std::vector<int> els = csp.aces().elements(static_cast<uint8_t>(h));
        fmt::print("queue header {:02X} at guest {:04X}: {} element(s)\n", h, ActionControlElementQueue::headerAddress(h),
                   els.size());
        for (int e : els) fmt::print("  {:04X}\n", e);
        return;
    }
    if (a.size() < 2) { fmt::print("ace <hexaddr> | ace queue <hex header>\n"); return; }
    csp.aces().dump(stdout, parseHexArg(a[1], 8));
}

void MonitorCli::showIob(const std::vector<std::string>& a)
{
    if (a.size() < 2) { fmt::print("iob <hexaddr>\n"); return; }
    fmt::print("{}", devices::IoBlock::dump(m_.state, parseHexArg(a[1], 8)));
}

// Read-only terminal-unit-block diagnostic.  The address is a guest address
// because unit block allocation is guest-visible and the fields are owned
// by SSP.  No field is repaired or synthesised.
void MonitorCli::showUnitBlock(const std::vector<std::string>& a)
{
    if (a.size() != 2) { fmt::print("usage: tu <hexaddr>\n"); return; }
    std::string h = a[1];
    bool hex = !h.empty();
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
    if (!hex || h.size() > 8) { fmt::print("tu: address must be hexadecimal\n"); return; }
    int block = static_cast<int>(std::strtoul(h.c_str(), nullptr, 16));
    if (!(block > 0 && block <= m_.state.backingBytes() - 0xA0)) {
        fmt::print("tu: {:06X}+00..9F is outside readable guest storage\n", block);
        return;
    }
    devices::UnitBlock::dump(stdout, m_.state, block);
}

void MonitorCli::sched()
{
    fmt::print("now = {}, {} pending\n", m_.scheduler.now(), m_.scheduler.pending());
    for (const auto& e : m_.scheduler.peek()) fmt::print("  {:>10}  {}\n", e.at, e.label);
}

// The control-storage IPL vectors: what a conforming control processor must
// leave behind after stage B, checked against the machine rather than
// against a transcript.
void MonitorCli::conformance()
{
    m_.reset();
    Score sc;
    auto& st = m_.state;

    bool eyes = true;
    for (int a : GuestLowStorage::kAceAddresses)
        if (st.readHalf(a) != GuestLowStorage::kEyeAce) eyes = false;
    report("low storage: ACE eyecatchers", eyes, sc);

    bool iobs = true;
    for (int a : GuestLowStorage::kDiskIobAddresses)
        if (st.readHalf(a) != GuestLowStorage::kEyeDiskIob) iobs = false;
    report("low storage: disk IOB eyecatchers", iobs, sc);

    report("low storage: system block at 6000", st.readHalf(GuestLowStorage::kSystemBlock) == GuestLowStorage::kEyeSystemBlock,
           sc);

    long long expectedDiskEnd = m_.diskBackend().sectorCount() + 1;
    report("setfd: fixed-disk end is exclusive 1-based image end",
           expectedDiskEnd <= 0xFFFFFF && st.readAddr24(GuestLowStorage::kFixedDiskEnd) == expectedDiskEnd, sc);
    report("setfd: post-IPL disk extent forms ten-sector blocks",
           (st.readAddr24(GuestLowStorage::kFixedDiskEnd) - st.readAddr24(GuestLowStorage::kTransientRegionEnd)) %
                   DiskBackend::kSectorsPerBlock ==
               0,
           sc);

    // The task post refuses a task block whose first halfword is not this,
    // so a conforming CSP must produce one that survives the machine's own
    // check.
    report("task block at F00 carries \"TB\" (E3C2)", st.readHalf(GuestLowStorage::kTaskBlock) == GuestLowStorage::kEyeTaskBlock,
           sc);

    bool ptrs = true;
    for (int a : {0x3C0, 0x400, 0x440, 0x4E0})
        if (st.readAddr24(a + GuestLowStorage::kAceTaskBlockPointer) != GuestLowStorage::kTaskBlock) ptrs = false;
    report("ace+19 points at the task block", ptrs, sc);

    // Phase 1 must be the boot record, byte for byte, not a library member.
    bool loaded = true;
    for (int s = 0; s < As36ControlStorageProcessor::kPhase1Sectors; s++) {
        uint8_t want[DiskBackend::kSectorBytes];
        if (!m_.diskBackend().readSector(As36ControlStorageProcessor::kPhase1Sector + s, want)) { loaded = false; break; }
        for (int i = 0; i < DiskBackend::kSectorBytes; i++)
            if (st.readByte(As36ControlStorageProcessor::kPhase1LoadAddress + s * DiskBackend::kSectorBytes + i) != want[i])
                loaded = false;
    }
    report("4 KB of phase 1 at 1000, from sector 8191", loaded, sc);

    report("MSP running, IAR inside phase 1",
           !m_.msp().stopped() && st.msp.iar >= As36ControlStorageProcessor::kPhase1LoadAddress &&
               st.msp.iar < As36ControlStorageProcessor::kPhase1LoadAddress + 0x1000,
           sc);

    // The machine must make progress without anything outside the control
    // processor touching a register.  The cap is a runaway guard, not a
    // target.
    int executed = 0;
    while (executed < 100000 && m_.msp().step()) executed++;
    report("MSP executes phase 1 without external help", executed > 0, sc);
    fmt::print("     {} instruction(s), then: {}\n", executed,
               m_.msp().stopReason().empty() ? std::string("still running") : m_.msp().stopReason());

    fmt::print("{} passed, {} failed\n", sc.pass, sc.fail);
}

}  // namespace sim36::monitor
