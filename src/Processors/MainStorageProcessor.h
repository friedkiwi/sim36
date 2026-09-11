// The main storage processor: the 16-bit processor that runs SSP and
// applications.  Decode is complete; execution covers all 28 instructions.
// Anything the machine would not do stops the processor and says so rather
// than guessing.
//
// The core is exception-free.  A reference outside guest storage or to a
// protected page is detected at the access, the remainder of the instruction
// is abandoned exactly where the hardware would have taken its exception,
// and the disposition (a stop, or delivery of the level 5 interrupt to the
// control processor) is decided once at the end of the step.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/InstructionSet.h"

namespace sim36::processors {

namespace controlstorage { class IControlStorageProcessor; }

class MainStorageProcessor {
public:
    MainStorageProcessor(machine::MachineState& m, controlstorage::IControlStorageProcessor& csp,
                         monitor::Tracer& trace)
        : m_(m), csp_(csp), trace_(trace) {}

    bool stopped() const { return stopped_; }
    const std::string& stopReason() const { return stopReason_; }
    long long instructionsExecuted() const { return instructions_; }
    // Softstop is honoured only after branch, jump and other instructions
    // that modify neither instructions nor data.  SSP mutates shared data
    // without locks and relies on this.
    bool atPreemptionPoint() const { return atPreemptionPoint_; }

    // Read-only monitor observation immediately before instruction fetch.
    std::function<void(uint16_t)> onBeforeInstruction;

    // ---- execution breakpoints ----------------------------------------
    // Halt when the guest IAR reaches an address, name it, let the operator
    // dump or poke, then continue.  Keyed by the 16-bit LOGICAL IAR.
    struct MemberBreakpoint {
        uint16_t address = 0;
        uint16_t offset = 0;
        std::string member;
        std::string description;
    };
    void setBreakpoint(uint16_t address, const std::string& description);
    bool clearBreakpoint(uint16_t address);
    void setMemberBreakpoint(uint16_t address, uint16_t offset, const std::string& member,
                             const std::string& description);
    bool clearMemberBreakpoint(uint16_t address, const std::string& member);
    void clearAllBreakpoints();
    const std::map<uint16_t, std::string>& breakpoints() const { return breakpoints_; }
    std::vector<MemberBreakpoint> memberBreakpoints() const;
    const std::string& lastBreakpointDescription() const { return lastBreakpointDescription_; }

    // ---- patch-on-reach ------------------------------------------------
    // When the guest IAR reaches a registered address, apply a memory or
    // register patch and CONTINUE, so a hypothesised value can be forced
    // exactly at the instruction that reads it.
    struct IarPatch { int kind = 0; int a = 0; int v = 0; };   // kind: 0 mem8, 1 mem16, 2 WR, 3 XR1, 4 XR2
    void addPatch(uint16_t iar, const IarPatch& p) { patches_[iar].push_back(p); }
    void clearPatches() { patches_.clear(); }
    const std::map<uint16_t, std::vector<IarPatch>>& patchList() const { return patches_; }

    // Optional member-name filter for breakpoints and for the full
    // instruction trace: many SSP modules share load base 0x1000.
    std::string breakMemberFilter;
    std::string traceMemberFilter;
    // Resolve the active member for the current IAR and task (with a +offset
    // suffix), and the exact member name alone; empty when unknown.
    std::function<std::string()> memberResolver;
    std::function<std::string()> memberNameResolver;

    void reset();
    void start() { stopped_ = false; stopReason_.clear(); }
    void restoreCheckpoint(bool stopped, const std::string& reason, long long instructions, bool atPreemptionPoint);
    void hardstop() { stop("hardstop"); }
    void softstop() { if (atPreemptionPoint_) stop("softstop"); }
    // Stop with a stated reason: the control processor owns every
    // transition out of Running, including "nothing left to dispatch".
    void halt(const std::string& why) { stop(why); }

    // Decode at a guest (real) address without executing or advancing.
    Instruction decode(int address);

    // Execute one instruction.  False when the processor stopped.
    bool step();

private:
    enum class Abort { None, Addressing, Protection };

