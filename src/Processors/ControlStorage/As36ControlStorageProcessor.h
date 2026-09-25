// The Advanced/36 control storage processor: the architected contract in
// native code, with no CSP hardware and no microcode.
//
// Supervisor calls are ordinary function calls made by the MSP interpreter;
// handlers mutate the request and task blocks and return.  There is no
// interrupt level to drop and no register bank to re-point, so the
// task-switch decision lives in the interpreter loop, not here.
//
// The machine scaffolding (guest low storage, the system queue space,
// phase 1, the initial task, the storage family, the control-block access
// calls and the device path) is in this file and As36Storage.cpp; the
// supervisor call families are split by subject into As36Transfer.cpp
// (transfer control, the loader, task termination), As36Dispatch.cpp (the
// dispatcher, waits, posts, events, resources, the action controller) and
// As36TaskCreate.cpp (task creation, user area pages, the task work area
// allocator, the transient bodies, print and SMFC), each with its member
// declarations in a fragment the class body includes.  A device family that
// does not exist yet (the work station controller, diskette, tape) is
// refused by name, and the refusal stops the machine with the reason.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Configuration/EmulatorConfig.h"
#include "Devices/DeviceSet.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Processors/ControlStorage/ActionControlElementQueue.h"
#include "Processors/ControlStorage/BasicAssist.h"
#include "Processors/ControlStorage/DirectArea.h"
#include "Processors/ControlStorage/FortranAssist.h"
#include "Processors/ControlStorage/GuestHeap.h"
#include "Processors/ControlStorage/IControlStorageProcessor.h"
#include "Processors/ControlStorage/NuPtt.h"
#include "Processors/ControlStorage/TaskWorkArea.h"
#include "Processors/ControlStorage/TransientArea.h"
#include "Processors/MainStorageProcessor.h"
#include "Storage/DiskBackend.h"

namespace sim36::monitor { class MonitorCli; }

namespace sim36::processors::controlstorage {

// A member whose bytes the loader has read, keyed by its program block.
struct LoadedMember {
    std::string name;        // 5-character member name from the load header
    long long extentSector;  // 0-based extent the bytes were read from
    int logicalBase;         // guest address the member runs at (pb+12 << 11)
};

class As36ControlStorageProcessor : public IControlStorageProcessor, public TransientHost {
public:
    // Phase 1 is the boot record plus the 15 sectors after it, read as one
    // flat 4 KB control storage transient.  0-based, so 8191.
    static constexpr int kPhase1Sector = 8191;
    static constexpr int kPhase1Sectors = 16;
    // Where phase 1 lands: the boot record carries it at +0x0F and every IPL
    // member's directory link field is 1000.
    static constexpr int kPhase1LoadAddress = 0x1000;

    // The unit definition table lives at 0-based sector 26, duplicated at
    // 8225, and is what the control processor's FIRST disk read fetches:
    // sixteen 256-byte sectors, which is why a walk may run off the end of
    // one record's fields into the next without faulting.
    static constexpr int kUdtSector = 26;
    static constexpr int kUdtSectors = 16;
    static constexpr int kUdtPersistedSectors = 4;
    static constexpr int kUdtMirrorSector = 8225;

    // The system queue space: guest 0x2000 through 0x10000 is the initial
    // segment, and the native pool grows in 64 KB units up to 0x6F0000.
    static constexpr int kSystemQueueSpace = 0x2000;
    static constexpr int kSystemQueueSpaceBytes = 0x10000 - kSystemQueueSpace;
    static constexpr int kNativeSystemQueueHigh = 0x006F0000;

    // Where this implementation puts the initial request block: emulator
    // policy, clearly marked.  The initial task block is at 0xF00 and is
    // architecturally visible (the ATR builder tests for it by value).
    static constexpr int kRequestBlock = 0xE00;
    static constexpr int kIplTaskBlock = 0xF00;

