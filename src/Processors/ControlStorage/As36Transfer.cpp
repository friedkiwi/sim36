// The Advanced/36 control storage processor: transfer control, the request
// block stack, the loader and task termination.
//
// SVC 04/10/14 (transfer by identifier, by address, by array), SVC 11 (main
// storage exit), SVC 05 (free second request block), SVC 0C/0D (fast
// transfer and exit), SVC 22 (dump / terminate task), SVC 52 (the relocating
// loader), the program block build and readiness, and the task-root
// termination path with its dependency scan.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>

#include <fmt/format.h>

#include "Devices/WorkStationIob.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/MapParameterList.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Storage/Ebcdic.h"

namespace sim36::processors::controlstorage {

namespace {

// The library extent of the phase-2 job entry module whose asynchronous
// attach the dispatcher orders after the console's job pointer is linked.
constexpr long long kSvatJobEntryExtent = 99816;

const char* continuationName(As36ControlStorageProcessor::NativeTransferContinuation c)
{
    return c == As36ControlStorageProcessor::NativeTransferContinuation::NuabSlot1D ? "NuabSlot1D" : "NuptermSlot4";
}

}  // namespace

void As36ControlStorageProcessor::pushNativeTransferContinuation(int tb, NativeTransferContinuation kind)
{
    nativeTransferContinuations_[tb].push_back(kind);
}

// ---- SVC 10, 14 and 04 ----------------------------------------------------------

// SVC 10, Transfer Control by Address.  Inline 1-2 address a transfer
// control table entry: a 3-byte disk sequential sector address, a 1-byte
// length in sectors and an attribute byte.  Inline 3 is an entry point
// number.  The entry address is resolved against the CALLER'S instruction
// fetch prefix, as the manual requires.
bool As36ControlStorageProcessor::transferControlByAddress(SvcRequest& req)
{
    int entryLogical = (req.inline1 << 8) | req.inline2;
    uint8_t piar = m_.readByte(req.requestBlock + RequestBlock::kOffPiar);
    int entry;
    if (!m_.resolve(static_cast<uint16_t>(entryLogical), piar, machine::MachineState::kAtrTaskGroup0, false, entry))
        return refuse("SVC 10: table entry {:04X} resolved against piar {:02X} is not addressable - the page is "
                      "protected or unmapped",
                      entryLogical, piar);

    trace_.csp("SVC 10: table entry {:04X} resolved against piar {:02X} -> {:06X}", entryLogical, piar, entry);
    return transferControl(req, "SVC 10", entry, req.inline3);
}

// SVC 14, the Advanced/36 array transfer: inline 1-2 are a REAL 16-bit
// guest address of a four-byte descriptor whose byte 0 is the largest valid
// selector and bytes 1-3 name a five-byte transfer control table; inline 3
// is the selector.  The entry is table + 5 * selector, entered at point 0.
bool As36ControlStorageProcessor::arrayTransfer(SvcRequest& req)
{
    int descriptor = (req.inline1 << 8) | req.inline2;
    if (descriptor < 0 || descriptor + 3 >= m_.backingBytes())
        return refuse("SVC 14: nuaxfer descriptor {:04X} is outside main storage (inline {:02X} {:02X} {:02X})",
                      descriptor, req.inline1, req.inline2, req.inline3);

    uint8_t selector = req.inline3;
    uint8_t lastSelector = m_.readByte(descriptor);
    // Only selector > last is invalid: equality selects the last entry.
    if (lastSelector < selector)
        return refuse("SVC 14: nuaxfer descriptor {:04X} last selector {:02X} is below requested selector {:02X}; nuaxfer "
                      "raises nuersvc code 6 (c18a42ac..c18a42b8)",
                      descriptor, lastSelector, selector);

    int table = m_.readAddr24(descriptor + 1);
    long long entryLong = static_cast<long long>(table) + 5LL * selector;
    if (entryLong < 0 || entryLong + 4 >= m_.backingBytes())
        return refuse("SVC 14: nuaxfer descriptor {:04X} selects entry {:06X} (table {:06X} + 5 * {:02X}), outside main "
                      "storage",
                      descriptor, entryLong, table, selector);
    int entry = static_cast<int>(entryLong);

    trace_.csp("SVC 14: nuaxfer descriptor {:04X} last selector {:02X}, table {:06X} + 5 * selector {:02X} -> entry "
               "{:06X}; nup1000 entry point 0 (c18a42cc..c18a42f0)",
               descriptor, lastSelector, table, selector, entry);
    return transferControl(req, "SVC 14 nuaxfer", entry, 0);
}

// SVC 04, Transfer Control by ID: the same operation as SVC 10 reached by
// identifier.  The system transfer control table is an array of the same
// five-byte entries, indexed by the identifier, with 80 entries; identifier
// 4 is reserved and terminates the task; the entry point is always 0.
bool As36ControlStorageProcessor::transferControlById(SvcRequest& req)
{
    uint8_t id = req.inline1;

    if (id == kTransferControlTerminate) {
        trace_.csp("SVC 04: identifier 4 is nupxfer's reserved 'terminate this task' (c18a4348 -> nupterm)");
        return terminateTaskRoot(req);
    }

    if (id > kTransferControlLastId) {
        trace_.csp("SVC 04: identifier {} is past the transfer control table's last entry ({}); nupxfer raises nuersvc "
                   "code 6",
                   id, kTransferControlLastId);
        return false;
    }

    int table = directArea_.read(DirectArea::kTransferControlTable);
    if (table == 0) {
        trace_.csp("SVC 04: the system transfer control table's base is zero. nupxfer takes it from NuEmul[0x448], which "
                   "is control storage direct area word {} (area {}, displacement {:02X}) and is written by SVC 0F; "
                   "nothing has written it yet",
                   DirectArea::kTransferControlTable, DirectArea::kTransferControlTable >> 8,
                   DirectArea::kTransferControlTable & 0xFF);
        return false;
    }

    int entry = table + kTransferControlEntryBytes * id;
    trace_.csp("SVC 04: identifier {} -> table {:04X} + 5 x {} = entry {:04X}", id, table, id, entry);
    return transferControl(req, "SVC 04", entry, 0);
}

// Monitor seam for the supervisor-only slot-4 termination continuation,
// entered on the current task's real register save.  Nothing is
// manufactured.
bool As36ControlStorageProcessor::enterTerminationContinuationExperiment()
{
    int tb = currentTaskBlock_;
    int rb = currentRequestBlock_;
    int table = directArea_.read(DirectArea::kTransferControlTable);
    if (!TaskBlock::isTaskBlock(m_, tb) || rb == 0 || table == 0) return false;

    SvcRequest req;
    req.r = 0x04;
    req.q = 0x01;
    req.requestBlock = rb;
    req.taskBlock = tb;
    stampRequest(rb, req);
    int entry = table + kTransferControlEntryBytes * kTransferControlTerminate;
    trace_.csp("EXPERIMENT xferterm: nupterm internal continuation -> transfer table {:04X} + 5 x 4 = {:04X}; current "
               "task {:04X}, request block {:04X}",
               table, entry, tb, rb);
    bool ok = transferControl(req, "EXPERIMENT xferterm slot 4", entry, 0);
    restoreRegisters(currentRequestBlock_);
    return ok;
}

// Shared by SVC 04 and SVC 10: validate the entry, find or build the
// program block for the module it names, and transfer.
bool As36ControlStorageProcessor::transferControl(SvcRequest& req, const std::string& call, int entry, int entryPoint)
{
    constexpr uint8_t kReturnToCaller = 0x01;   // Q bit 7
    constexpr uint8_t kAsynchronous = 0x20;     // Q bit 2

    int sector = m_.readAddr24(entry);           // disk sequential sector, 1-based
    int sectors = m_.readByte(entry + 3);        // length, in sectors
    uint8_t attribute = m_.readByte(entry + 4);  // flags

    trace_.csp("{}: entry {:04X} = sector {} (0-based {}), {} sectors, +4={:02X}; entry point {}, Q={:02X}{}{}", call,
               entry, sector, sector - 1, sectors, attribute, entryPoint, req.q,
               (req.q & kReturnToCaller) != 0 ? " return-to-caller" : " no-return",
               (req.q & kAsynchronous) != 0 ? " async" : "");

    // The privilege gate: an unprivileged caller may not transfer to an
    // entry whose attribute byte has bit 0x20; rb+48 bit 0x40 waives it,
    // which is how a fast exit reaches a privileged routine.
    bool waived = (m_.readByte(req.requestBlock + RequestBlock::kOffTransferFlags) & RequestBlock::kTransferFromFastExit) != 0;
    if (!RequestBlock::isPrivileged(m_, req.requestBlock) && !waived && (attribute & kAttributePrivileged) != 0) {
        return refuse("{}: entry {:04X} attribute {:02X} has bit 20 - the target is privileged and the caller is not "
                      "(rb+19 bit 0 set, rb+48 bit 40 clear); nup1000 raises nuerr code 6",
                      call, entry, attribute);
    }

    // SA21-9436 3-75: attribute bit 0x10 means bytes 0-2 are a program BLOCK
    // guest address, not a disk sector.  With a nonzero value the block is
    // taken directly with no hash lookup and no disk read; a zero value
    // falls through to the swapped construction path below.
    if ((attribute & kAttributeResident) != 0 && sector != 0) {
        // A value at or above 0x800000 is translated, and the machine waits
        // on it rather than transferring: a wait that needs a dispatcher this
        // emulator does not model on this path.
        if ((sector & kTranslatedBit) != 0) {
            return refuse("{}: entry {:06X}{} attribute {:02X} is resident and its program block address {:06X} is "
                          "translated (>= 800000); nup1000 waits on it through nugwaitc (c18a55a4), and there is no "
                          "dispatcher to wait on",
                          call, entry, tagSuffix(entry), attribute, sector);
        }

        int residentPb = sector;
        trace_.csp("{}: entry {:06X}{} attribute {:02X} ({}) is RESIDENT - bytes 0-2 are program block {:04X} directly, "
                   "no hash lookup and no disk read (nup1000 c18a5518)",
                   call, entry, tagSuffix(entry), attribute, describeAttribute(attribute), residentPb);

        int residentNeed = ProgramBlock::pageCount(m_, residentPb);
        int residentHave = ProgramBlock::pagesReady(m_, residentPb);
        if (residentHave < residentNeed) {
            m_.writeAddr24(req.taskBlock + kTbTransferInterlock, residentPb);
            return refuse("{}: resident program block {:04X} has {} of {} pages ready; tb+57 set as the transfer "
                          "interlock and the task would wait, but there is no dispatcher to wait on",
                          call, residentPb, residentHave, residentNeed);
        }

        return performTransfer(req, call, residentPb, entryPoint);
    }

    if (sector == 0) {
        // A zero disk address is the SWAPPED construction path, not a
        // missing address: a block-flags code is picked from the attribute
        // bits and a program block built with (count+7)/8 pages and no disk
        // read, and control RETURNS to the caller.  SA21-9436 3-75's
        // attribute bit 0x40 is "program requires swapping to disk", and
        // IPL phase 2's first job is to initialise the task work area, so a
        // swap-owning block with no disk image is exactly what belongs here.
        uint8_t flags = (attribute & kAttributeSwapped) != 0             ? ControlBlock::kFlagSwapArea
                        : (attribute & kAttributeNeedsTransientArea) == 0 ? static_cast<uint8_t>(0x42)
                                                                          : static_cast<uint8_t>(0x02);
        int pages = (sectors + 7) / 8;
        int failure;
        int swapped = buildControlBlock(ControlBlock::kTypeProgramBlock, flags, pages, pages, req.taskBlock, entry, call,
                                        failure);
        if (swapped == 0) {
            return refuse("{}: entry {:06X}{} is the swapped form and nucbldsb refused it (code {:06X})", call, entry,
                          tagSuffix(entry), failure);
        }

        trace_.csp("{}: entry {:06X}{} attribute {:02X} ({}) is the SWAPPED form - program block {:04X} built with flags "
                   "{:02X}, {} page(s), no disk read, and control RETURNS to the caller - which is what Q bit 0 asks for "
                   "and what the guest goes on to rely on.",
                   call, entry, tagSuffix(entry), attribute, describeAttribute(attribute), swapped, flags, pages);
        return true;
    }

    // Hash the sector to a chain head at guest 0xD03 + 4(n & 7) and walk it
    // comparing pb+48..50.  A hit means the module is already resident.
    int bucket = kProgramBlockHashTable + 4 * (sector & 7) + kProgramBlockChainLast;
    bool hashable = (attribute & ControlBlock::kAttributeNotHashed) == 0;
    int pb = hashable ? findProgramBlock(bucket, sector, call) : 0;
    bool resident = pb != 0;

    if (!resident) {
        pb = buildProgramBlock(sector, sectors, attribute, call);
        if (pb == 0)
            return refuse("{}: no program block could be built for sector {} ({} sector(s), attribute {:02X})", call,
                          sector, sectors, attribute);
        if (!makeProgramBlockReady(pb, call))
            return refuse("{}: program block {:04X} (sector {}) could not be made ready", call, pb, sector);
        // Attribute bit 40 gives a program block no queue head: such a
        // one-shot block must not be inserted in the resident hash.
        if (hashable)
            chainProgramBlock(bucket, pb, call);
        else
            trace_.csp("{}: program block {:04X} attribute {:02X} bit 40 is not hashable; nucwsbsq gives it no resident "
                       "queue",
                       call, pb, attribute);
    } else {
        trace_.csp("{}: sector {} is already resident, program block {:04X}", call, sector, pb);
        // A hashed block can outlive its bytes in the single real system
        // transient area; pb+20 alone does not prove it still owns 0x1000.
        if ((ProgramBlock::attribute(m_, pb) & kAttributeNeedsTransientArea) != 0 && currentTransientProgramBlock_ != pb) {
            trace_.csp("{}: resident transient PB {:04X} is not current owner {:04X}; reacquiring it before transfer", call,
                       pb, currentTransientProgramBlock_);
            if (!makeProgramBlockReady(pb, call + " transient reacquire"))
                return refuse("{}: resident transient program block {:04X} could not be reacquired into the system "
                              "transient area",
                              call, pb);
        }
    }

    // Every successful lookup/build takes one use-count reference before
    // testing readiness and constructing the request block; the release of
    // the frame drops exactly this reference.
    activateControlBlock(pb, call);

    // The last test, and note the sense: NOT less-than takes the transfer.
    int need = ProgramBlock::pageCount(m_, pb);
    int have = ProgramBlock::pagesReady(m_, pb);
    if (have < need) {
        m_.writeAddr24(req.taskBlock + kTbTransferInterlock, pb);
        return refuse("{}: program block {:04X} has {} of {} pages ready; tb+57 set as the transfer interlock and the task "
                      "would wait, but there is no dispatcher to wait on",
                      call, pb, have, need);
    }

    return performTransfer(req, call, pb, entryPoint);
}

// Render an attribute byte the way SA21-9436 3-75 names its bits.
std::string As36ControlStorageProcessor::describeAttribute(uint8_t a)
{
    std::vector<std::string> parts;
    if ((a & kAttributeTranslated) != 0) parts.push_back("translated");
    if ((a & kAttributeSwapped) != 0) parts.push_back("swapped to disk");
    if ((a & kAttributePrivileged) != 0) parts.push_back("privileged");
    if ((a & kAttributeResident) != 0) parts.push_back("RESIDENT: 0-2 is a program block address");
    if ((a & kAttributeCoreSizeDiffers) != 0) parts.push_back("core size != program size");
    if ((a & kAttributeNeedsTransientArea) != 0) parts.push_back("needs the system transient area");
    if ((a & 0x09) != 0) parts.push_back("bits 08/01 set, which the manual calls NOT USED");
    if (parts.empty()) return "none";
    std::string s;
    for (std::size_t i = 0; i < parts.size(); i++) {
        if (i != 0) s += ", ";
        s += parts[i];
    }
    return s;
}

// The four EBCDIC characters an SSP module writes in front of a transfer
// table entry, plus a 0A.  Not architected, but every entry in the phase 1
// member carries one, and naming it turns a refusal into a diagnosis.
std::string As36ControlStorageProcessor::tagSuffix(int entry)
{
    std::string t = entryTag(entry);
    return t.empty() ? std::string() : " (" + t + ")";
}

std::string As36ControlStorageProcessor::entryTag(int entry)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = m_.readByte(entry - 5 + i);
    for (uint8_t c : b)
        if (c < 0x40) return std::string();
    std::string s = storage::Ebcdic::toAscii(b, 4);
    // Trim as the reference does.
    std::size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return std::string();
    std::size_t last = s.find_last_not_of(' ');
    return s.substr(first, last - first + 1);
}