    bool atActiveBreakpoint(uint16_t iar, std::string& description);
    void applyPatchesAt(uint16_t iar);
    bool memberMatchesTraceFilter();
    void stop(const std::string& why);
    void stopAddressing();
    bool recoverProtection();
    std::string protectionStop() const;
    bool finishAbort();

    int readWidth(int addr, int width);
    uint16_t logical(OperandMode mode, int value, uint8_t& pact) const;
    uint16_t logicalOperand(OperandMode mode, int value) const;
    bool effective(OperandMode mode, int value, int& real);

    bool executeSvc(const Instruction& insn, int next);
    int count(uint8_t q) const { return q + 1; }
    bool operandByte(bool first, int displacement, bool forWrite, int& real);
    uint8_t readOperand(bool first, int displacement);
    void writeOperand(bool first, int displacement, uint8_t value);
    void moveCharacters(uint8_t q);
    void moveHex(uint8_t q);
    void compareCharacters(uint8_t q);
    void compareImmediate(uint8_t q);
    void setCompare(int cmp);
    void addLogical(uint8_t q, bool subtract);
    void subtractLogicalImmediate(uint8_t q);
    void testBits(uint8_t mask, bool testOn);
    long long readZoned(bool first, int len, bool& negative);
    bool writeZoned(int len, long long value);
    void setDecimalResult(long long result, bool overflow);
    static void decimalLengths(uint8_t q, int& l1, int& l2);
    void zeroAndAddZoned(uint8_t q);
    void addZoned(uint8_t q, bool subtract);
    void shiftRightCharacter(uint8_t q);
    void edit(uint8_t q);
    void insertAndTestCharacters(uint8_t q);
    bool transfer(const Instruction& insn);
    static bool threeByteForm(uint8_t q) { return q >= 0xA0 && q <= 0xA3; }
    uint16_t readField();
    void writeField(uint16_t v);
    void setSelected(uint8_t q, uint16_t v);
    uint16_t getSelected(uint8_t q, uint8_t& pact, bool& hasPact) const;
    void loadRegister(uint8_t q);
    void storeRegister(uint8_t q);
    void loadAddress(uint8_t q, int addr);
    void addToRegister(uint8_t q, bool subtract);
    bool loadProgramModeRegister(const Instruction& insn, int next);
    bool branchOnCondition(const Instruction& insn, int next, int target);
    bool jumpOnCondition(const Instruction& insn, int next);
    bool conditionTrue(uint8_t q);

    static constexpr uint8_t kPsrBinaryOverflow = 0x20;
    static constexpr uint8_t kPsrTestFalse = 0x10;
    static constexpr uint8_t kPsrDecimalOverflow = 0x08;
    static constexpr uint8_t kPsrHigh = 0x04;
    static constexpr uint8_t kPsrLow = 0x02;
    static constexpr uint8_t kPsrEqual = 0x01;
    static constexpr uint8_t kZonePlus = 0xF0;
    static constexpr uint8_t kZoneMinus = 0xD0;

    machine::MachineState& m_;
    controlstorage::IControlStorageProcessor& csp_;
    monitor::Tracer& trace_;
    bool stopped_ = false;
    std::string stopReason_;
    long long instructions_ = 0;
    bool atPreemptionPoint_ = true;
    bool iarWritten_ = false;
    // Multi-byte operands are logical ranges: keep the logical rightmost
    // address and PACT for both operands so every byte is translated
    // independently.
    uint16_t op1Logical_ = 0, op2Logical_ = 0;
    uint8_t op1Pact_ = 0, op2Pact_ = 0;
    Abort abort_ = Abort::None;
    machine::StorageProtection protectionFault_;
    std::map<uint16_t, std::string> breakpoints_;
    std::map<uint16_t, std::vector<MemberBreakpoint>> memberBreakpoints_;
    bool haveResumePastBreak_ = false;
    uint16_t resumePastBreak_ = 0;
    std::string lastBreakpointDescription_;
    std::map<uint16_t, std::vector<IarPatch>> patches_;
};

}  // namespace sim36::processors
