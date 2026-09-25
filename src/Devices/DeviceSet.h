// The machine's device complement and the device half of the delayed SVC
// path.  Every device SVC takes the IOB address in XR1 and each request
// becomes an action control element, so this is the ACE path in practice.
//
// The fixed disk and the work station controller are modelled; the devices
// the volume's unit definition table declares but the emulator does not
// model (data communications, the diskette data compression assist, the
// data storage controller) are answered "not in this configuration"; the
// diskette and tape drives arrive with milestone 7.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Devices/VirtualDiskette.h"
#include "Devices/VirtualFixedDisk.h"
#include "Devices/VirtualTape.h"
#include "Devices/VirtualPrinter.h"
#include "Devices/VirtualWorkstation.h"
#include "Devices/WorkStationController.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"
#include "Storage/DiskBackend.h"

namespace sim36::devices {

// A keyed table with the reference's enumeration order: entries are kept
// in the order they were added, a removed entry's slot is reused by the
// next addition (most recently freed first), and enumeration walks the
// slots in index order.  The pending-request tables are walked in that
// order when a response is delivered, so the order is observable.
template <typename K, typename V>
class SlotMap {
public:
    struct Entry {
        K key;
        V value;
        bool live;
    };

    V* find(const K& key)
    {
        for (auto& e : entries_)
            if (e.live && e.key == key) return &e.value;
        return nullptr;
    }
    const V* find(const K& key) const
    {
        for (const auto& e : entries_)
            if (e.live && e.key == key) return &e.value;
        return nullptr;
    }
    bool contains(const K& key) const { return find(key) != nullptr; }
    // Add or replace.
    void set(const K& key, V value)
    {
        if (V* at = find(key)) {
            *at = std::move(value);
            return;
        }
        if (!freeList_.empty()) {
            std::size_t slot = freeList_.back();
            freeList_.pop_back();
            entries_[slot] = Entry{key, std::move(value), true};
            return;
        }
        entries_.push_back(Entry{key, std::move(value), true});
    }
    bool erase(const K& key)
    {
        for (std::size_t i = 0; i < entries_.size(); i++)
            if (entries_[i].live && entries_[i].key == key) {
                entries_[i].live = false;
                entries_[i].value = V();
                freeList_.push_back(i);
                return true;
            }
        return false;
    }
    void clear()
    {
        entries_.clear();
        freeList_.clear();
    }
    int size() const
    {
        int n = 0;
        for (const auto& e : entries_)
            if (e.live) n++;
        return n;
    }
    // The live entries in enumeration order, as a snapshot: callers remove
    // entries while walking and then stop, as the reference does.
    std::vector<std::pair<K, V*>> items()
    {
        std::vector<std::pair<K, V*>> out;
        for (auto& e : entries_)
            if (e.live) out.emplace_back(e.key, &e.value);
        return out;
    }
    std::vector<K> keys() const
    {
        std::vector<K> out;
        for (const auto& e : entries_)
            if (e.live) out.push_back(e.key);
        return out;
    }

private:
    std::vector<Entry> entries_;
    std::vector<std::size_t> freeList_;
};

class DeviceSet {
public:
    // The byte the machine's own decline writes into the guest structure:
    // "not in this configuration".
    static constexpr uint8_t kNotInThisConfiguration = 0x20;
    static constexpr int kOffDeviceStatus = 12;
    // Emulator policy: the per-device status codes are not in this corpus,
    // so a single non-zero "did not succeed" stands in.
    static constexpr int kDeviceNotAvailable = 3;

    // A command-42 Read Input Fields result that has been parsed and
    // staged, held so it can also be delivered onto the work-space storage
    // block's OWN resident frame the moment the display manager maps that
    // block (SVC 2F action 4). The earlier A7 PUT page is retained only as
    // provenance that this is the display-manager handshake; it is a real
    // frame and must never be combined with this logical displacement.
    struct DeferredWorkStationInput {
        std::vector<uint8_t> bytes;   // the parsed field bytes (response+3..)
        int blockDisplacement = 0;    // byte offset into the work-space block
    };

    // What a snapshot of the pending device state carries (milestone 7).
    struct PendingCheckpoint {
        std::vector<int> inputReadPairs;
        std::vector<int> inputStagingPairs;
        std::vector<int> inputResponseStatus;
        std::vector<int> pendingC1Pairs;
        std::vector<int> controllerInvites;
        std::vector<int> printerOutputs;
        std::vector<int> pendingActivationUnits;
        std::vector<int> activatedUnits;
        std::vector<int> nativeActiveUnits;
        std::vector<int> configuredUnits;
        std::vector<int> internalRendererUnits;
        std::vector<int> transferRendererUnits;
    };

