#include "Devices/IWorkStationBackend.h"

#include "Devices/VirtualPrinter.h"
#include "Devices/VirtualWorkstation.h"

namespace sim36::devices {

std::string VirtualWorkstationBackend::name() const { return ws_.id(); }
bool VirtualWorkstationBackend::attached() const { return ws_.attached(); }
std::string VirtualWorkstationBackend::userName() const { return ws_.backend().userName(); }
bool VirtualWorkstationBackend::put(const uint8_t* data, int offset, int length) { return ws_.outputData(data, offset, length); }
bool VirtualWorkstationBackend::putWithInvite(const uint8_t* data, int offset, int length)
{
    return ws_.outputDataWithInvite(data, offset, length);
}
bool VirtualWorkstationBackend::beginSaveScreen() { return ws_.beginSaveScreen(); }
bool VirtualWorkstationBackend::tryTakeSaveScreen(std::vector<uint8_t>& body) { return ws_.tryTakeSaveScreen(body); }
int VirtualWorkstationBackend::pendingSaveScreens() const { return ws_.pendingSaveScreens(); }
bool VirtualWorkstationBackend::restoreScreen(const uint8_t* data, int offset, int length)
{
    return ws_.restoreScreen(data, offset, length);
}
bool VirtualWorkstationBackend::resumeSavedReadMode(uint8_t mode) { return ws_.resumeSavedReadMode(mode); }
void VirtualWorkstationBackend::setInputEnabled(bool enabled)
{
    if (enabled) ws_.invite();
    else ws_.cancelInvite();
}
bool VirtualWorkstationBackend::tryTakeInputStatus(uint16_t& cursor, uint8_t& aid) { return ws_.tryTakeInputStatus(cursor, aid); }
bool VirtualWorkstationBackend::tryCompleteInviteResponse() { return ws_.tryCompleteInviteResponse(); }
bool VirtualWorkstationBackend::inviteResponsePending() const { return ws_.inviteResponsePending(); }
bool VirtualWorkstationBackend::retainedInputHasFields() const { return ws_.retainedInputHasFields(); }
bool VirtualWorkstationBackend::pendingInputHasFields() const { return ws_.pendingInputHasFields(); }
bool VirtualWorkstationBackend::tryTakeInput(std::vector<uint8_t>& stream) { return ws_.tryTakeInput(stream); }
bool VirtualWorkstationBackend::tryTakeInputFields(uint8_t command, std::vector<uint8_t>& stream)
{
    return ws_.tryTakeInputFields(command, stream);
}
int VirtualWorkstationBackend::pendingInput() const { return ws_.pendingInput(); }

std::string VirtualPrinterBackend::name() const { return p_.id(); }
bool VirtualPrinterBackend::attached() const { return p_.attached(); }
bool VirtualPrinterBackend::sendDataStream(const uint8_t* data, int offset, int length)
{
    return p_.outputData(data, offset, length);
}
bool VirtualPrinterBackend::endJob() { return p_.endJob(); }

}  // namespace sim36::devices
