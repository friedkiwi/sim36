#include "Processors/InstructionSet.h"

#include <fmt/format.h>

#include "Processors/ControlStorage/SvcTable.h"

namespace sim36::processors {

void InstructionSet::operandWidths(uint8_t opcode, int& w1, int& w2)
{
    OperandMode m1, m2;
    modes(opcode, m1, m2);
    if (m1 == OperandMode::None && m2 == OperandMode::None) {
        w1 = 0; w2 = 1;          // nibble F: a control byte or an ARR displacement
        return;
    }
    w1 = m1 == OperandMode::None ? 0 : modeWidth(m1);
    w2 = m2 == OperandMode::None ? 0 : modeWidth(m2);
}

int InstructionSet::length(uint8_t opcode, int rByte)
{
    if (isSvc(opcode) && rByte >= 0)
        return 3 + controlstorage::SvcTable::inlineParameters(static_cast<uint8_t>(rByte));
    int w1, w2;
    operandWidths(opcode, w1, w2);
    return 2 + w1 + w2;
}

const char* InstructionSet::mnemonic(uint8_t opcode)
{
    const int high = (opcode >> 4) & 0xF, low = opcode & 0xF;
    if (high == 0xF) {
        // F1 and F2 are both JC; F4 and FC are both SVC.
        switch (low) {
            case 0x0: return "BC";
            case 0x1: return "JC";
            case 0x2: return "JC";
            case 0x4: return "SVC";
            case 0x5: return "XFER";
            case 0x6: return "LPMR";
            case 0xC: return "SVC";
            default: return nullptr;
        }
    }
    OperandMode m1, m2;
    modes(opcode, m1, m2);
    if (m1 != OperandMode::None && m2 != OperandMode::None) {
        // Two-address forms.  Low nibble 0 is unassigned; CLC is D (SA21-9436
        // 3-19 lists 0D 1D 2D 4D 5D 6D 8D 9D AD).
        switch (low) {
            case 0x4: return "ZAZ";
            case 0x6: return "AZ";
            case 0x7: return "SZ";
            case 0x8: return "MVX";
            case 0xA: return "ED";
            case 0xB: return "ITC";
            case 0xC: return "MVC";
            case 0xD: return "CLC";
            case 0xE: return "ALC";
            case 0xF: return "SLC";
            default: return nullptr;
        }
    }
    if (m1 != OperandMode::None) {
        switch (low) {
            case 0x4: return "ST";
            case 0x5: return "L";
            case 0x6: return "A";
            case 0x7: return "S";
            case 0x8: return "TBN";
            case 0x9: return "TBF";
            case 0xA: return "SBN";
            case 0xB: return "SBF";
            case 0xC: return "MVI";
            case 0xD: return "CLI";
            case 0xE: return "SRC";
            // The manual prints 3F/7F/BF for both ALI and SLI: ALI "is the
            // Subtract Logical Immediate instruction with a 2's complement of
            // the immediate data byte".  The machine has one operation.
            case 0xF: return "SLI";
            default: return nullptr;
        }
    }
    switch (low) {
        case 0x0: return "BC";
        case 0x2: return "LA";
        default: return nullptr;
    }
}

const char* InstructionSet::family(uint8_t opcode)
{
    if (((opcode >> 4) & 0xF) == 0xF) return "F";
    OperandMode m1, m2;
    modes(opcode, m1, m2);
    return (m1 != OperandMode::None && m2 != OperandMode::None) ? "2-addr" : "1-addr";
}

namespace {

const char* modeName(OperandMode m)
{
    switch (m) {
        case OperandMode::Direct: return "A";
        case OperandMode::Xr1: return "XR1";
        case OperandMode::Xr2: return "XR2";
        default: return "ARR";
    }
}

}  // namespace

std::string Instruction::operandText() const
{
    OperandMode m1, m2;
    InstructionSet::modes(opcode, m1, m2);
    std::vector<std::string> parts;
    if (operand1)
        parts.push_back(m1 == OperandMode::Direct ? fmt::format("${:04X}", *operand1)
                                                  : fmt::format("{:02X}({})", *operand1, modeName(m1)));
    if (operand2) {
        if (m1 == OperandMode::None && m2 == OperandMode::None)
            parts.push_back(fmt::format("{:02X}(ARR)", *operand2));
        else
            parts.push_back(m2 == OperandMode::Direct ? fmt::format("${:04X}", *operand2)
                                                      : fmt::format("{:02X}({})", *operand2, modeName(m2)));
    }
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) s += ", ";
        s += parts[i];
    }
    return s;
}

std::string Instruction::toString() const
{
    if (!valid()) {
        std::string s = ".byte   ";
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (i != 0) s += " ";
            s += fmt::format("0x{:02X}", raw[i]);
        }
        return s;
    }
    const std::string ops = operandText();
    std::string s = fmt::format("{:<6} q={:02X}", mnemonic, q);
    return ops.empty() ? s : s + " " + ops;
}

}  // namespace sim36::processors
