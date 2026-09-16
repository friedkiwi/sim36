// The Advanced/36 control storage processor: the native checkpoint.
//
// What a snapshot carries beyond guest storage: the controller and
// dispatcher latches, the residency maps, the allocators' free lists and
// the translation-file pool, in the record the snapshot file serialises.
#include "Processors/ControlStorage/As36ControlStorageProcessor.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "Processors/ControlStorage/ProgramBlock.h"

namespace sim36::processors::controlstorage {

bool As36ControlStorageProcessor::captureCheckpoint(CheckpointState& out, std::string& failure)
{
    failure.clear();
    if (dispatchDepth_ != 0) {
        failure = "cannot checkpoint inside the native dispatcher";
        return false;
    }
    CheckpointState s;
    s.pendingDeviceAcePairs = capturePairs(pendingDeviceAces_);
    s.pendingAsyncChildren = pendingAsyncChildren_;
    for (const auto& p : nativeTransferContinuations_) {
        // The serialised order is innermost first: the last pushed frame
        // comes first.
        s.nativeTransferContinuations.push_back(p.first);
        s.nativeTransferContinuations.push_back(static_cast<int>(p.second.size()));
        for (auto it = p.second.rbegin(); it != p.second.rend(); ++it)
            s.nativeTransferContinuations.push_back(static_cast<int>(*it));
    }
    s.redispatch = redispatch_;
    s.phase2SvatJobTask = phase2SvatJobTask_;
    s.phase2SvatDispatched = phase2SvatDispatched_;
    s.taskIdCounter = taskIdCounter_;
    s.requestBlockBeforeAttach = requestBlockBeforeAttach_;
    s.systemMeasurementEnabled = systemMeasurementEnabled_;
    s.transferredTub = transferredWorkStationTub_;
    s.pendingTransferredTub = pendingTransferredWorkStationTub_;
    s.transferredAutoSignOn = transferredAutoSignOn_;
    s.pendingTransferAutoSignOn = pendingTransferAutoSignOn_;
    s.lastTransferPostedGuestWork = lastM36WorkStationTransferPostedGuestWork_;
    s.wsDeviceStatusPending = wsDeviceStatusPending_;
    s.wsDeviceStatusDelivered = wsDeviceStatusDelivered_;
    s.actionStatusLow = actionStatusLow_;
    s.actionStatusHigh = actionStatusHigh_;
    s.actionCoverage = captureActionCoverage();
    s.wsPresentPairs = capturePairs(wsPresentByTask_);
    for (const auto& p : deferredWsInput_) {
        s.deferredWsInput.push_back(p.first);
        s.deferredWsInput.push_back(p.second.blockDisplacement);
        s.deferredWsInput.push_back(static_cast<int>(p.second.bytes.size()));
        for (uint8_t b : p.second.bytes) s.deferredWsInput.push_back(b);
    }
    s.directAreaWords = directArea_.captureCheckpoint();
    s.taskWorkAreaFree = twa_.captureCheckpoint();
    for (const auto& p : workSpaces_) {
        std::vector<int> free = p.second->captureCheckpoint();
        s.workSpaces.push_back(p.first);
        s.workSpaces.push_back(static_cast<int>(free.size()));
        s.workSpaces.insert(s.workSpaces.end(), free.begin(), free.end());
    }
    s.heap = heap_.captureCheckpoint();
    s.ptt = ptt_.captureCheckpoint();
    s.moduleStorageNext = moduleStorageNext_;
    s.currentTransientProgramBlock = currentTransientProgramBlock_;
    s.moduleStoragePairs = capturePairs(moduleStorage_);
    s.workSpaceStoragePairs = capturePairs(workSpaceStorage_);
    s.workSpaceStoragePages = capturePageMaps(workSpaceStoragePages_);
    s.moduleStorageSizePairs = capturePairs(moduleStorageSize_);
    s.moduleStorageFree = captureRanges(moduleStorageFree_);
    for (const auto& p : memberByProgramBlock_) {
        s.loadedMemberData.push_back(p.first);
        s.loadedMemberData.push_back(static_cast<int>(p.second.extentSector));
        s.loadedMemberData.push_back(p.second.logicalBase);
        s.loadedMemberNames.push_back(p.second.name);
    }
    s.aces = aces_.captureCheckpoint();
    out = std::move(s);
    return true;
}

bool As36ControlStorageProcessor::restoreCheckpoint(const CheckpointState& s, std::string& failure)
{
    failure.clear();
    if (!restorePairs(pendingDeviceAces_, s.pendingDeviceAcePairs, "pending device ACE", failure)) return false;
    pendingAsyncChildren_ = s.pendingAsyncChildren;
    nativeTransferContinuations_.clear();
    const auto& native = s.nativeTransferContinuations;
    for (std::size_t i = 0; i < native.size();) {
        if (i + 2 > native.size()) {
            failure = "truncated native-transfer continuation checkpoint";
            return false;
        }
        int tb = native[i++];
        int count = native[i++];
        if (count < 0 || i + static_cast<std::size_t>(count) > native.size()) {
            failure = "invalid native-transfer continuation depth";
            return false;
        }
        // Serialised innermost first; rebuild outermost first so the last
        // element of the vector is the innermost frame again.
        std::vector<NativeTransferContinuation> stack;
        for (int j = count - 1; j >= 0; j--) {
            int raw = native[i + static_cast<std::size_t>(j)];
            if (raw < static_cast<int>(NativeTransferContinuation::NuptermSlot4) ||
                raw > static_cast<int>(NativeTransferContinuation::NuabSlot1D)) {
                failure = "invalid native-transfer continuation kind";
                return false;
            }
            stack.push_back(static_cast<NativeTransferContinuation>(raw));
        }
        i += static_cast<std::size_t>(count);
        if (!stack.empty()) nativeTransferContinuations_[tb] = std::move(stack);
    }
    redispatch_ = s.redispatch;
    phase2SvatJobTask_ = s.phase2SvatJobTask;
    phase2SvatDispatched_ = s.phase2SvatDispatched;
    dispatchDepth_ = 0;
    taskIdCounter_ = s.taskIdCounter;
    requestBlockBeforeAttach_ = s.requestBlockBeforeAttach;
    systemMeasurementEnabled_ = s.systemMeasurementEnabled;
    transferredWorkStationTub_ = s.transferredTub;
    // A checkpoint records only the most recently bound unit block; reseed
    // the per-station set from it so a restored machine does not re-bind it.
    transferredWorkStations_.clear();
    if (s.transferredTub != 0) transferredWorkStations_[s.transferredTub] = s.transferredAutoSignOn;
    pendingTransferredWorkStationTub_ = s.pendingTransferredTub;
    transferredAutoSignOn_ = s.transferredAutoSignOn;
    pendingTransferAutoSignOn_ = s.pendingTransferAutoSignOn;
    lastM36WorkStationTransferPostedGuestWork_ = s.lastTransferPostedGuestWork;
    wsDeviceStatusPending_ = s.wsDeviceStatusPending;
    wsDeviceStatusDelivered_ = s.wsDeviceStatusDelivered;
    // The action queue is always empty at a checkpoint boundary: it is
    // drained inside the supervisor call that filled it.  The status words
    // and the coverage counters are the state that outlives a call.
    actionStatusLow_ = s.actionStatusLow;
    actionStatusHigh_ = s.actionStatusHigh;
    actionQueue_.clear();
    restoreActionCoverage(s.actionCoverage);
    if (!restorePairs(wsPresentByTask_, s.wsPresentPairs, "workstation-present", failure)) return false;
    deferredWsInput_.clear();
    for (std::size_t i = 0; i < s.deferredWsInput.size();) {
        if (i + 3 > s.deferredWsInput.size()) {
            failure = "truncated deferred workstation input checkpoint";
            return false;
        }
        const int task = s.deferredWsInput[i++];
        devices::DeviceSet::DeferredWorkStationInput input;
        input.blockDisplacement = s.deferredWsInput[i++];
        const int count = s.deferredWsInput[i++];
        if (input.blockDisplacement < 0 || count < 0 || i + static_cast<std::size_t>(count) > s.deferredWsInput.size()) {
            failure = "invalid deferred workstation input checkpoint";
            return false;
        }
        input.bytes.reserve(static_cast<std::size_t>(count));
        for (int n = 0; n < count; n++) {
            const int b = s.deferredWsInput[i++];
            if (b < 0 || b > 0xFF) {
                failure = "invalid byte in deferred workstation input checkpoint";
                return false;
            }
            input.bytes.push_back(static_cast<uint8_t>(b));
        }
        deferredWsInput_[task] = std::move(input);
    }
    if (!directArea_.restoreCheckpoint(s.directAreaWords)) {
        failure = "invalid direct-area checkpoint";
        return false;
    }
    if (!twa_.restoreCheckpoint(s.taskWorkAreaFree)) {
        failure = "invalid task-work-area checkpoint";
        return false;
    }
    workSpaces_.clear();
    const auto& spaces = s.workSpaces;
    for (std::size_t i = 0; i < spaces.size();) {
        if (i + 2 > spaces.size()) {
            failure = "truncated work-space checkpoint";
            return false;
        }
        int block = spaces[i++], count = spaces[i++];
        if (count < 0 || i + static_cast<std::size_t>(count) > spaces.size() || (count & 1) != 0) {
            failure = "invalid work-space free-list checkpoint";
            return false;
        }
        std::vector<int> free(spaces.begin() + static_cast<std::ptrdiff_t>(i),
                              spaces.begin() + static_cast<std::ptrdiff_t>(i) + count);
        i += static_cast<std::size_t>(count);
        auto heap = std::make_unique<WorkSpaceHeap>(std::max(0, StorageBlock::sizeBytes(m_, block)));
        if (!heap->restoreCheckpoint(free)) {
            failure = "invalid work-space free-list checkpoint";
            return false;
        }
        workSpaces_[block] = std::move(heap);
    }
    if (!heap_.restoreCheckpoint(s.heap)) {
        failure = "invalid system-queue heap checkpoint";
        return false;
    }
    if (!ptt_.restoreCheckpoint(s.ptt)) {
        failure = "invalid translation-file pool checkpoint";
        return false;
    }
    moduleStorageNext_ = s.moduleStorageNext;
    currentTransientProgramBlock_ = s.currentTransientProgramBlock;
    if (!restorePairs(moduleStorage_, s.moduleStoragePairs, "module storage", failure)) return false;
    if (!restorePairs(workSpaceStorage_, s.workSpaceStoragePairs, "workspace storage", failure)) return false;
    if (!restorePageMaps(workSpaceStoragePages_, s.workSpaceStoragePages, "workspace page storage", failure)) return false;
    if (!restorePairs(moduleStorageSize_, s.moduleStorageSizePairs, "module storage sizes", failure)) return false;
    moduleStorageFree_.clear();
    if ((s.moduleStorageFree.size() & 1) != 0) {
        failure = "invalid module-storage free list";
        return false;
    }
    for (std::size_t i = 0; i < s.moduleStorageFree.size(); i += 2)
        moduleStorageFree_.emplace_back(s.moduleStorageFree[i], s.moduleStorageFree[i + 1]);
    memberByProgramBlock_.clear();
    if (s.loadedMemberData.size() != s.loadedMemberNames.size() * 3) {
        failure = "invalid loaded-member checkpoint";
        return false;
    }
    for (std::size_t i = 0; i < s.loadedMemberNames.size(); i++) {
        LoadedMember m;
        m.name = s.loadedMemberNames[i];
        m.extentSector = s.loadedMemberData[i * 3 + 1];
        m.logicalBase = s.loadedMemberData[i * 3 + 2];
        memberByProgramBlock_[s.loadedMemberData[i * 3]] = m;
    }
    std::string why;
    if (!aces_.restoreCheckpoint(s.aces, why)) {
        failure = why;
        return false;
    }
    return true;
}

bool As36ControlStorageProcessor::restoreCheckpointMemory(const std::vector<uint8_t>& main, int currentTask,
                                                          int currentRequest, std::string& failure)
{
    systemPowerOffRequested_ = false;
    msp_->reset();
    transients_.reset();
    pendingDeviceAces_.clear();
    deferredWsInput_.clear();
    nativeTransferContinuations_.clear();
    pendingAsyncChildren_.clear();
    devices_.resetPendingIo();
    moduleStorage_.clear();
    memberByProgramBlock_.clear();
    workSpaceStorage_.clear();
    workSpaceStoragePages_.clear();
    moduleStorageFree_.clear();
    moduleStorageSize_.clear();
    workSpaces_.clear();
    ptt_.reset();
    std::memset(m_.raw(), 0, static_cast<std::size_t>(m_.backingBytes()));
    std::memcpy(m_.raw(), main.data(), std::min(main.size(), static_cast<std::size_t>(m_.backingBytes())));
    currentTaskBlock_ = currentTask;
    currentRequestBlock_ = currentRequest;
    msp_->start();
    trace_.csp("checkpoint restore: copied volatile main storage without DUMP MAIN reconstruction; native owner TB "
               "{:06X}/RB {:06X}",
               currentTask, currentRequest);
    failure.clear();
    return true;
}

std::vector<int> As36ControlStorageProcessor::captureActionCoverage() const
{
    std::vector<int> v;
    for (const auto& kv : actionCoverage_) {
        const ActionMaskCoverage& c = kv.second;
        v.push_back(c.mask);
        v.push_back(c.seen);
        v.push_back(c.coalesced);
        v.push_back(c.discharged);
        v.push_back(c.undischarged);
        v.push_back(c.lastIar);
        v.push_back(c.lastTaskBlock);
    }
    return v;
}

void As36ControlStorageProcessor::restoreActionCoverage(const std::vector<int>& v)
{
    actionCoverage_.clear();
    for (std::size_t i = 0; i + 6 < v.size(); i += 7) {
        ActionMaskCoverage c;
        c.mask = static_cast<uint8_t>(v[i]);
        c.seen = v[i + 1];
        c.coalesced = v[i + 2];
        c.discharged = v[i + 3];
        c.undischarged = v[i + 4];
        c.lastIar = static_cast<uint16_t>(v[i + 5]);
        c.lastTaskBlock = v[i + 6];
        actionCoverage_[c.mask] = c;
    }
}

std::vector<int> As36ControlStorageProcessor::capturePairs(const std::map<int, int>& from)
{
    std::vector<int> v;
    for (const auto& p : from) {
        v.push_back(p.first);
        v.push_back(p.second);
    }
    return v;
}

bool As36ControlStorageProcessor::restorePairs(std::map<int, int>& into, const std::vector<int>& pairs,
                                               const std::string& what, std::string& failure)
{
    into.clear();
    if ((pairs.size() & 1) != 0) {
        failure = "invalid " + what + " checkpoint";
        return false;
    }
    for (std::size_t i = 0; i < pairs.size(); i += 2) into[pairs[i]] = pairs[i + 1];
    return true;
}

std::vector<int> As36ControlStorageProcessor::capturePageMaps(const std::map<int, std::vector<int>>& from)
{
    std::vector<int> v;
    for (const auto& p : from) {
        v.push_back(p.first);
        v.push_back(static_cast<int>(p.second.size()));
        v.insert(v.end(), p.second.begin(), p.second.end());
    }
    return v;
}

bool As36ControlStorageProcessor::restorePageMaps(std::map<int, std::vector<int>>& into, const std::vector<int>& data,
                                                  const std::string& what, std::string& failure)
{
    into.clear();
    for (std::size_t i = 0; i < data.size();) {
        if (i + 2 > data.size()) {
            failure = "truncated " + what + " checkpoint";
            return false;
        }
        int block = data[i++], count = data[i++];
        if (count < 0 || i + static_cast<std::size_t>(count) > data.size()) {
            failure = "invalid " + what + " page count";
            return false;
        }
        into[block] = std::vector<int>(data.begin() + static_cast<std::ptrdiff_t>(i),
                                       data.begin() + static_cast<std::ptrdiff_t>(i) + count);
        i += static_cast<std::size_t>(count);
    }
    return true;
}

std::vector<int> As36ControlStorageProcessor::captureRanges(const std::vector<std::pair<int, int>>& from)
{
    std::vector<int> v;
    for (const auto& p : from) {
        v.push_back(p.first);
        v.push_back(p.second);
    }
    return v;
}

}  // namespace sim36::processors::controlstorage
