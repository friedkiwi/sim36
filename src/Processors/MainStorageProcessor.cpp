#include "Processors/MainStorageProcessor.h"

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "Processors/ControlStorage/IControlStorageProcessor.h"

namespace sim36::processors {

using machine::MachineState;
using machine::MspRegisters;

namespace {

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool containsIgnoreCase(const std::string& haystack, const std::string& needle)
{
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size() && match; ++k)
            if (std::tolower(static_cast<unsigned char>(haystack[i + k])) !=
                std::tolower(static_cast<unsigned char>(needle[k])))
                match = false;
        if (match) return true;
    }
    return false;
}

}  // namespace

// ---- breakpoints ---------------------------------------------------------

void MainStorageProcessor::setBreakpoint(uint16_t address, const std::string& description)
{
    breakpoints_[address] = description;
    // A fresh set means break next time, even if we halted here.
    if (haveResumePastBreak_ && resumePastBreak_ == address) haveResumePastBreak_ = false;
}

bool MainStorageProcessor::clearBreakpoint(uint16_t address)
{
    if (haveResumePastBreak_ && resumePastBreak_ == address) haveResumePastBreak_ = false;
    return breakpoints_.erase(address) != 0;
}

void MainStorageProcessor::setMemberBreakpoint(uint16_t address, uint16_t offset, const std::string& member,
                                               const std::string& description)
{
    std::vector<MemberBreakpoint>& list = memberBreakpoints_[address];
    MemberBreakpoint bp;
    bp.address = address;
    bp.offset = offset;
    bp.member = member;
    bp.description = description;
    bool replaced = false;
    for (MemberBreakpoint& existing : list)
        if (equalsIgnoreCase(existing.member, member)) { existing = bp; replaced = true; break; }
    if (!replaced) list.push_back(bp);
    if (haveResumePastBreak_ && resumePastBreak_ == address) haveResumePastBreak_ = false;
}

bool MainStorageProcessor::clearMemberBreakpoint(uint16_t address, const std::string& member)
{
    auto it = memberBreakpoints_.find(address);
    if (it == memberBreakpoints_.end()) return false;
    std::size_t before = it->second.size();
    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                    [&](const MemberBreakpoint& x) { return equalsIgnoreCase(x.member, member); }),
                     it->second.end());
    bool removed = it->second.size() != before;
    if (it->second.empty()) memberBreakpoints_.erase(it);
    if (removed && haveResumePastBreak_ && resumePastBreak_ == address) haveResumePastBreak_ = false;
    return removed;
}

void MainStorageProcessor::clearAllBreakpoints()
{
    breakpoints_.clear();
    memberBreakpoints_.clear();
    haveResumePastBreak_ = false;
}

std::vector<MainStorageProcessor::MemberBreakpoint> MainStorageProcessor::memberBreakpoints() const
{
    std::vector<MemberBreakpoint> all;
    for (const auto& kv : memberBreakpoints_) all.insert(all.end(), kv.second.begin(), kv.second.end());
    return all;
}

void MainStorageProcessor::applyPatchesAt(uint16_t iar)
{
    if (patches_.empty()) return;
    auto it = patches_.find(iar);
    if (it == patches_.end()) return;
    for (const IarPatch& p : it->second) {
        switch (p.kind) {
            case 0: m_.writeByte(p.a, static_cast<uint8_t>(p.v)); break;
            case 1: m_.writeHalf(p.a, static_cast<uint16_t>(p.v)); break;
            case 2: m_.msp.wr[p.a & 0xF] = static_cast<uint16_t>(p.v); break;
            case 3: m_.msp.xr1 = static_cast<uint16_t>(p.v); break;
            case 4: m_.msp.xr2 = static_cast<uint16_t>(p.v); break;
            default: break;
        }
        trace_.msp(iar, "PATCH @{:04X}: kind {} target {:04X} := {:04X} (continues)", iar, p.kind, p.a, p.v);
    }
}

bool MainStorageProcessor::atActiveBreakpoint(uint16_t iar, std::string& description)
{
    description.clear();
    if (breakpoints_.empty() && memberBreakpoints_.empty()) return false;
    auto ordinary = breakpoints_.find(iar);

    auto qualified = memberBreakpoints_.find(iar);
    if (qualified != memberBreakpoints_.end() && memberNameResolver) {
        const std::string active = memberNameResolver();
        for (const MemberBreakpoint& x : qualified->second) {
            if (!equalsIgnoreCase(x.member, active) || x.member.empty()) continue;
            if (haveResumePastBreak_ && resumePastBreak_ == iar) { haveResumePastBreak_ = false; return false; }
            description = x.description.empty() ? fmt::format("{}+{:04X}", x.member, x.offset)
                                                : fmt::format("{}+{:04X}: {}", x.member, x.offset, x.description);
            return true;
        }
    }

    if (ordinary == breakpoints_.end()) return false;
    // Filter by module BEFORE the step-past bookkeeping, so a non-target
    // module hitting the same aliased IAR neither halts nor consumes the flag.
    if (!breakMemberFilter.empty() && memberResolver) {
        if (!containsIgnoreCase(memberResolver(), breakMemberFilter)) return false;
    }
    if (haveResumePastBreak_ && resumePastBreak_ == iar) { haveResumePastBreak_ = false; return false; }
    description = ordinary->second;
    return true;
}

// ---- state -----------------------------------------------------------------

void MainStorageProcessor::reset()
{
    m_.msp.reset();
    stopped_ = false;
    stopReason_.clear();
    atPreemptionPoint_ = true;
    instructions_ = 0;
}