    // The System/36's translated-address flag in a 24-bit guest address, and
    // the same bit as a PACT prefix byte.  SA21-9436 calls the result "hex
    // 80nnnn".
    static constexpr int kTranslatedBit = 0x800000;
    static constexpr uint8_t kTranslatedPrefix = 0x80;

    // PSR condition bits, IBM numbering: bit 5 High, 6 Low, 7 Equal.
    static constexpr uint8_t kPsrHigh = 0x04, kPsrLow = 0x02, kPsrEqual = 0x01;
    static constexpr uint8_t kPsrClearForEqual = kPsrHigh | kPsrLow, kPsrClearForLow = kPsrHigh | kPsrEqual;

    static constexpr int kAtrCount = NuPtt::kAtrCount;

    As36ControlStorageProcessor(machine::MachineState& m, const configuration::EmulatorConfig& cfg,
                                devices::DeviceSet& devices, storage::DiskBackend& disk, monitor::Tracer& trace);

    // ---- IControlStorageProcessor ---------------------------------------
    std::string modelName() const override { return "advanced36"; }
    MainStorageProcessor& mainStorage() override { return *msp_; }
    void bringUpControlProcessor() override;
    void iplMainProcessor() override;
    void controlStorageTerminate() override;
    DispatchClass classify(uint8_t rByte) const override;
    bool isImplemented(uint8_t rByte) const override;
    bool svc(SvcRequest& req) override;
    std::string lastRefusal() const override { return lastRefusal_; }
    bool raiseStorageProtection(uint16_t logical, bool forWrite) override;
    bool consumeInvalidOpcodeCheck(uint16_t resumeIar) override;
    bool extendedControlStore(uint8_t q, uint8_t r, uint16_t sourceIar) override;
    ITransientArea& transients() override { return transients_; }

    // ---- TransientHost --------------------------------------------------
    bool runTransient(uint8_t transientId, uint8_t inline2, uint8_t inline3, int xr1, int xr2, int taskBlock,
                      int requestBlock) override;

    // ---- monitor-visible state ------------------------------------------
    int currentTaskBlock() const { return currentTaskBlock_; }
    int currentRequestBlock() const { return currentRequestBlock_; }
    GuestHeap& heap() { return heap_; }
    const NuPttPool& translationFiles() const { return ptt_; }
    ActionControlElementQueue& aces() { return aces_; }
    std::string describeSrcState() const;
    std::string describeCheckState() const;
    bool tryActiveMember(int taskBlock, int iar, LoadedMember& member, int& offset) const;
    std::string describeActiveMember(int taskBlock, int iar) const;
    // Attribute any request-frame program block, not only a task's top frame.
    bool tryProgramBlockMember(int programBlock, int iar, LoadedMember& member, int& offset) const;
    // SSP's POWER OFF command ends in #CCPW's hardware power-control wait.
    // Recognize that boundary and turn it into an emulator stop.
    bool detectSystemPowerOff();
    bool systemPowerOffRequested() const { return systemPowerOffRequested_; }
    // The emulator's main-storage backing arena, for the monitor: host
    // bookkeeping that diagnoses whether a transfer failed for lack of pages
    // or through fragmentation.
    std::vector<std::string> moduleStorageDiagnostics() const;

private:
    // The monitor's inspection commands read the processor's own bookkeeping
    // (module residency, the ATR file pool, the action controller, native
    // timers) without widening the architected interface.
    friend class sim36::monitor::MonitorCli;

    // ---- refusals -------------------------------------------------------
    template <typename... Args>
    bool refuse(fmt::format_string<Args...> f, Args&&... args)
    {
        return refuseText(fmt::format(f, std::forward<Args>(args)...));
    }
    bool refuseText(const std::string& reason);

