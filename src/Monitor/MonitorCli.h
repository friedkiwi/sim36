// A SIMH-style monitor over a constructed machine.  Inspection is a
// first-class feature rather than a debug aid: every structure the machine
// defines should be dumpable by name.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Machine/Machine.h"

namespace sim36::storage { class TapeManifest; }

namespace sim36::monitor {

struct SelfTestResult {
    int passed = 0;
    int failed = 0;
};

// Runs the manual's worked examples against a machine state and processor,
// printing one PASS/FAIL line per vector and the totals.
SelfTestResult runSelfTest(machine::MachineState& m, processors::MainStorageProcessor& msp);

class MonitorCli {
public:
    explicit MonitorCli(machine::Machine& m) : m_(m) {}
    ~MonitorCli();
    MonitorCli(const MonitorCli&) = delete;
    MonitorCli& operator=(const MonitorCli&) = delete;

    // Route an already canonicalised, registry-approved command to its
    // machine-state implementation.  The session is the sole public
    // dispatcher.
    void executeTokens(const std::vector<std::string>& a);

    // ---- the live monitor: a guest thread owns the machine while it runs
    // True while a guest thread owns the machine.
    bool executionActive() const { return liveRunning_.load(); }
    // True when a caller on some other thread must marshal its command onto
    // the guest thread instead of running it itself.
    bool shouldMarshal() const;
    // Run the work on whichever thread owns machine state.  With no
    // execution thread that is the caller; with one it is the guest thread,
    // at its next safe boundary, and the caller blocks meanwhile.  A command
    // that failed on the guest thread is rethrown to the caller.
    void runOnGuestThread(const std::function<void()>& work);
    // Stop a guest execution thread if there is one.  Used by `quit` and by
    // session teardown, where leaving it running would race the destructor.
    void stopExecutionForTeardown();

    // A hex dump in the monitor's format: address, 16 bytes, EBCDIC text.
    static std::string hexDump(const uint8_t* b, int len, int baseAddr);

private:
    void show(const std::vector<std::string>& a);
    void showVtoc(const std::vector<std::string>& a);
    void showLibrary(const std::vector<std::string>& a);
    void dumpSector(const std::vector<std::string>& a);
    void dump(const std::vector<std::string>& a);
    void setRegister(const std::vector<std::string>& a);
    void setTrace(const std::vector<std::string>& a);
    void ipl(const std::vector<std::string>& a);
    void boot();
    void load(const std::vector<std::string>& a);
    void loadFile(const std::vector<std::string>& a);
    void diskRead(const std::vector<std::string>& a);
    int issueDeviceSvc(uint8_t r, int iob);
    void showAce(const std::vector<std::string>& a);
    void showIob(const std::vector<std::string>& a);
    void showUnitBlock(const std::vector<std::string>& a);
    void sched();
    void conformance();

    // ---- the control storage processor's task, module and allocation views
    struct RequestFrame {
        int depth = 0, requestBlock = 0, previous = 0, programBlock = 0, resumeIar = 0, offset = 0;
        bool attributed = false;
        std::string member;
    };
    struct PendingMemberBreak {
        std::string member;
        int offset = 0;
        std::string description;
    };
    bool canReadGuest(int address, int length) const;
    std::vector<RequestFrame> requestBlockChain(int taskBlock);
    std::string moduleOwners(int programBlock);
    std::vector<std::string> moduleOwnerRows(int programBlock);
    void whereIs(const std::vector<std::string>& a);
    void taskList(const std::vector<std::string>& a);
    void taskDetail(int tb, int current);
    void mapState(const std::vector<std::string>& a);
    void systemQueueSpaceState(const std::vector<std::string>& a);
    void modules(const std::vector<std::string>& a);
    void moduleStorage(const std::vector<std::string>& a);
    void modulesActive(const std::string& name);
    void modulesLoaded(const std::string& name);
    void residency(const std::vector<std::string>& a);
    void systemMeasurement(const std::vector<std::string>& a);
    void printSystemMeasurementStatus(int selector);
    void printTaskMeasurementStatus(int tb, int id);
    void allocationChain(const std::vector<std::string>& a);
    void breakMember(const std::vector<std::string>& a);
    void armPendingMemberBreaks();
    void transferById(const std::vector<std::string>& a);
    void transferTerminationContinuation(const std::vector<std::string>& a);
    void terminationDependencyScan(const std::vector<std::string>& a);
    void actions();
    void timers(const std::vector<std::string>& a);
    void showPtt();
    std::vector<PendingMemberBreak> pendingMemberBreaks_;
    void disassemble(const std::vector<std::string>& a);
    void step(const std::vector<std::string>& a);
    long long driveMachine(long long cap);
    // Step the MSP until it stops on its own, delivering any pending device
    // attention at the idle-event-wait pull point and resuming.  When idle
    // with nothing pending, wait on the machine's native-event doorbell.  A
    // negative pollSeconds waits indefinitely; a positive value is a bounded
    // diagnostic wait and zero returns immediately.  Returns the instruction
    // count.
    long long driveMachine(processors::controlstorage::As36ControlStorageProcessor* csp, long long cap, int pollSeconds);
    void breakCommand(const std::vector<std::string>& a);
    void watch(const std::vector<std::string>& a);
    void installWatchReporter();
    void poke(const std::vector<std::string>& a);
    void patch(const std::vector<std::string>& a);
    void findMemory(const std::vector<std::string>& a);
    void addressMap(const std::vector<std::string>& a);
    void selfTest();
    static bool parseHexBytes(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out);