void MainStorageProcessor::restoreCheckpoint(bool stopped, const std::string& reason, long long instructions,
                                             bool atPreemptionPoint)
{
    stopped_ = stopped;
    stopReason_ = reason;
    instructions_ = instructions;
    atPreemptionPoint_ = atPreemptionPoint;
    iarWritten_ = false;
    haveResumePastBreak_ = false;
    lastBreakpointDescription_.clear();
}

void MainStorageProcessor::stop(const std::string& why)
{
    stopped_ = true;
    stopReason_ = why;
    // A deferred trace exists to be read at the stop it was armed for.
    trace_.flushDeferred(why);
}

// An addressing exception: a reference outside guest storage.  A program
// check (SA21-9436 2-6), reported under the `src` category before the stop.
void MainStorageProcessor::stopAddressing()
{
    const std::string message = m_.faultMessage();
    m_.clearFault();
    m_.reportCheck("program check - addressing exception", 0x0000, fmt::format("at IAR {:04X}: {}", m_.msp.iar, message));
    stop(message);
}

std::string MainStorageProcessor::protectionStop() const
{
    return fmt::format(
        "{}; PMR {:02X}, PACT iar {:02X}. This is a level 5 interrupt to the control "
        "processor (SA21-9436 1-30); it abnormally terminated the offending task, but "
        "no other task was ready to dispatch, so the machine cannot continue. "
        "docs/s36/storage-protection.md", protectionFault_.message(), m_.msp.pmr(), m_.msp.pactIar);
}

// Deliver a storage-protection violation to the control processor as the
// level 5 interrupt SA21-9436 1-30 specifies.  If a survivor was dispatched
// the MSP has already been repointed at it, so this step ends without
// retiring the faulting instruction.  If nothing is runnable, the machine
// stops.
bool MainStorageProcessor::recoverProtection()
{
    if (csp_.raiseStorageProtection(protectionFault_.address, protectionFault_.forWrite)) {
        atPreemptionPoint_ = true;   // a task switch is an architected preemption point
        return true;
    }
    stop(protectionStop());
    return false;
}

bool MainStorageProcessor::finishAbort()
{
    const Abort a = abort_;
    abort_ = Abort::None;
    if (a == Abort::Protection) { m_.clearFault(); return recoverProtection(); }
    stopAddressing();
    return false;
}

// ---- decode ----------------------------------------------------------------

int MainStorageProcessor::readWidth(int addr, int width)
{
    return width == 2 ? m_.readHalf(addr) : m_.readByte(addr);
}

Instruction MainStorageProcessor::decode(int address)
{
    Instruction insn;
    const uint8_t opcode = m_.readByte(address);
    const uint8_t q = m_.readByte(address + 1);
    const int rByte = InstructionSet::isSvc(opcode) ? m_.readByte(address + 2) : -1;
    const int len = InstructionSet::length(opcode, rByte);

    int w1, w2;
    InstructionSet::operandWidths(opcode, w1, w2);
    int p = address + 2;
    if (w1 > 0) { insn.operand1 = readWidth(p, w1); p += w1; }
    if (w2 > 0) { insn.operand2 = readWidth(p, w2); p += w2; }

    insn.raw.resize(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) insn.raw[static_cast<std::size_t>(i)] = m_.readByte(address + i);

    insn.address = address;
    insn.opcode = opcode;
    insn.q = q;
    insn.length = len;
    insn.mnemonic = InstructionSet::mnemonic(opcode);
    return insn;
}

// The PACT register is selected by the ADDRESSING MODE, not by which
// operand it is: each mode independently picks PDIR, PXR1 or PXR2, and the
// instruction fetch goes under PIAR.
uint16_t MainStorageProcessor::logical(OperandMode mode, int value, uint8_t& pact) const
{
    switch (mode) {
        case OperandMode::Direct: pact = m_.msp.pactDir; return static_cast<uint16_t>(value);
        case OperandMode::Xr1: pact = m_.msp.pactXr1; return static_cast<uint16_t>((m_.msp.xr1 + value) & 0xFFFF);
        case OperandMode::Xr2: pact = m_.msp.pactXr2; return static_cast<uint16_t>((m_.msp.xr2 + value) & 0xFFFF);
        default:
            // ARR-relative: the ARR has no PACT companion (SA21-9436 3-26).
            pact = m_.msp.pactDir;
            return static_cast<uint16_t>((m_.msp.arr + value) & 0xFFFF);
    }
}

uint16_t MainStorageProcessor::logicalOperand(OperandMode mode, int value) const
{
    uint8_t pact;
    return logical(mode, value, pact);
}

bool MainStorageProcessor::effective(OperandMode mode, int value, int& real)
{
    uint8_t pact;
    const uint16_t log = logical(mode, value, pact);
    if (m_.resolve(log, pact, MachineState::kAtrTaskGroup0, false, real, &protectionFault_)) return true;
    abort_ = Abort::Protection;
    return false;
}

// ---- execute ---------------------------------------------------------------