    // ---- stage B --------------------------------------------------------
    void buildFromUnitDefinitionTable();
    void buildSystemUnitBlock(bool terminal);
    void loadPhase1();
    // The data set IPL removable media carry phase 1 in.
    static constexpr const char* kIplDataSet = "#IPLBOOT";
    void loadPhase1FromDiskette(uint8_t* buf, int bytes);
    void loadPhase1FromTape(uint8_t* buf, int bytes);
    void postInitialTask();

    // ---- the supervisor call path ----------------------------------------
    void stampRequest(int rb, const SvcRequest& req);
    void saveRegisters(int rb);
    void restoreRegisters(int rb);
    bool service(SvcRequest& req);
    void setCondition(SvcRequest& req, uint8_t bits);

    // ---- SVC 0F, 0E and the queue engine --------------------------------
    bool systemControlBlockAccess(SvcRequest& req);
    void writeIndexRegister(int rb, bool xr1, int value, bool withHighByte);
    bool queueDequeue(SvcRequest& req);
    bool queueOperation(int headerField, int block, int chainLastByte, uint8_t flags);
    bool queueBlock(int headerField, int block, int chainField, uint8_t flags);
    bool dequeueBlock(int headerField, int block, int chainField);
    bool chainStepValid(int at, int steps, int headerField);
    static constexpr int kChainWalkLimit = 100000;

    // ---- SVC 06 and 07 --------------------------------------------------
    bool assign(SvcRequest& req);
    bool freeAssigned(SvcRequest& req);

    // ---- SVC 2C, 2D, 2F: work spaces and translation ----------------------
    bool translatedAssignOrFree(SvcRequest& req);
    WorkSpaceHeap* workSpaceFor(int block);
    bool map(SvcRequest& req);
    bool mapParameterListCore(SvcRequest& req, int rb, int pb, int list, const std::string& call);
    void compactMapTable(int rb, int pb, const std::string& call);
    bool mapRegister(int rb, uint8_t parm2, int action, int target, uint8_t* entry, int& source, int& logical);
    bool mapAction(SvcRequest& req, int action, uint8_t parm2, const uint8_t* entry, int pb, int source, int startPage,
                   int pages, int& entries, int& lastEntry);
    bool appendMapEntry(int rb, int pb, int startPage, int pages, int displacement, int block, bool whole,
                        int& entries, int& lastEntry);
    bool mapAnotherRequestBlock(int rb, int pb, int other, int sourcePage, int startPage, int pages, bool whole,
                                int& entries, int& lastEntry);
    bool mapByTypeAndId(SvcRequest& req, const uint8_t* entry, int pb, int sourcePage, int startPage, int pages, bool whole,
                        int& entries, int& lastEntry);
    int findWorkSpace(int taskBlock, uint8_t type, int id, std::string& why);
    int findTaskById(int id, int currentTaskBlock);
    bool resolveTranslated(int address, int& real);
    void buildTranslationRegisters(int tb);
    void applyMapTable(int rb, int pb, uint16_t* atr);
    // The resident frames of a storage block's pages, paged in from the task
    // work area on demand.  nullptr when the block has no swap area or the
    // request cannot be honoured; the pages then stay protected.
    const std::vector<int>* workSpaceResidentPages(int sb, const std::string& call, int firstPage, int pageCount);
    int moduleBytes(int pb);

    // ---- the module storage arena -------------------------------------------
    static constexpr int kModuleStorageLow = 0x700000, kModuleStorageHigh = 0x800000;
    int allocateModuleStorage(int pages, const std::string& call);
    bool ensureModuleStoragePages(int block, int pages, const std::string& call);
    void returnModuleStorage(int at, int bytes);
    void releaseBlockStorage(int block, const std::string& call);

    // ---- control blocks --------------------------------------------------
    void includeInDomain(int block, const std::string& call);
    void activateControlBlock(int block, const std::string& call);
    void deactivateControlBlock(int block, const std::string& call);
    void deleteControlBlock(int block, const std::string& call);
    void dequeueControlBlock(int block, const std::string& call);
    int queueHeadFor(int block, const std::string& call);
    static int programBlockHashBucket(int sector);
    static int swapAreaSectors(uint8_t flags, int regionPages);

