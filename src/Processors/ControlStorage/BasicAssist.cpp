#include "Processors/ControlStorage/BasicAssist.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/BasicNumber.h"

namespace sim36::processors::controlstorage {
namespace {

constexpr int kCbIp = 19;
constexpr int kCbStackBase = 23;
constexpr int kCbStackPointer = 25;
constexpr int kCbStackLimit = 27;
constexpr int kCbControlBase = 29;
constexpr int kCbControlPointer = 31;
constexpr int kCbControlLimit = 33;
constexpr int kCbExitVectors = 35;
constexpr int kCbClass01Control = 58;
constexpr int kCbStackErrorVector = 71;
constexpr int kCbStringRangeVector = 93;
constexpr int kCbClass01Vector = 97;
constexpr int kCbStringPad = 115;
constexpr int kMaxInstructionsPerCall = 100000;

class BasicMachine {
public:
    explicit BasicMachine(AssistContext& context) : context_(context)
    {
        xr1_ = (context_.requestByte(RequestBlock::kOffXr1High) << 16) |
               context_.requestHalf(RequestBlock::kOffXr1Low);
        translated_ = (xr1_ & 0x800000) != 0;
        control_ = xr1_;
    }

    AssistResult run()
    {
        if (!loadHalf(kCbIp, ip_) || !loadHalf(kCbStackBase, stackBase_) ||
            !loadHalf(kCbStackPointer, sp_) || !loadHalf(kCbStackLimit, stackLimit_) ||
            !loadHalf(kCbControlBase, controlBase_) ||
            !loadHalf(kCbControlPointer, controlPointer_))
            return storageError("reading the BASIC control block");

        uint8_t mode = 0;
        if (!read(control_, &mode, 1)) return storageError("reading the BASIC mode");
        precision_ = (mode & 0x40) != 0 ? BasicNumber::Precision::Long : BasicNumber::Precision::Short;
        width_ = static_cast<int>(BasicNumber::encodedSize(precision_));

        context_.trace().basicAssist(
            "BASIC entry XFER 02,00 at {:04X}: task={:06X} request={:06X} XR1={:06X} "
            "PSR={:02X} IAR={:04X} ARR={:04X} XR2={:02X}:{:04X} WR5={:04X} "
            "IP={:04X} SP={:04X} control={:04X} width={}",
            context_.sourceIar(), context_.taskBlock() & 0xFFFFFF,
            context_.requestBlock() & 0xFFFFFF, xr1_ & 0xFFFFFF,
            context_.requestByte(RequestBlock::kOffPsr), context_.requestHalf(RequestBlock::kOffIar),
            context_.requestHalf(RequestBlock::kOffArr), context_.requestByte(RequestBlock::kOffXr2High),
            context_.requestHalf(RequestBlock::kOffXr2Low), context_.requestWorkRegister(5),
            ip_, sp_, controlPointer_, width_);

        for (int step = 0; step < kMaxInstructionsPerCall; ++step) {
            const uint16_t at = ip_;
            uint8_t opcode = 0;
            if (!readOffset(ip_, &opcode, 1)) return storageError("fetching a BASIC opcode");
            ip_ = static_cast<uint16_t>(ip_ + 1);
            const BasicDecodedOpcode decoded = BasicAssist::decodeOpcode(opcode);
            const uint16_t beforeSp = sp_;
            std::string following;
            if ((context_.trace().flags.load() & monitor::TraceAssistBasic) != 0) {
                for (int i = 0; i < 12; ++i) {
                    uint8_t byte = 0;
                    if (!readOffset(static_cast<uint16_t>(ip_ + i), &byte, 1)) break;
                    following += fmt::format("{:02X}{}", byte, i == 11 ? "" : " ");
                }
            }
            context_.trace().basicAssist(
                "BASIC op IP={:04X} opcode={:02X} class={:X} op={:X} SP={:04X} next=[{}]",
                at, opcode, decoded.operandClass, decoded.operation, beforeSp, following);
            AssistResult result = execute(decoded);
            context_.trace().basicAssist(
                "BASIC op done IP={:04X}->{:04X} opcode={:02X} SP={:04X}->{:04X} control={:04X} status={}",
                at, ip_, opcode, beforeSp, sp_, controlPointer_, static_cast<int>(result.status));
            if (result.status != AssistStatus::Completed || exit_) {
                if (!commit()) return storageError("committing BASIC state");
                restoreTaskFlags();
                context_.trace().basicAssist("BASIC exit IP={:04X} SP={:04X} control={:04X} status={} detail={}",
                                             ip_, sp_, controlPointer_, static_cast<int>(result.status), result.detail);
                return result;
            }
        }
        return fatal(fmt::format("BASIC instruction limit reached at internal IP {:04X}", ip_));
    }

private:
    AssistResult execute(const BasicDecodedOpcode& op)
    {
        switch (op.operandClass) {
            case 0x0: case 0x1: return class01();
            case 0x2: return class2(op.operation);
            case 0x3: return class3(op.operation);
            case 0x5: return class5(op.operation);
            case 0x7: return class7(op.operation);
            case 0x8:
                if (!popBytes(width_)) return stackError("numeric operand");
                currentOperand_ = sp_;
                return numeric(op.operation, sp_);
            case 0x9: {
                uint16_t effective = 0;
                if (!fetchHalf(effective)) return storageError("decoding a direct numeric operand");
                currentOperand_ = effective;
                return numeric(op.operation, effective);
            }
            case 0xA: return indexedNumeric(op.operation, false);
            case 0xB: return indexedNumeric(op.operation, true);
            case 0xC: {
                if (!requireTop(1)) return stackError("expanded string operand");
                uint8_t length = 0;
                if (!readOffset(static_cast<uint16_t>(sp_ - 1), &length, 1))
                    return storageError("reading an expanded string length");
                if (!requireTop(static_cast<int>(length) + 2))
                    return stackError("expanded string operand");
                const uint16_t effective = static_cast<uint16_t>(sp_ - length - 2);
                sp_ = effective;
                return stringOperation(op.operation, effective, 0xFF);
            }
            case 0xD: {
                uint16_t descriptor = 0;
                if (!fetchHalf(descriptor))
                    return storageError("decoding a direct string operand");
                uint8_t capacity = 0;
                if (!readOffset(descriptor, &capacity, 1))
                    return storageError("reading a string descriptor");
                return stringOperation(op.operation, static_cast<uint16_t>(descriptor + 1), capacity);
            }
            case 0xE: return indexedString(op.operation, false);
            case 0xF: return indexedString(op.operation, true);
            case 0x4: case 0x6:
                return fatal(fmt::format("invalid BASIC opcode {:02X} at {:04X}", op.byte,
                                         static_cast<uint16_t>(ip_ - 1)));
            default:
                return fatal(fmt::format("unsupported BASIC operand class {:X} for opcode {:02X} at {:04X}",
                                         op.operandClass, op.byte, static_cast<uint16_t>(ip_ - 1)));
        }
    }