bool MainStorageProcessor::step()
{
    if (stopped_) return false;
    const int iar = m_.msp.iar;
    abort_ = Abort::None;
    m_.clearFault();

    // Patch-on-reach fires BEFORE the breakpoint check and BEFORE the fetch.
    applyPatchesAt(static_cast<uint16_t>(iar));

    // Halt BEFORE the fetch when the IAR reaches a breakpoint, so the
    // operator inspects storage exactly as it stands on entry.
    std::string bpDescription;
    if (atActiveBreakpoint(static_cast<uint16_t>(iar), bpDescription)) {
        haveResumePastBreak_ = true;
        resumePastBreak_ = static_cast<uint16_t>(iar);
        lastBreakpointDescription_ = bpDescription;
        stop(fmt::format("breakpoint at {:04X}{}", iar, bpDescription.empty() ? "" : " (" + bpDescription + ")"));
        return false;
    }

    if (onBeforeInstruction) onBeforeInstruction(static_cast<uint16_t>(iar));

    // Instruction fetch goes through PIAR, its own addressing path.  The IAR
    // itself stays LOGICAL; only the read is resolved.
    int fetch = 0;
    if (!m_.resolve(static_cast<uint16_t>(iar), m_.msp.pactIar, MachineState::kAtrTaskGroup0, false, fetch,
                    &protectionFault_))
        return recoverProtection();
    Instruction insn = decode(fetch);
    if (m_.faulted()) { stopAddressing(); return false; }
    insn.address = iar;

    if (!insn.valid()) {
        const int logicalPage = iar >> MachineState::kPageShift;
        const uint16_t atr = m_.atr[MachineState::kAtrTaskGroup0 + logicalPage];
        trace_.msp(iar, "unassigned opcode {:02X}; PIAR {:02X}, logical page {:02X}, ATR {:04X}, physical fetch {:06X}",
                   insn.opcode, m_.msp.pactIar, logicalPage, atr, fetch);
        // A program check: an invalid instruction.  The commonest shape is
        // the sign-on family's branch-to-0, opcode 00 at IAR 0000.
        m_.reportCheck("program check - unassigned opcode", 0x0000,
                       fmt::format("opcode {:02X} at {:04X}, PIAR {:02X}, ATR[{:02X}]={:04X}, physical {:06X}{}",
                                   insn.opcode, iar, m_.msp.pactIar, logicalPage, atr, fetch,
                                   insn.opcode == 0 && iar == 0 ? " (branch-to-0)" : ""));
        stop(fmt::format("unassigned opcode {:02X} at {:04X} (PIAR {:02X}, ATR[{:02X}]={:04X}, physical {:06X})",
                         insn.opcode, iar, m_.msp.pactIar, logicalPage, atr, fetch));
        return false;
    }

    trace_.msp(iar, "{}", insn.toString());
    if (trace_.isnOn() && memberMatchesTraceFilter()) {
        // Emitted BEFORE execution so every instruction is logged; the
        // registers are the state ENTERING the instruction.
        const MspRegisters& rg = m_.msp;
        trace_.isn("{:04X} {:<22} ARR={:04X} XR1={:02X}:{:04X} XR2={:02X}:{:04X} DIR={:02X} IAR={:02X} PSR={:02X} WR6={:04X}",
                   iar, insn.toString(), rg.arr, rg.pactXr1, rg.xr1, rg.pactXr2, rg.xr2, rg.pactDir, rg.pactIar,
                   rg.psr(), rg.wr[6]);
    }
    const int next = (iar + insn.length) & 0xFFFF;
    atPreemptionPoint_ = false;
    iarWritten_ = false;

    OperandMode m1, m2;
    InstructionSet::modes(insn.opcode, m1, m2);
    if (insn.operand1) op1Logical_ = logical(m1, *insn.operand1, op1Pact_);
    if (insn.operand2) op2Logical_ = logical(m2, *insn.operand2, op2Pact_);
    int a1 = 0, a2 = 0;
    const std::string mn = insn.mnemonic;
    // Control-flow instructions (BC, JC, SVC, XFER, LPMR) and LA do not use a
    // resolved storage address: a branch target, inline parameters, an
    // immediate PMR value or a computed address.  Resolving them through
    // the ATRs is wrong and FAULTS when ARR + the displacement lands in a
    // protected page.
    if ((insn.opcode & 0xF0) == 0xF0 || mn == "BC" || mn == "LA") {
        a1 = insn.operand1 ? logicalOperand(m1, *insn.operand1) : 0;
        a2 = insn.operand2 ? logicalOperand(m2, *insn.operand2) : 0;
    } else {
        if (insn.operand1 && !effective(m1, *insn.operand1, a1)) return finishAbort();
        if (insn.operand2 && !effective(m2, *insn.operand2, a2)) return finishAbort();
    }
    (void)a1;
    (void)a2;

    if (mn == "SVC") return executeSvc(insn, next);
    else if (mn == "MVC") moveCharacters(insn.q);
    else if (mn == "MVI") writeOperand(true, 0, insn.q);
    else if (mn == "CLC") compareCharacters(insn.q);
    else if (mn == "CLI") compareImmediate(insn.q);
    else if (mn == "ALC") addLogical(insn.q, false);
    else if (mn == "SLC") addLogical(insn.q, true);
    else if (mn == "SLI") subtractLogicalImmediate(insn.q);
    else if (mn == "TBN") { testBits(insn.q, true); atPreemptionPoint_ = true; }
    else if (mn == "TBF") { testBits(insn.q, false); atPreemptionPoint_ = true; }
    else if (mn == "SBN") { uint8_t v = readOperand(true, 0); writeOperand(true, 0, static_cast<uint8_t>(v | insn.q)); }
    else if (mn == "SBF") { uint8_t v = readOperand(true, 0); writeOperand(true, 0, static_cast<uint8_t>(v & ~insn.q)); }
    else if (mn == "MVX") moveHex(insn.q);
    else if (mn == "ZAZ") zeroAndAddZoned(insn.q);
    else if (mn == "AZ") addZoned(insn.q, false);
    else if (mn == "SZ") addZoned(insn.q, true);
    else if (mn == "SRC") shiftRightCharacter(insn.q);
    else if (mn == "ED") edit(insn.q);
    else if (mn == "ITC") insertAndTestCharacters(insn.q);
    else if (mn == "XFER") return transfer(insn);
    else if (mn == "LPMR") return loadProgramModeRegister(insn, next);
    else if (mn == "L") loadRegister(insn.q);
    else if (mn == "ST") storeRegister(insn.q);
    else if (mn == "A") addToRegister(insn.q, false);
    else if (mn == "S") addToRegister(insn.q, true);
    else if (mn == "LA") {
        // LA follows SA21-9436 3-31 literally: the direct form catenates PDIR
        // to the two immediate bytes, the indexed forms catenate PXR1/PXR2 to
        // the 16-bit sum, and neither resolves through the ATRs.
        OperandMode lam = insn.operand2 ? m2 : m1;
        int lav = insn.operand2 ? *insn.operand2 : *insn.operand1;
        uint8_t lapact;
        uint16_t lalog = logical(lam, lav, lapact);
        loadAddress(insn.q, (lapact << 16) | lalog);
        atPreemptionPoint_ = true;
    }
    else if (mn == "BC") {
        // BC stays logical: its operand is a branch target and PIAR
        // translates it on the next fetch.
        return branchOnCondition(insn, next, insn.operand2 ? logicalOperand(m2, *insn.operand2)
                                                           : logicalOperand(m1, *insn.operand1));
    }
    else if (mn == "JC") return jumpOnCondition(insn, next);
    else {
        trace_.msp(iar, "{}: execution not implemented", mn);
        stop(fmt::format("{} not implemented at {:04X}", mn, iar));
        return false;
    }

    if (abort_ != Abort::None) return finishAbort();
    if (m_.faulted()) { stopAddressing(); return false; }

    // An instruction whose Q-byte selected the IAR has already branched:
    // `LA q=10 <addr>` is the machine's unconditional branch.
    if (!iarWritten_) {
        m_.msp.iar = static_cast<uint16_t>(next);
    } else if (trace_.flowOn()) {
        const std::string mem = memberResolver ? memberResolver() : std::string();
        trace_.flow("{:04X} -> {:04X}  {:<4}{}", iar, m_.msp.iar, mn, mem.empty() ? "" : "  " + mem);
    }
    ++instructions_;
    return true;
}