// Give the callee a fresh request block, an address space and an
// instruction address, and hand the processor to it.  Both arms are here:
// the synchronous one, a call in the caller's own task, and the
// asynchronous one, which builds a whole new task for the module.
// Everything after the choice is common to both.
bool As36ControlStorageProcessor::performTransfer(SvcRequest& req, const std::string& call, int pb, int entryPoint)
{
    constexpr uint8_t kKeepCallersBlock = 0x21;   // Q bits 2 and 7 together
    constexpr uint8_t kAsynchronousBit = 0x20;    // Q bit 2

    // When pb+52 bit 0x10 (RESIDENT) is set the 3-byte value at pb+48..50 is
    // the module's byte address DIRECTLY: a resident module placed by SVC 52
    // into a system-queue-space load area is not page aligned and cannot be
    // expressed as a load page.
    bool residentInPlace = (ProgramBlock::attribute(m_, pb) & kAttributeResident) != 0;
    int module = residentInPlace ? m_.readAddr24(pb + ProgramBlock::kOffSector) : moduleBytes(pb);

    bool asynchronous = (req.q & kAsynchronousBit) != 0;
    int taskBlock = req.taskBlock;
    int units = requestBlockUnits(pb, req.requestBlock);
    int rb;

    if (asynchronous) {
        // The task block, the return ACE and the request block are allocated
        // as ONE chunk, built, and the NEW task is relinked and made current.
        taskBlock = attachNewTask(pb, req.taskBlock, req.requestBlock, req.q, call, rb);
        if (taskBlock == 0)
            return refuse("{}: nuptask could not attach a task for the asynchronous transfer to program block {:04X}", call,
                          pb);
    } else {
        // The size is in 16-byte units and pb+63 is a floor rather than the
        // answer.
        rb = heap_.allocate(units * GuestHeap::kGranularity, call + " request block");
        if (rb == 0) {
            return refuse("{}: nuprba could not assign a {}-byte request block ({} units) for program block {:04X} - {}",
                          call, units * 16, units, pb,
                          heap_.lastRefusal().empty() ? std::string("no reason recorded") : heap_.lastRefusal());
        }

        for (int i = 0; i < units * GuestHeap::kGranularity; i++) m_.writeByte(rb + i, 0);

        m_.writeHalf(rb, GuestLowStorage::kEyeRequestBlock);
        m_.writeByte(rb + RequestBlock::kOffLengthUnits, static_cast<uint8_t>(units));
        m_.writeAddr24(rb + RequestBlock::kOffProgramBlock, pb);
        m_.writeAddr24(rb + RequestBlock::kOffSentinel, 0xFFFFFF);

        // The new block chains to the CALLER's: a transfer pushes a frame
        // and this is the link SVC 11 returns along.
        m_.writeAddr24(rb + RequestBlock::kOffPrevious, req.requestBlock);
    }

    // The callee's instruction-fetch and direct-operand prefixes both come
    // from pb+56 bit 0x80: the module's own header byte says whether it runs
    // translated.
    uint8_t prefix = static_cast<uint8_t>(ProgramBlock::mode(m_, pb) & machine::MspRegisters::kPactTranslate);
    m_.writeByte(rb + RequestBlock::kOffPiar, prefix);
    m_.writeByte(rb + RequestBlock::kOffPdir, prefix);

    // ... and bits 0x81 of the mode byte are cleared.  Bit 0 clear is
    // privileged.
    m_.writeByte(rb + RequestBlock::kOffPrivilege, static_cast<uint8_t>(m_.readByte(rb + RequestBlock::kOffPrivilege) & ~0x81));

    // The PSR is not transferred: the new request block is zero-filled and
    // the callee starts with the field at zero.

    int target = resolveEntryPoint(module, entryPoint);

    // The entry point table is not always consulted: a zero entry point
    // number, or a module header whose +10..11 table offset is zero, enters
    // the module at its own load address.
    int tableOffset = m_.readHalf(module + kEntryPointTableOffset);
    if (entryPoint == 0 || tableOffset == 0) {
        target = residentInPlace ? module : ProgramBlock::moduleAddress(m_, pb);
        trace_.csp("{}: entry point {} with table offset {:04X} - nup2000 skips the lookup and enters the module at its "
                   "load address {:04X} (c18a57c8, c18a57dc)",
                   call, entryPoint, tableOffset, target);
    }

    m_.writeHalf(rb + RequestBlock::kOffIar, static_cast<uint16_t>(target));

    // The callee INHERITS the caller's index registers and work registers:
    // the fresh request block is the callee's register save area and was
    // cleared, so without this the reload would hand the module zeros.  The
    // machine relocates the pair for the callee's addressing; that
    // relocation is NOT modelled, only the propagation, which is correct
    // wherever caller and callee address the same storage the same way.
    m_.writeByte(rb + RequestBlock::kOffXr1High, m_.msp.pactXr1);
    m_.writeHalf(rb + RequestBlock::kOffXr1Low, m_.msp.xr1);
    m_.writeByte(rb + RequestBlock::kOffXr2High, m_.msp.pactXr2);
    m_.writeHalf(rb + RequestBlock::kOffXr2Low, m_.msp.xr2);
    for (int n = 4; n <= 7; n++) RequestBlock::writeWr(m_, rb, n, m_.msp.wr[n]);
    trace_.csp("{}: the callee inherits XR1 {:02X}{:04X}, XR2 {:02X}{:04X}, and WR4..WR7 {:04X}/{:04X}/{:04X}/{:04X} "
               "(nucm1000 c1891494/c18914f8; its 2 KB relocation at c1891470 is passed verbatim - faithful where caller "
               "and callee address the same storage the same way, which is every reached transfer; see "
               "docs/s36/nucmap-work-base-fix.md)",
               call, m_.msp.pactXr1, m_.msp.xr1, m_.msp.pactXr2, m_.msp.xr2, m_.msp.wr[4], m_.msp.wr[5], m_.msp.wr[6],
               m_.msp.wr[7]);

    // Relink the task and make both blocks current.  The relink has to come
    // BEFORE the register build, which reaches the new request block through
    // tb+65 and the program block through rb+41.  On the asynchronous arm
    // the attach has already done both into the NEW task block.
    m_.writeAddr24(taskBlock + TaskBlock::kOffRequestBlock, rb);
    currentRequestBlock_ = rb;
    saveTaskWorkBase(taskBlock, call);

    // The new block gets its OWN ATR file, which is what makes the caller's
    // survive the call.
    createTranslationFile(rb, call);

    // The register build's argument is the task block the transfer is now
    // working in: the LOCAL taskBlock, which differs from the request's
    // exactly when the transfer forked a task.
    buildTranslationRegisters(taskBlock);

    // Every callee whose mode is not A0-class gets the incoming request
    // block's map table copied into the fresh block, with every mapped
    // object pinned.  The request-block sizing reserves exactly this room.
    bool inheritedMap = copyCallerAddressability(req.requestBlock, rb, pb, call);

    // When the load-member header carries a non-zero MAP-list offset at
    // +12, the list embedded in the module is applied as part of every
    // transfer.
    int transferMapOffset = m_.readHalf(module + 12);
    if (transferMapOffset != 0) {
        SvcRequest transferMap;
        transferMap.r = 0x2F;
        transferMap.q = 0;
        transferMap.requestBlock = rb;
        transferMap.taskBlock = taskBlock;
        if (!mapParameterListCore(transferMap, rb, pb, module + transferMapOffset, call + " transfer MAP"))
            return refuse("{}: the callee's header MAP list at {:04X}+{:04X} was refused, so nup2000 cannot complete the "
                          "transfer (this is NOT a missing SVC variant - see the reason above)",
                          call, module, transferMapOffset);
    } else if (inheritedMap) {
        // The final register rebuild is conditional on either the copy or
        // the header MAP having changed addressability.
        buildTranslationRegisters(taskBlock);
    }

    // The task is no longer mid-transfer.
    m_.writeAddr24(taskBlock + kTbTransferInterlock, 0);

    // With neither Q bit 2 (asynchronous) nor Q bit 7 (return control to
    // the requesting program) set the transfer is a jump, not a call, and
    // the caller's frame is discarded here.
    if ((req.q & kKeepCallersBlock) == 0)
        freeChainedRequestBlock(rb + RequestBlock::kOffPrevious, req.taskBlock, call,
                                "the caller's frame is discarded, Q bits 2 and 7 both off");

    if (asynchronous) {
        // The processor does NOT go to the new task: the attach left it
        // waiting on TB_STAT2 bit 0x40 with no place on the ready list, so
        // the dispatcher selects somebody else, and the caller is the
        // obvious somebody.  This emulator's dispatcher runs only when
        // asked, so the asynchronous arm asks.
        redispatch_ = true;
        pendingAsyncChildren_.push_back(taskBlock);
        trace_.csp("{}: ASYNCHRONOUS transfer - task {:04X} built, entry {:04X}; waiting on TB_STAT2 40 until nucready "
                   "posts it (deferred)",
                   call, taskBlock, target);

        // Console sign-on phase ordering: an asynchronous transfer into the
        // phase-2 job entry module attaches the job task whose copy of the
        // console unit block's job pointer must see phase 1's link; the
        // dispatcher orders its continuation after it.
        auto entryMember = memberByProgramBlock_.find(pb);
        if (entryMember != memberByProgramBlock_.end() && entryMember->second.extentSector == kSvatJobEntryExtent) {
            phase2SvatJobTask_ = taskBlock;
            phase2SvatDispatched_ = false;
        }
    } else {
        m_.msp.iar = static_cast<uint16_t>(target);
        m_.msp.pactIar = prefix;
        m_.msp.pactDir = prefix;
        // XR1 and XR2 address through their own prefixes at rb+9 and rb+11,
        // which a freshly zeroed request block leaves untranslated: indexed
        // operands run real until something sets them.
        m_.msp.pactXr1 = m_.readByte(rb + RequestBlock::kOffXr1High);
        m_.msp.pactXr2 = m_.readByte(rb + RequestBlock::kOffXr2High);
    }

    // The active member for any task is read live from its request block
    // stack; this only annotates the transfer trace with the member entered.
    auto active = memberByProgramBlock_.find(pb);
    bool activeFound = active != memberByProgramBlock_.end();
    std::string activeDesc =
        activeFound ? fmt::format(", member {} (extent {})", active->second.name, active->second.extentSector) : std::string();

    trace_.csp("{}: transferred to {:04X} - module at {:04X}, program block {:04X}, task block {:04X}{}, request block "
               "{:04X} ({} units), prefix {:02X} ({}), returns to {:04X}",
               call, target, module, pb, taskBlock, activeDesc, rb, units, prefix, prefix == 0 ? "real" : "translated",
               m_.readAddr24(rb + RequestBlock::kOffPrevious));

    // Read-only: record the terminal-unit-block builder's replacement/clone
    // decision at every entry, since its configuration record only exists
    // during IPL and cannot be recovered after the fact.
    if (activeFound && active->second.name == "SVTUB") traceSvtubCloneGate(m_.msp.xr1, m_.msp.xr2);

    // The reference's forced display-write probe (SignonWddqWrite) is an
    // experiment that is off by default and is not ported.
    return true;
}