    DeviceSet(machine::MachineState& m, storage::DiskBackend& volume, monitor::Tracer& trace);
    DeviceSet(const DeviceSet&) = delete;
    DeviceSet& operator=(const DeviceSet&) = delete;

    VirtualFixedDisk disk;
    VirtualDiskette diskette;
    VirtualTape tape;
    WorkStationController& workStations() { return workStations_; }
    const WorkStationController& workStations() const { return workStations_; }

    // Whether auto-configuration is enabled gates the configuration reader's
    // whole loop and one of the configurer's checks.  It is a persistent
    // machine attribute with nothing on the volume to derive it from, so it
    // is modelled as on, which is what a machine that has stations to
    // report does.
    bool autoConfigEnabled = true;

    // The station and printer models are owned by the machine; the device
    // set keeps them by identity and gives each a controller slot.
    void addStation(VirtualWorkstation& ws);
    // A work-station-attached printer takes a slot on the same controller as
    // the displays (SA21-9436 5-50).  What separates it is the device class
    // in the configuration record, not a separate controller.
    void addPrinter(VirtualPrinter& p);
    std::vector<VirtualWorkstation*> stations();
    std::vector<VirtualPrinter*> printers();

    // True while the controller owns a guest request: a Read Input Fields,
    // a class-C1 wait, a put-with-invite, a screen save and the persistent
    // unit-FF controller invite all retain their element instead of posting
    // an immediate completion.
    bool isPending(int iob) const;
    // Drop one controller-owned operation without producing a completion.
    // Task termination uses this before releasing the operation's ACE.
    bool cancelPendingOperation(int iob);
    void resetPendingIo();
    // The IPL walks the station list after publishing the bootstrap unit
    // block; with a physical controller the stations the host acquired are
    // present.  It creates no activation status and no response.
    void activateIplWorkStations();

    PendingCheckpoint capturePendingCheckpoint() const;
    // False with the reason when the checkpoint is malformed; the pending
    // state is reset either way.
    bool restorePendingCheckpoint(const PendingCheckpoint& s, std::string& failure);

    int pendingControllerInviteCount() const { return static_cast<int>(pendingControllerInvites_.size()); }
    int pendingPutWithInviteCount() const { return pendingPutWithInvites_.size(); }
    int countPendingPutWithInvitesForUnit(int unitAddress) const;
    bool hasPendingInputForUnit(int unitAddress) const;
    int pendingScreenSaveCount() const { return pendingScreenSaves_.size(); }
    int pendingAction0ActivationCount() const { return static_cast<int>(pendingAction0ActivationUnits_.size()); }

    // Record the native one-shot produced by the successful code-0 bind's
    // power-on / vary-on / activate call: the controller's own activation
    // status, not an SSP event.
    void recordAction0Activation(int unitAddress);
    // The same one-shot when power-on was reached by another genuine
    // producer; the response join is common.
    void recordPowerOnActivation(int unitAddress, const std::string& producer);
    // The terminal behind a unit was switched off: the next power-on through
    // the action-0 bind reaches activate afresh and produces a NEW one-shot,
    // which is what makes SSP treat the returning terminal as a newly
    // powered-on station.  False when the unit was not active.
    bool deactivateUnit(int unitAddress);
    // Consume the first native initial-activation status the response scan
    // can see.
    bool tryTakeAction0Activation(int& unitAddress);
    // Whether a pending read now has transport input behind it, without
    // consuming or completing it.
    bool tryFindPendingInput(int& iob);
    // Deliver the status half of one real terminal response to the guest
    // terminal unit block, without consuming the response's field bytes.
    bool tryDeliverInputStatus(int unitBlock);
    // The terminal behind a unit was switched off while the controller still
    // owned a guest request against it: fail one retained operation with the
    // "device not attached" status.  One call fails one; the caller loops.
    bool tryFailPendingOperationForUnit(int unitAddress, int& failedIob, std::string& what);
    // Complete one retained operation whose real response has arrived.
    bool tryCompletePendingInput(int& completedIob);
    // A printer Put is accepted at once but its operation ends only after
    // the record has left the machine.  The retained IOB completes when the
    // guest next gives the processor away (see the wait path), never inside
    // the issuing SVC.
    bool tryCompletePendingPrinterOutput(int& completedIob);
    // Hand off the parsed command-42 result held for delivery onto the
    // work-space block's resident frame, if any.
    std::optional<DeferredWorkStationInput> takeDeferredWorkStationInput();

    // The device half of SVC 40-48.
    bool deviceSvc(processors::controlstorage::SvcRequest& req);