bool MainStorageProcessor::memberMatchesTraceFilter()
{
    if (traceMemberFilter.empty() || !memberResolver) return true;
    return containsIgnoreCase(memberResolver(), traceMemberFilter);
}

bool MainStorageProcessor::executeSvc(const Instruction& insn, int next)
{
    const uint8_t r = insn.raw.size() > 2 ? insn.raw[2] : 0;
    controlstorage::SvcRequest req;
    req.r = r;
    req.q = insn.q;
    req.inline1 = insn.raw.size() > 3 ? insn.raw[3] : 0;
    req.inline2 = insn.raw.size() > 4 ? insn.raw[4] : 0;
    req.inline3 = insn.raw.size() > 5 ? insn.raw[5] : 0;
    req.dispatch = csp_.classify(r);
    req.sourceIar = static_cast<uint16_t>(insn.address);
    req.sourceMember = memberResolver ? memberResolver() : std::string();
    // Control returns immediately after the last byte of the instruction.
    m_.msp.iar = static_cast<uint16_t>(next);
    const bool serviced = csp_.svc(req);
    ++instructions_;
    atPreemptionPoint_ = true;      // SVC is an architectural preemption point

    // A call the control processor could not service must STOP the machine;
    // continuing runs phase 1 on state the supervisor never produced.  The
    // step that actually refused is named, not merely the R-byte.
    if (!serviced) {
        const std::string why = csp_.lastRefusal();
        const std::string tail = why.empty() ? std::string(" - the control storage processor gave no reason")
                                             : ":\n  " + why;
        m_.reportCheck("SVC not serviced", 0x1000,
                       fmt::format("SVC {:02X} at {:04X} ({}){}", r, insn.address, csp_.modelName(), tail));
        stop(fmt::format("SVC {:02X} at {:04X} refused by the {} control storage processor{}",
                         r, insn.address, csp_.modelName(), tail));
        return false;
    }
    return true;
}

// ---- operations ------------------------------------------------------------
// The Q-byte is a length, decremented as the instruction runs, so a Q of zero
// moves one byte.  Operands of every multi-byte instruction (ALC, AZ, CLC,
// ED, MVC, SLC, SZ, ZAZ) are addressed by their RIGHTMOST byte: a field of
// length L at address A occupies A-L+1 through A, and processing runs right
// to left.

bool MainStorageProcessor::operandByte(bool first, int displacement, bool forWrite, int& real)
{
    if (abort_ != Abort::None) return false;
    const uint16_t log = static_cast<uint16_t>((first ? op1Logical_ : op2Logical_) + displacement);
    const uint8_t pact = first ? op1Pact_ : op2Pact_;
    if (m_.resolve(log, pact, MachineState::kAtrTaskGroup0, forWrite, real, &protectionFault_)) {
        if (m_.inRange(real, 1)) return true;
        m_.readByte(real);   // records the addressing fault
        abort_ = Abort::Addressing;
        return false;
    }
    abort_ = Abort::Protection;
    return false;
}