    AssistResult class01()
    {
        uint8_t control = 0;
        if (!readOffset(static_cast<uint16_t>((xr1_ & 0xFFFF) + kCbClass01Control), &control, 1))
            return storageError("reading BASIC class-0 control");
        ip_ = static_cast<uint16_t>(ip_ + 5);
        currentOperand_ = ip_;
        if ((control & 0x04) == 0) return AssistResult::complete();
        uint16_t vector = 0;
        if (!loadHalf(kCbClass01Vector, vector)) return storageError("reading BASIC class-0 vector");
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
        exit_ = true;
        return AssistResult::complete();
    }

    AssistResult class2(uint8_t low)
    {
        uint16_t vector = 0;
        if (!loadHalf(kCbExitVectors + 2 * low, vector)) return storageError("reading a BASIC exit vector");
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
        exit_ = true;
        return AssistResult::complete();
    }

    AssistResult class3(uint8_t low)
    {
        if (low == 0) return computedTransfer(ip_, false);
        if (low == 0xA || low == 0xB) return fatal(fmt::format("invalid BASIC control opcode 3{:X}", low));
        if (low == 0xC || low == 0xE) {
            uint16_t target = 0;
            if (!fetchHalf(target)) return storageError("decoding a BASIC guest exit");
            context_.setRequestHalf(RequestBlock::kOffIar, target);
            exit_ = true;
            return AssistResult::complete();
        }
        if (low == 0xD) {
            uint16_t xr2 = 0;
            if (!fetchHalf(xr2)) return storageError("decoding a BASIC XR2 exit");
            context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
            context_.setRequestHalf(RequestBlock::kOffXr2Low, xr2);
            exit_ = true;
            return AssistResult::complete();
        }
        if (low == 0xF) {
            uint16_t target = 0;
            if (!readHalfOffset(ip_, target)) return storageError("decoding a BASIC internal branch");
            ip_ = target;
            return AssistResult::complete();
        }
        if (low == 1) return pushControlAndDispatch();
        if (low == 5) {
            if (!popBytes(1)) return stackError("conditional transfer");
            uint8_t condition = 0;
            if (!readOffset(sp_, &condition, 1))
                return storageError("reading a BASIC branch condition");
            const uint16_t record = static_cast<uint16_t>(ip_ + ((condition & 1) ? 0 : 4));
            return computedTransfer(record, false);
        }
        if (low == 6) return forLoopControl();
        if (low == 7) return forLoopGate();
        if (low == 8) return unwindControl();
        return fatal(fmt::format("unsupported BASIC control opcode 3{:X} at {:04X}", low,
                                 static_cast<uint16_t>(ip_ - 1)));
    }