// ---- the request block stack -----------------------------------------------------

// The one primitive all three request-block releases go through: read the
// 3-byte pointer at p+3, unlink the block it names by storing THAT block's
// own +3..5 back into the field, and free it for rb+2 sixteen-byte units.
void As36ControlStorageProcessor::freeChainedRequestBlock(int pointerField, const std::string& call, const std::string& why)
{
    freeChainedRequestBlock(pointerField, 0, call, why);
}

void As36ControlStorageProcessor::freeChainedRequestBlock(int pointerField, int, const std::string& call,
                                                          const std::string& why)
{
    int rb = m_.readAddr24(pointerField);
    if (rb == 0) {
        trace_.csp("{}: nothing to free at {:04X} ({})", call, pointerField, why);
        return;
    }

    int deadPb = m_.readAddr24(rb + RequestBlock::kOffProgramBlock);
    int next = m_.readAddr24(rb + RequestBlock::kOffPrevious);
    m_.writeAddr24(pointerField, next);

    releaseRequestMappedBlocks(rb, deadPb, call);

    // Before the free: the block's ATR file goes back on the free list and
    // its ownership word is cleared.
    releaseTranslationFile(rb, call);

    int bytes = m_.readByte(rb + RequestBlock::kOffLengthUnits) * GuestHeap::kGranularity;
    trace_.csp("{}: request block {:04X} unlinked from {:04X} (now {:04X}) and freed, {} bytes - {}", call, rb, pointerField,
               next, bytes, why);

    // The IPL's own request block is not an allocation; freeing it would
    // corrupt the allocator's accounting rather than reclaim anything.
    if (heap_.contains(rb) && bytes > 0)
        heap_.free(rb, bytes);
    else
        trace_.csp("{}: {:04X} is outside the supervisor's pool - not returned to it (this is where csipl put the initial "
                   "pair)",
                   call, rb);

    // Drop the use-count reference the transfer took for this frame; the
    // shared-module decision and the deletion belong to the release.
    if (deadPb != 0 && heap_.contains(deadPb) && m_.readHalf(deadPb) == GuestLowStorage::kEyeProgramBlock)
        deactivateControlBlock(deadPb, call);
}

// The termination routine frees the task block itself: 160 bytes, the task
// block's share of the one composite allocation.  MEASURED DEAD END on
// this host, kept as a record and not wired in: the machine keeps the
// terminating task block in a native register across the free, while this
// emulator re-reads it from guest storage after termination returns, so a
// reused allocation overwrites it under a live task.
void As36ControlStorageProcessor::freeTerminatedTaskBlock(int tb, const std::string& call)
{
    if (!heap_.contains(tb)) {
        trace_.csp("{}: task block {:04X} is outside the system queue space - csipl's own task block is not an assignment "
                   "and nufree would abort on it (c18e2fa0)",
                   call, tb);
        return;
    }
    heap_.free(tb, kTaskBlockBytes, call + " nupterm task block");
    trace_.csp("{}: task block {:04X} freed, {} bytes - nupterm c18a4ee4-c18a4ef8", call, tb, kTaskBlockBytes);
}

// The termination routine's repeated frame releases, restricted to the
// independently allocated state this emulator can safely return.  The
// request block bytes themselves are part of the one composite task
// allocation and remain as the documented bounded leak.
int As36ControlStorageProcessor::releaseTerminatingTaskProgramState(int tb, const std::string& call)
{
    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    int released = releaseRequestChainProgramState(rb, call);
    m_.writeAddr24(tb + TaskBlock::kOffRequestBlock, 0);
    trace_.csp("{}: nupterm released translation/program state for {} request block(s) on task {:04X}; retained their "
               "composite nuptask bytes",
               call, released, tb);
    return released;
}

// Release the independently owned state referenced by a request block
// chain while retaining the block bytes.  Clearing rb+41 afterwards
// prevents a later walk from treating a reused program block address as
// another live reference.
int As36ControlStorageProcessor::releaseRequestChainProgramState(int rb, const std::string& call)
{
    int released = 0;
    for (int guard = 0; rb != 0 && guard < 256; guard++) {
        int next = m_.readAddr24(rb + RequestBlock::kOffPrevious);
        int pb = m_.readAddr24(rb + RequestBlock::kOffProgramBlock);
        releaseRequestMappedBlocks(rb, pb, call);
        releaseTranslationFile(rb, call);
        if (pb != 0 && heap_.contains(pb) && m_.readHalf(pb) == GuestLowStorage::kEyeProgramBlock)
            deactivateControlBlock(pb, call);
        m_.writeAddr24(rb + RequestBlock::kOffProgramBlock, 0);
        released++;
        rb = next;
    }
    return released;
}

// The first half of the frame release: rb+40 is the map entry count, the
// table begins at rb+64+pb[63]*16, and each eight-byte entry's +5..7 block
// address receives one release unless it is the FFFFFF sentinel.  The final
// program block deactivation is left to the caller.
void As36ControlStorageProcessor::releaseRequestMappedBlocks(int rb, int pb, const std::string& call)
{
    int count = m_.readByte(rb + RequestBlock::kOffMapEntryCount);
    if (count == 0 || pb == 0 || !heap_.contains(pb) || m_.readHalf(pb) != GuestLowStorage::kEyeProgramBlock) return;

    int table = MapTable::base(m_, rb, pb);
    int released = 0;
    for (int i = 0; i < count; i++) {
        int block = m_.readAddr24(table + i * MapTable::kEntryBytes + MapTable::kOffBlock);
        if (block == 0xFFFFFF) continue;
        if (!heap_.contains(block)) {
            trace_.csp("{}: nucmdsbq map entry {} in rb {:04X} names out-of-pool block {:06X}; not deactivated", call, i,
                       rb, block);
            continue;
        }
        uint16_t eye = m_.readHalf(block);
        if (eye != GuestLowStorage::kEyeProgramBlock && eye != GuestLowStorage::kEyeSystemBlock) {
            trace_.csp("{}: nucmdsbq map entry {} in rb {:04X} names block {:06X} with eyecatcher {:04X}; not deactivated",
                       call, i, rb, block, eye);
            continue;
        }
        deactivateControlBlock(block, call + " mapped block");
        released++;
    }
    if (released != 0)
        trace_.csp("{}: nucmdsbq released {} mapped control-block reference(s) from rb {:04X} (c1897094-c18970cc)", call,
                   released, rb);
}

// The termination routine's task-workspace loop: the queue head is the
// three-byte field ending at tb+43.  An unused block is deleted; a
// referenced block has its pin bit cleared, is detached, and has its owner
// cleared.  Repeated because the dequeue rewrites tb+41.
int As36ControlStorageProcessor::releaseTaskWorkSpaces(int tb, const std::string& call)
{
    int headField = tb + ControlBlock::kTaskWorkSpaceChain - 2;
    int released = 0;
    for (int guard = 0; guard < 4096; guard++) {
        int block = m_.readAddr24(headField);
        if (block == 0) break;
        if (!heap_.contains(block) || m_.readHalf(block) != GuestLowStorage::kEyeSystemBlock) {
            trace_.csp("{}: nupterm workspace cleanup stops at malformed tb+41 head {:06X}", call, block);
            break;
        }

        uint8_t references = m_.readByte(block + ControlBlock::kOffUseCount);
        if (references == 0) {
            trace_.csp("{}: nupterm nucwdel deletes unused task workspace {:06X}", call, block);
            deleteControlBlock(block, call);
        } else {
            uint8_t flags = m_.readByte(block + StorageBlock::kOffFlags);
            m_.writeByte(block + StorageBlock::kOffFlags, static_cast<uint8_t>(flags & ~ControlBlock::kFlagPinned));
            dequeueControlBlock(block, call);
            m_.writeAddr24(block + ProgramBlock::kOffOwningTask, 0);
            trace_.csp("{}: nupterm nucwdel detaches referenced task workspace {:06X} (+27={})", call, block, references);
        }
        released++;
    }
    trace_.csp("{}: nupterm released/detached {} task workspace(s); tb+41..43={:06X} (c18a4d44-c18a4d7c)", call, released,
               m_.readAddr24(headField));
    return released;
}

// Make a terminal task invisible to both scheduler views: queue 39 (chain
// 27) then queue 40 (chain 35).
void As36ControlStorageProcessor::retireTerminatingTaskFromScheduler(int tb, const std::string& call)
{
    constexpr uint8_t kDequeueSystem = 0x60;
    bool wasOnTaskQueue =
        queueOperation(GuestLowStorage::queueHeader(kTaskPriorityQueue), tb, TaskBlock::kChainLastQueue39, kDequeueSystem);
    bool wasOnReadyQueue =
        queueOperation(GuestLowStorage::queueHeader(kTaskReadyQueue), tb, TaskBlock::kChainLastQueue40, kDequeueSystem);
    trace_.csp("{}: nupterm scheduling retirement removed task {:04X} from queue 39={}, queue 40={} (nudeqsys "
               "c18a4d94/c18a4db0)",
               call, tb, wasOnTaskQueue ? "True" : "False", wasOnReadyQueue ? "True" : "False");
}

// Remove the dying task's timer and termination-I/O registrations: queue 54
// with the task field ending at element+7 and the chain at +11, then
// queues 32, 30 and 29 with the task field ending at +21 and the chain at
// +4, leaving entries whose +28 high bit is set in place.
int As36ControlStorageProcessor::releaseTaskTerminationIoQueues(int tb, const std::string& call)
{
    int released = 0;
    released += releaseTaskQueueMatches(54, tb, 7, 11, 16, false, "nutetqdq", call);
    for (uint8_t q : {static_cast<uint8_t>(32), static_cast<uint8_t>(30), static_cast<uint8_t>(29)})
        released += releaseTaskQueueMatches(q, tb, 21, 4, 32, true, "nuteiopg", call);
    trace_.csp("{}: nupterm timer/I-O queue cleanup removed {} element(s) for task {:04X} from Q54/Q32/Q30/Q29 "
               "(nutetqdq/nuteiopg c18a48c8-c18a49e4)",
               call, released, tb);
    return released;
}

int As36ControlStorageProcessor::releaseTaskQueueMatches(uint8_t queue, int tb, int keyLastByte, int chainLastByte,
                                                         int bytes, bool honorRetainFlag, const std::string& routine,
                                                         const std::string& call)
{
    int header = GuestLowStorage::queueHeader(queue);
    int at = m_.readAddr24(header);
    int steps = 0, released = 0;
    while (at != 0) {
        if (!chainStepValid(at, steps++, header)) break;
        int next = m_.readAddr24(at + chainLastByte - 2);
        bool matches = m_.readAddr24(at + keyLastByte - 2) == tb;
        bool retained = honorRetainFlag && (m_.readByte(at + 28) & 0x80) != 0;
        if (matches && !retained) {
            bool dequeued = queueOperation(header, at, chainLastByte, 0x20);
            trace_.csp("{}: nupterm {} {} task {:04X} element {:06X} from queue {} (key +{}, chain +{})", call, routine,
                       dequeued ? "dequeued" : "could not dequeue", tb, at, queue, keyLastByte, chainLastByte);
            if (dequeued) {
                // The I/O queues always free their 32-byte cell; the timer
                // queue frees its 16-byte cell only when the +3 ownership
                // byte is zero.
                bool freeCell = routine == "nuteiopg" || m_.readByte(at + 3) == 0;
                if (freeCell && heap_.contains(at)) heap_.free(at, bytes);
                released++;
            }
        } else if (matches) {
            trace_.csp("{}: nupterm {} retains task {:04X} element {:06X} on queue {}: element+28 bit 80 is set", call,
                       routine, tb, at, queue);
        }
        at = next;
    }
    return released;
}