uint8_t MainStorageProcessor::readOperand(bool first, int displacement)
{
    int real;
    if (!operandByte(first, displacement, false, real)) return 0;
    return m_.readByte(real);
}

void MainStorageProcessor::writeOperand(bool first, int displacement, uint8_t value)
{
    int real;
    if (!operandByte(first, displacement, true, real)) return;
    m_.writeByte(real, value);
}

void MainStorageProcessor::moveCharacters(uint8_t q)
{
    for (int i = 0; i < count(q); ++i) writeOperand(true, -i, readOperand(false, -i));
}

// Move Hexadecimal Character.  Both operands are 1-byte fields, so the
// Q-byte names which half moves where: 00 zone to zone, 01 numeric to zone,
// 02 zone to numeric, 03 numeric to numeric.  SA21-9436 3-40.
void MainStorageProcessor::moveHex(uint8_t q)
{
    const uint8_t d = readOperand(true, 0), sv = readOperand(false, 0);
    const int half = (q & 0x01) != 0 ? (sv & 0x0F) : (sv >> 4) & 0x0F;
    const uint8_t res = (q & 0x02) != 0 ? static_cast<uint8_t>((d & 0xF0) | half)
                                        : static_cast<uint8_t>((d & 0x0F) | (half << 4));
    writeOperand(true, 0, res);
}

void MainStorageProcessor::compareCharacters(uint8_t q)
{
    const int n = count(q);
    int cmp = 0;
    for (int i = n - 1; i >= 0 && cmp == 0; --i) {   // leftmost byte first
        const int a = readOperand(true, -i), b = readOperand(false, -i);
        cmp = a < b ? -1 : a > b ? 1 : 0;
    }
    if (abort_ != Abort::None) return;
    setCompare(cmp);
}

void MainStorageProcessor::compareImmediate(uint8_t q)
{
    const int a = readOperand(true, 0);
    if (abort_ != Abort::None) return;
    setCompare(a < q ? -1 : a > q ? 1 : 0);
}

void MainStorageProcessor::setCompare(int cmp)
{
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~(kPsrHigh | kPsrLow | kPsrEqual));
    if (cmp > 0) p |= kPsrHigh;
    else if (cmp < 0) p |= kPsrLow;
    else p |= kPsrEqual;
    m_.msp.loadPsr(p);
}

// ALC and SLC.  The status byte is decided by the carry out of the
// high-order byte (SA21-9436 3-5, 3-52); once the subtract is done as an add
// of the complement a carry out means no borrow, so operand 1 was the
// larger.  Binary overflow follows the carry on ALC and is not affected by
// SLC.
void MainStorageProcessor::addLogical(uint8_t q, bool subtract)
{
    const int n = count(q);
    int carry = subtract ? 1 : 0;
    bool nonZero = false;
    for (int i = 0; i < n; ++i) {                     // low order is the rightmost byte
        const int d = readOperand(true, -i);
        int sv = readOperand(false, -i);
        if (subtract) sv = (~sv) & 0xFF;
        const int sum = d + sv + carry;
        carry = sum > 0xFF ? 1 : 0;
        const uint8_t res = static_cast<uint8_t>(sum);
        if (res != 0) nonZero = true;
        writeOperand(true, -i, res);
    }
    if (abort_ != Abort::None) return;
    const uint8_t keep = subtract ? static_cast<uint8_t>(kPsrHigh | kPsrLow | kPsrEqual)
                                  : static_cast<uint8_t>(kPsrHigh | kPsrLow | kPsrEqual | kPsrBinaryOverflow);
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~keep);
    p |= !nonZero ? kPsrEqual : (carry != 0 ? kPsrHigh : kPsrLow);
    if (carry != 0 && !subtract) p |= kPsrBinaryOverflow;
    m_.msp.loadPsr(p);
}

// Subtract Logical Immediate: the status byte compares operand 1 as it was
// BEFORE the subtract against the Q-byte (SA21-9436 3-54).
void MainStorageProcessor::subtractLogicalImmediate(uint8_t q)
{
    const uint8_t before = readOperand(true, 0);
    writeOperand(true, 0, static_cast<uint8_t>(before + ((~q) & 0xFF) + 1));
    if (abort_ != Abort::None) return;
    setCompare(before < q ? -1 : before > q ? 1 : 0);
}

// TBN/TBF may turn test-false ON but cannot turn it off: SSP chains several
// tests before one BC/JC, computing an accumulated AND (SA21-9436 3-60/3-62).
void MainStorageProcessor::testBits(uint8_t mask, bool testOn)
{
    const uint8_t v = readOperand(true, 0);
    if (abort_ != Abort::None) return;
    const bool result = testOn ? (v & mask) == mask : (v & mask) == 0;
    if (!result) m_.msp.loadPsr(static_cast<uint8_t>(m_.msp.psr() | kPsrTestFalse));
}

// ---- zoned decimal ---------------------------------------------------------
// Zone in the high half, digit in the low half, sign in the zone of the
// rightmost byte: D is negative, anything else positive.  The written-back
// sign zone is F for plus and D for minus (SA21-9436 3-13, 3-56, 3-65), and
// ZAZ, AZ and SZ also disturb the ARR.

long long MainStorageProcessor::readZoned(bool first, int len, bool& negative)
{
    long long v = 0;
    for (int i = len - 1; i >= 0; --i) v = v * 10 + (readOperand(first, -i) & 0x0F);
    negative = (readOperand(first, 0) & 0xF0) == kZoneMinus;
    return negative ? -v : v;
}

