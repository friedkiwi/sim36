#include "Processors/ControlStorage/ExtendedControlStoreAssist.h"

#include <vector>

#include <fmt/format.h>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"

namespace sim36::processors::controlstorage {

bool AssistContext::resolveGuest(int address, bool forWrite, int& real)
{
    if (machine_.resolveGuest24(address, forWrite, real)) return true;
    failedStorage(forWrite ? "resolve for write" : "resolve for read", address, 1);
    return false;
}

bool AssistContext::readGuest(int address, uint8_t* destination, int length)
{
    std::vector<std::pair<int, int>> extents;
    if (length < 0 || (length != 0 && destination == nullptr) ||
        !machine_.guest24Extents(address, length, false, extents)) {
        failedStorage("read", address, length);
        return false;
    }
    for (const auto& extent : extents) {
        if (!machine_.inRange(extent.first, extent.second)) {
            failedStorage("read", address, length);
            return false;
        }
    }
    int offset = 0;
    for (const auto& extent : extents) {
        machine_.read(extent.first, destination + offset, extent.second);
        offset += extent.second;
    }
    if (!machine_.faulted()) return true;
    failedStorage("read", address, length);
    return false;
}

bool AssistContext::writeGuest(int address, const uint8_t* source, int length)
{
    std::vector<std::pair<int, int>> extents;
    if (length < 0 || (length != 0 && source == nullptr) ||
        !machine_.guest24Extents(address, length, true, extents)) {
        failedStorage("write", address, length);
        return false;
    }
    // Validate the complete destination first.  A bad tail must not leave a
    // partially updated BASIC string, stack value, or FORTRAN operand.
    for (const auto& extent : extents) {
        if (!machine_.inRange(extent.first, extent.second)) {
            failedStorage("write", address, length);
            return false;
        }
    }
    int offset = 0;
    for (const auto& extent : extents) {
        machine_.write(extent.first, source + offset, extent.second);
        offset += extent.second;
    }
    if (!machine_.faulted()) return true;
    failedStorage("write", address, length);
    return false;
}

bool AssistContext::readGuestByte(int address, uint8_t& value)
{
    return readGuest(address, &value, 1);
}

bool AssistContext::writeGuestByte(int address, uint8_t value)
{
    return writeGuest(address, &value, 1);
}

bool AssistContext::readGuestHalf(int address, uint16_t& value)
{
    uint8_t bytes[2] = {};
    if (!readGuest(address, bytes, 2)) return false;
    value = static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
    return true;
}

bool AssistContext::writeGuestHalf(int address, uint16_t value)
{
    const uint8_t bytes[] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    return writeGuest(address, bytes, 2);
}

int AssistContext::guestOffset(int address, int displacement)
{
    if ((address & 0x800000) != 0)
        return 0x800000 | ((address + displacement) & 0xFFFF);
    return (address + displacement) & 0x7FFFFF;
}

uint8_t AssistContext::requestByte(int offset) { return machine_.readByte(requestBlock_ + offset); }
uint16_t AssistContext::requestHalf(int offset) { return machine_.readHalf(requestBlock_ + offset); }
int AssistContext::requestAddress(int offset) { return machine_.readAddr24(requestBlock_ + offset); }
uint16_t AssistContext::requestWorkRegister(int number)
{
    return RequestBlock::readWr(machine_, requestBlock_, number);
}
void AssistContext::setRequestByte(int offset, uint8_t value) { machine_.writeByte(requestBlock_ + offset, value); }
void AssistContext::setRequestHalf(int offset, uint16_t value) { machine_.writeHalf(requestBlock_ + offset, value); }
void AssistContext::setRequestAddress(int offset, int value) { machine_.writeAddr24(requestBlock_ + offset, value); }
void AssistContext::setRequestWorkRegister(int number, uint16_t value)
{
    RequestBlock::writeWr(machine_, requestBlock_, number, value);
}

uint8_t AssistContext::taskByte(int offset) { return machine_.readByte(taskBlock_ + offset); }
uint16_t AssistContext::taskHalf(int offset) { return machine_.readHalf(taskBlock_ + offset); }
int AssistContext::taskAddress(int offset) { return machine_.readAddr24(taskBlock_ + offset); }
void AssistContext::setTaskByte(int offset, uint8_t value) { machine_.writeByte(taskBlock_ + offset, value); }
void AssistContext::setTaskHalf(int offset, uint16_t value) { machine_.writeHalf(taskBlock_ + offset, value); }
void AssistContext::setTaskAddress(int offset, int value) { machine_.writeAddr24(taskBlock_ + offset, value); }

void AssistContext::failedStorage(const char* operation, int address, int length)
{
    if (!storageFailure_.empty()) return;
    storageFailure_ = fmt::format("{} of {} byte(s) at guest {:06X} failed", operation, length,
                                  address & 0xFFFFFF);
    if (machine_.faulted()) storageFailure_ += ": " + machine_.faultMessage();
    trace_.csp("extended-control-storage assist: {}", storageFailure_);
}

}  // namespace sim36::processors::controlstorage