// Recompute the task's scratch base, tb+69..71, and publish it in queue
// header 38: walk the request block chain from tb+65..67 back through
// rb+3..5 and take the FIRST block whose program block asks for scratch
// space (pb+63); the answer is that block's address + 64.  Zero when no
// block on the chain asks for any.
void As36ControlStorageProcessor::saveTaskWorkBase(int taskBlock, const std::string& call)
{
    int rb = m_.readAddr24(taskBlock + TaskBlock::kOffRequestBlock);
    int firstRb = rb;
    int found = 0;
    std::string inspected;
    for (int guard = 0; rb != 0 && guard < 64; guard++) {
        int pb = m_.readAddr24(rb + RequestBlock::kOffProgramBlock);
        uint8_t units = pb == 0 ? static_cast<uint8_t>(0) : m_.readByte(pb + ProgramBlock::kOffRequestBlockUnits);
        if (!inspected.empty()) inspected += "; ";
        inspected += fmt::format("rb {:06X} -> pb {:06X} scratch {:02X}", rb, pb, units);
        if (pb != 0 && units != 0) {
            found = rb + 64;
            break;
        }
        rb = m_.readAddr24(rb + RequestBlock::kOffPrevious);
    }
    m_.writeAddr24(taskBlock + TaskBlock::kOffWorkBase, found);
    m_.writeAddr24(GuestLowStorage::queueHeader(kTaskWorkBaseQueue), found);
    trace_.csp("{}: task work base tb+69..71 = {:06X}, queue header {} (nupsavwb c18a63b0)", call, found,
               kTaskWorkBaseQueue);
    if (firstRb != 0 && found == 0) trace_.csp("{}: nupsavwb found no scratch-bearing frame: {}", call, inspected);
}

// ---- SVC 11 ----------------------------------------------------------------------

// Main Storage Exit: "returns control to the caller of a routine ... at the
// next sequential instruction following the transfer supervisor call".  The
// exiting block's rb+3..5 names the frame to return to; zero means the
// block is a task's ROOT frame and the task terminates.
bool As36ControlStorageProcessor::mainStorageExit(SvcRequest& req)
{
    int prev = m_.readAddr24(req.requestBlock + RequestBlock::kOffPrevious);
    if (prev == 0) return terminateTaskRoot(req);

    freeChainedRequestBlock(req.taskBlock + TaskBlock::kOffRequestBlock, "SVC 11", "nuprbf, the exiting routine's own frame");
    currentRequestBlock_ = prev;

    // The caller's registers are RESTORED by repointing its own ATR file;
    // there is nothing to rebuild unless the block is not fully ready or
    // rb+44 bit 0x40 says so.
    selectTranslationFile(prev, "SVC 11");

    int pb = m_.readAddr24(prev + RequestBlock::kOffProgramBlock);
    if (pb != 0 && (ProgramBlock::attribute(m_, pb) & kAttributeNeedsTransientArea) != 0 &&
        currentTransientProgramBlock_ != pb) {
        trace_.csp("SVC 11: caller PB {:04X} needs the system transient but current owner is {:04X}; reacquiring before "
                   "resume",
                   pb, currentTransientProgramBlock_);
        if (!makeProgramBlockReady(pb, "SVC 11 transient reacquire")) return false;
    }
    uint8_t stale = m_.readByte(prev + RequestBlock::kOffAtrStale);
    bool ready = pb != 0 && ProgramBlock::pageCount(m_, pb) == ProgramBlock::pagesReady(m_, pb);
    if (pb != 0) {
        if (ready && (stale & kAtrStaleFlag) == 0) {
            trace_.csp("SVC 11: nupexit skips nucratr - program block {:04X} is ready and rb+44 bit 40 is clear, and the "
                       "caller's own ATR file was never touched (c18a4184)",
                       pb);
        } else {
            m_.writeByte(prev + RequestBlock::kOffAtrStale, static_cast<uint8_t>(stale & ~kAtrStaleFlag));
            buildTranslationRegisters(req.taskBlock);
        }
    }

    trace_.csp("SVC 11: returned to request block {:04X}, program block {:04X}, {:04X} with prefix {:02X}", prev, pb,
               m_.readHalf(prev + RequestBlock::kOffIar), m_.readByte(prev + RequestBlock::kOffPiar));

    // The callee's transfer set queue header 38 to ITS scratch base; the
    // resumed caller re-derives its continuation through it, so it is
    // republished as the caller's.
    saveTaskWorkBase(req.taskBlock, "SVC 11 nupexit");
    return true;
}

// The task-root arm the exit reaches when the exiting request block is a
// task's root: post the "return control to the requesting program" element
// to the task named at ace+19, run the cleanup the status byte selects, and
// continue in the TERMINATING task through transfer slot 4.
bool As36ControlStorageProcessor::terminateTaskRoot(SvcRequest& req)
{
    int tb = req.taskBlock;
    int terminatingRb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    int terminatingPb = terminatingRb == 0 ? 0 : m_.readAddr24(terminatingRb + RequestBlock::kOffProgramBlock);
    auto terminatingMember = memberByProgramBlock_.find(terminatingPb);
    std::string terminatingIdentity =
        terminatingMember != memberByProgramBlock_.end()
            ? fmt::format("{} extent {}", terminatingMember->second.name, terminatingMember->second.extentSector)
            : std::string("unattributed");

    trace_.csp("SVC 11: nupterm entry task {:04X}, rb {:06X}, pb {:06X} ({}): +24={:02X} +32={:02X} +40={:02X} +48={:02X} "
               "+52/+60={:02X}/{:02X} MIC={:04X} +130/+150={:04X}/{:04X}",
               tb, terminatingRb, terminatingPb, terminatingIdentity, m_.readByte(tb + TaskBlock::kOffTerminationState),
               m_.readByte(tb + TaskBlock::kOffStatus), m_.readByte(tb + 40),
               m_.readByte(tb + TaskBlock::kOffTerminationDependencyFlags), m_.readByte(tb + TaskBlock::kOffTerminationDepth),
               m_.readByte(tb + TaskBlock::kOffTerminationPass), m_.readHalf(tb + TaskBlock::kOffMic),
               m_.readHalf(tb + TaskBlock::kOffTerminationWait130), m_.readHalf(tb + TaskBlock::kOffTerminationWait150));

    // Entry bookkeeping: advance the two counters and mark this task as
    // actively terminating; the dependency scan uses the same fields.
    m_.writeByte(tb + TaskBlock::kOffTerminationDepth, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffTerminationDepth) + 1));
    m_.writeByte(tb + TaskBlock::kOffTerminationPass, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffTerminationPass) + 1));
    m_.writeByte(tb + TaskBlock::kOffTerminationState,
                 static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffTerminationState) | TaskBlock::kTerminationActive));

    int ace = m_.readAddr24(tb + TaskBlock::kOffReturnAce);
    bool aceIsElement = ace != 0 && m_.readHalf(ace + ActionControlElement::kOffEyecatcher) == ActionControlElement::kEyecatcher;
    int target = aceIsElement ? m_.readAddr24(ace + ActionControlElement::kOffTaskBlock) : 0;

    if (ace != 0 && (!aceIsElement || !TaskBlock::isTaskBlock(m_, target))) {
        // A nonzero malformed return ACE is not a context in which it is
        // safe to approximate the post.  A zero ACE is valid: only the post
        // is skipped and the routine still reaches its slot-4 continuation.
        trace_.csp("SVC 11: request block {:04X} is a task root (rb+3..5 = 0), so nupexit reaches nupterm (c18a405c), but "
                   "task block {:04X} carries a malformed return ACE at tb+17..19 ({:04X})",
                   req.requestBlock, tb, ace);
        return false;
    }

    // Drain tb+53..55 through the ordinary resource dequeue path, then
    // remove this task's timer and termination-I/O registrations, before
    // considering dependent tasks.
    releaseTaskResources(tb, "SVC 11");
    releaseTaskTerminationIoQueues(tb, "SVC 11");

    int immediateDependents;
    int dependents = scanTerminationDependents(tb, immediateDependents, "SVC 11 nupterm");

    bool posted = true;
    if (aceIsElement) {
        // The return ACE's event type is stamped 0x13 before posting and
        // tb+17..19 is cleared once taken.  The post readies the target if
        // it is in an event wait; it does not become current, and no
        // dispatch runs inline: the dying task must first restore itself
        // and enter slot 4.
        m_.writeHalf(ace + ActionControlElement::kOffEventType, kReturnAceEventType);
        m_.writeAddr24(tb + TaskBlock::kOffReturnAce, 0);
        posted = completeToTask(ace, 0, "SVC 11 nupterm", true, true);
    } else {
        trace_.csp("SVC 11: nupterm task {:04X} has no return ACE; c18a4c34 skips the post but retains the "
                   "terminating-task slot-4 continuation",
                   tb);
    }

    // The cleanup predicate: a task with tb+32 bit 0x40 CLEAR skips the
    // request-block, workspace and task cleanup; a SET bit performs it.  A
    // posted return ACE cannot turn a clear bit into cleanup.
    uint8_t terminationStatus = m_.readByte(tb + TaskBlock::kOffStatus);
    bool cleanupFromStatus = (terminationStatus & 0x40) != 0;
    bool cleanupTaskEnvironment = cleanupFromStatus;
    if (cleanupTaskEnvironment) {
        trace_.csp("SVC 11: nupterm cleanup for task {:04X}: tb+32={:02X} supplies nonzero status-derived r31; c18a4cf4 "
                   "does not take the zero branch{}",
                   tb, terminationStatus,
                   aceIsElement ? "; the posted-return-ACE path later replaces r31 with tb+65..67" : "");
        releaseTaskWorkSpaces(tb, "SVC 11 nupterm cleanup");
        retireTerminatingTaskFromScheduler(tb, "SVC 11 nupterm cleanup");
    } else {
        trace_.csp("SVC 11: nupterm {} task {:04X} has tb+32={:02X}, so status-derived r31=0; BC 4,1 at c18a4cf4 branches "
                   "to c18a4fa0 and retains its workspaces and Q39/Q40 membership",
                   aceIsElement ? "return-ACE" : "no-return-ACE", tb, terminationStatus);

        // The retained-context arm is conditional on the status byte's bit
        // 0x80: it stores the status with that bit cleared and posts the
        // task with condition 08.
        uint8_t statusAtRetain = m_.readByte(tb + TaskBlock::kOffStatus);
        if ((statusAtRetain & 0x80) != 0) {
            uint8_t retainedStatus = static_cast<uint8_t>(statusAtRetain & 0x7F);
            m_.writeByte(tb + TaskBlock::kOffStatus, retainedStatus);
            postTaskConditionsDirect(tb, 0x08, "SVC 11 nupterm retained-context nupotb");
            trace_.csp("SVC 11: nupterm retained context: tb+32 bit 80 was set, so c18a4fbc stores tb+32 = {:02X} & 7F = "
                       "{:02X} and calls nupotb(tb,08) (c18a4fa0-c18a4fcc)",
                       statusAtRetain, retainedStatus);
        } else {
            trace_.csp("SVC 11: nupterm retained context: tb+32 = {:02X} has bit 80 clear, so c18a4fa8 branches to c18a4fd0 "
                       "- no status store and no nupotb (c18a4fa0-c18a4fa8)",
                       statusAtRetain);
        }
    }

    // Return from a nested native call rather than start another
    // termination continuation: the abnormal-termination path entered slot
    // 1D from native code, and the guest termination program eventually
    // invokes termination again; the machine observes the outstanding
    // native state and unwinds to its native caller.  No host call stack
    // survives across guest execution, so that frame is per-task state.
    auto frames = cleanupTaskEnvironment ? nativeTransferContinuations_.find(tb) : nativeTransferContinuations_.end();
    if (frames != nativeTransferContinuations_.end() && !frames->second.empty()) {
        int nativeDepth = static_cast<int>(frames->second.size());
        NativeTransferContinuation innermost = frames->second.back();
        nativeTransferContinuations_.erase(frames);
        // The return itself is not a second cleanup predicate; the
        // independently-owned program state is released here only because
        // the cleanup arm deferred it until the retained request chain has
        // been read.
        if (cleanupTaskEnvironment) releaseTerminatingTaskProgramState(tb, "SVC 11 native continuation return after cleanup");
        redispatch_ = true;
        enableDispatching("SVC 11 native continuation return", "guest termination returned to native nup1000 caller");
        std::string nativeReturn =
            innermost == NativeTransferContinuation::NuabSlot1D
                ? "SLIC nuab resumes at c18cdc90, requeues its 32-byte native context at c18cdc98-c18cdcd8, and clears "
                  "NuEmul's direct current fields at c18cdce0-c18cdd00 (the context payload is not represented by this host "
                  "continuation)"
                : "the outer nupterm resumes after its c18a502c nup1000 call and immediately returns through c18a5030 into "
                  "nupexit";
        trace_.csp("SVC 11: task {:04X} completes {} pending native nup1000 continuation(s), innermost {}; nested nupterm "
                   "returns at c18a5030 and {}; slot 4 is not entered again; request state was {}",
                   tb, nativeDepth, continuationName(innermost), nativeReturn,
                   cleanupTaskEnvironment ? "released by the cleanup arm" : "retained by tb+32.40 clear");
        if (!dispatch("SVC 11 native continuation return")) {
            // The native frame has unwound and the exit finds an empty ready
            // queue: the ordinary external-event idle, exactly as after a
            // guest wait, not a refused SVC.
            msp_->halt(fmt::format("SVC 11: task block {:04X} completed its native termination continuation and no task is "
                                   "ready - nudspchA's no-task exit (c180e04c){}",
                                   tb, undischargedActionsSuffix()));
            idleEventWait_ = true;
            trace_.csp("SVC 11: task {:04X} completed its native nup1000 continuation; no runnable task remains, so nudspchA "
                       "takes its normal no-task exit and waits for external work",
                       tb);
            return posted;
        }
        idleEventWait_ = false;
        return posted;
    }

    // The final continuation is not an ordinary dispatch: the terminating
    // task and its request block are made current, Q=01/R=04 is stamped,
    // and slot 4 is entered directly.  The ACE target is only the
    // completion recipient above, never this context.
    bool continued = false;
    int table = directArea_.read(DirectArea::kTransferControlTable);
    if (posted && terminatingRb != 0 && table != 0) {
        // The final handoff balances the entry increments before entering
        // the termination program.
        m_.writeByte(tb + TaskBlock::kOffTerminationDepth, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffTerminationDepth) - 1));
        m_.writeByte(tb + TaskBlock::kOffTerminationPass, static_cast<uint8_t>(m_.readByte(tb + TaskBlock::kOffTerminationPass) - 1));
        trace_.csp("SVC 11: nupterm final handoff decremented +52/+60 to {:02X}/{:02X} (c18a4fd0-c18a4ff0)",
                   m_.readByte(tb + TaskBlock::kOffTerminationDepth), m_.readByte(tb + TaskBlock::kOffTerminationPass));

        currentTaskBlock_ = tb;
        currentRequestBlock_ = terminatingRb;
        restoreRegisters(terminatingRb);

        SvcRequest continuation;
        continuation.r = 0x04;
        continuation.q = 0x01;
        continuation.requestBlock = terminatingRb;
        continuation.taskBlock = tb;
        stampRequest(terminatingRb, continuation);
        int entry = table + kTransferControlEntryBytes * kTransferControlTerminate;
        trace_.csp("SVC 11: nupterm internal continuation -> transfer table {:04X} + 5 x 4 = {:04X}; terminating task "
                   "{:04X}, request block {:04X} (c18a4ff8..c18a502c)",
                   table, entry, tb, terminatingRb);
        continued = transferControl(continuation, "SVC 11 nupterm slot 4", entry, 0);
        if (continued) {
            pushNativeTransferContinuation(tb, NativeTransferContinuation::NuptermSlot4);
            // The machine released these frames before the transfer; here
            // the program block deletion waits until the transfer has
            // finished reading the retained terminating request block, since
            // an immediately reused program block would alias rb+41.  There
            // is no intervening guest execution, so the observable ordering
            // is identical.
            int released = releaseRequestChainProgramState(terminatingRb, "SVC 11 nupterm post-slot-4 release");
            trace_.csp("SVC 11: nupterm post-slot-4 released {} old request frame reference(s); current rb is {:04X}",
                       released, currentRequestBlock_);
        }
        // The post may have requested a dispatch when the return target was
        // waiting; the termination program is nevertheless entered at once
        // in the terminating task and decides its own disposition.
        if (continued) redispatch_ = false;
    } else {
        trace_.csp("SVC 11: nupterm cannot enter internal transfer slot 4: posted={}, terminating rb={:04X}, table={:04X}",
                   posted ? "True" : "False", terminatingRb, table);
    }

    trace_.csp("SVC 11: nupterm - task block {:04X} root-exits; return ACE {:04X} post target {:04X} (zero means no post); "
               "slot-4 continuation entered in the terminating task={} (c18a4c70 nupo0024 + c18a5010 nup1000)",
               tb, ace, target, continued ? "True" : "False");
    trace_.csp("SVC 11: nupterm dependency scan selected {} task(s), {} entered CTEI immediately and {} were armed for "
               "deferred termination",
               dependents, immediateDependents, dependents - immediateDependents);
    return posted && continued;
}