bool MainStorageProcessor::writeZoned(int len, long long value)
{
    const bool negative = value < 0;
    long long v = negative ? -value : value;
    for (int i = 0; i < len; ++i) {
        const int digit = static_cast<int>(v % 10);
        v /= 10;
        const uint8_t zone = i == 0 ? (negative ? kZoneMinus : kZonePlus) : static_cast<uint8_t>(0xF0);
        writeOperand(true, -i, static_cast<uint8_t>(zone | digit));
    }
    return v != 0;                                    // decimal overflow
}

void MainStorageProcessor::setDecimalResult(long long result, bool overflow)
{
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~(kPsrHigh | kPsrLow | kPsrEqual | kPsrDecimalOverflow));
    if (result > 0) p |= kPsrHigh;
    else if (result < 0) p |= kPsrLow;
    else p |= kPsrEqual;
    if (overflow) p |= kPsrDecimalOverflow;
    m_.msp.loadPsr(p);
}

// The decimal Q-byte: high four bits are L1-L2, low four bits are L2-1
// (SA21-9436 3-64).
void MainStorageProcessor::decimalLengths(uint8_t q, int& l1, int& l2)
{
    l2 = (q & 0x0F) + 1;
    l1 = ((q >> 4) & 0x0F) + l2;
}

void MainStorageProcessor::zeroAndAddZoned(uint8_t q)
{
    int dlen, slen;
    decimalLengths(q, dlen, slen);
    bool neg;
    const long long v = readZoned(false, slen, neg);
    const bool ovf = writeZoned(dlen, v);
    if (abort_ != Abort::None) return;
    m_.msp.arr = op1Logical_;
    setDecimalResult(v, ovf);
}

void MainStorageProcessor::addZoned(uint8_t q, bool subtract)
{
    int dlen, slen;
    decimalLengths(q, dlen, slen);
    bool dn, sn;
    const long long a = readZoned(true, dlen, dn);
    const long long b = readZoned(false, slen, sn);
    const long long r = subtract ? a - b : a + b;
    const bool ovf = writeZoned(dlen, r);
    if (abort_ != Abort::None) return;
    m_.msp.arr = op1Logical_;
    setDecimalResult(r, ovf);
}

// Shift Right Character: high four bits of Q are bits-to-shift minus 1, low
// four bits are bytes minus 1 (SA21-9436 3-45).  Equal when the remaining
// string is all zeros, Low when even and not zero, High when odd, binary
// overflow when any ones were shifted out.
void MainStorageProcessor::shiftRightCharacter(uint8_t q)
{
    const int bits = ((q >> 4) & 0x0F) + 1;
    const int len = (q & 0x0F) + 1;
    bool lost = false, nonZero = false;
    for (int shifted = 0; shifted < bits; ++shifted) {
        int carry = 0;
        for (int i = len - 1; i >= 0; --i) {
            const uint8_t v = readOperand(true, -i);
            writeOperand(true, -i, static_cast<uint8_t>((carry << 7) | (v >> 1)));
            carry = v & 0x01;
        }
        if (carry != 0) lost = true;
    }
    for (int i = 0; i < len; ++i) if (readOperand(true, -i) != 0) nonZero = true;
    if (abort_ != Abort::None) return;
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~(kPsrHigh | kPsrLow | kPsrEqual | kPsrBinaryOverflow));
    if (!nonZero) p |= kPsrEqual;
    else if ((readOperand(true, 0) & 0x01) != 0) p |= kPsrHigh;
    else p |= kPsrLow;
    if (lost) p |= kPsrBinaryOverflow;
    m_.msp.loadPsr(p);
}

// Edit: replace every hex 20 in operand 1 with a character from operand 2,
// right to left in both, setting the zone of each replaced byte to F.  The
// status byte reflects operand 2 as a zoned field (SA21-9436 3-23).
void MainStorageProcessor::edit(uint8_t q)
{
    const int len = count(q);
    int taken = 0;
    for (int i = 0; i < len; ++i) {
        if (readOperand(true, -i) != 0x20) continue;
        const uint8_t v = readOperand(false, -taken);
        writeOperand(true, -i, static_cast<uint8_t>(0xF0 | (v & 0x0F)));
        ++taken;
    }
    if (abort_ != Abort::None) return;
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~(kPsrHigh | kPsrLow | kPsrEqual));
    if (taken == 0) { m_.msp.loadPsr(p); return; }
    bool neg;
    const long long v2 = readZoned(false, taken, neg);
    if (abort_ != Abort::None) return;
    if (v2 == 0) p |= kPsrEqual;
    else if (v2 < 0) p |= kPsrLow;
    else p |= kPsrHigh;
    m_.msp.loadPsr(p);
}

// Insert and Test Characters: the character at operand 2 replaces every
// character to the left of the first significant digit (1 through 9) in
// operand 1, running LEFT to RIGHT from operand 1's LEFTMOST byte.  The ARR
// is left at the first significant digit, or one past the field.  The PSR
// is not affected (SA21-9436 3-26).
void MainStorageProcessor::insertAndTestCharacters(uint8_t q)
{
    const int len = count(q);
    const uint8_t fill = readOperand(false, 0);
    for (int i = 0; i < len; ++i) {
        const uint8_t v = readOperand(true, i);
        if (abort_ != Abort::None) return;
        const int digit = v & 0x0F;
        if (digit >= 1 && digit <= 9) { m_.msp.arr = static_cast<uint16_t>(op1Logical_ + i); return; }
        writeOperand(true, i, fill);
    }
    if (abort_ != Abort::None) return;
    m_.msp.arr = static_cast<uint16_t>(op1Logical_ + len);
}