    AssistResult computedTransfer(uint16_t record, bool addDisplacement)
    {
        // The shared resolver reads only the fields required by the selected
        // path.  This matters when a compact/cached descriptor ends at a
        // translated-storage boundary.
        uint8_t bytes[4]{};
        if (!readOffset(record, bytes, 1))
            return storageError("reading a BASIC lazy-transfer record");
        if ((bytes[0] & 0x80) != 0) {
            if (!readOffset(static_cast<uint16_t>(record + 1), bytes + 1, 2))
                return storageError("reading a cached BASIC transfer target");
            const uint16_t target = static_cast<uint16_t>((bytes[1] << 8) | bytes[2]);
            uint16_t displacement = 0;
            if (addDisplacement &&
                !readHalfOffset(static_cast<uint16_t>(record + 4), displacement))
                return storageError("reading a BASIC transfer displacement");
            ip_ = static_cast<uint16_t>(target + (addDisplacement ? displacement : 0));
            return AssistResult::complete();
        }

        if ((bytes[0] & 0x10) == 0) {
            if (!readOffset(static_cast<uint16_t>(record + 1), bytes + 1, 3))
                return storageError("reading a BASIC transfer key");
            uint16_t node = 0;
            if (!loadHalf(17, node))
                return storageError("reading the BASIC transfer lookup root");
            const uint32_t wanted = (static_cast<uint32_t>(bytes[0] & 0x0F) << 24) |
                                    (static_cast<uint32_t>(bytes[1]) << 16) |
                                    (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
            for (int links = 0; node >= 0x0100 && links < 0x8000; ++links) {
                uint8_t entry[6]{};
                if (!readOffset(node, entry, 4))
                    return storageError("walking the BASIC transfer lookup chain");
                const uint32_t candidate = (static_cast<uint32_t>(entry[0]) << 24) |
                                           (static_cast<uint32_t>(entry[1]) << 16) |
                                           (static_cast<uint32_t>(entry[2]) << 8) | entry[3];
                if (candidate == wanted) {
                    const uint8_t cache[] = {0x80, static_cast<uint8_t>(node >> 8),
                                             static_cast<uint8_t>(node)};
                    if (!writeOffset(record, cache, sizeof cache))
                        return storageError("caching a BASIC transfer target");
                    uint16_t displacement = 0;
                    if (addDisplacement &&
                        !readHalfOffset(static_cast<uint16_t>(record + 4), displacement))
                        return storageError("reading a BASIC transfer displacement");
                    ip_ = static_cast<uint16_t>(node + (addDisplacement ? displacement : 0));
                    return AssistResult::complete();
                }
                if (!readOffset(static_cast<uint16_t>(node + 4), entry + 4, 2))
                    return storageError("reading a BASIC transfer-chain link");
                node = static_cast<uint16_t>((entry[4] << 8) | entry[5]);
            }
        }

        uint16_t vector = 0;
        if (!loadHalf(addDisplacement ? 69 : 67, vector))
            return storageError("reading the BASIC lazy-transfer continuation");
        context_.setRequestHalf(RequestBlock::kOffIar,
                                context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
        ip_ = record;
        exit_ = true;
        context_.trace().basicAssist(
            "BASIC transfer unresolved record={:04X} flags={:02X} guest-vector={:04X}",
            record, bytes[0], vector);
        return AssistResult::complete();
    }

    AssistResult pushControlAndDispatch()
    {
        std::array<uint8_t, 8> record{};
        uint16_t field = 0, controlLimit = 0;
        if (!loadHalf(kCbControlLimit, controlLimit))
            return storageError("reading the BASIC control-record limit");
        if (controlPointer_ > controlLimit) {
            uint16_t vector = 0;
            if (!loadHalf(75, vector))
                return storageError("reading the BASIC control-overflow continuation");
            context_.setRequestHalf(RequestBlock::kOffIar,
                                    context_.requestHalf(RequestBlock::kOffArr));
            context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
            exit_ = true;
            return AssistResult::complete();
        }
        if (!readOffset(controlPointer_, record.data(), static_cast<int>(record.size())) ||
            !readHalfOffset(static_cast<uint16_t>(ip_ + 4), field))
            return storageError("decoding BASIC computed control");
        record[0] = 0xFF;
        record[1] = static_cast<uint8_t>(ip_ >> 8); record[2] = static_cast<uint8_t>(ip_);
        record[4] = static_cast<uint8_t>(field >> 8); record[5] = static_cast<uint8_t>(field);
        record[6] = static_cast<uint8_t>(sp_ >> 8); record[7] = static_cast<uint8_t>(sp_);
        if (!writeOffset(controlPointer_, record.data(), static_cast<int>(record.size())))
            return storageError("pushing a BASIC control record");
        controlPointer_ = static_cast<uint16_t>(controlPointer_ + record.size());
        ip_ = static_cast<uint16_t>(ip_ + 6);
        return computedTransfer(ip_, false);
    }

    AssistResult unwindControl()
    {
        if (controlPointer_ <= controlBase_) {
            uint16_t vector = 0;
            if (!loadHalf(73, vector))
                return storageError("reading the BASIC control-underflow continuation");
            context_.setRequestHalf(RequestBlock::kOffIar,
                                    context_.requestHalf(RequestBlock::kOffArr));
            context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
            exit_ = true;
            return AssistResult::complete();
        }
        controlPointer_ = static_cast<uint16_t>(controlPointer_ - 8);
        uint8_t record[8]{};
        if (!readOffset(controlPointer_, record, sizeof record))
            return storageError("reading a BASIC control record");
        const uint16_t continuation = static_cast<uint16_t>((record[4] << 8) | record[5]);
        if (continuation == 0) {
            uint16_t vector = 0;
            if (!loadHalf(81, vector))
                return storageError("reading the BASIC control-unwind continuation");
            context_.setRequestHalf(RequestBlock::kOffIar,
                                    context_.requestHalf(RequestBlock::kOffArr));
            context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
            exit_ = true;
            context_.trace().basicAssist(
                "BASIC control unwind record={:04X} continuation={:04X} guest-vector={:04X}",
                controlPointer_, continuation, vector);
            return AssistResult::complete();
        }
        const uint16_t descriptor = record[0] == 0xFF
                                        ? static_cast<uint16_t>((record[1] << 8) | record[2])
                                        : controlPointer_;
        context_.trace().basicAssist(
            "BASIC control return record={:04X} descriptor={:04X} displacement={:04X}",
            controlPointer_, descriptor, continuation);
        return computedTransfer(descriptor, true);
    }

    AssistResult forLoopControl()
    {
        uint16_t variable = 0, descriptor = 0;
        if (!readHalfOffset(ip_, variable) ||
            !readHalfOffset(static_cast<uint16_t>(ip_ + 2), descriptor))
            return storageError("decoding a BASIC FOR control operation");
        ip_ = static_cast<uint16_t>(ip_ + 4);

        uint8_t flags = 0, stepSign = 0;
        std::vector<uint8_t> limit(static_cast<std::size_t>(width_));
        std::vector<uint8_t> current(static_cast<std::size_t>(width_));
        if (!readOffset(descriptor, &flags, 1) ||
            !readOffset(static_cast<uint16_t>(descriptor + 1), limit.data(), width_) ||
            !readOffset(variable, current.data(), width_))
            return storageError("reading a BASIC FOR descriptor");
        if ((flags & 1) == 0 &&
            !readOffset(static_cast<uint16_t>(descriptor + 1 + width_), &stepSign, 1))
            return storageError("reading a BASIC FOR step sign");

        std::string error;
        auto limitNumber = BasicNumber::decode(limit.data(), limit.size(), precision_, &error);
        if (!limitNumber) return fatal("malformed BASIC FOR limit: " + error);
        auto currentNumber = BasicNumber::decode(current.data(), current.size(), precision_, &error);
        if (!currentNumber) return fatal("malformed BASIC FOR variable: " + error);
        const int relation = currentNumber->compare(*limitNumber);
        const int comparison = relation == 0 ? 0 : relation < 0 ? 1 : 2;
        bool direction = true;
        if ((flags & 1) == 0 && (stepSign & 0x80) != 0) direction = false;
        const bool keepActive = comparison == 0 || (comparison == 1 && direction) ||
                                (comparison == 2 && !direction);
        const uint8_t updatedFlags = keepActive ? static_cast<uint8_t>(flags | 0x80)
                                                : static_cast<uint8_t>(flags & 0x7F);
        if (!writeOffset(descriptor, &updatedFlags, 1))
            return storageError("updating a BASIC FOR descriptor");

        context_.trace().basicAssist(
            "BASIC FOR variable={:04X} descriptor={:04X} compare={} direction={} active={} "
            "flags={:02X}->{:02X} limit=[{}] current=[{}] step-sign={:02X}",
            variable, descriptor, comparison, direction ? 1 : 0, keepActive ? 1 : 0,
            flags, updatedFlags, fmt::join(limit, " "), fmt::join(current, " "),
            stepSign);
        return computedTransfer(static_cast<uint16_t>(ip_ + (keepActive ? 0 : 4)), false);
    }

    AssistResult forLoopGate()
    {
        uint16_t gateAddress = 0;
        uint8_t gate = 0;
        if (!readHalfOffset(ip_, gateAddress) || !readOffset(gateAddress, &gate, 1))
            return storageError("reading a BASIC FOR gate");
        context_.trace().basicAssist("BASIC FOR gate={:04X}:{:02X}", gateAddress, gate);
        if ((gate & 0x80) == 0) {
            uint16_t vector = 0;
            if (!loadHalf(85, vector))
                return storageError("reading the BASIC FOR continuation");
            ip_ = static_cast<uint16_t>(ip_ - 1);
            context_.setRequestHalf(RequestBlock::kOffIar,
                                    context_.requestHalf(RequestBlock::kOffArr));
            context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
            exit_ = true;
            return AssistResult::complete();
        }
        return computedTransfer(static_cast<uint16_t>(ip_ + 2), false);
    }

    AssistResult class5(uint8_t low)
    {
        if (low == 0) {
            if (!canPush(2)) return stackError("current operand reference");
            if (!writeHalfOffset(sp_, currentOperand_))
                return storageError("pushing the current BASIC operand reference");
            sp_ = static_cast<uint16_t>(sp_ + 2);
            return AssistResult::complete();
        }
        if (low == 1) {
            if (!requireTop(width_)) return stackError("absolute value");
            uint8_t first = 0; const uint16_t at = static_cast<uint16_t>(sp_ - width_);
            if (!readOffset(at, &first, 1)) return storageError("reading BASIC absolute-value operand");
            first &= 0x7F;
            if (!writeOffset(at, &first, 1)) return storageError("writing BASIC absolute-value result");
            return AssistResult::complete();
        }
        if (low == 2) {
            if (!requireTop(width_)) return stackError("numeric truncation");
            const uint16_t start = static_cast<uint16_t>(sp_ - width_);
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(start, value.data(), width_))
                return storageError("reading a BASIC truncate operand");
            const int exponent = value[0] & 0x7F;
            if (exponent < width_ + 64) {
                const int first = exponent > 64 ? exponent - 63 : 0;
                std::fill(value.begin() + first, value.end(), 0);
                if (!writeOffset(start, value.data(), width_))
                    return storageError("writing a BASIC truncate result");
            }
            return AssistResult::complete();
        }
        if (low == 3) {
            if (!popBytes(width_)) return stackError("cached numeric store");
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(sp_, value.data(), width_) ||
                !writeOffset(currentOperand_, value.data(), width_))
                return storageError("storing the cached BASIC numeric operand");
            return AssistResult::complete();
        }
        if (low == 4) {
            if (!requireTop(width_) || !canPush(width_)) return stackError("duplicate");
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(static_cast<uint16_t>(sp_ - width_), value.data(), width_) ||
                !writeOffset(sp_, value.data(), width_)) return storageError("duplicating a BASIC numeric value");
            sp_ = static_cast<uint16_t>(sp_ + width_);
            return AssistResult::complete();
        }
        if (low == 5) {
            uint16_t destination = 0;
            if (!fetchHalf(destination)) return storageError("decoding BASIC numeric store");
            if (!requireTop(width_)) return stackError("numeric store without pop");
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(static_cast<uint16_t>(sp_ - width_), value.data(), width_) ||
                !writeOffset(destination, value.data(), width_)) return storageError("storing a BASIC numeric value");
            return AssistResult::complete();
        }
        return fatal(fmt::format("unsupported BASIC class-5 opcode 5{:X}", low));
    }