// Walk queue 39, then each task's ACE chain rooted at tb+17.  An ACE whose
// task field equals the dying task is a termination dependency
// registration.  Eligible tasks either enter transfer slot 4 now or
// receive tb+6=FF so their next wait enters termination.
int As36ControlStorageProcessor::scanTerminationDependents(int terminatingTb, int& immediate, const std::string& call)
{
    immediate = 0;
    int selected = 0;
    int header = GuestLowStorage::queueHeader(kTaskPriorityQueue);
    int task = m_.readAddr24(header);
    int taskSteps = 0;
    int table = directArea_.read(DirectArea::kTransferControlTable);

    while (task != 0) {
        if (!chainStepValid(task, taskSteps++, header) || task + TaskBlock::kOffRequestBlock + 3 > m_.backingBytes() ||
            !TaskBlock::isTaskBlock(m_, task)) {
            trace_.csp("{}: nupterm dependency scan stops at malformed Queue-39 task {:06X}", call, task);
            break;
        }

        // Save links before a post or transfer can change task state.
        int nextTask = m_.readAddr24(task + TaskBlock::kOffQueue39Link);
        int ace = m_.readAddr24(task + TaskBlock::kOffReturnAce);
        int aceSteps = 0;
        while (ace != 0) {
            if (!chainStepValid(ace, aceSteps++, task + TaskBlock::kOffReturnAce) ||
                ace + ActionControlElement::kSize > m_.backingBytes() ||
                m_.readHalf(ace + ActionControlElement::kOffEyecatcher) != ActionControlElement::kEyecatcher) {
                trace_.csp("{}: nupterm dependency scan stops malformed ACE chain {:06X} owned by task {:04X}", call, ace,
                           task);
                break;
            }

            int nextAce = m_.readAddr24(ace + ActionControlElement::kOffChainLink);
            bool targetsDyingTask = m_.readAddr24(ace + ActionControlElement::kOffTaskBlock) == terminatingTb;
            uint8_t status = m_.readByte(task + TaskBlock::kOffStatus);
            bool eligible = targetsDyingTask && (status & TaskBlock::kStatusTerminationScanEligible) != 0 &&
                            (m_.readByte(task + TaskBlock::kOffTerminationState) & TaskBlock::kTerminationActive) == 0;

            if (eligible) {
                selected++;
                uint8_t flags = m_.readByte(task + TaskBlock::kOffTerminationDependencyFlags);
                m_.writeByte(task + TaskBlock::kOffTerminationDependencyFlags,
                             static_cast<uint8_t>(flags | TaskBlock::kTerminationDependencySelected));

                bool suspendable = canSuspendAsynchronously(task, call + " scan");
                bool enterNow = suspendable && m_.readByte(task + TaskBlock::kOffTerminationDepth) != 0;
                if (!enterNow) {
                    m_.writeByte(task + TaskBlock::kOffDeferredWait, 0xFF);
                    m_.writeByte(task + TaskBlock::kOffStatus, static_cast<uint8_t>(status | TaskBlock::kStatusDeferredTermination));
                    trace_.csp("{}: nupterm dependency ACE {:04X} selects task {:04X} for dying task {:04X}; deferred: "
                               "tb+6=FF, tb+32 {:02X}->{:02X} (c18a4b74..c18a4b80)",
                               call, ace, task, terminatingTb, status, m_.readByte(task + TaskBlock::kOffStatus));
                } else {
                    int rb = m_.readAddr24(task + TaskBlock::kOffRequestBlock);
                    bool entered = rb != 0 && table != 0;
                    if (entered) {
                        currentTaskBlock_ = task;
                        currentRequestBlock_ = rb;
                        restoreRegisters(rb);
                        SvcRequest continuation;
                        continuation.r = 0x04;
                        continuation.q = 0x01;
                        continuation.requestBlock = rb;
                        continuation.taskBlock = task;
                        stampRequest(rb, continuation);
                        postTaskConditionsDirect(task, 0xFF, call + " scan nupotb");
                        int entry = table + kTransferControlEntryBytes * kTransferControlTerminate;
                        entered = transferControl(continuation, call + " dependency slot 4", entry, 0);
                    }

                    if (entered) {
                        pushNativeTransferContinuation(task, NativeTransferContinuation::NuptermSlot4);
                        immediate++;
                        trace_.csp("{}: nupterm dependency ACE {:04X} selects task {:04X} for dying task {:04X}; immediate "
                                   "CTEI (c18a4b8c..c18a4bcc)",
                                   call, ace, task, terminatingTb);
                    } else {
                        m_.writeByte(task + TaskBlock::kOffDeferredWait, 0xFF);
                        m_.writeByte(task + TaskBlock::kOffStatus,
                                     static_cast<uint8_t>(status | TaskBlock::kStatusDeferredTermination));
                        trace_.csp("{}: nupterm could not enter dependency task {:04X} immediately (rb={:04X}, table={:04X}); "
                                   "armed deferred termination instead",
                                   call, task, rb, table);
                    }
                }
            }
            ace = nextAce;
        }
        task = nextTask;
    }

    // Restore the original terminating task and request block after every
    // selected task has had its immediate continuation built.
    int terminatingRb = m_.readAddr24(terminatingTb + TaskBlock::kOffRequestBlock);
    currentTaskBlock_ = terminatingTb;
    if (terminatingRb != 0) {
        currentRequestBlock_ = terminatingRb;
        restoreRegisters(terminatingRb);
    }
    return selected;
}

int As36ControlStorageProcessor::scanTerminationDependentsExperiment(int& immediate)
{
    return scanTerminationDependents(currentTaskBlock_, immediate, "EXPERIMENT nuptermscan");
}

// SVC 05, Free Second Request Block: rb+3..5 is relinked to the freed
// block's own +3..5, so "the RB_RBQ field of the current RB now contains
// the address of what was the 3rd RB on the stack".
bool As36ControlStorageProcessor::freeSecondRequestBlock(SvcRequest& req)
{
    freeChainedRequestBlock(req.requestBlock + RequestBlock::kOffPrevious, "SVC 05",
                            "nupunstk, the second block on the task's stack");
    return true;
}

// ---- program blocks -------------------------------------------------------------------

// Walk a chain of program blocks from a 3-byte head, comparing each block's
// pb+48..50 against the sector.  A chain step must be a resident program
// block before its key is read or its link followed: a reused slot left in
// the chain is treated as the end of the chain rather than faulted on.
int As36ControlStorageProcessor::findProgramBlock(int bucket, int sector, const std::string& call)
{
    int at = m_.readAddr24(bucket - 2);
    int guard = 0;
    while (at != 0) {
        if (at + ProgramBlock::kBytes > m_.backingBytes() ||
            m_.readHalf(at + ProgramBlock::kOffEyecatcher) != GuestLowStorage::kEyeProgramBlock) {
            trace_.csp("{}: SVC 10 hash bucket {:04X} chains to {:06X}, which is not a resident program block (a "
                       "freed/reused slot left in the chain) - treating as end of chain",
                       call, bucket, at);
            return 0;
        }
        if (ProgramBlock::sector(m_, at) == sector) return at;
        if (++guard > 4096) {
            trace_.csp("{}: program block chain at {:04X} does not terminate", call, bucket);
            return 0;
        }
        at = ProgramBlock::chainLink(m_, at);
    }
    return 0;
}

// Push the new block on the front of its bucket.
void As36ControlStorageProcessor::chainProgramBlock(int bucket, int pb, const std::string& call)
{
    m_.writeAddr24(pb + ProgramBlock::kOffChainLink, m_.readAddr24(bucket - 2));
    m_.writeAddr24(bucket - 2, pb);
    trace_.csp("{}: program block {:04X} chained at {:04X}", call, pb, bucket - 2);
}

// A member's identity is captured once, when its bytes are read: its
// 5-character header name, its 0-based extent sector, and the logical base
// it runs at.  The program block is the stable identity, since many members
// load at logical 0x1000 and alias one another.
void As36ControlStorageProcessor::recordLoadedMember(int pb, const uint8_t* nameBytes, int nameLength, long long extentSector)
{
    std::string name = storage::Ebcdic::toAscii(nameBytes, static_cast<std::size_t>(nameLength));
    std::size_t first = name.find_first_not_of(' ');
    if (first == std::string::npos) {
        name.clear();
    } else {
        std::size_t last = name.find_last_not_of(' ');
        name = name.substr(first, last - first + 1);
    }
    LoadedMember member;
    member.name = name;
    member.extentSector = extentSector;
    member.logicalBase = ProgramBlock::moduleAddress(m_, pb);
    memberByProgramBlock_[pb] = member;
}