    // ---- diskette and tape (MediaCommands.cpp)
    void diskette(const std::vector<std::string>& a);
    void disketteRead(const std::vector<std::string>& a);
    void disketteWrite(const std::vector<std::string>& a);
    void tape(const std::vector<std::string>& a);
    void tapeStatus();
    void tapeVtoc();
    void tapeFiles();
    void tapeTest(const std::vector<std::string>& a);
    void tapeSvc(const std::vector<std::string>& a);
    void saveMain(const std::vector<std::string>& a);
    std::unique_ptr<storage::TapeManifest> loadTapeManifest(std::string& reason);

    // ---- the live monitor (MonitorCliLive.cpp)
    struct LiveWork {
        std::function<void()> work;
        std::mutex gate;
        std::condition_variable doneSignal;
        bool done = false;
        std::exception_ptr error;
    };
    void pumpLiveWork();
    void runLiveItem(LiveWork& item);
    void startExecution(const std::vector<std::string>& a);
    void startExecution(processors::controlstorage::As36ControlStorageProcessor* csp, const std::string& what);
    void liveBody(processors::controlstorage::As36ControlStorageProcessor* csp);
    void stopExecution(const std::vector<std::string>& a);
    void joinLiveThread();
    void waitCommand(const std::vector<std::string>& a);
    bool refuseWhileRunning(const char* verb);
    static std::string exceptionMessage(const std::exception_ptr& e);
    bool deliverPendingAttentions(processors::controlstorage::As36ControlStorageProcessor& csp);
    bool deliverPendingInputStatuses(processors::controlstorage::As36ControlStorageProcessor& csp);
    bool powerOffStation(processors::controlstorage::As36ControlStorageProcessor& csp, devices::VirtualWorkstation& s);
    void resetHostEventState();

    mutable std::mutex liveLock_;
    std::deque<std::shared_ptr<LiveWork>> liveQueue_;
    // Read once per MSP preemption point, so both are deliberately plain
    // atomic loads: no lock is taken on the hot path when nothing is queued
    // and no stop has been asked for.
    std::atomic<int> livePending_{0};
    std::atomic<bool> liveStopRequested_{false};
    std::atomic<bool> liveIdle_{false};      // guest thread is parked in its event wait
    std::atomic<bool> liveRunning_{false};   // a guest thread exists and owns the machine
    std::thread liveThread_;
    std::thread::id liveThreadId_;
    long long liveInstructions_ = 0;
    std::exception_ptr liveFault_;

    // Console sign-on wake progress: 0 = none, 1 = device-present delivered,
    // 2 = completion delivered.  Reset on boot.
    int consoleSignOnStage_ = 0;
    bool consoleSignOnStatementPosted_ = false;
    // EXPERIMENTAL (ws_interactive): which non-console stations have already
    // had a per-station device-present delivered, keyed by station id.
    std::set<std::string> wsPresented_;
    // The controller retains a busy scan for a later action retry; retain
    // input-driven scans by unit for the same reason.
    std::deque<int> pendingWsEntryUnits_;