// Transfer control to the extended control store supervisor: the MSP is
// halted until it is again started by the control storage processor.  Does
// not affect the PSR (SA21-9436 3-63).
bool MainStorageProcessor::transfer(const Instruction& insn)
{
    const uint8_t r = insn.operand2 ? static_cast<uint8_t>(*insn.operand2) : 0;
    trace_.msp(insn.address, "XFER q={:02X} r={:02X} - MSP halted for the control processor", insn.q, r);
    m_.msp.iar = static_cast<uint16_t>((insn.address + insn.length) & 0xFFFF);
    ++instructions_;
    atPreemptionPoint_ = true;
    stop("XFER - halted until restarted by the control storage processor");
    return false;
}

// Operand 1 is addressed by its rightmost byte, so a 2-byte field at A
// occupies A-1 and A.
uint16_t MainStorageProcessor::readField()
{
    const int hi = readOperand(true, -1), lo = readOperand(true, 0);
    return static_cast<uint16_t>((hi << 8) | lo);
}

void MainStorageProcessor::writeField(uint16_t v)
{
    writeOperand(true, -1, static_cast<uint8_t>(v >> 8));
    writeOperand(true, 0, static_cast<uint8_t>(v));
}

// The Q-byte on L, ST, A, S and LA is a register selector.  Q values A0-A3
// name a PACT register as well as a 2-byte register, which is why those
// forms address a 3-byte field.
void MainStorageProcessor::setSelected(uint8_t q, uint16_t v)
{
    MspRegisters& r = m_.msp;
    switch (q) {
        case 0x00: return;                                  // documented no-op
        case 0x01: case 0x03: case 0x41: r.xr1 = v; return;
        case 0x02: case 0x42: r.xr2 = v; return;
        case 0x04: r.loadPsr(static_cast<uint8_t>(v)); return;
        case 0x08: case 0x43: r.arr = v; return;
        case 0x10: case 0x20: case 0x40: r.iar = v; iarWritten_ = true; return;
        case 0x44: case 0x45: case 0x46: case 0x47: r.wr[q - 0x40] = v; return;
        case 0xA0: r.pactDir = readOperand(true, -2); return;
        case 0xA1: r.pactXr1 = readOperand(true, -2); r.xr1 = v; return;
        case 0xA2: r.pactXr2 = readOperand(true, -2); r.xr2 = v; return;
        case 0xA3: r.pactIar = readOperand(true, -2); r.iar = v; iarWritten_ = true; return;
        default: r.xr1 = v; return;
    }
}

uint16_t MainStorageProcessor::getSelected(uint8_t q, uint8_t& pact, bool& hasPact) const
{
    const MspRegisters& r = m_.msp;
    pact = 0;
    hasPact = false;
    switch (q) {
        case 0x01: case 0x03: case 0x41: return r.xr1;
        case 0x02: case 0x42: return r.xr2;
        case 0x04: return r.psr();
        case 0x08: case 0x43: return r.arr;
        case 0x10: case 0x20: case 0x40: return r.iar;
        case 0x44: case 0x45: case 0x46: case 0x47: return r.wr[q - 0x40];
        case 0xA0: pact = r.pactDir; hasPact = true; return 0;
        case 0xA1: pact = r.pactXr1; hasPact = true; return r.xr1;
        case 0xA2: pact = r.pactXr2; hasPact = true; return r.xr2;
        case 0xA3: pact = r.pactIar; hasPact = true; return r.iar;
        default: return r.xr1;
    }
}

void MainStorageProcessor::loadRegister(uint8_t q)
{
    const uint16_t v = readField();
    if (abort_ != Abort::None) return;
    setSelected(q, v);
}

void MainStorageProcessor::storeRegister(uint8_t q)
{
    uint8_t pact;
    bool hasPact;
    const uint16_t v = getSelected(q, pact, hasPact);
    writeField(v);
    if (hasPact) writeOperand(true, -2, pact);
}

// Load Address places the address of the operand into the selected register
// without reading storage.  On the three-byte forms A0-A3 the destination
// is a PACT register beside a two-byte register, so the 24-bit address is
// split across the pair; Load's writer, which takes the prefix from storage
// at rightmost-2, must not be used here.
void MainStorageProcessor::loadAddress(uint8_t q, int addr)
{
    MspRegisters& r = m_.msp;
    if (threeByteForm(q)) {
        const uint8_t prefix = static_cast<uint8_t>((addr >> 16) & 0xFF);
        const uint16_t low = static_cast<uint16_t>(addr);
        switch (q) {
            case 0xA0: r.pactDir = prefix; return;
            case 0xA1: r.pactXr1 = prefix; r.xr1 = low; return;
            case 0xA2: r.pactXr2 = prefix; r.xr2 = low; return;
            case 0xA3: r.pactIar = prefix; r.iar = low; iarWritten_ = true; return;
            default: break;
        }
    }
    setSelected(q, static_cast<uint16_t>(addr));
}