// Snapshot every loader-attributed program block for monitor diagnostics.
std::vector<As36ControlStorageProcessor::LoadedProgramBlock> As36ControlStorageProcessor::getLoadedProgramBlocks()
{
    std::vector<LoadedProgramBlock> result;
    result.reserve(memberByProgramBlock_.size());
    for (const auto& pair : memberByProgramBlock_) {
        int pb = pair.first;
        const LoadedMember& member = pair.second;
        LoadedProgramBlock b;
        b.programBlock = pb;
        b.name = member.name;
        b.extentSector = member.extentSector;
        b.logicalBase = member.logicalBase;
        b.physicalBase = moduleBytes(pb);
        b.programBlockSector = ProgramBlock::sector(m_, pb);
        b.attribute = ProgramBlock::attribute(m_, pb);
        b.pagesReady = ProgramBlock::pagesReady(m_, pb);
        b.pageCount = ProgramBlock::pageCount(m_, pb);
        b.ready = (m_.readByte(pb + ProgramBlock::kOffFlags) & ProgramBlock::kFlagReady) != 0;
        result.push_back(b);
    }
    return result;
}

// The transfer-time addressability copier: unless the new program is
// A0-class it copies the incoming request block's complete MAP table to the
// new block.  An untranslated callee can additionally get a leading entry
// for the caller program itself.  Each copied block receives the same
// domain and use-count references the frame release later balances.
bool As36ControlStorageProcessor::copyCallerAddressability(int callerRb, int calleeRb, int calleePb, const std::string& call)
{
    uint8_t mode = ProgramBlock::mode(m_, calleePb);
    if ((mode & 0xA0) == 0xA0) {
        // The reference's copy-map-on-transfer probe for A0-class programs
        // (SignonCopyMapOnTransfer) is an experiment that is off by default
        // and is not ported.
        return false;
    }
    if (callerRb == 0 || callerRb == calleeRb) return false;

    int callerPb = m_.readAddr24(callerRb + RequestBlock::kOffProgramBlock);
    if (callerPb == 0) return false;

    int sourceCount = MapTable::count(m_, callerRb);
    int sourceTable = MapTable::base(m_, callerRb, callerPb);
    int destinationTable = MapTable::base(m_, calleeRb, calleePb);

    // Translated callees skip the caller-program entry; untranslated ones
    // also skip it when the predecessor program has attribute bit 0x02 or
    // 0x10.
    bool skipCallerProgram = (mode & 0x80) != 0 || (ProgramBlock::attribute(m_, callerPb) & 0x12) != 0;
    int required = sourceCount + (skipCallerProgram ? 0 : 1);
    if (destinationTable + required * MapTable::kEntryBytes > MapTable::blockEnd(m_, calleeRb)) {
        trace_.csp("{}: nucmclr needs {} map entries copied from rb {:06X}, past the end of destination rb {:06X} - nuersvc "
                   "code 83",
                   call, required, callerRb, calleeRb);
        return false;
    }

    int destinationCount = 0;
    if (!skipCallerProgram) {
        int pages = m_.readHalf(callerPb + ProgramBlock::kOffPageCount) & 0xFF;
        MapTable::write(m_, destinationTable, ProgramBlock::loadPage(m_, callerPb), pages, 0, callerPb);
        referenceInheritedMapObject(callerPb, call + " nucmclr caller program");
        destinationCount++;
    }

    for (int i = 0; i < sourceCount; i++) {
        int source = sourceTable + i * MapTable::kEntryBytes;
        int destination = destinationTable + destinationCount * MapTable::kEntryBytes;
        for (int b = 0; b < MapTable::kEntryBytes; b++) m_.writeByte(destination + b, m_.readByte(source + b));

        int block = m_.readAddr24(destination + MapTable::kOffBlock);
        referenceInheritedMapObject(block, call + " nucmclr map");
        destinationCount++;
    }

    MapTable::setCount(m_, calleeRb, destinationCount);
    trace_.csp("{}: nucmclr copied {} caller map entr{} from rb {:06X} to rb {:06X}{} (callee mode {:02X}; "
               "c1891178..c189123C)",
               call, sourceCount, sourceCount == 1 ? "y" : "ies", callerRb, calleeRb,
               skipCallerProgram ? "" : " and prepended the caller program", mode);
    return destinationCount != 0;
}

// Take the copier's reference on a copied map object.  Request-block
// entries are an emulator-only representation of resident work-area
// storage and have no counters to change.
void As36ControlStorageProcessor::referenceInheritedMapObject(int block, const std::string& call)
{
    if (!heap_.contains(block)) return;
    uint16_t eye = m_.readHalf(block);
    if (eye != GuestLowStorage::kEyeProgramBlock && eye != GuestLowStorage::kEyeSystemBlock) return;
    includeInDomain(block, call);
    activateControlBlock(block, call);
}

// A 64-byte block with the "PB" eyecatcher carrying the transfer control
// table entry field for field.  Unless the attribute says the module is
// addressed in place, its bytes do not live at its logical address, and
// this is where its storage is decided.
int As36ControlStorageProcessor::buildProgramBlock(int sector, int sectors, uint8_t attribute, const std::string& call)
{
    int pb = heap_.allocate(ProgramBlock::kBytes, call + " program block");
    if (pb == 0) {
        trace_.csp("{}: no space for a program block", call);
        return 0;
    }
    ProgramBlock::build(m_, pb, sector, sectors, attribute);

    if ((attribute & ProgramBlock::kAttributeAddressedInPlace) == 0) {
        int modulePages = m_.readHalf(pb + ProgramBlock::kOffPageCount);
        if (modulePages == 0)
            modulePages = (sectors * storage::DiskBackend::kSectorBytes + machine::MachineState::kPageBytes - 1) >>
                          machine::MachineState::kPageShift;
        int storage = allocateModuleStorage(modulePages, call);
        if (storage == 0) return 0;
        moduleStorage_[pb] = storage;
        trace_.csp("{}: module storage {:05X} for program block {:04X} (main storage; IBM uses a transient work area block, "
                   "which a System/36's ATRs cannot address)",
                   call, storage, pb);
    }

    trace_.csp("{}: program block {:04X} built for sector {}, {} sector(s), attribute {:02X}, {} page(s)", call, pb, sector,
               sectors, attribute, (sectors + 7) / 8);
    return pb;
}

// Read the module, then stamp the module's own header into the block.  A
// machine with a real disk reads into the storage the module is addressed
// at, or into the module's own storage when it is not addressed in place.
bool As36ControlStorageProcessor::makeProgramBlockReady(int pb, const std::string& call)
{
    int sector = ProgramBlock::sector(m_, pb);
    int sectors = ProgramBlock::sectors(m_, pb);

    // The sector address is 1-based.
    long long first = static_cast<long long>(sector) - 1;
    if (first < 0 || first + sectors > disk_.sectorCount()) {
        trace_.csp("{}: sectors {}..{} are outside the volume", call, first, first + sectors - 1);
        return false;
    }

    std::vector<uint8_t> image(static_cast<std::size_t>(sectors) * storage::DiskBackend::kSectorBytes);
    for (int i = 0; i < sectors; i++)
        diskRead(first + i, image.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes, "transient");

    LoadMemberHeader hdr;
    if (!LoadMemberHeader::parse(image.data(), static_cast<int>(image.size()), hdr)) {
        trace_.csp("{}: the module image of {} byte(s) is shorter than a load member header", call, image.size());
        return false;
    }
    ProgramBlock::applyHeader(m_, pb, hdr);

    recordLoadedMember(pb, hdr.name.data(), static_cast<int>(hdr.name.size()), first);

    // pb+52 bit 0x02 clear goes straight to the read; only with the bit SET
    // does the contents manager decide, and only its "unchanged" answer
    // takes the module where it is addressed.
    bool inPlace = (ProgramBlock::attribute(m_, pb) & ProgramBlock::kAttributeAddressedInPlace) != 0;
    int at = inPlace ? ProgramBlock::moduleAddress(m_, pb) : moduleBytes(pb);
    if (at == 0 || at + static_cast<int>(image.size()) > m_.backingBytes()) {
        trace_.csp("{}: module wants guest {:04X} for {} bytes, past the end of main storage", call, at, image.size());
        return false;
    }

    // The contents manager is an IDENTITY cache: the same block still
    // owning the system transient storage restores pb+20 from pb+18 without
    // reading; a different block first invalidates the old owner.
    if (inPlace && currentTransientProgramBlock_ == pb) {
        m_.writeHalf(pb + ProgramBlock::kOffPagesReady, static_cast<uint16_t>(ProgramBlock::pageCount(m_, pb)));
        trace_.csp("{}: PB {:04X} still owns system-transient storage at {:04X}; NuMsContentsMgmt returns 0 without a read",
                   call, pb, at);
    } else {
        if (inPlace) invalidateSystemTransientOwner(call + " transient replacement");
        m_.write(at, image.data(), static_cast<int>(image.size()));
        if (inPlace) currentTransientProgramBlock_ = pb;
        trace_.csp("{}: read {} sector(s) from 0-based {} to {:05X}{}", call, sectors, first, at,
                   inPlace ? " (addressed in place)" : " - the module's own storage");
    }

    trace_.csp("{}: header {} -> load page {} (guest {:04X}), {} page(s)", call, hdr.toString(), hdr.loadPage, at,
               ProgramBlock::pageCount(m_, pb));
    return true;
}

// Invalidate the block displaced from real system-transient storage.  It
// remains hashed, but a later transfer or return must reacquire it.
void As36ControlStorageProcessor::invalidateSystemTransientOwner(const std::string& call)
{
    int old = currentTransientProgramBlock_;
    if (old == 0) return;
    if (m_.readHalf(old) == GuestLowStorage::kEyeProgramBlock) {
        m_.writeHalf(old + ProgramBlock::kOffPagesReady, 0);
        m_.writeAddr24(old + ProgramBlock::kOffOwningTask, 0xFF0000);
        m_.writeByte(old + ProgramBlock::kOffFlags57, static_cast<uint8_t>(m_.readByte(old + ProgramBlock::kOffFlags57) & ~0x40));
    }
    currentTransientProgramBlock_ = 0;
    trace_.csp("{}: system-transient owner PB {:04X} invalidated; pages-ready cleared", call, old);
}

// Invalidate the owner's identity when another loader overwrites any part
// of its real system-transient image.
void As36ControlStorageProcessor::invalidateSystemTransientForWrite(int at, int bytes, const std::string& call)
{
    int old = currentTransientProgramBlock_;
    if (old == 0 || bytes <= 0) return;
    int first = ProgramBlock::moduleAddress(m_, old);
    int length = ProgramBlock::pageCount(m_, old) << ProgramBlock::kLoadPageShift;
    if (at < first + length && first < at + bytes) invalidateSystemTransientOwner(call);
}

// Is the module already in main storage where it wants to be?  Compared on
// the header only: a module rewrites its body as it runs but not its first
// nineteen bytes.  Emulator policy.
bool As36ControlStorageProcessor::alreadyResident(int at, const std::vector<uint8_t>& image)
{
    for (int i = 0; i <= LoadMemberHeader::kOffSectors; i++)
        if (m_.readByte(at + i) != image[static_cast<std::size_t>(i)]) return false;
    return true;
}

// pb+63 is a FLOOR, not the length: the block is pb+63 + 4 + (pb+57 & 0x0F)
// units of 16 bytes, plus (rb+40 + 2) / 2 unless pb+56 & 0xA0 == 0xA0.  The
// last term is precisely the space the callee needs for the caller's map
// table plus one unit of slack.
int As36ControlStorageProcessor::requestBlockUnits(int pb, int callerRb)
{
    uint8_t mode = ProgramBlock::mode(m_, pb);
    int units = m_.readByte(pb + ProgramBlock::kOffRequestBlockUnits) + 4 + (m_.readByte(pb + ProgramBlock::kOffFlags57) & 0x0F);
    if ((mode & 0xA0) != 0xA0) units += (m_.readByte(callerRb + RequestBlock::kOffMapEntryCount) + 2) / 2;
    return units;
}

// Resolve an entry point number against a loaded module: 0 is the load
// address; a zero table offset at module+10 likewise; otherwise the
// halfword at module + offset + (n-1)*2.  Entry point 0 meaning "the start
// of the program" is SA21-9436's own sentence, implemented as a skip rather
// than as entry zero of the table, which is why the index is n-1.
int As36ControlStorageProcessor::resolveEntryPoint(int moduleAddress, int entryPointNumber)
{
    if (entryPointNumber == 0) return moduleAddress;

    int tableOffset = m_.readHalf(moduleAddress + kEntryPointTableOffset);
    if (tableOffset == 0) return moduleAddress;

    int at = moduleAddress + tableOffset + (entryPointNumber - 1) * 2;
    int entry = m_.readHalf(at);
    trace_.csp("entry point {}: table at module+{:04X}, halfword at {:04X} = {:04X}", entryPointNumber, tableOffset, at,
               entry);
    return entry;
}

