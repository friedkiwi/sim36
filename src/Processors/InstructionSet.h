// The System/36 main storage processor instruction set.
//
// The model in one line: an opcode's high nibble is two 2-bit operand modes
// and its low nibble is the operation:
//
//     opcode = (mode1 << 6) | (mode2 << 4) | operation
//
// Instruction length follows from the high nibble alone, which is what lets
// a decoder advance without consulting the operation table.  The one
// exception is SVC, which is 3 to 6 bytes depending on the inline
// parameters its R-byte takes.  28 machine operations.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sim36::processors {

// Which register or base an operand is reached through.
enum class OperandMode {
    Direct = 0,   // 2 operand bytes, an absolute address
    Xr1 = 1,      // 1 byte, displacement from index register 1
    Xr2 = 2,      // 1 byte, displacement from index register 2
    None = 3,     // absent, or - when both modes are 3 - displacement from the ARR
};

class InstructionSet {
public:
    static constexpr uint8_t kSvcOpcode = 0xF4;
    // The second SVC op code: SSP 5.1 for the 5363/5364 is assembled with FC
    // where the 5360/5362 and 7.5 code has F4, and the Advanced/36 accepts
    // both.  Treating them alike boots the 5.1 volume.
    static constexpr uint8_t kSvcOpcodeAlt = 0xFC;
    // JC backwards: the displacement byte is unsigned and the sign is the op
    // code, F1 subtracts and F2 adds (SA21-9436 3-28).
    static constexpr uint8_t kJumpBackwardOpcode = 0xF1;

    static bool isSvc(uint8_t opcode) { return opcode == kSvcOpcode || opcode == kSvcOpcodeAlt; }
    static int modeWidth(OperandMode m) { return m == OperandMode::Direct ? 2 : 1; }
    static void modes(uint8_t opcode, OperandMode& m1, OperandMode& m2)
    {
        m1 = static_cast<OperandMode>((opcode >> 6) & 3);
        m2 = static_cast<OperandMode>((opcode >> 4) & 3);
    }
    // Operand widths in bytes; 0 means the operand is not present.
    static void operandWidths(uint8_t opcode, int& w1, int& w2);
    // Total length: opcode + Q-byte + operands.  Pass the R-byte for SVC;
    // without it every parameterised supervisor call desynchronises.
    static int length(uint8_t opcode, int rByte = -1);
    // Mnemonic for an opcode, or nullptr if the encoding is unassigned.
    static const char* mnemonic(uint8_t opcode);
    static const char* family(uint8_t opcode);
};

// One decoded instruction.  Renders to the same text the disassembler emits.
struct Instruction {
    int address = 0;
    uint8_t opcode = 0;
    uint8_t q = 0;
    std::optional<int> operand1;
    std::optional<int> operand2;
    int length = 0;
    const char* mnemonic = nullptr;
    std::vector<uint8_t> raw;

    bool valid() const { return mnemonic != nullptr; }
    std::string operandText() const;
    std::string toString() const;
};

}  // namespace sim36::processors