    long long unmodelledRequests() const { return unmodelledRequests_; }
    long long dataStorageControllerRequests() const { return dataStorageControllerRequests_; }

    static std::string commandName(int cmd);

private:
    struct PendingInputRead {
        WorkStationSlot* slot = nullptr;
        uint8_t command = 0;
        int bufferField = 0;
        std::vector<int> destination;
        int capacity = 0;
        // The byte offset within the work-space BLOCK the display manager's
        // return copy reads, or -1 when command 42 did not resolve through
        // the display-manager input handshake.
        int stagingBlockDisplacement = -1;
    };
    struct PendingScreenSave {
        WorkStationSlot* slot = nullptr;
        uint8_t savedReadMode = 0;
        std::vector<int> destination;
        int capacity = 0;
    };

    bool declineUnmodelled(int iob, uint8_t r, const char* what);
    bool declineNoDataStorageController(int iob);
    bool workStationIoch(int iob, uint8_t r);
    void recordUnitBlock(int unitAddress, int unitBlock);
    bool request(int iob, uint8_t r, int cmd, int unitBlock);
    bool beginOutputRequest(int iob, bool withInvite, bool& phaseResult);
    bool invite(int iob);
    bool clear(int iob, WorkStationSlot& slot);
    bool cancelInvite(int iob, WorkStationSlot& slot);
    bool readInputFields(int iob, WorkStationSlot& slot);
    bool saveScreen(int iob, WorkStationSlot& slot);
    bool restoreScreen(int iob, WorkStationSlot& slot);
    bool outputData(int iob, WorkStationSlot& slot, bool withInvite);
    void applyC1Response(int iob, const std::vector<uint8_t>& responseStatus);
    bool readCurrentConfiguration(int iob);
    bool configureNewWorkStations(int iob);
    bool captureBuffer(int bufferField, int length, bool forWrite, std::vector<int>& addresses);
    void readCaptured(const std::vector<int>& source, int sourceOffset, uint8_t* destination, int destinationOffset, int length);
    void writeCaptured(const std::vector<int>& destination, int destinationOffset, const uint8_t* source, int sourceOffset,
                       int length);
    int readCapturedHalf(const std::vector<int>& source, int offset);
    void writeCapturedHalf(const std::vector<int>& destination, int offset, uint16_t value);
    bool resolveBuffer(int bufferField, int& addr);
    void writeDeviceNotAttached(int iob);
    void writeDisplayPoweredOff(int iob);
    void completeScreenSave(int iob, PendingScreenSave& pending);
    static std::string hexPreview(const std::vector<uint8_t>& d, int len, int max);
    static std::string ebcdicPreview(const std::vector<uint8_t>& d, int len, int max);

    machine::MachineState& m_;
    monitor::Tracer& trace_;
    WorkStationController workStations_;
    SlotMap<std::string, VirtualWorkstation*> stations_;
    SlotMap<std::string, VirtualPrinter*> printers_;
    SlotMap<int, PendingInputRead> pendingInputReads_;
    std::optional<DeferredWorkStationInput> lastDeferredInput_;
    // The response to a put-with-invite is staged in the translated work
    // page from which the display manager built that put; the page the
    // guest selected is remembered, while the full displacement within the
    // logical work-space block stays guest state at TUB+0x43..0x45.
    SlotMap<int, int> inputStagingPages_;
    // The response-side control field remains controller-owned until SSP
    // re-enters the output path with class C1, which copies it to the unit
    // block and consumes it.
    SlotMap<int, std::vector<uint8_t>> inputResponseStatus_;
    // A response-side C1 SVC is a wait when the controller has no return
    // control field yet.  It owns the SVC-43 element until a response.
    SlotMap<int, int> pendingC1Completions_;
    // A7 is one atomic write/read operation that retains its action until
    // the terminal response arrives; it is not a PUT completed at send time.
    SlotMap<int, WorkStationSlot*> pendingPutWithInvites_;
    std::vector<int> pendingPrinterOutputs_;   // IOBs, in order of issue
    SlotMap<int, PendingScreenSave> pendingScreenSaves_;
    // Guest unit-FF requests remain pending while their all-stations invite
    // has no completion.
    std::set<int> pendingControllerInvites_;
    // The activation one-shot each unit produces once, kept ordered because
    // the native scan is index 0..63; and the active state kept separately
    // so a repeated code-0 request cannot manufacture a second response.
    std::set<int> pendingAction0ActivationUnits_;
    std::set<int> action0ActivatedUnits_;
    long long unmodelledRequests_ = 0;
    long long dataStorageControllerRequests_ = 0;
};

}  // namespace sim36::devices