// ---- SVC 0C and 0D --------------------------------------------------------------------

// A fast transfer table word, and the save area, share one four-byte
// shape: byte 0 the mode/privilege byte (rb+19), byte 1 the PACT prefix
// (rb+13 and rb+7), bytes 2-3 the instruction address (rb+24).
int As36ControlStorageProcessor::readTransferVector(int at)
{
    return (m_.readByte(at) << 24) | (m_.readByte(at + 1) << 16) | m_.readHalf(at + 2);
}

void As36ControlStorageProcessor::writeTransferVector(int at, uint8_t mode, uint8_t prefix, uint16_t iar)
{
    m_.writeByte(at, mode);
    m_.writeByte(at + 1, prefix);
    m_.writeHalf(at + 2, iar);
}

// Apply a four-byte transfer vector to a request block: the address to
// rb+24, the prefix to rb+13 AND rb+7.
void As36ControlStorageProcessor::applyTransferVector(int rb, int vector)
{
    uint8_t prefix = static_cast<uint8_t>((vector >> 16) & 0xFF);
    m_.writeHalf(rb + RequestBlock::kOffIar, static_cast<uint16_t>(vector));
    m_.writeByte(rb + RequestBlock::kOffPiar, prefix);
    m_.writeByte(rb + RequestBlock::kOffPdir, prefix);
}

// SVC 0C, Fast Transfer: "passes control from the requester to a routine
// that is resident in main storage ... The requested routine receives
// control with dispatching disabled and in privileged mode."  The caller's
// {rb+19, rb+13, rb+24} are saved into the table's own save area, the mode
// byte rewritten (bit 0x01 clear is privileged), and the new address and
// prefix loaded out of the selected entry.
bool As36ControlStorageProcessor::fastTransfer(SvcRequest& req)
{
    int offset = req.inline1 & 0xFC;
    if (offset > kFastTransferLastEntry) {
        trace_.csp("SVC 0C: inline 1 = {:02X} is past the last documented fast transfer entry ({:02X}, folder "
                   "management); the table's real extent is not established",
                   req.inline1, kFastTransferLastEntry);
        return false;
    }

    int entryAt = kFastTransferEntries + offset;
    int vector = readTransferVector(entryAt);
    if (vector == 0) {
        trace_.csp("SVC 0C: fast transfer table entry {:02X} at guest {:04X} is zero - the resident routine has not "
                   "registered itself. Nothing in SLIC writes this table (slicfield.py 628 finds no store), so SSP builds it "
                   "and it is genuinely empty",
                   offset, entryAt);
        return false;
    }

    uint8_t mode = m_.readByte(req.requestBlock + RequestBlock::kOffPrivilege);
    writeTransferVector(kFastTransferSaveArea, mode, m_.readByte(req.requestBlock + RequestBlock::kOffPiar),
                        m_.readHalf(req.requestBlock + RequestBlock::kOffIar));

    m_.writeByte(req.requestBlock + RequestBlock::kOffPrivilege,
                 static_cast<uint8_t>((mode & ~kFastTransferModeClear) | kFastTransferModeSet));

    applyTransferVector(req.requestBlock, vector);

    trace_.csp("SVC 0C: entry {:02X} -> {:04X} prefix {:02X}; saved {:04X} prefix {:02X} mode {:02X} at guest {:04X}. "
               "Dispatching would be disabled (NuEmul[0x671] = 1) but there is no dispatcher to disable",
               offset, vector & 0xFFFF, (vector >> 16) & 0xFF, m_.readHalf(kFastTransferSaveArea + 2),
               m_.readByte(kFastTransferSaveArea + 1), m_.readByte(kFastTransferSaveArea), kFastTransferSaveArea);
    return true;
}

// SVC 0D, Fast Exit: restore all four saved bytes, including the mode byte,
// and with Q bit 7 on rewrite the request block into an SVC 04 with rb+48
// bit 0x40 set ("the fast exit instruction is changed to an overlapped SVC
// with an R-Byte of hex 04"), which is what lets it reach a privileged
// target.
bool As36ControlStorageProcessor::fastExit(SvcRequest& req)
{
    constexpr uint8_t kTransferOnward = 0x01;   // Q bit 7

    int saved = readTransferVector(kFastTransferSaveArea);
    applyTransferVector(req.requestBlock, saved);
    m_.writeByte(req.requestBlock + RequestBlock::kOffPrivilege, static_cast<uint8_t>(static_cast<unsigned>(saved) >> 24));

    if ((req.q & kTransferOnward) == 0) {
        trace_.csp("SVC 0D: returned to {:04X} prefix {:02X} mode {:02X} from the save area at guest {:04X}", saved & 0xFFFF,
                   (saved >> 16) & 0xFF, (static_cast<unsigned>(saved) >> 24) & 0xFF, kFastTransferSaveArea);
        return true;
    }

    m_.writeByte(req.requestBlock + RequestBlock::kOffRByte, 0x04);
    m_.writeByte(req.requestBlock + RequestBlock::kOffTransferFlags,
                 static_cast<uint8_t>(m_.readByte(req.requestBlock + RequestBlock::kOffTransferFlags) |
                                      RequestBlock::kTransferFromFastExit));
    trace_.csp("SVC 0D: Q bit 7 on - rb+22 rewritten to 04 and rb+48 bit 40 set, now transferring by identifier {}",
               req.inline1);

    req.r = 0x04;
    return transferControlById(req);
}

// ---- SVC 22 ----------------------------------------------------------------------------

// Dump Task / Terminate Task: "ends the calling task with the message
// identification (error) code passed in inline parameters 1 and 2".  The
// "this task" case reads queue header 37; the MIC has its top bit stripped;
// MIC 0000 continues; MIC 0038 dumps without terminating; MICs below 0100
// (other than 0010) are a system program check that stops the machine;
// the rest continue through transfer slot 1D, the guest termination
// program.
bool As36ControlStorageProcessor::dumpTask(SvcRequest& req)
{
    constexpr uint8_t kTaskBlockInXr1 = 0x01;   // Q bit 7
    constexpr int kDumpNoAbend = 0x38;

    int tb = (req.q & kTaskBlockInXr1) != 0 ? RequestBlock::readXr1Field(m_, req.requestBlock)
                                             : m_.readAddr24(GuestLowStorage::queueHeader(kDumpTaskQueueHeader));

    int raw = (req.inline1 << 8) | req.inline2;
    int mic = raw >= 0x8000 ? raw - 0x8000 : raw;

    // A second dump request on a task already in abnormal termination is a
    // recursive abend and goes directly to the system stop.
    uint8_t status = m_.readByte(tb + TaskBlock::kOffStatus);
    bool recursive = (status & TaskBlock::kStatusAbnormalTermination) != 0;
    if (recursive) {
        // The reference's CrustyInteractiveSession reap is an experiment that
        // is off by default and is not ported.
        trace_.csp("SVC 22: recursive abnormal termination in task {:04X} (tb+32={:02X}); nuerdump goes directly to nuerabt "
                   "code 2D at c18d8b80, so the machine stops",
                   tb, status);
        return false;
    }

    if (mic == 0) {
        trace_.csp("SVC 22: MIC 0000 - no termination, control continues at {:04X}",
                   m_.readHalf(req.requestBlock + RequestBlock::kOffIar));
        return true;
    }

    // The DECIMAL MIC is stored, not the hex one.
    int shown = hexToDecimal(mic);
    m_.writeHalf(GuestLowStorage::kLowStorage, static_cast<uint16_t>(shown));
    m_.writeHalf(tb + TaskBlock::kOffMic, static_cast<uint16_t>(shown));

    if (mic == kDumpNoAbend) {
        trace_.csp("SVC 22: task block {:04X} MIC 0038 - a task dump is taken but the task is NOT terminated (SA21-9436 "
                   "3-109; nuab's r20 = 65 arm at c18cc810 leaves tb+32 bit 10 clear). tb+62 = 0x0800 = {:04X}, control "
                   "continues at {:04X}",
                   tb, shown, m_.readHalf(req.requestBlock + RequestBlock::kOffIar));
        return true;
    }

    m_.writeByte(tb + TaskBlock::kOffStatus, static_cast<uint8_t>(status | TaskBlock::kStatusAbnormalTermination));

    bool hasGuestTerminationContinuation = mic >= 0x0100 || mic == 0x0010;
    const char* effect = !hasGuestTerminationContinuation
                             ? "0001-00FF, so the system program checks - nuab reaches nuerabt at c18cc828, which is "
                               "NuEmul::abTermSys: the machine stops and displays it"
                             : "0100 or greater, so a task dump is taken, a message issued and the task continues through "
                               "system transfer slot 1D (#FETDP on this SSP)";

    std::string source = req.sourceIar == 0
                             ? std::string()
                             : fmt::format(" issued at {:04X}{}", req.sourceIar,
                                           req.sourceMember.empty() ? std::string() : " in " + req.sourceMember);
    trace_.csp("SVC 22: task block {:04X} ends abnormally with MIC {:04X}{}{} (decimal {:04X}) - {}. tb+32 |= 10 (abnormal "
               "termination){}, tb+62 and guest 0800 = {:04X}",
               tb, mic, raw != mic ? fmt::format(" (from {:04X})", raw) : std::string(), source, shown, effect, "", shown);

    if (!hasGuestTerminationContinuation) return false;

    // The tail: the abending task and its live request block stay current,
    // the supervisor call is stamped, and table+145 is entered directly.
    // The dump, message and resource cleanup before this tail are not
    // modelled; reaching the guest termination program is the disposition.
    int table = directArea_.read(DirectArea::kTransferControlTable);
    int rb = m_.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (table == 0 || rb == 0) {
        trace_.csp("SVC 22: nuab cannot enter slot 1D: table={:04X}, rb={:04X}", table, rb);
        return false;
    }

    currentTaskBlock_ = tb;
    currentRequestBlock_ = rb;
    restoreRegisters(rb);
    SvcRequest continuation;
    continuation.r = 0x04;
    continuation.q = 0x01;
    continuation.inline1 = kAbnormalTerminationTransferId;
    continuation.requestBlock = rb;
    continuation.taskBlock = tb;
    stampRequest(rb, continuation);

    // rb+20..23 becomes the encoded SVC image (the inline byte is stored
    // separately at rb+16); tb+4..5 is cleared and tb+52 advanced.
    m_.writeByte(rb + 20, 0xF4);
    m_.writeByte(rb + RequestBlock::kOffPsr, 0x01);
    m_.writeHalf(tb + TaskBlock::kOffState, 0);
    m_.writeByte(tb + 52, static_cast<uint8_t>(m_.readByte(tb + 52) + 1));
    int entry = table + kTransferControlEntryBytes * kAbnormalTerminationTransferId;
    trace_.csp("SVC 22: nuab internal continuation -> transfer table {:04X} + 5 x 1D = {:04X}; task {:04X}, request block "
               "{:04X} (c18cdc6c..c18cdc88)",
               table, entry, tb, rb);
    bool continued = transferControl(continuation, "SVC 22 nuab slot 1D", entry, 0);
    if (continued) {
        // The native context assignment succeeded before this transfer on
        // the machine and is recorded in tb+32 bit 01; the per-task
        // continuation is the corresponding native context.
        uint8_t liveStatus = m_.readByte(tb + TaskBlock::kOffStatus);
        m_.writeByte(tb + TaskBlock::kOffStatus, static_cast<uint8_t>(liveStatus | TaskBlock::kStatusNuabContextAssigned));
        pushNativeTransferContinuation(tb, NativeTransferContinuation::NuabSlot1D);
        trace_.csp("SVC 22: nuab slot 1D is active for task {:04X}; tb+32 {:02X}->{:02X} (nuasgncs success and OR 01 at "
                   "c18cc884-c18cc8b4); its nested nupterm will return to the native continuation instead of re-entering "
                   "slot 4",
                   tb, liveStatus, m_.readByte(tb + TaskBlock::kOffStatus));
        redispatch_ = false;
    }
    return continued;
}

// Binary to packed decimal: everything that shows a MIC to a human runs
// it first, so hex 1F is 0031.
int As36ControlStorageProcessor::hexToDecimal(int v)
{
    int r = 0;
    for (int shift = 12; shift >= 0; shift -= 4) {
        int unit = 1;
        for (int i = 0; i < shift / 4; i++) unit *= 10;
        r |= (v / unit % 10) << shift;
    }
    return r;
}