    // ---- the task work area ------------------------------------------------
    void ensureTaskWorkArea();
    int taskWorkAreaHeader(int baseIdentifier);
    bool resolveRelativeDiskAddress(int relative, int& sector, std::string& why);
    int taskWorkAreaSectorOf(int relative);
    bool clearTaskWorkArea(int relative, int sectors, std::string& why);
    bool taskWorkAreaAccess(SvcRequest& req);

    // ---- the ATR file, which belongs to the request block --------------------
    void createTranslationFile(int rb, const std::string& call);
    void releaseTranslationFile(int rb, const std::string& call);
    void selectTranslationFile(int rb, const std::string& call);

    // ---- the dispatcher (milestone 5 carries the waits and posts) -------------
    void readyQueueInsert(int tb, uint8_t flags);
    void dispatchIfRequested(const std::string& call);
    static constexpr int kTaskReadyQueue = 40, kTaskPriorityQueue = 39;

    // ---- device path ------------------------------------------------------
    bool deviceSvc(SvcRequest& req, int ace);

    // ---- disk access, traced ---------------------------------------------
    void diskRead(long long sector, uint8_t* dst, const std::string& what);
    void diskWrite(long long sector, const uint8_t* src, const std::string& what);

    // ---- member attribution ---------------------------------------------
    int activeProgramBlock(int taskBlock) const;

    // ---- milestone 5 families, one declaration fragment each ----------------
    // Transfer control, the loader and task termination; the dispatcher,
    // waits, posts, events, resources and the action controller; task
    // creation, user area pages, the task work area allocator, the
    // transient bodies, print and SMFC.
#include "Processors/ControlStorage/As36Transfer.members.inc"
#include "Processors/ControlStorage/As36Dispatch.members.inc"
#include "Processors/ControlStorage/As36TaskCreate.members.inc"
#include "Processors/ControlStorage/As36WorkStation.members.inc"
#include "Processors/ControlStorage/As36Checkpoint.members.inc"

    machine::MachineState& m_;
    const configuration::EmulatorConfig& cfg_;
    devices::DeviceSet& devices_;
    storage::DiskBackend& disk_;
    monitor::Tracer& trace_;
    GuestHeap heap_;
    DirectArea directArea_;
    BasicAssist basicAssist_;
    FortranAssist fortranAssist_;
    ActionControlElementQueue aces_;
    TransientArea transients_;
    NuPttPool ptt_;
    TaskWorkArea twa_;
    std::unique_ptr<MainStorageProcessor> msp_;
    std::string lastRefusal_;

    int currentTaskBlock_ = 0;
    int currentRequestBlock_ = 0;
    bool inSupervisorCall_ = false;
    bool redispatch_ = false;
    bool systemPowerOffRequested_ = false;
    int dispatchDepth_ = 0;
    static constexpr int kDispatchDepthLimit = 16;
    int currentTransientProgramBlock_ = 0;

    // Retained device ACEs, keyed by IOB: a request the device could not
    // complete synchronously keeps its element until the response arrives.
    std::map<int, int> pendingDeviceAces_;

    // Work spaces, keyed by the storage block's guest address.
    std::map<int, std::unique_ptr<WorkSpaceHeap>> workSpaces_;
    // Module residency: program block -> real address of its bytes; storage
    // block -> per-page frames.
    std::map<int, int> moduleStorage_;
    std::map<int, int> workSpaceStorage_;
    std::map<int, std::vector<int>> workSpaceStoragePages_;
    std::vector<std::pair<int, int>> moduleStorageFree_;   // (address, bytes), sorted
    std::map<int, int> moduleStorageSize_;
    int moduleStorageNext_ = kModuleStorageLow;
    std::map<int, LoadedMember> memberByProgramBlock_;
};

}  // namespace sim36::processors::controlstorage
