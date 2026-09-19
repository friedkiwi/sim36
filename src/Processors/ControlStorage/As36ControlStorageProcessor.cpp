#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <fmt/format.h>

#include "Configuration/IplSourceTable.h"
#include "Devices/IoBlock.h"
#include "Devices/WorkStationIob.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/MapParameterList.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/SvcTable.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Storage/Ebcdic.h"

namespace sim36::processors::controlstorage {

namespace {

// Task block fields, decimal offsets.
constexpr int kTbPriority = 7;        // dispatching priority, the queue sort key
constexpr int kTbPriorityFloor = 20;  // priority may not be lowered past this
constexpr int kTbRequestBlock = 65;   // 3 bytes: guest address of the rb (0x41)

// Chain-field displacements the unit-block queueing is called with: the
// block's own queue link ends at +82, the shared queue-50 link at +77.
constexpr int kOwnQueueChainField = 82, kSharedQueueChainField = 77;
// The unit-block queue insert's constant flags byte: bit 0x40 is "system
// request"; Priority and Lifo are both clear, so the insert is FIFO.
constexpr uint8_t kUnitBlockQueueFlags = 0x40;

constexpr uint8_t kUdtSystemEntry = 0x01;

// The dispatch class as the reference prints its enum.
const char* svcClassName(DispatchClass c)
{
    switch (c) {
        case DispatchClass::Immediate: return "Immediate";
        case DispatchClass::Overlapped: return "Overlapped";
        case DispatchClass::Delayed: return "Delayed";
    }
    return "?";
}

}  // namespace

As36ControlStorageProcessor::As36ControlStorageProcessor(machine::MachineState& m,
                                                         const configuration::EmulatorConfig& cfg,
                                                         devices::DeviceSet& devices, storage::DiskBackend& disk,
                                                         monitor::Tracer& trace)
    : m_(m), cfg_(cfg), devices_(devices), disk_(disk), trace_(trace),
      // The system queue space.  0x2000..0xFFFF is the initial segment; the
      // native pool grows in 64 KB units up to its ceiling, and the machine's
      // wider backing keeps resident module frames above that ceiling.
      heap_(m, kSystemQueueSpace, kSystemQueueSpaceBytes, std::min(m.backingBytes(), kNativeSystemQueueHigh), trace),
      // Control storage, not guest storage: SVC 04's transfer control table
      // pointer lives here and nowhere else.
      directArea_(), aces_(m, heap_, trace), transients_(*this, trace),
      // The control processor constructs the processor it owns.
      msp_(std::make_unique<MainStorageProcessor>(m, *this, trace))
{
}

// Record a refusal reason, trace it, and return false, so a refusing step
// reads as one statement.  Refusals nest, and the inner reason is the one
// that matters, so an outer step prepends itself rather than overwriting.
bool As36ControlStorageProcessor::refuseText(const std::string& reason)
{
    trace_.csp("{}", reason);
    lastRefusal_ = lastRefusal_.empty() ? reason : reason + "\n    caused by: " + lastRefusal_;
    return false;
}

// ---- stage A ----------------------------------------------------------------

// Vacuous by construction: there is no control storage to load microcode
// into and no microcode to load.  Kept explicit so a microcode
// implementation has somewhere to put its equivalent.
void As36ControlStorageProcessor::bringUpControlProcessor()
{
    systemPowerOffRequested_ = false;
    heap_.reset();
    // The ACEs live in the pool the line above just rebuilt.
    aces_.reset();
    systemMeasurementEnabled_ = false;
    pendingDeviceAces_.clear();
    deferredWsInput_.clear();
    nutixTimers_.clear();
    nativeTransferContinuations_.clear();
    currentTransientProgramBlock_ = 0;
    devices_.resetPendingIo();
    trace_.csp("control processor bring-up: native, no microcode to load");

    // The panel's load-source switch is latched into control storage at
    // power-on, before the main storage IPL reads it.  This is stage A
    // because on a real machine it is the control processor that owns both
    // the panel and the direct areas; phase 1 reads word 1074 as its fourth
    // instruction and could not if a later stage set it.
    int src = cfg_.iplSource();
    directArea_.write(DirectArea::kIplSource, src);
    trace_.csp("panel: load source '{}', IPL type '{}' -> direct area word {} = {:04X} - {}", cfg_.iplSourceName,
               cfg_.iplType, DirectArea::kIplSource, src,
               configuration::IplSourceTable::describe(cfg_.iplSourceName, src));

    // Word 1124 is the other direct area word a module reads before anything
    // writes it: IPL phase 2 hands it to SVC 33 as the task work area's
    // region size.  Same stage and same reason as the load source.
    directArea_.write(DirectArea::kTaskWorkAreaSize, cfg_.taskWorkAreaSectors);
    trace_.csp("machine: task work area {} sector(s) -> direct area word {} (SA21-9436 TWAL's default is 60; "
               "which machine property supplies it is emulator policy)",
               cfg_.taskWorkAreaSectors, DirectArea::kTaskWorkAreaSize);
}

// ---- stage B: the main storage IPL ---------------------------------------------

// The architected half: build guest low storage, load phase 1, build the
// task block, post it, and leave the MSP running.  The MSP is handed an
// UNTRANSLATED machine on purpose: phase 1 runs in real storage and
// transfers to itself to run translated, so the ATRs are not populated
// here.
void As36ControlStorageProcessor::iplMainProcessor()
{
    wsPresentByTask_.clear();
    transferredWorkStationTub_ = 0;
    transferredAutoSignOn_ = false;
    transferredWorkStations_.clear();
    pendingTransferredWorkStationTub_ = 0;
    pendingTransferAutoSignOn_ = false;
    lastM36WorkStationTransferPostedGuestWork_ = false;
    GuestLowStorage::HostInfo host;
    host.model = cfg_.hostModel;
    host.processorFeature = cfg_.hostProcessorFeature;
    host.processorModel = cfg_.hostProcessorModel;
    std::string error;
    if (!GuestLowStorage::build(m_, trace_, static_cast<int>(std::min<long long>(disk_.sectorCount(), 0x7FFFFFFF)),
                                host, cfg_.systemCustomize1(), error)) {
        // A volume too large for the 24-bit disk-end field is a configuration
        // fault; the low storage written so far stands, as it does on the
        // reference, and the IPL stops here with the reason.
        lastRefusal_ = error;
        trace_.line("csp", "{}", error);
        return;
    }
    buildFromUnitDefinitionTable();
    // The IPL's order is the disk-end setup, the UDT read, the host
    // processor information, then this, which asks for the terminal block
    // only.
    buildSystemUnitBlock(true);
    // Only after the bootstrap unit block is published does the IPL walk the
    // work-station list and activate the displays acquired at IPL: a physical
    // report-in that is already pending may then enter the controller's
    // lookup, which starts at the block just created.
    devices_.activateIplWorkStations();
    loadPhase1();
    postInitialTask();

    // The main storage IPL is complete and the MSP is running, so the M36's
    // reference code is the running value 0000, which is what a healthy
    // machine reports.
    m_.postM36Src(0x0000, "main storage IPL complete, MSP running");
}

// The unit definition table, walked as the IPL walks it: the device walk,
// then the IPL parameter byte, which is read out of the SAME record (the
// system entry's byte 3) by the IPL's prologue.
void As36ControlStorageProcessor::buildFromUnitDefinitionTable()
{
    std::vector<uint8_t> rec(static_cast<std::size_t>(kUdtSectors) * storage::DiskBackend::kSectorBytes);
    for (int i = 0; i < kUdtSectors; i++)
        diskRead(kUdtSector + i, rec.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes, "UDT");

    const std::vector<uint8_t> generated =
        GuestLowStorage::synthesizeUnitDefinitionTable(cfg_.systemCustomize1());
    // Recognize a table previously generated by SIM/36 even when its old
    // system-personality byte differs.  SSP 5.1 accepts 8D (5364) and 8B
    // (5363), but rejects the short-lived 89 Advanced/36 identity during
    // IPL.  Refreshing our own table migrates affected writable volumes back
    // to the compatible 5364 personality without touching an IBM table.
    bool emulatorTable = rec.size() == generated.size();
    for (int i = 0; emulatorTable && i < kUdtPersistedSectors * storage::DiskBackend::kSectorBytes; i++) {
        constexpr int kSystemCustomize1 = 11;
        if (i != kSystemCustomize1 && rec[static_cast<std::size_t>(i)] != generated[static_cast<std::size_t>(i)])
            emulatorTable = false;
    }

    if (rec[GuestLowStorage::kUdtDeviceId] != kUdtSystemEntry || emulatorTable) {
        uint8_t oldId = rec[GuestLowStorage::kUdtDeviceId];
        uint8_t oldPersonality = rec[11];
        rec = generated;

        // The Advanced/36 power-on writes the generated table both to the
        // IPL UDT area and to its protected-area mirror before emIPL reads
        // it.  CNFIGSSP consults the on-disk hardware description later, so
        // populating low storage alone cannot make configuration validation
        // work.  Overlay attachments retain these writes in their overlay;
        // a genuinely read-only attachment still gets the synthesized table
        // for this IPL but cannot, by definition, apply a master-record update.
        int written = 0;
        for (int base : {kUdtSector, kUdtMirrorSector}) {
            if (base + kUdtPersistedSectors > disk_.sectorCount()) continue;
            for (int i = 0; i < kUdtPersistedSectors; i++) {
                const uint8_t* sector = rec.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes;
                if (disk_.writeSector(base + i, sector)) written++;
            }
        }
        trace_.csp("UDT: sector {} began with device id {:02X}, personality {:02X}; power-on {} the emulator "
                   "hardware table with personality {:02X} and persisted {}/{} sectors at {} and its mirror {}{}",
                   kUdtSector, oldId, oldPersonality, emulatorTable ? "refreshed" : "synthesized",
                   rec[11], written, 2 * kUdtPersistedSectors, kUdtSector, kUdtMirrorSector,
                   written == 0 ? " (attachment is read-only)" : "");
    }

    GuestLowStorage::walkUnitDefinitionTable(m_, trace_, rec.data(), static_cast<int>(rec.size()));

    // 0x08B1 and 0x08B2 are the two bits phase 1 branches on at 0x113D,
    // 0x145B and 0x1BD6.  The IPL sets them behind two gates: bit 0x01 of
    // the IPL parameter byte SET performs the writes, and 0x08B1 additionally
    // waits for the machine's first-ever IPL.
    //
    // The volume's byte is NOT the input.  The IPL prologue takes byte 3 of
    // the system entry, forces it to 0xFF and writes the entry back, and the
    // UDT is regenerated from the machine's configuration at every machine
    // start, so the byte on the image is the previous run's output.  The
    // request comes from the configuration, which is the panel this emulator
    // gives the operator.
    constexpr int kUdtIplParameter = 3;   // byte 3 of the system entry
    constexpr uint8_t kB2ParameterBit = 0x08, kB1FirstIplBit = 0x01;

    if (cfg_.iplRequestsReload()) {
        m_.writeByte(0x8B2, static_cast<uint8_t>(m_.readByte(0x8B2) | kB2ParameterBit));
        trace_.csp("UDT: load source '{}' -> IPL parameter byte bit 01 SET -> a reload is requested -> 08B2 |= {:02X} "
                   "(csipl c1832b14)",
                   cfg_.iplSourceName, kB2ParameterBit);
    } else {
        trace_.csp("UDT: load source '{}' -> IPL parameter byte bit 01 CLEAR -> no reload requested, csipl skips the "
                   "08B1/08B2 writes",
                   cfg_.iplSourceName);
    }

    trace_.csp("UDT: system entry byte {} on the volume = {:02X} - NOT read as an input: csipl stamps it 0xFF every IPL "
               "and powerOn regenerates the UDT before emIPL, so it is last run's output",
               kUdtIplParameter, rec[kUdtIplParameter]);

    // The first-IPL counter lives in the persistent machine object and has
    // no representation on the volume, so this models an already-installed
    // machine and leaves the bit clear.
    trace_.csp("UDT: 08B1 bit {:02X} left clear - it needs incrIplCount == 1, a persistent counter with nothing on the "
               "volume to derive it from",
               kB1FirstIplBit);
}

// The system console and printer unit blocks, built by the CONTROL PROCESSOR
// at IPL.  A block is assigned out of the system queue space, zeroed, eleven
// fields are filled, its guest address is published in the system
// communications area and it is queued.  The block is zero before any field
// is set, so the fields below are its complete initial state.
void As36ControlStorageProcessor::buildSystemUnitBlock(bool terminal)
{
    int bytes = GuestLowStorage::kSystemUnitBlockBytes;

    int block = heap_.allocate(bytes);
    if (block == 0) {
        trace_.csp("unit block: the system queue space could not supply {} bytes; the {} unit block is NOT built and "
                   "guest {:04X} stays zero",
                   bytes, terminal ? "console terminal" : "printer",
                   terminal ? GuestLowStorage::kSystemConsoleUnitBlockPointer
                            : GuestLowStorage::kSystemPrinterUnitBlockPointer);
        return;
    }

    for (int i = 0; i < bytes; i++) m_.writeByte(block + i, 0);

    // Set before the arms diverge.  Arguments 2 and 3 are zero, so +10 and
    // +12 are written with zero: written, not merely left.  +0x0A takes the
    // block size OR-ed in, which leaves the console block with the display
    // class 0xC0 before any guest code runs.
    m_.writeByte(block + 12, 0);
    m_.writeByte(block + 39, static_cast<uint8_t>(bytes));
    m_.writeByte(block + 10, static_cast<uint8_t>(m_.readByte(block + 10) | bytes));
    m_.writeByte(block + 6, 0x40);
    m_.writeHalf(block + 34, 0x1000);
    m_.writeByte(block + 36, 0xE2);

    if (terminal) {
        m_.writeHalf(block + 40, 0x000B);
        m_.writeByte(block + 7, 0x28);
        m_.writeByte(block + 5, 0x80);
        m_.writeByte(block + 117, 0xA0);
        m_.writeHalf(block + 0, 0xE3E4);   // "TU"
    } else {
        // The printer arm sets neither +5 nor +117.
        m_.writeHalf(block + 40, 0x0009);
        m_.writeByte(block + 69, 0x42);
        m_.writeByte(block + 7, 0x08);
        m_.writeHalf(block + 0, 0xD7E4);   // "PU"
    }

    int pointer = terminal ? GuestLowStorage::kSystemConsoleUnitBlockPointer
                           : GuestLowStorage::kSystemPrinterUnitBlockPointer;
    m_.writeAddr24(pointer, block);

    trace_.csp("unit block: {} block of {} bytes at guest {:06X}, eyecatcher {}, published at guest {:04X}",
               terminal ? "console terminal \"TU\"" : "printer \"PU\"", bytes, block, terminal ? "E3E4" : "D7E4",
               pointer);

    // Queue it, twice: the block carries two chain links, one for its own
    // header (49 terminal, 51 printer) and one for the shared header 50.
    int ownQueue = terminal ? GuestLowStorage::kConsoleUnitBlockQueue : GuestLowStorage::kPrinterUnitBlockQueue;
    queueOperation(GuestLowStorage::queueHeader(ownQueue), block, kOwnQueueChainField, kUnitBlockQueueFlags);
    queueOperation(GuestLowStorage::queueHeader(GuestLowStorage::kSharedUnitBlockQueue), block,
                   kSharedQueueChainField, kUnitBlockQueueFlags);
    trace_.csp("unit block: queued onto headers {} (chain field +{}) and {} (chain field +{}); flags 40, FIFO because "
               "Priority and Lifo are clear",
               ownQueue, kOwnQueueChainField, GuestLowStorage::kSharedUnitBlockQueue, kSharedQueueChainField);
}

void As36ControlStorageProcessor::loadPhase1()
{
    int bytes = kPhase1Sectors * storage::DiskBackend::kSectorBytes;
    std::vector<uint8_t> buf(static_cast<std::size_t>(bytes));
    if (cfg_.loadsFromDiskette()) {
        loadPhase1FromDiskette(buf.data(), bytes);
        m_.write(kPhase1LoadAddress, buf.data(), bytes);
        return;
    }
    for (int i = 0; i < kPhase1Sectors; i++)
        diskRead(kPhase1Sector + i, buf.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes,
                 "phase 1");
    trace_.csp("phase 1: {} sectors from {} ({} bytes) to guest {:04X}", kPhase1Sectors, kPhase1Sector, bytes,
               kPhase1LoadAddress);
    m_.write(kPhase1LoadAddress, buf.data(), bytes);
}

// Stage B when the panel's load source is DISKETTE: the control processor
// reads the diskette-resident phase 1, the first 4 KB of the #IPLBOOT data
// set, instead of the disk boot record.  The two are the same thing for two
// different devices: #IPLBOOT is [phase 1, 4 KB][the reload members], which
// is why the disk phase 1 starts its sequential-sector read at sector 5.
// Every failure here is an operator error at power-on rather than a
// machine state (an empty drive, a non-IPL volume), so each one says which,
// the way a bad `volume =` does: a host-layer error the monitor reports.
void As36ControlStorageProcessor::loadPhase1FromDiskette(uint8_t* buf, int bytes)
{
    auto& drive = devices_.diskette;
    if (!drive.hasMedium())
        throw std::runtime_error("load_source = diskette, but the diskette drive is empty: the control processor has "
                                 "nowhere to read phase 1 from. Insert a volume carrying " +
                                 std::string(kDisketteIplDataSet));

    storage::DisketteBackend* medium = drive.medium();
    storage::DisketteBackend::DataSet ds;
    if (!medium->findDataSet(kDisketteIplDataSet, ds))
        throw std::runtime_error(fmt::format(
            "load_source = diskette, but {} carries no {} data set, so it is not an IPL volume (volume {}, owner {}). "
            "Base-SSP volume 01 carries one; a program-product volume does not",
            medium->path(), kDisketteIplDataSet, medium->geometry().volumeId(), medium->geometry().ownerId()));

    if (ds.recordBytes <= 0 || bytes % ds.recordBytes != 0)
        throw std::runtime_error(fmt::format("{} on {} is recorded {} bytes per record, which does not divide the {}-byte "
                                             "phase 1",
                                             kDisketteIplDataSet, medium->path(), ds.recordBytes, bytes));

    int records = bytes / ds.recordBytes;
    int c = ds.cylinder, h = ds.head, r = ds.record;
    for (int i = 0; i < records; i++) {
        if (!medium->geometry().isValid(c, h, r))
            throw std::runtime_error(fmt::format("{} record {} of {} is at C/H/R {}/{}/{}, which is not on {}",
                                                 kDisketteIplDataSet, i + 1, records, c, h, r, medium->path()));
        std::vector<uint8_t> rec;
        if (!medium->readRecord(c, h, r, rec) || static_cast<int>(rec.size()) < ds.recordBytes)
            throw std::runtime_error(fmt::format("{} claims {}-byte records but C/H/R {}/{}/{} is recorded {}",
                                                 kDisketteIplDataSet, ds.recordBytes, c, h, r, rec.size()));
        std::copy(rec.begin(), rec.begin() + ds.recordBytes, buf + static_cast<std::ptrdiff_t>(i) * ds.recordBytes);
        if (i + 1 < records && !medium->next(c, h, r))
            throw std::runtime_error(fmt::format("{} runs past the end of {} after {} record(s)", kDisketteIplDataSet,
                                                 medium->path(), i + 1));
    }

    trace_.csp("phase 1: {} record(s) of {} B from {} at C/H/R {}/{}/{} (volume {}, extent {:02}{}{:02}..{:02}{}{:02}), {} "
               "bytes to guest {:04X} - MSPID, the diskette-resident phase 1",
               records, ds.recordBytes, kDisketteIplDataSet, ds.cylinder, ds.head, ds.record,
               medium->geometry().volumeId(), ds.cylinder, ds.head, ds.record, ds.endCylinder, ds.endHead, ds.endRecord,
               bytes, kPhase1LoadAddress);
}

// On the real machine the MSP is started by making a task runnable, never
// by setting an instruction address from outside, so the entry point is a
// FIELD of the task block, and building the block is what chooses it.
void As36ControlStorageProcessor::postInitialTask()
{
    // The task post validates the eyecatcher before it will use the block,
    // and the IPL writes a halfword here, not just the two tag bytes.
    m_.writeHalf(GuestLowStorage::kTaskBlock, GuestLowStorage::kEyeTaskBlock);
    m_.writeHalf(GuestLowStorage::kTaskBlock + 2, 0x0009);
    m_.writeByte(GuestLowStorage::kTaskBlock + kTbPriority, 0xFC);
    m_.writeByte(GuestLowStorage::kTaskBlock + kTbPriorityFloor, 0xFC);

    // The task block does NOT hold the instruction address.  tb+0x41 is the
    // guest address of the task's REQUEST BLOCK, and the instruction address
    // lives at rb+24 as a halfword with its PACT prefix at rb+13.
    m_.writeHalf(kRequestBlock, GuestLowStorage::kEyeRequestBlock);
    m_.writeHalf(kRequestBlock + RequestBlock::kOffIar, static_cast<uint16_t>(kPhase1LoadAddress));
    m_.writeByte(kRequestBlock + RequestBlock::kOffPiar, 0);     // untranslated at IPL
    m_.writeByte(kRequestBlock + RequestBlock::kOffPsr, 0x01);   // Equal, the post-reset PSR
    m_.writeAddr24(GuestLowStorage::kTaskBlock + kTbRequestBlock, kRequestBlock);

    currentTaskBlock_ = GuestLowStorage::kTaskBlock;
    currentRequestBlock_ = kRequestBlock;

    // The IPL builds the initial block's ATR file the same way every other
    // request block gets one, so the block is not a special case: it owns a
    // translation file and carries the handle at rb+56..58.  The pool itself
    // is emptied first because a power-on constructs a fresh one.
    ptt_.reset();
    createTranslationFile(kRequestBlock, "csipl");

    // The closing post is a QUEUE INSERT: the task block goes on system
    // queue 40, the ready list, by its priority.  Low storage seeds headers
    // 37 and 39 with the block; 40 is this.
    readyQueueInsert(GuestLowStorage::kTaskBlock, 0xC7);

    uint16_t entry = m_.readHalf(kRequestBlock + RequestBlock::kOffIar);
    m_.msp.iar = entry;
    m_.msp.pactIar = m_.readByte(kRequestBlock + RequestBlock::kOffPiar);
    msp_->start();

    trace_.csp("task block {:03X} posted: rb at {:03X}, entry from rb+{} = {:04X}; MSP running",
               GuestLowStorage::kTaskBlock, kRequestBlock, RequestBlock::kOffIar, entry);
}

// ---- the supervisor call path ---------------------------------------------------

DispatchClass As36ControlStorageProcessor::classify(uint8_t rByte) const { return SvcTable::classify(rByte); }

bool As36ControlStorageProcessor::isImplemented(uint8_t rByte) const { return SvcTable::isImplemented(rByte); }

bool As36ControlStorageProcessor::svc(SvcRequest& req)
{
    // Each call starts with no refusal recorded, so whatever the stop
    // reports belongs to THIS call.
    lastRefusal_.clear();

    if (!SvcTable::isImplemented(req.r)) {
        refuse("SVC {:02X} is not an implemented R-byte (SvcTable): nuerr code 6", req.r);
        return false;
    }

    // Handlers reach the caller's blocks through the request.
    req.taskBlock = currentTaskBlock_;
    req.requestBlock = currentRequestBlock_;

    // THE REQUEST BLOCK IS THE REGISTER SAVE AREA.  Handlers read their
    // arguments out of it and write their results back to it, so the live
    // MSP registers have to be there before a handler runs and have to come
    // back afterwards.
    saveRegisters(currentRequestBlock_);
    // ... and the CALL itself, which is stamped into the same block: rb+22
    // the R-byte, rb+21 the Q-byte, rb+16..18 the inline parameters.
    stampRequest(currentRequestBlock_, req);
    bool wasInSvc = inSupervisorCall_;
    inSupervisorCall_ = true;
    bool ok = service(req);
    inSupervisorCall_ = wasInSvc;
    // A last-resort record at the dispatcher boundary, so every refused SVC
    // is self-diagnosing even when a handler forgot to say why.
    if (!ok && lastRefusal_.empty()) {
        refuse("SVC {:02X} handler returned false without a diagnostic: q={:02X}, inline={:02X}{:02X}{:02X}, "
               "task={:06X}, rb={:06X}, XR1={:06X}, XR2={:06X}, WR4..7={:04X}/{:04X}/{:04X}/{:04X}",
               req.r, req.q, req.inline1, req.inline2, req.inline3, req.taskBlock, req.requestBlock,
               RequestBlock::readXr1Field(m_, req.requestBlock), RequestBlock::readXr2Field(m_, req.requestBlock),
               RequestBlock::readWr(m_, req.requestBlock, 4), RequestBlock::readWr(m_, req.requestBlock, 5),
               RequestBlock::readWr(m_, req.requestBlock, 6), RequestBlock::readWr(m_, req.requestBlock, 7));
    }
    // SA21-9436 3-67: on completion the processor is started "either for the
    // task that issued the immediate SVC or for another more important task
    // that is ready (if task switching is not disabled)".  Nothing happens
    // unless a wait or a post asked for it.
    // The host action scheduler's turn is approximated here too: every
    // action SVC 0B queued is executed before the dispatcher runs, because
    // the dispatcher is itself one of those actions and the emulator has no
    // other point at which it yields to host work between guest instructions.
    wasInSvc = inSupervisorCall_;
    inSupervisorCall_ = true;
    drainActionScheduler(fmt::format("SVC {:02X}", req.r));
    dispatchIfRequested(fmt::format("SVC {:02X}", req.r));
    inSupervisorCall_ = wasInSvc;
    // A transfer switches request blocks mid-call, so the reload has to use
    // whatever is current NOW, not what was current on entry.
    restoreRegisters(currentRequestBlock_);
    return ok;
}

void As36ControlStorageProcessor::stampRequest(int rb, const SvcRequest& req)
{
    if (rb == 0) return;
    m_.writeByte(rb + RequestBlock::kOffOpcode, 0xF4);
    m_.writeByte(rb + RequestBlock::kOffRByte, req.r);
    m_.writeByte(rb + RequestBlock::kOffQByte, req.q);
    m_.writeByte(rb + RequestBlock::kOffInline1, req.inline1);
    m_.writeByte(rb + RequestBlock::kOffInline2, req.inline2);
    m_.writeByte(rb + RequestBlock::kOffInline3, req.inline3);
}

// Spill the MSP's registers into the request block: a prefix byte and a
// low halfword per index register, because an index register is 24 bits
// and its top byte is the PACT prefix for that addressing path.  WR4..WR7
// are part of the save area too: a call that returns a work register
// writes the block, and the guest must not read the stale live register.
void As36ControlStorageProcessor::saveRegisters(int rb)
{
    if (rb == 0) return;
    m_.writeByte(rb + RequestBlock::kOffPdir, m_.msp.pactDir);
    m_.writeByte(rb + RequestBlock::kOffXr1High, m_.msp.pactXr1);
    m_.writeHalf(rb + RequestBlock::kOffXr1Low, m_.msp.xr1);
    m_.writeByte(rb + RequestBlock::kOffXr2High, m_.msp.pactXr2);
    m_.writeHalf(rb + RequestBlock::kOffXr2Low, m_.msp.xr2);
    m_.writeByte(rb + RequestBlock::kOffPiar, m_.msp.pactIar);
    m_.writeByte(rb + RequestBlock::kOffPsr, m_.msp.psr());
    m_.writeHalf(rb + RequestBlock::kOffIar, m_.msp.iar);
    m_.writeHalf(rb + RequestBlock::kOffArr, m_.msp.arr);
    for (int n = 4; n <= 7; n++) RequestBlock::writeWr(m_, rb, n, m_.msp.wr[n]);
    // rb+19 bit 0 is the task's privilege byte, the one nusvc tests against
    // its per-R-byte table and nudyprv (SVC 0A) clears.  It is the saved
    // image of PMR bit 7: an SSP program drops privilege with LPMR, asks
    // for it back with SVC 0A and then issues a privileged LPMR, so the bit
    // has to travel through the request block in both directions.
    m_.writeByte(rb + RequestBlock::kOffPrivilege,
                 static_cast<uint8_t>((m_.readByte(rb + RequestBlock::kOffPrivilege) & ~machine::MspRegisters::kPmrNotPrivileged) |
                                      (m_.msp.pmr() & machine::MspRegisters::kPmrNotPrivileged)));
}

void As36ControlStorageProcessor::restoreRegisters(int rb)
{
    if (rb == 0) return;
    m_.msp.pactDir = m_.readByte(rb + RequestBlock::kOffPdir);
    m_.msp.pactXr1 = m_.readByte(rb + RequestBlock::kOffXr1High);
    m_.msp.xr1 = m_.readHalf(rb + RequestBlock::kOffXr1Low);
    m_.msp.pactXr2 = m_.readByte(rb + RequestBlock::kOffXr2High);
    m_.msp.xr2 = m_.readHalf(rb + RequestBlock::kOffXr2Low);
    m_.msp.pactIar = m_.readByte(rb + RequestBlock::kOffPiar);
    m_.msp.loadPsr(m_.readByte(rb + RequestBlock::kOffPsr));
    m_.msp.iar = m_.readHalf(rb + RequestBlock::kOffIar);
    m_.msp.arr = m_.readHalf(rb + RequestBlock::kOffArr);
    for (int n = 4; n <= 7; n++) m_.msp.wr[n] = RequestBlock::readWr(m_, rb, n);
    m_.msp.setPmr(static_cast<uint8_t>((m_.msp.pmr() & ~machine::MspRegisters::kPmrNotPrivileged) |
                                       (m_.readByte(rb + RequestBlock::kOffPrivilege) & machine::MspRegisters::kPmrNotPrivileged)));
}

// NuEmul::nudspchA c180def0..c180df30 distinguishes a genuine invalid-opcode
// check from an interpreter burst exit with request-block byte +0x30 bit 0.
// BASIC deliberately executes unassigned 00 encodings as burst boundaries;
// treating every such escape as a check stops in BLGTE's generated code.
bool As36ControlStorageProcessor::consumeInvalidOpcodeCheck(uint16_t resumeIar)
{
    if (currentRequestBlock_ == 0) return true;
    // emmsp's common escape at c1841974 stores the opcode-advanced IAR and
    // the complete register image before returning.  Publishing only the
    // live IAR lets the next native dispatch restore a stale continuation.
    m_.msp.iar = resumeIar;
    saveRegisters(currentRequestBlock_);
    const int at = currentRequestBlock_ + RequestBlock::kOffTransferFlags;
    const uint8_t flags = m_.readByte(at);
    if ((flags & 0x01) == 0) return false;
    m_.writeByte(at, static_cast<uint8_t>(flags & ~0x01));
    return true;
}

// F5 leaves the MSP interpreter through the same state-save path as F4/FC.
// Advanced/36 SLIC's NuEmul::nuecs accepts Q=01 for NuFortran and Q=02,R=00
// for NuBasic.  Keep that boundary explicit: an XFER is a synchronous CSP
// call, never a permanent machine halt or a scheduler event.  The native
// language assists are deliberately not faked here; resuming without running
// one leaves its private stack and instruction pointer stale and corrupts the
// application a few instructions later.
bool As36ControlStorageProcessor::extendedControlStore(uint8_t q, uint8_t r, uint16_t sourceIar)
{
    lastRefusal_.clear();
    saveRegisters(currentRequestBlock_);
    if (currentRequestBlock_ != 0) {
        m_.writeByte(currentRequestBlock_ + RequestBlock::kOffOpcode, 0xF5);
        m_.writeByte(currentRequestBlock_ + RequestBlock::kOffQByte, q);
        m_.writeByte(currentRequestBlock_ + RequestBlock::kOffRByte, r);
    }

    if (q == 0x02 && r == 0x00)
        return refuse("XFER 02,00 at {:04X}: the NuBasic extended-control-storage assist is not implemented", sourceIar);
    if (q == 0x01)
        return refuse("XFER 01,{:02X} at {:04X}: the NuFortran extended-control-storage assist is not implemented", r, sourceIar);
    return refuse("XFER {:02X},{:02X} at {:04X}: invalid extended-control-storage function", q, r, sourceIar);
}

bool As36ControlStorageProcessor::service(SvcRequest& req)
{
    switch (req.r) {
        case 0x09:   // Sense Data Switches - no switches on an A/36
            m_.writeHalf(req.requestBlock + RequestBlock::kOffXr1Low, 0);
            return true;

        case 0x0A: {   // Set Task Privileged: only if the task block permits it (tb+48 bit 3), clear bit 0 of rb+19.
            if ((m_.readByte(req.taskBlock + 48) & 0x10) == 0) return true;
            uint8_t p = m_.readByte(req.requestBlock + RequestBlock::kOffPrivilege);
            m_.writeByte(req.requestBlock + RequestBlock::kOffPrivilege, static_cast<uint8_t>(p & ~0x01));
            return true;
        }

        case 0x0E:   // Queue/Dequeue
            return queueDequeue(req);

        case 0x0F:   // System Control Block Access
            return systemControlBlockAccess(req);

        case 0x06:   // Assign
            return assign(req);

        case 0x07:   // Free Assigned Areas
            return freeAssigned(req);

        case 0x2C:   // Translated Assign
        case 0x2D:   // Translated Free
            return translatedAssignOrFree(req);

        case 0x2F:   // MAP
            return map(req);

        case 0x51:   // Task Work Area Accesses
            return taskWorkAreaAccess(req);

        case 0x50:   // Control Storage Transient Scheduler
            transients_.schedule(req.inline1, req.inline2, req.inline3, RequestBlock::readXr1Field(m_, req.requestBlock),
                                 RequestBlock::readXr2Field(m_, req.requestBlock), req.taskBlock, req.requestBlock);
            return true;

        case 0x18:   // Set Transient Area Not Busy
            transients_.setNotBusy();
            return true;

        case 0x4C: {   // Action Control Element Build and Queue
            int ace = aces_.buildAndQueue(req.requestBlock, req.taskBlock, req.q, req.inline1);
            if (ace == 0)
                return refuse("SVC 4C: nubldace could not build and queue an action control element (queue header {})",
                              req.inline1);
            return true;
        }

        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x48: {
            // Every delayed call becomes an action control element before
            // the device sees it.  The queue header number is not an inline
            // parameter on the device calls, so the request is built and
            // tracked without being chained to a header.
            int ace = aces_.allocate();
            if (ace != 0) {
                ActionControlElement::build(m_, ace, req.requestBlock, req.taskBlock, req.q);
                // The delayed device-SVC family shares Q bit 3 with SVC 4C:
                // completion belongs to the TB supplied in XR2, applied after
                // build has captured XR2.
                ActionControlElement::applyTaskAssociation(m_, ace, req.q);
                // The event type at ace+22 is set per SVC: 42 -> 0x2000, 43 ->
                // 0x1000.  The other device SVCs' types are not decoded and
                // keep the built-in zero.
                int eventType = req.r == 0x43 ? 0x1000 : req.r == 0x42 ? 0x2000 : 0;
                if (eventType != 0)
                    m_.writeHalf(ace + ActionControlElement::kOffEventType, static_cast<uint16_t>(eventType));
                trace_.ace("device SVC {:02X} -> ace {:04X}{}", req.r, ace,
                           eventType != 0 ? fmt::format(", event type {:04X}", eventType) : std::string());
            }
            return deviceSvc(req, ace);
        }

        case 0x00: return generalWait(req);
        case 0x17: return asynchronousTaskWait(req);
        case 0x1E: return taskWait(req);
        case 0x25: return asynchronousTaskReadyCheck(req);
        case 0x30: return quickLock(req);
        case 0x20: return specificResourceDequeue(req);
        case 0x21: return resourceEnqueueDequeue(req);
        case 0x2E: return timeOfDay(req);
        case 0x31: return attachTask(req);
        case 0x32: return detachTask(req);
        case 0x01: return generalPost(req);
        case 0x02: return eventWait(req);
        case 0x03: return eventPost(req);
        case 0x08: return incrementSystemEventCounters(req);
        case 0x0B: return postActionControllerStatusWord(req);
        case 0x19: return postActionControlElement(req);
        case 0x1A: return logTraceInformation(req);
        case 0x1B: return scanSystemQueue(req);
        case 0x1D: return taskPost(req);
        case 0x23: return testAndSet(req);
        case 0x24: return taskBlockPriorityQueue(req);
        case 0x2B: return postTaskByTaskId(req);
        case 0x04: return transferControlById(req);
        case 0x05: return freeSecondRequestBlock(req);
        case 0x0C: return fastTransfer(req);
        case 0x0D: return fastExit(req);
        case 0x10: return transferControlByAddress(req);
        case 0x14: return arrayTransfer(req);
        case 0x11: return mainStorageExit(req);
        case 0x22: return dumpTask(req);
        case 0x12: return getPage(req);
        case 0x13: return maintainUserAreaPages(req);
        case 0x26: return preparePrintBuffer(req);
        case 0x36: return smfc(req);
        case 0x33: return taskWorkAreaAllocate(req);
        case 0x34: return taskWorkAreaFree(req);
        case 0x35: return workSpaceMaintenance(req);
        case 0x52: return mainStorageRelocatingLoader(req);

        default:
            return refuse("SVC {:02X} ({}) is dispatched but not yet implemented - there is no handler for this R-byte",
                          req.r, svcClassName(SvcTable::classify(req.r)));
    }
}

// Set the caller's condition: clear the low three bits, then OR.  The mask
// is 0xF8.
void As36ControlStorageProcessor::setCondition(SvcRequest& req, uint8_t bits)
{
    uint8_t psr = static_cast<uint8_t>(m_.readByte(req.requestBlock + RequestBlock::kOffPsr) & 0xF8);
    m_.writeByte(req.requestBlock + RequestBlock::kOffPsr, static_cast<uint8_t>(psr | bits));
}

// ---- SVC 0F --------------------------------------------------------------------

// "This instruction allows a main storage user to access 2 or 3 bytes from a
// control storage direct area.  This routine also allows a main storage user
// to pick up 3 bytes from a system queue header in main storage."  Inline 1
// bit 0x02 SET selects the queue header (clear, a direct area); bit 0x80
// selects XR1 when set; bit 0x01 writes a direct area; bit 0x08 transfers
// the high byte too.  The queue header table is at guest 0xB00 + 4n and the
// 3-byte header at 0xB03 + 4n.
bool As36ControlStorageProcessor::systemControlBlockAccess(SvcRequest& req)
{
    constexpr uint8_t kQueueHeader = 0x02;
    constexpr uint8_t kIntoXr1 = 0x80;
    constexpr uint8_t kWrite = 0x01;
    constexpr uint8_t kThreeBytes = 0x08;

    bool xr1 = (req.inline1 & kIntoXr1) != 0;

    if ((req.inline1 & kQueueHeader) != 0) {
        int header = GuestLowStorage::kQueueHeaderTable + req.inline2 * 4;
        int value = m_.readAddr24(header + 1);   // the word's low three bytes
        writeIndexRegister(req.requestBlock, xr1, value, true);
        trace_.csp("SVC 0F: queue header {} at {:04X} -> {} = {:06X}", req.inline2, header + 1, xr1 ? "XR1" : "XR2",
                   value);
        return true;
    }

    int word = DirectArea::word(req.inline1, req.inline2);
    if (!DirectArea::inRange(word)) {
        trace_.csp("SVC 0F: direct area word {} (area {}, displacement {:02X}) is outside nuscb's own range {}..{}; it "
                   "raises nuerr code 79",
                   word, word >> 8, word & 0xFF, DirectArea::kFirstWord, DirectArea::kLastWord);
        return false;
    }

    if ((req.inline1 & kWrite) != 0) {
        int value = xr1 ? RequestBlock::readXr1Field(m_, req.requestBlock) : RequestBlock::readXr2Field(m_, req.requestBlock);
        directArea_.write(word, value);
        trace_.csp("SVC 0F: {} = {:06X} -> direct area word {} (area {}, displacement {:02X}) - {}", xr1 ? "XR1" : "XR2",
                   value, word, word >> 8, word & 0xFF, DirectArea::describe(word));
        return true;
    }

    bool three = (req.inline1 & kThreeBytes) != 0;
    int read = directArea_.read(word);
    if (three) read |= directArea_.readHigh(word) << 16;
    writeIndexRegister(req.requestBlock, xr1, read, three);

    // Zero for a word nothing has written yet is said in the trace rather
    // than invented or refused.
    trace_.csp("SVC 0F: direct area word {} (area {}, displacement {:02X}) = {:06X} -> {}, {} byte(s) - {}{}", word,
               word >> 8, word & 0xFF, read, xr1 ? "XR1" : "XR2", three ? 3 : 2, DirectArea::describe(word),
               directArea_.wasWritten(word)
                   ? ""
                   : "; NOTHING HAS WRITTEN IT - its initial contents are a property of the NuEmul object and are not "
                     "established");
    return true;
}

// An index register is a prefix byte and a low halfword, far apart in the
// request block.  The halfword is written always and the prefix byte only
// when the caller asked for three bytes.
void As36ControlStorageProcessor::writeIndexRegister(int rb, bool xr1, int value, bool withHighByte)
{
    int low = xr1 ? RequestBlock::kOffXr1Low : RequestBlock::kOffXr2Low;
    int high = xr1 ? RequestBlock::kOffXr1High : RequestBlock::kOffXr2High;
    m_.writeHalf(rb + low, static_cast<uint16_t>(value));
    if (withHighByte) m_.writeByte(rb + high, static_cast<uint8_t>(value >> 16));
}

// ---- SVC 0E and the queue engine ---------------------------------------------

// SVC 0E, Queue/Dequeue.  Inline 1 the system queue header number; inline 2
// the displacement of the chaining field's RIGHT byte; inline 3 bit 0x80
// priority, 0x40 system request, 0x20 dequeue, 0x10 LIFO, 0x0F the priority
// field's displacement; XR1 the control block; XR2 the queue header for
// non-system requests (right byte).  Both addresses are REAL: the high
// byte's 0x80 task-translated bit is dropped, which is why SA21-9436 says
// the operand "must be a real address" rather than checking one.
bool As36ControlStorageProcessor::queueDequeue(SvcRequest& req)
{
    constexpr uint8_t kSystemRequest = 0x40, kDequeue = 0x20;
    constexpr int kRealAddressMask = 0x7FFFFF;

    int block = RequestBlock::readXr1Field(m_, req.requestBlock) & kRealAddressMask;
    uint8_t flags = req.inline3;

    int header = (flags & kSystemRequest) != 0 ? GuestLowStorage::kQueueHeaderTable + 4 * req.inline1 + 3
                                                : RequestBlock::readXr2Field(m_, req.requestBlock) & kRealAddressMask;
    int headerField = header - 2;
    int chainField = block + req.inline2 - 2;

    trace_.csp("SVC 0E: {} block {:04X} {} queue header {} at {:04X}, chain at block+{}, flags {:02X}",
               (flags & kDequeue) != 0 ? "dequeue" : "queue", block, (flags & kDequeue) != 0 ? "from" : "on",
               (flags & kSystemRequest) != 0 ? std::to_string(req.inline1) : std::string("(XR2)"), headerField,
               req.inline2 - 2, flags);

    if (block == 0 || chainField < 0) {
        trace_.csp("SVC 0E: control block {:04X} with chain displacement {} is not addressable", block, req.inline2);
        return false;
    }

    bool ok = (flags & kDequeue) != 0 ? dequeueBlock(headerField, block, chainField)
                                      : queueBlock(headerField, block, chainField, flags);

    // Clear the PSR's low bits, then Equal for success and High for "already
    // there" / "not found", written to rb+23 because the request block IS
    // the register save area.
    setCondition(req, ok ? kPsrEqual : kPsrHigh);
    return true;
}

// One queue or dequeue in the internal form: the header named by the FIRST
// byte of its 3-byte value, the chain field named by its LAST byte, and the
// flags byte SVC 0E calls inline parameter 3.
bool As36ControlStorageProcessor::queueOperation(int headerField, int block, int chainLastByte, uint8_t flags)
{
    constexpr uint8_t kDequeue = 0x20;
    int chainField = block + chainLastByte - 2;
    return (flags & kDequeue) != 0 ? dequeueBlock(headerField, block, chainField)
                                   : queueBlock(headerField, block, chainField, flags);
}

// Queue FIFO, LIFO or by priority.  The walk does double duty: it refuses to
// queue a block that is already on the chain, and for a priority request it
// freezes the insertion point at the first element the new block outranks
// while still scanning the rest for a duplicate.  SA21-9436 3-86: FIFO by
// priority lands after its equals (strict outranking), LIFO by priority
// lands ahead of them (inclusive).  Absolute head insert is the plain LIFO
// case only; a priority request that outranks nothing falls to the tail.
bool As36ControlStorageProcessor::queueBlock(int headerField, int block, int chainField, uint8_t flags)
{
    constexpr uint8_t kPriority = 0x80, kLifo = 0x10;
    bool priorityReq = (flags & kPriority) != 0;
    bool lifo = (flags & kLifo) != 0;
    int priorityDisplacement = flags & 0x0F;
    uint8_t priority = m_.readByte(block + priorityDisplacement);

    int prevField = headerField;   // the field that points at `at`
    int at = m_.readAddr24(headerField);
    int insertBeforeField = 0;     // the field to relink; 0 = none found

    for (int steps = 0; at != 0; steps++) {
        if (at == block) {
            trace_.csp("SVC 0E: block {:04X} is already on this queue", block);
            return false;
        }
        if (!chainStepValid(at, steps, headerField)) return false;

        if (priorityReq && insertBeforeField == 0) {
            uint8_t elementPriority = m_.readByte(at + priorityDisplacement);
            if (elementPriority < priority || (lifo && elementPriority == priority)) insertBeforeField = prevField;
        }

        prevField = at + (chainField - block);
        at = m_.readAddr24(prevField);
    }

    if (insertBeforeField != 0) {
        m_.writeAddr24(chainField, m_.readAddr24(insertBeforeField));
        m_.writeAddr24(insertBeforeField, block);
        trace_.csp("SVC 0E: block {:04X} queued by priority {:02X} ({})", block, priority,
                   lifo ? "LIFO, front of equals" : "FIFO, after equals");
        return true;
    }

    if (lifo && !priorityReq) {
        m_.writeAddr24(chainField, m_.readAddr24(headerField));
        m_.writeAddr24(headerField, block);
        trace_.csp("SVC 0E: block {:04X} queued LIFO", block);
        return true;
    }

    m_.writeAddr24(chainField, 0);
    m_.writeAddr24(prevField, block);
    trace_.csp("SVC 0E: block {:04X} queued FIFO", block);
    return true;
}

// Dequeue removes the NAMED block rather than the head: each element is
// compared against XR1 and the one that matches is unlinked.
bool As36ControlStorageProcessor::dequeueBlock(int headerField, int block, int chainField)
{
    int prevField = headerField;
    int at = m_.readAddr24(headerField);

    for (int steps = 0; at != 0; steps++) {
        if (at == block) {
            m_.writeAddr24(prevField, m_.readAddr24(chainField));
            m_.writeAddr24(chainField, 0);
            trace_.csp("SVC 0E: block {:04X} dequeued", block);
            return true;
        }
        if (!chainStepValid(at, steps, headerField)) return false;
        prevField = at + (chainField - block);
        at = m_.readAddr24(prevField);
    }

    trace_.csp("SVC 0E: block {:04X} is not on this queue", block);
    return false;
}

// Every walk is guarded two ways: the step count is capped, and each chain
// pointer is range-checked against main storage, which is the honest bound
// on a machine whose heap placement is policy.
bool As36ControlStorageProcessor::chainStepValid(int at, int steps, int headerField)
{
    if (steps < kChainWalkLimit && at >= 0 && at + 3 <= m_.backingBytes()) return true;
    trace_.csp("SVC 0E: chain from {:04X} is corrupt - {} at step {}", headerField,
               steps >= kChainWalkLimit ? std::string("no end after 100000 elements")
                                        : fmt::format("{:06X} is outside storage", at),
               steps);
    return false;
}

// ---- SVC 06 and 07 --------------------------------------------------------------

// SVC 06, Assign.  XR1 in is a byte length (a two-byte register; the PACT
// prefix beside it is not part of the count); XR1 out is the guest address
// of the assigned area, or zero when there is no space and Q bit 7 is off.
// Areas come in 16-byte multiples on 16-byte boundaries.  The length is
// validated first, for the chained and unchained arms alike: zero, more than
// 2K with a wait (SA21-9436 3-78: "A wait request to assign more than 2K
// bytes of space is invalid"), or more than 0xFFF0 without one, and each is
// the task-terminating error path.
bool As36ControlStorageProcessor::assign(SvcRequest& req)
{
    constexpr uint8_t kWaitForSpace = 0x01;       // Q bit 7
    constexpr uint8_t kChainToTaskBlock = 0x04;   // Q bit 5

    int length = RequestBlock::readXr1Count(m_, req.requestBlock);

    constexpr int kMaxWaitLength = 0x0800;
    constexpr int kMaxLength = 0xFFF0;
    int lengthCeiling = (req.q & kWaitForSpace) != 0 ? kMaxWaitLength : kMaxLength;
    if (length == 0 || length > lengthCeiling) {
        trace_.csp("SVC 06: length {} is invalid for a {} request (allowed 1..{:04X}) - nuasgnms calls nuersvc (c18e29fc "
                   "/ c18e2a4c / c18e2a70), the task-terminating error path",
                   length, (req.q & kWaitForSpace) != 0 ? "wait" : "no-wait", lengthCeiling);
        return false;
    }

    if ((req.q & kChainToTaskBlock) != 0) {
        // The chained arm: "the LAST 5 bytes assigned are then used to
        // maintain the queue", a 3-byte chain link and a 2-byte cell whose
        // content is INFERRED (the rounded total size) and traced as such.
        // The queue element is the last byte of the link and the anchor is
        // the task block's +49..51, so differently-sized areas share one
        // queue.
        constexpr uint8_t kChainToXr2Block = 0x20;   // Q bit 2; bit 5 must also be on
        int total = GuestHeap::roundedSize(length + 5);
        int chained = heap_.allocate(total);
        if (chained == 0) {
            trace_.csp("SVC 06: no space for {}+5 bytes (chained)", length);
            RequestBlock::writeXr1(m_, req.requestBlock, 0);
            return true;
        }
        int anchorTb = (req.q & kChainToXr2Block) != 0 ? RequestBlock::readXr2Field(m_, req.requestBlock) : req.taskBlock;
        int queueElement = chained + total - 3;
        queueOperation(anchorTb + TaskBlock::kOffMeasurementBlock, queueElement, 0, 0);
        m_.writeHalf(chained + total - 2, static_cast<uint16_t>(total));
        trace_.csp("SVC 06: assign {}+5 -> {} bytes at guest {:04X}, chained on tb {:04X}+49..51 as queue element {:04X} "
                   "(nuasgnms c18e2b78..c18e2bb4); the trailing halfword = total size is INFERRED, not decoded",
                   length, total, chained, anchorTb, queueElement);
        RequestBlock::writeXr1(m_, req.requestBlock, chained);
        return true;
    }

    int at = heap_.allocate(length);
    if (at == 0 && (req.q & kWaitForSpace) != 0) {
        trace_.csp("SVC 06: no space for {} bytes and Q bit 7 asks to wait; there is no dispatcher to wait on", length);
        return false;
    }

    m_.writeByte(req.requestBlock + RequestBlock::kOffXr1High, static_cast<uint8_t>(at >> 16));
    m_.writeHalf(req.requestBlock + RequestBlock::kOffXr1Low, static_cast<uint16_t>(at));
    trace_.csp("SVC 06: assign {} bytes -> XR1 = {:06X}", length, at);
    return true;
}

// SVC 07, Free Assigned Areas: XR1 the area, WR6 its length.  The chained
// arm is the inverse of Assign's; the plain arm rounds the count to the
// 16-byte granularity (allocation-class rounding would extend a partial free
// into the allocation beside it).
bool As36ControlStorageProcessor::freeAssigned(SvcRequest& req)
{
    constexpr uint8_t kQueueToTaskBlock = 0x04;   // Q bit 5
    constexpr uint8_t kQueueToXr2Block = 0x20;    // Q bit 2

    int at = RequestBlock::readXr1Field(m_, req.requestBlock);
    int length = RequestBlock::readWr(m_, req.requestBlock, 6);

    if ((req.q & kQueueToTaskBlock) != 0) {
        int total = GuestHeap::roundedSize(length + 5);
        int anchorTb = (req.q & kQueueToXr2Block) != 0 ? RequestBlock::readXr2Field(m_, req.requestBlock) : req.taskBlock;
        int queueElement = at + total - 3;
        queueOperation(anchorTb + TaskBlock::kOffMeasurementBlock, queueElement, 0, ControlBlock::kQueueDequeue);
        heap_.free(at, total);
        trace_.csp("SVC 07: free {}+5 -> {} bytes at {:06X}, dequeued from tb {:04X}+49..51 as queue element {:06X} "
                   "(nufreems c18e2e58, the undo of nuasgnms' chain)",
                   length, total, at, anchorTb, queueElement);
        return true;
    }

    int rounded = (length + GuestHeap::kGranularity - 1) & ~(GuestHeap::kGranularity - 1);
    heap_.free(at, rounded, "SVC 07 plain free");
    trace_.csp("SVC 07: free {} -> {} bytes at {:06X}", length, rounded, at);
    return true;
}

// ---- the device path ------------------------------------------------------------

bool As36ControlStorageProcessor::deviceSvc(SvcRequest& req, int ace)
{
    // Capture the submitted block and command before dispatch.
    int submittedField = RequestBlock::readXr1Field(m_, req.requestBlock);
    int submittedBlock;
    if (!m_.resolveGuest24(submittedField, false, submittedBlock)) submittedBlock = 0;
    int submittedCommand = submittedBlock != 0 ? devices::WorkStationIob::command(m_, submittedBlock) : -1;

    bool ok = devices_.deviceSvc(req);
    if (!ok)
        refuse("SVC {:02X}: the device set refused the request (DeviceSvc returned false; IOB {:06X}, command {:02X}, ace "
               "{:04X})",
               req.r, submittedBlock, submittedCommand, ace);
    if (ace != 0 && submittedBlock != 0 && devices_.isPending(submittedBlock)) {
        pendingDeviceAces_[submittedBlock] = ace;
        trace_.ace("device SVC {:02X}: retained ace {:04X} with pending IOB {:06X}; no ECM post or task completion yet",
                   req.r, ace, submittedBlock);
        // If action 0 activated the native display before SSP issued its
        // unit-FF Invite, the invite's immediate scan consumes that
        // already-pending activation status here.  The response byte does
        // not complete or release this Invite IOB or its element.
        tryDeliverAction0ActivationStatus("SVC 43 unit-FF Invite immediate scan", false);
        return ok;
    }
    bool cnfwsPowerOn = ok && req.r == 0x43 && submittedBlock != 0 &&
                        submittedCommand == devices::WorkStationIob::kCmdConfigureNewWorkStations &&
                        devices_.workStations().lastConfigureIncludedZeroAddress();

    // The ACE's event control mask IS the IOB (ace+13 is the caller's XR1),
    // so posting here rewrites the byte the device has just written.  Post
    // the DEVICE's completion code, never the dispatcher's opinion of whether
    // the call was answered: phase 1 has branches for completion 43 as well
    // as 40.
    if (ace != 0) {
        int ecmField = m_.readAddr24(ace + ActionControlElement::kOffXr1);
        int ecm;
        if (!m_.resolveGuest24(ecmField, false, ecm)) ecm = 0;
        aces_.post(ace, ecm != 0 && Ecm::isComplete(m_, ecm) ? m_.readByte(ecm + Ecm::kOffCompletion) & 0x0F
                                                             : (ok ? 0 : 4));

        // A delayed device request completes through its ACE even when the
        // requester is still running.  completeToTask always puts the element
        // on tb+45..47; postTaskCheck then either wakes an existing event wait
        // or deliberately leaves it there for the task's next SVC 02.  The
        // latter is load-bearing for SSP's spool writer: it issues SVC 42 and
        // only then enters a multiple-event wait, which cannot poll the IOB's
        // completed ECM directly.
        int target = m_.readAddr24(ace + ActionControlElement::kOffTaskBlock);
        if ((m_.readByte(ace + ActionControlElement::kOffFlags) & ActionControlElement::kFlagsBase) != 0 &&
            TaskBlock::isTaskBlock(m_, target))
            completeToTask(ace, 0, "device event post");
        else
            aces_.release(ace);
    }
    // The configure command's success tail is performed only AFTER the
    // controller has completed the request: with unit zero in the accepted
    // list the controller stores the power-on response through the unit
    // block supplied as the command's IOB and raises the internal condition
    // of the IPL/command task.  This is the legitimate power-on-aid producer.
    if (cnfwsPowerOn && cnfwsPowerOnAidExperiment) {
        int tub = submittedBlock;
        if (tub != 0) {
            int configuredTub = resolveConfiguredTubByUnit(m_.readByte(tub + 12));
            if (workStationDiagnosticObserver)
                workStationDiagnosticObserver("cnfws-f7-target", m_.readByte(tub + 12), tub, configuredTub);
            if (cnfwsPowerOnAidConfiguredTargetExperiment) {
                int configured = configuredTub;
                if (configured != 0) {
                    trace_.csp("SVC 43 cnfws experiment: redirect bootstrap TU {:06X} power-on aid to configured TU {:06X}",
                               tub, configured);
                    tub = configured;
                }
            }
            if (deferCnfwsPowerOnAid) {
                // A real twinax device cannot answer instantly; deferring to
                // the next all-tasks-waiting boundary is a deterministic
                // stand-in for that latency, with no host clock.
                pendingCnfwsPowerOnTub_ = tub;
                trace_.csp("SVC 43 cnfws success tail: power-on AID for TU {:06X} deferred to the next idle boundary "
                           "(device-latency experiment)",
                           tub);
            } else {
                deliverWorkStationControllerFunction(tub, 0xF7, true, true, "SVC 43 cnfws success tail", false);
            }
        }
    }
    return ok;
}

// ---- traced disk access ----------------------------------------------------------

void As36ControlStorageProcessor::diskRead(long long sector, uint8_t* dst, const std::string& what)
{
    trace_.diskIo("csp read  sector {} for {}", sector, what);
    disk_.readSector(sector, dst);
}

void As36ControlStorageProcessor::diskWrite(long long sector, const uint8_t* src, const std::string& what)
{
    trace_.diskIo("csp write sector {} for {}", sector, what);
    disk_.writeSector(sector, src);
}

// ---- the ATR file, which belongs to the request block --------------------------------

// Every request block that exists gets a translation file when it is built:
// one is taken from the pool, stamped with the request block, and the
// handle is stored into rb+56..58.  "This block has no ATR file" is not a
// state the machine has.
void As36ControlStorageProcessor::createTranslationFile(int rb, const std::string& call)
{
    NuPtt* p = ptt_.allocate(rb);
    m_.writeAddr24(rb + RequestBlock::kOffTranslationHandle, p->handle());
    trace_.csp("{}: request block {:04X} owns ATR file {:04X} (rb+56..58; nuprbbld c18a62d8, csipl c1831968)", call, rb,
               p->handle());
}

void As36ControlStorageProcessor::releaseTranslationFile(int rb, const std::string& call)
{
    int handle = m_.readAddr24(rb + RequestBlock::kOffTranslationHandle);
    if (ptt_.free(handle, rb))
        trace_.csp("{}: ATR file {:04X} released by request block {:04X} (nuprbf2 c18a5c2c)", call, handle, rb);
    else
        trace_.csp("{}: request block {:04X} has no ATR file of its own to release - rb+56..58 = {:04X} names one owned "
                   "by another block, or none. Nothing freed (nuprbf2 c18a5c2c)",
                   call, rb, handle);
}

// The "A5 PATR" operation (SA21-9436 1-29, "Fast task switch for ATRs"):
// make a request block's ATR file the LIVE one.  The file is REPOINTED on
// every change of current block, never rebuilt; here the file is real
// registers, so the image is copied into them.  The mismatch arm has no
// decoded outcome: all 32 registers are left protected so a translated
// access stops rather than using another program's mapping.
void As36ControlStorageProcessor::selectTranslationFile(int rb, const std::string& call)
{
    int b = machine::MachineState::kAtrTaskGroup0;
    int handle = m_.readAddr24(rb + RequestBlock::kOffTranslationHandle);
    NuPtt* p = ptt_.owned(handle, rb);
    if (p == nullptr) {
        for (int i = 0; i < kAtrCount; i++) m_.atr[b + i] = machine::MachineState::kAtrProtect;
        trace_.csp("{}: request block {:04X} does not own ATR file {:04X} - NuPtt[0x128] names another block, so SLIC "
                   "would make the live file NULL (NuEmul[0x1170] = 0). No decoded behaviour follows that; all 32 "
                   "registers are left protected so a translated access stops rather than using someone else's mapping",
                   call, rb, handle);
        return;
    }
    for (int i = 0; i < kAtrCount; i++) m_.atr[b + i] = p->atr[i];
}

// ---- the dispatcher ------------------------------------------------------------------

// The ready list, keyed on tb+7: system queue 40, chain field ending at
// tb+35, with the caller's flags byte.
void As36ControlStorageProcessor::readyQueueInsert(int tb, uint8_t flags)
{
    queueOperation(GuestLowStorage::queueHeader(kTaskReadyQueue), tb, TaskBlock::kChainLastQueue40, flags);
}

// dispatchIfRequested, raiseStorageProtection and controlStorageTerminate
// are defined with the dispatcher in As36Dispatch.cpp.

// ---- member attribution -------------------------------------------------------------

// The program block a task is currently executing: tb+65 is its live
// request block, and rb+41 is that frame's program block.
int As36ControlStorageProcessor::activeProgramBlock(int taskBlock) const
{
    if (taskBlock == 0) return 0;
    auto& m = const_cast<machine::MachineState&>(m_);
    int rb = m.readAddr24(taskBlock + kTbRequestBlock);
    if (rb == 0) return 0;
    return m.readAddr24(rb + RequestBlock::kOffProgramBlock);
}

bool As36ControlStorageProcessor::tryProgramBlockMember(int programBlock, int iar, LoadedMember& member,
                                                        int& offset) const
{
    offset = 0;
    if (programBlock == 0) return false;
    auto it = memberByProgramBlock_.find(programBlock);
    if (it == memberByProgramBlock_.end()) return false;
    member = it->second;
    offset = iar - member.logicalBase;
    return true;
}

bool As36ControlStorageProcessor::tryActiveMember(int taskBlock, int iar, LoadedMember& member, int& offset) const
{
    return tryProgramBlockMember(activeProgramBlock(taskBlock), iar, member, offset);
}

std::string As36ControlStorageProcessor::describeActiveMember(int taskBlock, int iar) const
{
    LoadedMember m;
    int off;
    if (!tryActiveMember(taskBlock, iar, m, off)) return std::string();
    return fmt::format("{}+{:04X} (extent {}, pb {:04X})", m.name, off, m.extentSector, activeProgramBlock(taskBlock));
}

// The physical machine removes power after #CCPW has completed shutdown and
// entered its final three-byte wait: JC condition-on 87 back by its own
// length (F1 87 03). On an emulator there is no power-control switch to
// satisfy that loop, so interpreting it forever is observably a hang.
//
// Restrict recognition to the loaded #CCPW member and to the exact self-loop.
// The panic dump which established this boundary had #CCPW+03E5 at logical
// 13E5, but the member-relative offset is deliberately not part of the test:
// it may move between SSP builds while the terminal instruction does not.
bool As36ControlStorageProcessor::detectSystemPowerOff()
{
    if (systemPowerOffRequested_ || msp_ == nullptr || msp_->stopped()) return systemPowerOffRequested_;

    LoadedMember member;
    int offset = 0;
    const uint16_t iar = m_.msp.iar;
    if (!tryActiveMember(currentTaskBlock_, iar, member, offset) || member.name != "#CCPW") return false;

    int physical = 0;
    if (!m_.resolve(iar, m_.msp.pactIar, machine::MachineState::kAtrTaskGroup0, false, physical)) return false;
    if (!m_.inRange(physical, 3) || m_.readByte(physical) != InstructionSet::kJumpBackwardOpcode ||
        m_.readByte(physical + 1) != 0x87 || m_.readByte(physical + 2) != 0x03)
        return false;

    systemPowerOffRequested_ = true;
    const std::string reason = fmt::format(
        "system powered off by SSP at #CCPW+{:04X} (final power-control wait F1 87 03)", offset);
    trace_.csp("{}", reason);
    msp_->halt(reason);
    return true;
}

// One-line description of the current task and the module running at the
// live IAR, for the M36-SRC state dump.
std::string As36ControlStorageProcessor::describeSrcState() const
{
    int tb = currentTaskBlock_;
    if (tb == 0) return "no dispatched task";
    auto& m = const_cast<machine::MachineState&>(m_);
    int id = m.readHalf(tb + TaskBlock::kOffTaskId);
    int rb = m.readAddr24(tb + TaskBlock::kOffRequestBlock);
    int pb = rb == 0 ? 0 : m.readAddr24(rb + RequestBlock::kOffProgramBlock);
    std::string member = describeActiveMember(tb, m_.msp.iar);
    return fmt::format("task {:04X} (id {:04X}), request block {:06X}, program block {:06X}{}", tb, id, rb, pb,
                       !member.empty() ? ", " + member : std::string(" (no member at IAR)"));
}

// Freeze the guest control blocks whose contents explain a processor check.
std::string As36ControlStorageProcessor::describeCheckState() const
{
    auto& m = const_cast<machine::MachineState&>(m_);
    std::string s;
    int tb = currentTaskBlock_;
    if (tb == 0) return "check-control-blocks: no current task\n";

    auto appendRange = [&](const std::string& label, int address, int length) {
        if (address < 0 || address >= m.backingBytes()) {
            s += fmt::format("{} {:06X}: outside main storage\n", label, address);
            return;
        }
        if (address + length > m.backingBytes()) length = m.backingBytes() - address;
        s += fmt::format("{} {:06X} ({} bytes)\n", label, address, length);
        for (int off = 0; off < length; off += 16) {
            s += fmt::format("  {:06X} ", address + off);
            for (int i = 0; i < 16 && off + i < length; i++) s += fmt::format(" {:02X}", m.readByte(address + off + i));
            s += "\n";
        }
    };

    appendRange("task-block", tb, 160);
    int rb = m.readAddr24(tb + TaskBlock::kOffRequestBlock);
    int depth = 0;
    while (rb != 0 && depth++ < 8) {
        int units = m.readByte(rb + RequestBlock::kOffLengthUnits);
        int bytes = std::max(64, std::min(units * GuestHeap::kGranularity, 4096));
        appendRange(depth == 1 ? "request-block" : "previous-request-block", rb, bytes);
        if (depth == 1) {
            int pb = m.readAddr24(rb + RequestBlock::kOffProgramBlock);
            if (pb != 0) appendRange("program-block", pb, 64);
        }
        rb = m.readAddr24(rb + RequestBlock::kOffPrevious);
    }

    int ws = m.readAddr24(tb + ControlBlock::kTaskWorkSpaceChain - 2);
    int guard = 0;
    while (ws != 0 && guard++ < 32) {
        uint16_t eye = m.readHalf(ws);
        int bytes = eye == GuestLowStorage::kEyeProgramBlock ? 64 : 48;
        appendRange("task-workspace", ws, bytes);
        ws = m.readAddr24(ws + ProgramBlock::kOffChainLink);
    }
    return s;
}

}  // namespace sim36::processors::controlstorage