    // ---- the work-station commands (WorkStationCommands.cpp)
    struct WorkstationTraceField {
        int address = 0, length = 0;
        std::string name;
        bool topology = false;
    };
    struct WorkstationLifecycleTrace {
        std::string stationId;
        int unit = 0, limit = 256;
        bool lifecycle = false, classifier = false;
        std::string lastNativeState;
        std::vector<std::string> lifecycleEvents;
        std::vector<std::string> classifierEvents;
        std::vector<WorkstationTraceField> fields;
        std::map<int, int> classifierNodeByTask;
    };
    struct CaseInsensitiveLess {
        bool operator()(const std::string& a, const std::string& b) const;
    };
    void stations();
    void listenerAutoSignOn(const std::vector<std::string>& a);
    devices::VirtualWorkstation* findStation(const std::string& id);
    devices::VirtualPrinter* findPrinter(const std::string& id);
    devices::VirtualWorkstation* findDisplayStation(const std::string& name);
    devices::VirtualWorkstation* stationById(const std::string& id, bool displayOnly);
    host::WorkstationBackend* findConsoleBackend();
    void layWorkstationIob(int unit, const std::vector<uint8_t>& stream);
    void workstationOutput(const std::vector<std::string>& a);
    void printerWrite(const std::vector<std::string>& a);
    bool printerDataStream(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out);
    void printerEndJob(const std::vector<std::string>& a);
    void consoleCommand(const std::vector<std::string>& a);
    void workstationWrite(const std::vector<std::string>& a);
    void workstationFormat(const std::vector<std::string>& a);
    bool readFormatSectors(int sector, int count, std::vector<uint8_t>& out);
    void workstationInvite(const std::vector<std::string>& a);
    void workstationInput(const std::vector<std::string>& a);
    void workstationRead(const std::vector<std::string>& a);
    bool buildDataStream(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out);
    void wsPresent(const std::vector<std::string>& a);
    void wsPresentStation(const std::vector<std::string>& a);
    void tfrM36(const std::vector<std::string>& a);
    void wsAid(const std::vector<std::string>& a);
    void wsOc(const std::vector<std::string>& a);
    void wsUser(const std::vector<std::string>& a);
    void wsPost(const std::vector<std::string>& a);
    void wsPresentWs(const std::vector<std::string>& a);
    void postRk(const std::vector<std::string>& a);
    void callSsp(const std::vector<std::string>& a);
    void signOnStatement(const std::vector<std::string>& a);
    void signOnCommand(const std::vector<std::string>& a);
    void signOnRequest(const std::vector<std::string>& a);
    void wsAttach(const std::vector<std::string>& a);
    void msscMsg(const std::vector<std::string>& a);
    void conDbElem(const std::vector<std::string>& a);
    void signonExp(const std::vector<std::string>& a);
    void wsConfig(const std::vector<std::string>& a);
    void setVolumeWorkStationConfiguration(const std::vector<std::string>& a);
    void wsIoch(const std::vector<std::string>& a);
    void wddqState(const std::vector<std::string>& a);
    void dumpWddqRequest(const char* source, int request);
    void dumpWddqCompleteQueue(int tb);
    void cptcState(const std::vector<std::string>& a);
    void cptcClassifierTrace(int chainHead);
    std::string cptcClassifierRoute(int node, uint16_t eye, int oc, int p62, int p65, uint16_t f99);
    bool cptcNodeIsActive(int node, std::string& detail);
    void workstationPipeline(const std::string& requestedId);
    void workstationTrace(const std::vector<std::string>& a);
    void showWorkstationTrace(const WorkstationLifecycleTrace& trace, const std::string& kind);
    bool anyWorkstationTrace(bool lifecycle, bool classifier) const;
    std::vector<WorkstationLifecycleTrace*> lifecycleTraces();
    void installWorkstationObservers();
    void removeWorkstationObserversIfIdle();
    void observeWorkstationControllerBoundary(const std::string& kind, int unit, int exactTub, int configuredTub);
    void recordWorkstationNativeState(const char* boundary);
    void addWorkstationTraceEvent(WorkstationLifecycleTrace& trace, const std::string& kind, const std::string& detail);
    void observeWorkstationWrite(int address, int length, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after);
    void observeWorkstationInstruction(uint16_t iar);
    void observeConsoleQualificationInstruction(const processors::controlstorage::LoadedMember& member, uint16_t iar, int tb);
    bool traceNodeIsRelevant(const WorkstationLifecycleTrace& trace, int node);
    void addWorkstationTraceField(WorkstationLifecycleTrace& trace, int address, int length, const std::string& name, bool topology);
    void refreshWorkstationTraceFields(WorkstationLifecycleTrace& trace);
    void addWorkstationTaskFields(WorkstationLifecycleTrace& trace, int task);
    std::string guestHex(int address, int length);
    void cptcClassifierTraceForStation(devices::VirtualWorkstation& station);
    void wsContract(const std::vector<std::string>& a);
    void armWsContractWatches(const std::string& stationId);
    int addWsContractWatch(int address, int length);
    int refreshWsContractWatches();
    int countWsContractElements();
    void wsContractSnapshot(const std::string& stationId, const char* boundary);
    void wsEntry(const std::vector<std::string>& a);
    void workStationHriReport(const std::vector<std::string>& a);
    void drainWsEntry(processors::controlstorage::As36ControlStorageProcessor& csp, const std::string& terminal);
    void wsEntryScan(const std::vector<std::string>& a);
    void tuTopology(const std::vector<std::string>& a);
    void printTuChain(const char* name, int queueNumber, int nextOffset, int published);
    void workstationSessionState(const std::vector<std::string>& a);
    bool occursOnQueue(int wanted, int headerField, int nextField);
    void svtubDecisionState(const std::vector<std::string>& a);

    std::string wsContractTraceStation_;
    long long wsContractSequence_ = 0;
    bool wsContractAutoWatch_ = false;
    std::string wsContractWatchStation_;
    std::set<long long> wsContractDerivedWatchKeys_;
    std::map<std::string, WorkstationLifecycleTrace, CaseInsensitiveLess> workstationTraces_;
    long long workstationTraceSequence_ = 0;

    machine::Machine& m_;
};

}  // namespace sim36::monitor