    AssistResult class7(uint8_t low)
    {
        if (low == 6) {
            if (!requireTop(1)) return stackError("Boolean NOT");
            uint8_t value = 0;
            if (!readOffset(static_cast<uint16_t>(sp_ - 1), &value, 1)) return storageError("reading Boolean");
            value ^= 1;
            if (!writeOffset(static_cast<uint16_t>(sp_ - 1), &value, 1)) return storageError("writing Boolean");
            return AssistResult::complete();
        }
        if (low != 4 && low != 5) return fatal(fmt::format("invalid BASIC Boolean opcode 7{:X}", low));
        if (!requireTop(2)) return stackError("Boolean binary operation");
        uint8_t pair[2]{};
        if (!readOffset(static_cast<uint16_t>(sp_ - 2), pair, 2)) return storageError("reading Booleans");
        pair[0] = low == 4 ? static_cast<uint8_t>(pair[0] & pair[1]) : static_cast<uint8_t>(pair[0] | pair[1]);
        if (!writeOffset(static_cast<uint16_t>(sp_ - 2), pair, 1)) return storageError("writing Boolean result");
        --sp_;
        return AssistResult::complete();
    }

    AssistResult numeric(uint8_t low, uint16_t effective)
    {
        if (low == 0) {
            if (!canPush(2)) return stackError("reference push");
            if (!writeHalfOffset(sp_, effective)) return storageError("pushing a BASIC reference");
            sp_ = static_cast<uint16_t>(sp_ + 2); return AssistResult::complete();
        }
        if (low == 1) {
            if (!canPush(width_)) return stackError("numeric load");
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(effective, value.data(), width_) || !writeOffset(sp_, value.data(), width_))
                return storageError("loading a BASIC numeric value");
            sp_ = static_cast<uint16_t>(sp_ + width_); return AssistResult::complete();
        }
        if (low == 2) {
            if (!popBytes(2)) return stackError("assignment reference");
            uint16_t destination = 0; std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readHalfOffset(sp_, destination) || !readOffset(effective, value.data(), width_) ||
                !writeOffset(destination, value.data(), width_)) return storageError("assigning a BASIC numeric value");
            return AssistResult::complete();
        }
        if (low == 3) {
            if (!requireTop(width_)) return stackError("numeric store");
            std::vector<uint8_t> value(static_cast<std::size_t>(width_));
            if (!readOffset(static_cast<uint16_t>(sp_ - width_), value.data(), width_) ||
                !writeOffset(effective, value.data(), width_)) return storageError("storing a BASIC numeric value");
            sp_ = static_cast<uint16_t>(sp_ - width_); return AssistResult::complete();
        }
        if (low >= 4 && low <= 7) return arithmetic(low, effective);
        if (low >= 8 && low <= 0xD) return comparison(low, effective);
        if (low == 0xE) {
            std::vector<uint8_t> bytes(static_cast<std::size_t>(width_));
            if (!readOffset(effective, bytes.data(), width_)) return storageError("reading BASIC negate operand");
            std::string error; auto value = BasicNumber::decode(bytes.data(), bytes.size(), precision_, &error);
            if (!value) return fatal("malformed BASIC number: " + error);
            bytes = value->negated().encode();
            if (!canPush(width_)) return stackError("numeric negate result");
            if (!writeOffset(sp_, bytes.data(), width_)) return storageError("pushing BASIC negate result");
            sp_ = static_cast<uint16_t>(sp_ + width_);
            return AssistResult::complete();
        }
        if (low == 0xF) {
            std::vector<uint8_t> bytes(static_cast<std::size_t>(width_));
            if (!readOffset(effective, bytes.data(), width_))
                return storageError("reading BASIC integer-conversion operand");
            std::string error;
            auto value = BasicNumber::decode(bytes.data(), bytes.size(), precision_, &error);
            if (!value) return fatal("malformed BASIC number: " + error);
            BasicNumber::Condition condition = BasicNumber::Condition::None;
            const uint16_t integer = static_cast<uint16_t>(value->roundedInteger(condition));
            if (!canPush(2)) return stackError("integer-conversion result");
            if (!writeHalfOffset(sp_, integer)) return storageError("pushing BASIC integer-conversion result");
            sp_ = static_cast<uint16_t>(sp_ + 2);
            return AssistResult::complete();
        }
        return fatal("unsupported BASIC numeric conversion opcode");
    }

    AssistResult indexedNumeric(uint8_t low, bool twoDimensions)
    {
        uint16_t descriptor = 0;
        if (!fetchHalf(descriptor)) return storageError("decoding a numeric array descriptor");
        uint16_t base = 0, stride = 0, secondExtent = 0;
        if (!readHalfOffset(descriptor, base) ||
            !readHalfOffset(static_cast<uint16_t>(descriptor + 2), stride) ||
            !readHalfOffset(static_cast<uint16_t>(descriptor + 4), secondExtent))
            return storageError("reading a numeric array descriptor");

        int first = 0;
        bool indexOk = false;
        AssistResult indexResult = popArrayIndex(first, indexOk);
        if (!indexOk) return indexResult;
        const int lowerBound = (context_.taskByte(0) & 0x80) != 0 ? 0 : 1;
        const uint32_t firstOffset = static_cast<uint32_t>(width_) * static_cast<uint32_t>(first);
        if (first < lowerBound || firstOffset > 0xFFFF || firstOffset >= stride)
            return arrayRangeError("first numeric array index");
        uint32_t offset = firstOffset;

        if (twoDimensions) {
            int second = 0;
            indexResult = popArrayIndex(second, indexOk);
            if (!indexOk) return indexResult;
            if (second < lowerBound) return arrayRangeError("second numeric array index");
            if (secondExtent == 0) return undefinedArrayError("numeric array");
            if (static_cast<uint32_t>(second - lowerBound) >= secondExtent)
                return arrayRangeError("second numeric array index");
            const uint32_t secondOffset = static_cast<uint32_t>(second - lowerBound) * stride;
            if (secondOffset > 0xFFFF) return arrayRangeError("second numeric array index");
            offset += secondOffset;
        } else if (secondExtent == 0) return undefinedArrayError("numeric array");
        currentOperand_ = static_cast<uint16_t>(base + offset);
        return numeric(low, currentOperand_);
    }

    AssistResult popArrayIndex(int& value, bool& ok)
    {
        ok = false;
        if (!popBytes(width_)) return stackError("numeric array index");
        std::vector<uint8_t> bytes(static_cast<std::size_t>(width_));
        if (!readOffset(sp_, bytes.data(), width_))
            return storageError("reading a numeric array index");
        std::string error;
        auto number = BasicNumber::decode(bytes.data(), bytes.size(), precision_, &error);
        if (!number) return fatal("malformed BASIC array index: " + error);
        BasicNumber::Condition condition = BasicNumber::Condition::None;
        value = number->roundedInteger(condition);
        if (condition != BasicNumber::Condition::None)
            return arrayRangeError("numeric array index");
        ok = true;
        return AssistResult::complete();
    }

    AssistResult indexedString(uint8_t low, bool twoDimensions)
    {
        uint16_t descriptor = 0;
        if (!fetchHalf(descriptor)) return storageError("decoding a string array descriptor");
        uint16_t base = 0, stride = 0, secondExtent = 0;
        uint8_t capacity = 0;
        if (!readHalfOffset(descriptor, base) ||
            !readHalfOffset(static_cast<uint16_t>(descriptor + 2), stride) ||
            !readHalfOffset(static_cast<uint16_t>(descriptor + 4), secondExtent) ||
            !readOffset(static_cast<uint16_t>(descriptor + 8), &capacity, 1))
            return storageError("reading a string array descriptor");

        int first = 0;
        bool indexOk = false;
        AssistResult indexResult = popArrayIndex(first, indexOk);
        if (!indexOk) return indexResult;
        const int lowerBound = (context_.taskByte(0) & 0x80) != 0 ? 0 : 1;
        const uint32_t elementSize = static_cast<uint32_t>(capacity) + 1;
        const uint32_t firstOffset = elementSize * static_cast<uint32_t>(first);
        if (first < lowerBound || firstOffset > 0xFFFF || firstOffset >= stride)
            return arrayRangeError("first string array index");
        uint32_t offset = firstOffset;

        if (twoDimensions) {
            int second = 0;
            indexResult = popArrayIndex(second, indexOk);
            if (!indexOk) return indexResult;
            if (second < lowerBound) return arrayRangeError("second string array index");
            if (secondExtent == 0) return undefinedArrayError("string array");
            const uint32_t adjusted = static_cast<uint32_t>(second - lowerBound);
            if (adjusted >= secondExtent) return arrayRangeError("second string array index");
            const uint32_t secondOffset = adjusted * stride;
            if (secondOffset > 0xFFFF) return arrayRangeError("second string array index");
            offset += secondOffset;
        } else if (secondExtent == 0) return undefinedArrayError("string array");

        return stringOperation(low, static_cast<uint16_t>(base + offset), capacity);
    }

    AssistResult arrayRangeError(const char* operation)
    {
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, context_.taskHalf(83));
        exit_ = true;
        return {AssistStatus::Completed, fmt::format("BASIC {} is outside its declared bounds", operation)};
    }

    AssistResult undefinedArrayError(const char* operation)
    {
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, context_.taskHalf(95));
        exit_ = true;
        return {AssistStatus::Completed, fmt::format("BASIC {} is not allocated", operation)};
    }

    AssistResult stringOperation(uint8_t low, uint16_t effective, uint8_t capacity)
    {
        if (low == 0) {
            if (!canPush(3)) return stackError("string lvalue descriptor");
            const uint8_t descriptor[] = {capacity, static_cast<uint8_t>(effective >> 8),
                                          static_cast<uint8_t>(effective)};
            if (!writeOffset(sp_, descriptor, sizeof descriptor))
                return storageError("pushing a string lvalue descriptor");
            sp_ = static_cast<uint16_t>(sp_ + sizeof descriptor);
            return AssistResult::complete();
        }

        uint8_t rightLength = 0;
        if (!readOffset(effective, &rightLength, 1))
            return storageError("reading a BASIC string length");

        if (low == 1) {
            const int resultSize = static_cast<int>(rightLength) + 2;
            if (!canPush(resultSize)) return stackError("expanded string result");
            std::vector<uint8_t> result(static_cast<std::size_t>(resultSize));
            if (!readOffset(effective, result.data(), rightLength + 1))
                return storageError("reading a BASIC string value");
            result.back() = rightLength;
            if (!writeOffset(sp_, result.data(), resultSize))
                return storageError("pushing a BASIC string value");
            sp_ = static_cast<uint16_t>(sp_ + resultSize);
            return AssistResult::complete();
        }

        if (low == 2) {
            if (!popBytes(3)) return stackError("string assignment reference");
            uint8_t descriptor[3]{};
            if (!readOffset(sp_, descriptor, sizeof descriptor))
                return storageError("reading a string lvalue descriptor");
            uint8_t n = rightLength;
            if (!limitString(n, descriptor[0])) return stringRangeError("string assignment");
            const uint16_t destination = static_cast<uint16_t>((descriptor[1] << 8) | descriptor[2]);
            std::vector<uint8_t> value(static_cast<std::size_t>(n) + 1);
            value[0] = n;
            if (n != 0 && !readOffset(static_cast<uint16_t>(effective + 1), value.data() + 1, n))
                return storageError("reading a string assignment value");
            if (!writeOffset(destination, value.data(), static_cast<int>(value.size())))
                return storageError("writing a string assignment value");
            return AssistResult::complete();
        }

        if (low == 3) {
            if (!requireTop(1)) return stackError("expanded string assignment");
            uint8_t n = 0;
            if (!readOffset(static_cast<uint16_t>(sp_ - 1), &n, 1))
                return storageError("reading an expanded string assignment length");
            if (!requireTop(static_cast<int>(n) + 2)) return stackError("expanded string assignment");
            const uint16_t source = static_cast<uint16_t>(sp_ - n - 2);
            sp_ = source;
            if (!limitString(n, capacity)) return stringRangeError("string assignment");
            std::vector<uint8_t> value(static_cast<std::size_t>(n) + 1);
            value[0] = n;
            if (n != 0 && !readOffset(static_cast<uint16_t>(source + 1), value.data() + 1, n))
                return storageError("reading an expanded string assignment value");
            if (!writeOffset(effective, value.data(), static_cast<int>(value.size())))
                return storageError("writing an expanded string assignment value");
            return AssistResult::complete();
        }

        if (low == 5 || low > 0xD)
            return fatal(fmt::format("invalid BASIC string opcode {:X}", low));

        if (low == 6) {
            if (!requireTop(1)) return stackError("string concatenation");
            uint8_t leftLength = 0;
            if (!readOffset(static_cast<uint16_t>(sp_ - 1), &leftLength, 1))
                return storageError("reading the left string length");
            if (!requireTop(static_cast<int>(leftLength) + 2)) return stackError("string concatenation");
            unsigned total = static_cast<unsigned>(leftLength) + rightLength;
            uint8_t append = rightLength;
            if (total > 255) {
                append = static_cast<uint8_t>(255 - leftLength);
                if (!stringTruncationEnabled()) return stringRangeError("string concatenation");
                total = 255;
            }
            if (!canPush(append)) return stackError("string concatenation result");
            std::vector<uint8_t> chars(append);
            if (append != 0 && !readOffset(static_cast<uint16_t>(effective + 1), chars.data(), append))
                return storageError("reading the right string value");
            const uint16_t oldTrailer = static_cast<uint16_t>(sp_ - 1);
            if (append != 0 && !writeOffset(oldTrailer, chars.data(), append))
                return storageError("appending a BASIC string");
            const uint16_t resultStart = static_cast<uint16_t>(sp_ - leftLength - 2);
            const uint8_t resultLength = static_cast<uint8_t>(total);
            if (!writeOffset(resultStart, &resultLength, 1) ||
                !writeOffset(static_cast<uint16_t>(oldTrailer + append), &resultLength, 1))
                return storageError("writing a concatenated string length");
            sp_ = static_cast<uint16_t>(sp_ + append);
            return AssistResult::complete();
        }

        if (low == 7) {
            BasicNumber::Condition condition = BasicNumber::Condition::None;
            const auto encoded = BasicNumber::fromInteger(rightLength, precision_, condition).encode();
            if (!canPush(width_)) return stackError("string length result");
            if (!writeOffset(sp_, encoded.data(), width_))
                return storageError("pushing a BASIC string length");
            sp_ = static_cast<uint16_t>(sp_ + width_);
            return AssistResult::complete();
        }

        if (low >= 8 && low <= 0xD) {
            if (!requireTop(1)) return stackError("string comparison");
            uint8_t leftLength = 0;
            if (!readOffset(static_cast<uint16_t>(sp_ - 1), &leftLength, 1))
                return storageError("reading the left string length");
            if (!requireTop(static_cast<int>(leftLength) + 2)) return stackError("string comparison");
            const uint16_t left = static_cast<uint16_t>(sp_ - leftLength - 2);
            sp_ = left;
            uint8_t pad = 0;
            if (!readOffset(static_cast<uint16_t>((xr1_ & 0xFFFF) + kCbStringPad), &pad, 1))
                return storageError("reading the BASIC string comparison pad");
            int relation = 0;
            const unsigned common = std::max<unsigned>(leftLength, rightLength);
            for (unsigned i = 0; i < common; ++i) {
                uint8_t lhs = pad, rhs = pad;
                if (i < leftLength && !readOffset(static_cast<uint16_t>(left + 1 + i), &lhs, 1))
                    return storageError("reading the left string comparison operand");
                if (i < rightLength && !readOffset(static_cast<uint16_t>(effective + 1 + i), &rhs, 1))
                    return storageError("reading the right string comparison operand");
                if (lhs != rhs) { relation = lhs < rhs ? -1 : 1; break; }
            }
            const bool answer = low == 8 ? relation < 0 : low == 9 ? relation <= 0
                              : low == 0xA ? relation > 0 : low == 0xB ? relation >= 0
                              : low == 0xC ? relation != 0 : relation == 0;
            const uint8_t value = answer ? 1 : 0;
            if (!writeOffset(sp_, &value, 1))
                return storageError("writing a string comparison result");
            ++sp_;
            return AssistResult::complete();
        }

        return fatal("unsupported BASIC substring opcode");
    }

    bool stringTruncationEnabled()
    {
        uint8_t mode = 0;
        return readOffset(static_cast<uint16_t>((xr1_ & 0xFFFF) + 4), &mode, 1) && mode == 1;
    }

    bool limitString(uint8_t& length, uint8_t capacity)
    {
        if (length <= capacity) return true;
        if (!stringTruncationEnabled()) return false;
        length = capacity;
        return true;
    }

    AssistResult stringRangeError(const char* operation)
    {
        uint16_t vector = 0;
        if (!loadHalf(kCbStringRangeVector, vector))
            return storageError("reading the BASIC string-range continuation");
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
        exit_ = true;
        return {AssistStatus::Completed, fmt::format("BASIC {} exceeded its string capacity", operation)};
    }

    AssistResult arithmetic(uint8_t low, uint16_t effective)
    {
        if (!requireTop(width_)) return stackError("numeric arithmetic");
        std::vector<uint8_t> leftBytes(static_cast<std::size_t>(width_)), rightBytes(static_cast<std::size_t>(width_));
        const uint16_t leftAt = static_cast<uint16_t>(sp_ - width_);
        if (!readOffset(leftAt, leftBytes.data(), width_) || !readOffset(effective, rightBytes.data(), width_))
            return storageError("reading BASIC arithmetic operands");
        std::string error;
        auto left = BasicNumber::decode(leftBytes.data(), leftBytes.size(), precision_, &error);
        if (!left) return fatal("malformed BASIC left operand: " + error);
        auto right = BasicNumber::decode(rightBytes.data(), rightBytes.size(), precision_, &error);
        if (!right) return fatal("malformed BASIC right operand: " + error);
        BasicNumber::Condition condition = BasicNumber::Condition::None;
        BasicNumber result = low == 4 ? left->add(*right, condition) : low == 5 ? left->subtract(*right, condition)
                           : low == 6 ? left->multiply(*right, condition) : left->divide(*right, condition);
        std::vector<uint8_t> encoded = result.encode();
        if (condition == BasicNumber::Condition::DivideByZero) {
            encoded.assign(static_cast<std::size_t>(width_), 99);
            encoded[0] = 0x7F;
        }
        if (condition != BasicNumber::Condition::None) {
            const int modeOffset = condition == BasicNumber::Condition::Overflow ? 1
                                 : condition == BasicNumber::Condition::Underflow ? 2 : 3;
            const int vectorOffset = condition == BasicNumber::Condition::Overflow ? 87
                                   : condition == BasicNumber::Condition::Underflow ? 89 : 91;
            uint8_t mode = 0;
            if (!readOffset(static_cast<uint16_t>((xr1_ & 0xFFFF) + modeOffset), &mode, 1))
                return storageError("reading a BASIC arithmetic error mode");
            if (mode != 2 && !writeOffset(leftAt, encoded.data(), width_))
                return storageError("writing a substituted BASIC arithmetic result");
            if (mode != 1) {
                uint16_t vector = 0;
                if (!loadHalf(vectorOffset, vector))
                    return storageError("reading a BASIC arithmetic error continuation");
                context_.setRequestHalf(RequestBlock::kOffIar,
                                        context_.requestHalf(RequestBlock::kOffArr));
                context_.setRequestHalf(RequestBlock::kOffXr2Low, vector);
                exit_ = true;
                context_.trace().basicAssist(
                    "BASIC arithmetic condition={} mode={} guest-vector={:04X}",
                    static_cast<int>(condition), mode, vector);
                return AssistResult::complete();
            }
        } else if (!writeOffset(leftAt, encoded.data(), width_)) {
            return storageError("writing BASIC arithmetic result");
        }
        context_.trace().basicAssist("BASIC arithmetic op={:X} condition={}", low, static_cast<int>(condition));
        return AssistResult::complete();
    }

    AssistResult comparison(uint8_t low, uint16_t effective)
    {
        if (!requireTop(width_)) return stackError("numeric comparison");
        std::vector<uint8_t> leftBytes(static_cast<std::size_t>(width_)), rightBytes(static_cast<std::size_t>(width_));
        sp_ = static_cast<uint16_t>(sp_ - width_);
        if (!readOffset(sp_, leftBytes.data(), width_) || !readOffset(effective, rightBytes.data(), width_))
            return storageError("reading BASIC comparison operands");
        std::string error;
        auto left = BasicNumber::decode(leftBytes.data(), leftBytes.size(), precision_, &error);
        if (!left) return fatal("malformed BASIC comparison operand: " + error);
        auto right = BasicNumber::decode(rightBytes.data(), rightBytes.size(), precision_, &error);
        if (!right) return fatal("malformed BASIC comparison operand: " + error);
        const int relation = left->compare(*right);
        const bool answer = low == 8 ? relation < 0 : low == 9 ? relation <= 0 : low == 0xA ? relation > 0
                          : low == 0xB ? relation >= 0 : low == 0xC ? relation != 0 : relation == 0;
        const uint8_t value = answer ? 1 : 0;
        context_.trace().basicAssist(
            "BASIC compare op={:X} left={} right={} relation={} result={}", low,
            fmt::join(leftBytes, " "), fmt::join(rightBytes, " "), relation, value);
        if (!writeOffset(sp_, &value, 1)) return storageError("writing BASIC comparison result");
        ++sp_; return AssistResult::complete();
    }

    bool commit()
    {
        return context_.writeGuestHalf(AssistContext::guestOffset(control_, kCbIp), ip_) &&
               context_.writeGuestHalf(AssistContext::guestOffset(control_, kCbStackPointer), sp_) &&
               context_.writeGuestHalf(AssistContext::guestOffset(control_, kCbControlPointer), controlPointer_);
    }
    void restoreTaskFlags() { context_.setTaskByte(4, static_cast<uint8_t>(context_.taskByte(4) & 0xF5)); }
    AssistResult fatal(std::string detail)
    {
        if (!commit()) return storageError("committing BASIC state after an error");
        restoreTaskFlags(); return AssistResult::guestError(61, std::move(detail));
    }
    AssistResult stackError(const char* operation)
    {
        uint16_t vector = 0;
        if (!loadHalf(kCbStackErrorVector, vector)) return storageError("reading BASIC stack-error vector");
        context_.setRequestHalf(RequestBlock::kOffIar, context_.requestHalf(RequestBlock::kOffArr));
        context_.setRequestHalf(RequestBlock::kOffXr2Low, vector); exit_ = true;
        return {AssistStatus::Completed, fmt::format("BASIC {} used stack outside {:04X}..{:04X}",
                                                    operation, stackBase_, stackLimit_)};
    }
    AssistResult storageError(const char* operation)
    {
        return AssistResult::failed(fmt::format("{}: {}", operation, context_.storageFailure()));
    }

    int address(uint16_t offset) const { return translated_ ? 0x800000 | offset : offset; }
    bool read(int guestAddress, uint8_t* bytes, int length) { return context_.readGuest(guestAddress, bytes, length); }
    bool readOffset(uint16_t offset, uint8_t* bytes, int length) { return read(address(offset), bytes, length); }
    bool writeOffset(uint16_t offset, const uint8_t* bytes, int length) { return context_.writeGuest(address(offset), bytes, length); }
    bool readHalfOffset(uint16_t offset, uint16_t& value) { return context_.readGuestHalf(address(offset), value); }
    bool writeHalfOffset(uint16_t offset, uint16_t value) { return context_.writeGuestHalf(address(offset), value); }
    bool fetchHalf(uint16_t& value)
    {
        if (!readHalfOffset(ip_, value)) return false;
        ip_ = static_cast<uint16_t>(ip_ + 2); return true;
    }
    bool loadHalf(int cbOffset, uint16_t& value) { return context_.readGuestHalf(AssistContext::guestOffset(control_, cbOffset), value); }
    bool canPush(int count) const
    {
        return count >= 0 && static_cast<unsigned>(sp_) + static_cast<unsigned>(count) <=
                                 static_cast<unsigned>(stackLimit_) + 1;
    }
    bool requireTop(int count) const { return count >= 0 && sp_ >= stackBase_ + count; }
    bool popBytes(int count) { if (!requireTop(count)) return false; sp_ = static_cast<uint16_t>(sp_ - count); return true; }

    AssistContext& context_;
    int xr1_ = 0, control_ = 0;
    bool translated_ = false, exit_ = false;
    uint16_t ip_ = 0, stackBase_ = 0, sp_ = 0, stackLimit_ = 0;
    uint16_t controlBase_ = 0, controlPointer_ = 0;
    uint16_t currentOperand_ = 0;
    int width_ = 5;
    BasicNumber::Precision precision_ = BasicNumber::Precision::Short;
};

}  // namespace

AssistResult BasicAssist::execute(AssistContext& context) { return BasicMachine(context).run(); }

}  // namespace sim36::processors::controlstorage