// Read-only: the terminal-unit-block builder's replacement/clone decision,
// decoded from its configuration record at entry.
void As36ControlStorageProcessor::traceSvtubCloneGate(int old, int cfg)
{
    if (cfg <= 0 || cfg + 21 > m_.backingBytes()) {
        trace_.csp("#SVTUB entry: config record {:06X} is not readable", cfg);
        return;
    }
    uint8_t c0 = m_.readByte(cfg), c3 = m_.readByte(cfg + 3);
    uint8_t c13 = m_.readByte(cfg + 13), c20 = m_.readByte(cfg + 20);
    bool oldReadable = old > 0 && old + 0x84 <= m_.backingBytes();
    uint16_t eye = oldReadable ? m_.readHalf(old) : static_cast<uint16_t>(0);
    bool rebuild = (c20 & 0x01) != 0;
    bool terminal = c3 == 0xC0;
    bool cloneKind = (c0 & 0x01) != 0;
    bool cloneRequested = (c13 & 0x02) != 0;
    bool cloneSuppressed = (c13 & 0x01) != 0;
    const char* decision = !rebuild          ? "1576 exits the rebuild tail; 1054 takes the ordinary build path"
                           : !terminal       ? "+3 != C0: printer path exits before cloning"
                           : !cloneKind      ? "1581 rejects the clone"
                           : !cloneRequested ? "15A3 exits before cloning"
                           : cloneSuppressed ? "15B2 diverts around the clone body"
                                             : "all five pass: clone body at 15B6 is eligible";
    trace_.csp("#SVTUB entry: old {:06X} (eye {:04X}{}), cfg {:06X} +0={:02X} id={:04X} +3={:02X} +13={:02X} +20={:02X}; "
               "gates +20.01={} +3=C0={} +0.01={} +13.02={} +13.01clear={} -> {}",
               old, eye, eye == devices::WorkStationIob::kUnitBlockEyecatcher ? " TU" : eye == 0xD7E4 ? " PU" : "", cfg, c0,
               m_.readHalf(cfg + 1), c3, c13, c20, rebuild ? "y" : "n", terminal ? "y" : "n", cloneKind ? "y" : "n",
               cloneRequested ? "y" : "n", !cloneSuppressed ? "y" : "n", decision);
    std::string sb;
    for (int i = 0; i < 32 && cfg + i < m_.backingBytes(); i++) sb += fmt::format("{:02x}", m_.readByte(cfg + i));
    trace_.csp("#SVTUB entry:   cfg bytes {}", sb);
    if (oldReadable) {
        std::string ob;
        for (int i = 0; i < 16; i++) ob += fmt::format("{:02x}", m_.readByte(old + i));
        trace_.csp("#SVTUB entry:   old bytes {}  +2A={:02X} +7C={:02X} +7E={:02X}", ob, m_.readByte(old + 0x2A),
                   m_.readByte(old + 0x7C), m_.readByte(old + 0x7E));
    }
}

// ---- SVC 52, the relocating loader ------------------------------------------------------

// Index register 2 addresses a 17-byte parameter list (+0..2 disk address,
// +3..4 sectors, +5..7 link address, +8..10 start control address, +11 the
// relocation directory's byte offset, +14..16 the explicit load address);
// inline parameter 1 is a bit mask (0x01 relative, 0x02 to address, 0x04
// fetch, 0x08 system, 0x10 memory resident overlay); the answer is a module
// in storage, optionally relocated, optionally with control passed to it.
// Delayed and NOT privileged, so any program may issue it.
bool As36ControlStorageProcessor::mainStorageRelocatingLoader(SvcRequest& req)
{
    uint8_t type = req.inline1;
    int rb = req.requestBlock;
    int tb = req.taskBlock;

    if ((req.q & ~kLoaderQNotRefreshable) != 0) {
        trace_.csp("SVC 52: Q-byte {:02X} has bits the manual reserves; only bit 6 (0x02, reusable program is not "
                   "refreshable) is defined (3-145)",
                   req.q);
    }
    if ((req.q & kLoaderQNotRefreshable) != 0) {
        trace_.csp("SVC 52: Q bit 6 asks that the reusable program not be refreshable. s36refemu has no reusable-program "
                   "ownership model, so the request is recorded and the load proceeds");
    }

    if ((type & kLoaderMemoryResidentOverlay) != 0) {
        // This arm resolves the relative address and hands off to the
        // memory resident overlay pool, which is the machine's own native
        // state and not a guest control block; no pool can be sourced.
        trace_.csp("SVC 52: request type {:02X} bit 10 is 'load a memory resident overlay by relative address'. nuLdr "
                   "resolves the address and calls nupmro/nupstma (c1894194, c18941b4) - the overlay POOL, which lives in "
                   "SLIC's own object and not in guest storage. No pool can be sourced "
                   "(docs/s36/svc-relocating-loader.md)",
                   type);
        return false;
    }

    int listAddress = RequestBlock::readXr2Field(m_, rb);
    int list;
    if (!resolveTranslated(listAddress, list)) {
        trace_.csp("SVC 52: the parameter list XR2 = {:06X} is translated and its page is not mapped", listAddress);
        return false;
    }
    if (list < 0 || list + kLoaderPlBytes > m_.backingBytes()) {
        trace_.csp("SVC 52: the parameter list at {:06X} runs past the end of main storage", list);
        return false;
    }

    int diskAddress = m_.readAddr24(list + kLoaderPlDiskAddress);
    int sectors = m_.readHalf(list + kLoaderPlSectors);
    int linkAddress = m_.readAddr24(list + kLoaderPlLinkAddress);
    int startControl = m_.readAddr24(list + kLoaderPlStartControl);
    int rldOffset = m_.readByte(list + kLoaderPlRldOffset);
    int loadAddress = m_.readAddr24(list + kLoaderPlLoadAddress);

    // The relocation base: what the post-processing subtracts from the
    // load address.
    int relocationBase = linkAddress;

    if ((type & kLoaderToAddress) == 0) {
        // Without "to address" the load address is built from the LINK
        // address, and +14..16 is not consulted at all.
        loadAddress = linkAddress;

        if ((type & kLoaderRelativeAddress) != 0) {
            // Only 16 bits of the relative address are kept before the
            // task's base is added.
            relocationBase = linkAddress - m_.readHalf(tb + kTbRelocationFactor);
            diskAddress = (diskAddress & 0xFFFF) + m_.readAddr24(tb + kTbLoaderDiskAddress);
        } else {
            // "Adds the task relocation factor to the module's link-edit
            // address and uses the result as the load address."
            loadAddress = linkAddress + m_.readHalf(tb + kTbRelocationFactor);
        }
    }

    // The "system" types update the task block BEFORE the read; the relative
    // arm jumps straight past this, so a "system" bit on a relative request
    // is not honoured by the machine.
    if ((type & kLoaderSystem) != 0 && (type & kLoaderRelativeAddress) == 0) {
        m_.writeHalf(tb + kTbRelocationFactor, static_cast<uint16_t>(loadAddress - relocationBase));
        m_.writeAddr24(tb + kTbLoaderDiskAddress, diskAddress);
        trace_.csp("SVC 52: system request - task block {:04X} relocation factor := {:04X}, loader disk address := {:06X}",
                   tb, (loadAddress - relocationBase) & 0xFFFF, diskAddress);
    }

    int at;
    if (!resolveTranslated(loadAddress, at)) {
        trace_.csp("SVC 52: the load address {:06X} is translated and its page is not mapped", loadAddress);
        return false;
    }

    trace_.csp("SVC 52: type {:02X}{}{}{}{} - {} sector(s) from 1-based {} to {:06X} (real {:06X}); link {:06X}, start "
               "control {:06X}",
               type, (type & kLoaderRelativeAddress) != 0 ? " relative" : "", (type & kLoaderToAddress) != 0 ? " to-address" : "",
               (type & kLoaderFetch) != 0 ? " fetch" : " load", (type & kLoaderSystem) != 0 ? " system" : "", sectors,
               diskAddress, loadAddress, at, linkAddress, startControl);

    if (!readModule(diskAddress, sectors, at)) return false;

    // The relocation transient runs when, and only when, the load address
    // differs from the link address, tested on the LOW HALFWORD.
    int delta = (loadAddress - relocationBase) & 0xFFFF;
    if (delta != 0) {
        if (!relocateModule(at, delta, diskAddress, sectors, rldOffset)) return false;
        // The start control address moves with the module.
        startControl = (startControl + delta) & 0xFFFFFF;
    } else {
        trace_.csp("SVC 52: load address {:06X} equals link address {:06X}, so no relocation is required (3-146, and "
                   "nuLdrPostProcess c1893d5c)",
                   loadAddress, relocationBase);
    }

    if ((type & kLoaderFetch) != 0) {
        // The same three fields a transfer writes: rb+24 the instruction
        // address, rb+13 the instruction-fetch prefix, rb+7 the
        // direct-operand prefix.
        uint8_t prefix = static_cast<uint8_t>((startControl >> 16) & 0xFF);
        m_.writeHalf(rb + RequestBlock::kOffIar, static_cast<uint16_t>(startControl & 0xFFFF));
        m_.writeByte(rb + RequestBlock::kOffPiar, prefix);
        m_.writeByte(rb + RequestBlock::kOffPdir, prefix);
        trace_.csp("SVC 52: FETCH - control passes to the module's start control address {:06X} (iar {:04X}, piar and pdir "
                   "{:02X})",
                   startControl, startControl & 0xFFFF, prefix);
    }
    return true;
}

// The loader's read: an ordinary read of the module's sectors, issued the
// way SVC 51's get arm issues one.
bool As36ControlStorageProcessor::readModule(int diskAddress, int sectors, int at)
{
    if (sectors <= 0) {
        trace_.csp("SVC 52: the parameter list asks for {} sectors; nothing is read", sectors);
        return false;
    }
    long long sector = static_cast<long long>(diskAddress) - 1;   // the wire address is 1-based
    if (sector < 0 || sector + sectors > disk_.sectorCount()) {
        trace_.csp("SVC 52: sectors {}..{} are outside the volume ({})", sector, sector + sectors - 1, disk_.sectorCount());
        return false;
    }
    int bytes = sectors * storage::DiskBackend::kSectorBytes;
    if (at < 0 || at + bytes > m_.backingBytes()) {
        trace_.csp("SVC 52: {} bytes at guest {:06X} run past the end of main storage", bytes, at);
        return false;
    }
    std::vector<uint8_t> image(static_cast<std::size_t>(bytes));
    for (int i = 0; i < sectors; i++)
        diskRead(sector + i, image.data() + static_cast<std::ptrdiff_t>(i) * storage::DiskBackend::kSectorBytes, "loader");
    invalidateSystemTransientForWrite(at, bytes, "SVC 52 loader write");
    m_.write(at, image.data(), bytes);
    return true;
}

// The relocation transient.  The directory is a run-length list of
// displacements on DISK: each byte advances the cursor; hex FD is an error;
// hex 80 advances 128 bytes and relocates nothing; a byte above 0x80
// cancels its own advance, and hex FF among those ends the directory;
// otherwise the HALFWORD ENDING at the cursor is relocated.  The directory
// starts at sector disk address + length, one lower when the parameter
// list's byte offset is non-zero, and is re-read at every 256-byte
// boundary.
bool As36ControlStorageProcessor::relocateModule(int at, int delta, int diskAddress, int sectors, int rldOffset)
{
    long long sector = static_cast<long long>(diskAddress) + sectors;
    if (rldOffset != 0) sector -= 1;
    sector -= 1;   // the wire address is 1-based

    int cursor = at;
    int offset = rldOffset;
    uint8_t page[storage::DiskBackend::kSectorBytes] = {};
    long long pageSector = -1;
    int relocated = 0;

    // The directory is a stream with an in-band terminator, so a bound is
    // needed or a corrupt one runs the volume: one byte can cover at most
    // 255 bytes of module.
    int limit = sectors * storage::DiskBackend::kSectorBytes + 2;

    for (int step = 0; step < limit; step++) {
        if (sector < 0 || sector >= disk_.sectorCount()) {
            trace_.csp("SVC 52: the relocation directory runs off the volume at sector {}", sector);
            return false;
        }
        if (pageSector != sector) {
            diskRead(sector, page, "loader page");
            pageSector = sector;
        }
        uint8_t b = page[offset];

        cursor += b;
        if (b == kRldInvalid) {
            trace_.csp("SVC 52: relocation directory byte FD at sector {}+{} - nulx raises nuerr (c1893f2c)", sector, offset);
            return false;
        }

        offset++;
        if (offset >= storage::DiskBackend::kSectorBytes) {
            offset = 0;
            sector++;
        }

        if (b > kRldSkip) {
            cursor -= b;
            if (b == kRldEnd) {
                trace_.csp("SVC 52: relocated {} address(es) by {:04X}", relocated, delta);
                return true;
            }
            continue;
        }
        if (b == kRldSkip) continue;

        if (cursor - 1 < 0 || cursor + 1 > m_.backingBytes()) {
            trace_.csp("SVC 52: the relocation cursor left main storage at {:06X}", cursor);
            return false;
        }
        m_.writeHalf(cursor - 1, static_cast<uint16_t>(m_.readHalf(cursor - 1) + delta));
        relocated++;
    }
    trace_.csp("SVC 52: the relocation directory has no FF terminator within {} bytes", limit);
    return false;
}

}  // namespace sim36::processors::controlstorage