// Add to Register and Subtract from Register.  Operand 1 is a 2-byte field
// addressed by its rightmost byte and is not changed.  A: binary overflow
// on a carry from the high-order byte (SA21-9436 3-11); S: binary overflow
// not affected (3-50).  Neither adds a carry into the PACT of the selected
// register.
void MainStorageProcessor::addToRegister(uint8_t q, bool subtract)
{
    uint8_t pact;
    bool hasPact;
    const uint16_t reg = getSelected(q, pact, hasPact);
    const int operand = readField();
    if (abort_ != Abort::None) return;
    const int sum = reg + (subtract ? ((~operand) & 0xFFFF) + 1 : operand);
    const uint16_t result = static_cast<uint16_t>(sum);
    const bool carry = subtract ? operand <= reg : sum > 0xFFFF;

    if (q != 0x00) {
        setSelected(q, result);
        if (hasPact) {
            MspRegisters& r = m_.msp;
            switch (q) {
                case 0xA0: r.pactDir = pact; break;
                case 0xA1: r.pactXr1 = pact; break;
                case 0xA2: r.pactXr2 = pact; break;
                case 0xA3: r.pactIar = pact; break;
                default: break;
            }
        }
    }

    const uint8_t keep = subtract ? static_cast<uint8_t>(kPsrHigh | kPsrLow | kPsrEqual)
                                  : static_cast<uint8_t>(kPsrHigh | kPsrLow | kPsrEqual | kPsrBinaryOverflow);
    uint8_t p = static_cast<uint8_t>(m_.msp.psr() & ~keep);
    p |= result == 0 ? kPsrEqual : (carry ? kPsrHigh : kPsrLow);
    if (carry && !subtract) p |= kPsrBinaryOverflow;
    if (q != 0x04) m_.msp.loadPsr(p);                  // else the PSR is the target
}

// Load Program Mode Register.  Privileged.  Q of 80 replaces PMR bits 0 and
// 7 only; Q of 00 replaces all eight bits and the PACT bits with them; any
// other Q is treated as an invalid op code (SA21-9436 3-34).
bool MainStorageProcessor::loadProgramModeRegister(const Instruction& insn, int next)
{
    const uint8_t r = insn.operand2 ? static_cast<uint8_t>(*insn.operand2) : 0;
    MspRegisters& regs = m_.msp;

    if (!regs.privileged()) {
        stop(fmt::format("LPMR at {:04X} while not privileged - storage exception check", insn.address));
        return false;
    }

    if (insn.q == 0x80) {
        const int bits = MspRegisters::kPmrTaskDispatch | MspRegisters::kPmrNotPrivileged;
        regs.setPmr(static_cast<uint8_t>((regs.pmr() & ~bits) | (r & bits)));
    } else if (insn.q == 0x00) {
        regs.setPmr(r);
        regs.setPactPdirBit((r & MspRegisters::kPmrPdir) != 0);
        regs.setPactXr1Bit((r & MspRegisters::kPmrPxr1) != 0);
        regs.setPactXr2Bit((r & MspRegisters::kPmrPxr2) != 0);
        regs.setPactIarBit((r & MspRegisters::kPmrPiar) != 0);
    } else {
        stop(fmt::format("LPMR q={:02X} at {:04X} - treated as an invalid op code", insn.q, insn.address));
        return false;
    }

    trace_.msp(insn.address, "LPMR pmr={:02X} task-dispatch={} privileged={}", regs.pmr(),
               regs.taskDispatchingEnabled() ? "True" : "False", regs.privileged() ? "True" : "False");
    regs.iar = static_cast<uint16_t>(next);
    ++instructions_;
    atPreemptionPoint_ = true;
    return true;
}

// On any branch the ARR receives the address of the instruction after the
// branch, which is how return addresses are kept.
bool MainStorageProcessor::branchOnCondition(const Instruction& insn, int next, int target)
{
    atPreemptionPoint_ = true;
    if (conditionTrue(insn.q)) {
        m_.msp.arr = static_cast<uint16_t>(next);
        m_.msp.iar = static_cast<uint16_t>(target);
    } else {
        m_.msp.iar = static_cast<uint16_t>(next);
    }
    ++instructions_;
    return true;
}

// The displacement is unsigned and the DIRECTION is in the op code: F2 is a
// jump forwards, F1 backwards (SA21-9436 3-28).
bool MainStorageProcessor::jumpOnCondition(const Instruction& insn, int next)
{
    atPreemptionPoint_ = true;
    const int disp = insn.operand2 ? *insn.operand2 : 0;
    const bool backwards = insn.opcode == InstructionSet::kJumpBackwardOpcode;
    const int target = (backwards ? next - disp : next + disp) & 0xFFFF;
    m_.msp.iar = static_cast<uint16_t>(conditionTrue(insn.q) ? target : next);
    ++instructions_;
    return true;
}

// The Q-byte on BC and JC: bits 2-7 select PSR bits to test; bit 0 is the
// sense - condition true branches if any tested indicator is on, condition
// false if all are off (SA21-9436 3-16, 3-28).  Testing decimal overflow or
// test false also turns that bit off.
bool MainStorageProcessor::conditionTrue(uint8_t q)
{
    const uint8_t psr = m_.msp.psr();
    const uint8_t mask = static_cast<uint8_t>(q & 0x3F);
    const bool any = (psr & mask) != 0;
    const bool onTrue = (q & 0x80) != 0;
    if ((mask & (kPsrTestFalse | kPsrDecimalOverflow)) != 0)
        m_.msp.loadPsr(static_cast<uint8_t>(psr & ~(mask & (kPsrTestFalse | kPsrDecimalOverflow))));
    return onTrue ? any : !any;
}

}  // namespace sim36::processors
