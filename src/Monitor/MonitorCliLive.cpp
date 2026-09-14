// The execution controller: the machine keeps executing on a guest thread of
// its own while the operator, or a command file, keeps typing.
//
// The emulator's single most valuable invariant is that exactly one thread
// ever touches guest storage, the scheduler, the loader tables and the disk
// backend.  Nothing here weakens it.  Instead the monitor thread stops
// touching machine state entirely: it wraps each parsed command in a closure,
// enqueues it, rings the machine doorbell and blocks until the GUEST thread
// has run that closure at one of the two boundaries where the driver loop
// already folds in asynchronous host state - an MSP preemption point, or the
// driver-idle park.  So a running `dump` or `console send` executes on
// precisely the thread, and at precisely the instruction boundary, that a
// scripted one does.
//
// This file also carries the driver loop itself and the host-event chain it
// folds in: station attentions, input statuses, and display power-off.
#include "Monitor/MonitorCli.h"

#include <chrono>
#include <cmath>
#include <thread>

#include <fmt/format.h>

#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Processors/ControlStorage/GuestLowStorage.h"

namespace sim36::monitor {

using processors::controlstorage::As36ControlStorageProcessor;

// ---- marshalling -------------------------------------------------------

bool MonitorCli::shouldMarshal() const
{
    std::lock_guard<std::mutex> lock(liveLock_);
    return liveRunning_.load() && liveThreadId_ != std::this_thread::get_id();
}

void MonitorCli::runOnGuestThread(const std::function<void()>& work)
{
    auto item = std::make_shared<LiveWork>();
    item->work = work;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        if (liveRunning_.load() && liveThreadId_ != std::this_thread::get_id()) {
            liveQueue_.push_back(item);
            livePending_.store(static_cast<int>(liveQueue_.size()));
            queued = true;
        }
    }
    if (!queued) { work(); return; }

    m_.signalNativeEvent();          // wake a parked driver loop
    {
        std::unique_lock<std::mutex> lock(item->gate);
        item->doneSignal.wait(lock, [&] { return item->done; });
    }
    if (item->error) std::rethrow_exception(item->error);
}

// Drain queued monitor work.  Called ONLY from the guest thread, at an MSP
// preemption point or the driver-idle park.
void MonitorCli::pumpLiveWork()
{
    for (;;) {
        std::shared_ptr<LiveWork> item;
        {
            std::lock_guard<std::mutex> lock(liveLock_);
            if (liveQueue_.empty()) { livePending_.store(0); return; }
            item = liveQueue_.front();
            liveQueue_.pop_front();
            livePending_.store(static_cast<int>(liveQueue_.size()));
        }
        runLiveItem(*item);
    }
}

// A command that failed on the guest thread is rethrown to the monitor
// thread that was waiting for it, with its own message, so command files
// report exactly what they always did.
void MonitorCli::runLiveItem(LiveWork& item)
{
    try {
        item.work();
    } catch (...) {
        item.error = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(item.gate);
        item.done = true;
    }
    item.doneSignal.notify_all();
}

// ---- start / stop / wait ------------------------------------------------

// Start or resume continuous guest execution.
void MonitorCli::startExecution(const std::vector<std::string>& a)
{
    if (a.size() != 1) throw MonitorError("usage: start");
    if (liveRunning_.load()) {
        fmt::print("execution: already running\n");
        return;
    }
    m_.msp().start();
    startExecution(&m_.nativeControlStorage(), "started");
}

// Start the guest execution thread.  Used by `ipl` and `start`.
void MonitorCli::startExecution(As36ControlStorageProcessor* csp, const std::string& what)
{
    if (liveRunning_.load()) {
        fmt::print("execution: already running; 'stop' to pause it\n");
        return;
    }
    liveStopRequested_.store(false);
    liveFault_ = nullptr;
    liveInstructions_ = 0;
    liveIdle_.store(false);
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        if (liveThread_.joinable()) liveThread_.join();
        liveRunning_.store(true);
        liveThread_ = std::thread([this, csp] { liveBody(csp); });
        liveThreadId_ = liveThread_.get_id();
    }
    fmt::print("execution: {}\n", what);
}

void MonitorCli::liveBody(As36ControlStorageProcessor* csp)
{
    try {
        liveInstructions_ = driveMachine(csp, std::numeric_limits<long long>::max(), -1);
    } catch (...) {
        liveFault_ = std::current_exception();
    }
    // Flip the flag under the same lock the producer enqueues under, then
    // drain: a command enqueued in the instant before the flip has a caller
    // blocked on it and must still be answered.
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        liveRunning_.store(false);
        liveThreadId_ = std::thread::id();
    }
    for (;;) {
        std::shared_ptr<LiveWork> item;
        {
            std::lock_guard<std::mutex> lock(liveLock_);
            if (liveQueue_.empty()) { livePending_.store(0); break; }
            item = liveQueue_.front();
            liveQueue_.pop_front();
            livePending_.store(static_cast<int>(liveQueue_.size()));
        }
        runLiveItem(*item);
    }
    liveIdle_.store(false);
}

std::string MonitorCli::exceptionMessage(const std::exception_ptr& e)
{
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& x) {
        return x.what();
    } catch (...) {
        return "unknown error";
    }
}

// `stop` - ask the guest loop to leave at its next safe point.
void MonitorCli::stopExecution(const std::vector<std::string>& a)
{
    if (a.size() != 1) throw MonitorError("usage: stop");
    std::thread::id owner;
    bool running;
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        owner = liveThreadId_;
        running = liveRunning_.load();
    }
    if (!running) {
        fmt::print("execution: already stopped\n");
        return;
    }
    if (owner == std::this_thread::get_id()) {
        fmt::print("execution: 'stop' runs on the monitor thread, not the guest thread\n");
        return;
    }
    liveStopRequested_.store(true);
    m_.signalNativeEvent();
    joinLiveThread();
    liveStopRequested_.store(false);
    if (liveFault_) {
        fmt::print("execution: guest thread ended with an error: {}\n", exceptionMessage(liveFault_));
        liveFault_ = nullptr;
        return;
    }
    fmt::print("execution: stopped after {} instruction(s); {}\n", liveInstructions_,
               m_.msp().stopped() ? (m_.msp().stopReason().empty() ? "stopped" : m_.msp().stopReason())
                                  : "machine remains runnable ('start' resumes)");
}

void MonitorCli::joinLiveThread()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        t = std::move(liveThread_);
    }
    if (t.joinable()) t.join();
}

// Stop a guest execution thread if there is one.  Used by `quit` and by
// session teardown, where leaving it running would race the destructor.
void MonitorCli::stopExecutionForTeardown()
{
    std::thread::id owner;
    bool running;
    {
        std::lock_guard<std::mutex> lock(liveLock_);
        owner = liveThreadId_;
        running = liveRunning_.load();
    }
    // Nothing to stop, or called from the guest thread itself (which cannot
    // join itself): a finished thread is merely reaped.
    if (owner == std::this_thread::get_id()) return;
    if (!running) {
        joinLiveThread();
        return;
    }
    liveStopRequested_.store(true);
    m_.signalNativeEvent();
    joinLiveThread();
    liveStopRequested_.store(false);
}

MonitorCli::~MonitorCli()
{
    stopExecutionForTeardown();
}

namespace {

// double.Parse with the invariant culture: a plain decimal, optional sign,
// fraction and exponent.
double parseSeconds(const std::string& s)
{
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') throw MonitorError("Input string was not in a correct format.");
    return v;
}

}  // namespace

// `wait idle [seconds]` and `wait <seconds>`.  Runs on the MONITOR thread
// and blocks it, which is why it is never marshalled: it is how a command
// file says "when the machine has settled" instead of guessing an
// instruction count.
void MonitorCli::waitCommand(const std::vector<std::string>& a)
{
    if (a.size() > 1 && equalsIgnoreCase(a[1], "idle")) {
        const double seconds = a.size() > 2 ? parseSeconds(a[2]) : 60;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::microseconds(static_cast<long long>(seconds * 1e6));
        while (std::chrono::steady_clock::now() < deadline) {
            if (!liveRunning_.load()) {
                fmt::print("wait: no execution in progress\n");
                return;
            }
            if (liveIdle_.load() && livePending_.load() == 0) {
                fmt::print("wait: guest is idle after {} instruction(s)\n", m_.msp().instructionsExecuted());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fmt::print("wait: guest did not reach its idle wait within {}s\n", seconds);
        return;
    }
    if (a.size() > 1) {
        const double seconds = parseSeconds(a[1]);
        const double ms = std::max(0.0, std::min(2147483647.0, seconds * 1000));
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(ms)));
        fmt::print("wait: {}s elapsed\n", seconds);
        return;
    }
    fmt::print("usage: wait idle [seconds] | wait <seconds>\n");
}

// Nesting the driver loop inside itself would be a genuine bug, so the run
// verbs refuse rather than surprise.  Returns true if refused.
bool MonitorCli::refuseWhileRunning(const char* verb)
{
    if (!liveRunning_.load()) return false;
    fmt::print("'{}' is not available while the machine is running; 'stop' first "
               "(docs/s36/live-monitor-design-2026-09-08.md)\n", verb);
    return true;
}

// ---- the driver loop ----------------------------------------------------

long long MonitorCli::driveMachine(long long cap) { return driveMachine(&m_.nativeControlStorage(), cap, 0); }

long long MonitorCli::driveMachine(As36ControlStorageProcessor* csp, long long cap, int pollSeconds)
{
    using clock = std::chrono::steady_clock;
    long long total = 0;
    const bool hasDeadline = pollSeconds > 0;
    const clock::time_point deadline = hasDeadline ? clock::now() + std::chrono::seconds(pollSeconds) : clock::time_point::max();
    while (total < cap) {
        while (total < cap && m_.msp().step()) {
            total++;
            // The native-to-SSP scheduler is asynchronous, not an "only after
            // every guest task is idle" service.  Poll socket-side latches
            // only at the MSP's architected preemption points, on this guest
            // thread.  This lets a 5250 negotiation completed during IPL
            // enter the controller scheduler before unrelated background
            // teardown reaches a later stop, without mutating guest state
            // mid-instruction.
            if (csp != nullptr && m_.msp().atPreemptionPoint()) {
                // Live monitor.  Two atomic loads when nothing is queued and
                // no stop is pending, which is every existing script: no
                // guest instruction, no scheduler event and no device state
                // is touched.  Draining sits BEFORE the host event chain
                // below so an operator keystroke arriving here occupies the
                // same position a slightly earlier real one would, rather
                // than reordering that chain against itself.
                if (livePending_.load() != 0) pumpLiveWork();
                if (liveStopRequested_.load()) break;
                armPendingMemberBreaks();
                recordWorkstationNativeState("preemption");
                const bool inputStatusWork = deliverPendingInputStatuses(*csp);
                const bool inputCompletionWork = csp->completePendingWorkStationInput();
                if (inputStatusWork || inputCompletionWork || deliverPendingAttentions(*csp)) {
                    // Every enqueue raises the new-work flag.
                    csp->signalNewWork("host event at preemption point");
                    // The instruction which exposed this preemption point may
                    // itself have been SVC 02's no-ready-task exit.  The
                    // dispatcher can select and dispatch the newly readied
                    // SSP task, but dispatching register state does not clear
                    // the MSP's host stop latch.  The idle-boundary arm below
                    // already resumes after the same native completion; do so
                    // here as well.  Otherwise a real listener attach requires
                    // an unrelated manual `start` before the guest can consume
                    // its F7.
                    m_.msp().start();
                    continue;
                }
                // Do not let a few tens of thousands of interpreted
                // instructions outrun an already-accepted socket's Telnet
                // negotiation on another host thread.  No guest clock or
                // state changes during this yield; it merely allows the
                // native device to finish becoming present.
                bool negotiating = false;
                for (auto& s : m_.stations())
                    if (s->attached() && !s->ready()) { negotiating = true; break; }
                if (negotiating) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (liveStopRequested_.load()) break;
        if (csp == nullptr || !m_.msp().stopped() || !csp->idleEventWait()) break;  // fault, or capped
        recordWorkstationNativeState("driver-idle");
        // The second safe boundary.  A `console send` serviced here is
        // consumed by the very delivery chain that follows, in the same
        // pass, exactly as a socket-side response would be.
        if (livePending_.load() != 0) pumpLiveWork();
        if (liveStopRequested_.load()) break;
        // Native timers share the appliance event pump, but their exact
        // second-level disposition decides whether execution resumes.  In
        // particular the work-station controller's subtype-2 maintenance
        // timer expires and rearms daily without posting any SSP task;
        // elapsed wall time must therefore never become a generic `start`
        // stimulus.
        // A deferred configure power-on AID is released here: the guest has
        // reached its all-tasks-waiting boundary, i.e. it is ready to
        // receive the wake a slow real device would only now be delivering.
        if (csp->flushDeferredCnfwsPowerOnAid()) {
            m_.msp().start();
            continue;
        }
        int expiredTimers = 0;
        if (csp->serviceDueNativeTimers(expiredTimers)) {
            csp->signalNewWork("native timer expiry");
            m_.msp().start();
            continue;
        }
        // One real response is consumed in reference order: first its
        // one-shot status half into the unit block, then (if the guest has
        // issued a decoded 32/42 read) its retained SVC-43 action.
        const bool idleInputStatusWork = deliverPendingInputStatuses(*csp);
        const bool idleInputCompletionWork = csp->completePendingWorkStationInput();
        if (idleInputStatusWork || idleInputCompletionWork) {
            csp->signalNewWork("work-station input completion");
            m_.msp().start();
            continue;
        }
        if (!wsContractTraceStation_.empty()) wsContractSnapshot(wsContractTraceStation_, "driver-idle");
        if (deliverPendingAttentions(*csp)) {
            csp->signalNewWork("work-station attention");
            if (!wsContractTraceStation_.empty()) wsContractSnapshot(wsContractTraceStation_, "attention-delivered");
            m_.msp().start();
            continue;
        }
        if (pollSeconds == 0 || (hasDeadline && clock::now() >= deadline)) break;
        int waitMilliseconds = -1;
        if (pollSeconds > 0) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
            waitMilliseconds = static_cast<int>(std::max<long long>(1, std::min<long long>(2147483647LL, remaining)));
        }
        const int timerMilliseconds = csp->millisecondsUntilNextNativeTimer();
        if (timerMilliseconds >= 0 && (waitMilliseconds < 0 || timerMilliseconds < waitMilliseconds))
            waitMilliseconds = timerMilliseconds;
        // Socket/session threads only ring this doorbell.  The loop wakes and
        // consumes their latched state above on this guest thread, preserving
        // the single-owner storage contract.  The live monitor rings the same
        // doorbell when it queues work, and `wait idle` reads the flag set
        // here: it is what lets a command file say "when the machine has
        // settled" instead of guessing an instruction count.
        liveIdle_.store(true);
        m_.waitForNativeEvent(waitMilliseconds);
        liveIdle_.store(false);
    }
    return total;
}

// ---- the host event chain -----------------------------------------------

// Drain each station's I/O-attention latch into the producer.  Returns true
// if any attention was delivered (so the caller resumes).  Only the console
// has a wired owner task today (the IPL/command-processor task at guest
// 0x0F00); other stations are noted and skipped.
bool MonitorCli::deliverPendingAttentions(As36ControlStorageProcessor& csp)
{
    bool any = false;
    for (auto& sp : m_.stations()) {
        devices::VirtualWorkstation& s = *sp;
        // The opposite edge first: a client that hung up switched its
        // terminal off.  Whatever the guest still had outstanding against
        // that terminal fails now, and the station's transfer binding is
        // released so that a client arriving afterwards - possibly already
        // latched below - is a fresh power-on and gets its own action-0 bind
        // and sign-on rather than inheriting a session whose display no
        // longer exists.
        if (s.takePowerOffPending()) any = powerOffStation(csp, s) || any;
        // A response has two independently consumed halves.  Its AID and
        // cursor first pass through the status path into the unit block.
        // Its fields remain queued until a guest Read Input Fields owns an
        // IOB.  Do not manufacture a controller doorbell: the exact
        // unsolicited-action element producer is a separate
        // controller-scheduler contract.
        // A completed display negotiation is the standalone replacement for
        // the host system's action-0 bind.  Resolve the exact configured TU
        // from THIS listener's port/address; do not retain the bootstrap
        // console TU merely because command 82 recorded it earlier.  The
        // attention latch is deliberately not consumed until the bind has
        // completed and the normal presentation arm below can use it.
        if (!s.isPrinter() && s.ready() && s.attentionPending()) {
            const int unit = (s.port() << 4) | s.address();
            const int configuredTub = csp.resolveConfiguredTubByUnit(unit);
            const bool autoSignOn = m_.config.listenerAutoSignOn;

            // Signon-at-IPL=Y is consumed while the physical HRI is acquired,
            // before SSP configuration.  A socket becoming ready later is a
            // new display entering through the transfer seam; replaying the
            // IPL-time report here would turn the policy bit into a second
            // HRI report and make W1 fundamentally unlike W6/W7.  Every
            // listener therefore follows the same action-0 path below.
            // Signon-at-IPL still controls the cold native inventory; it does
            // not classify a subsequent transport connection.
            devices::WorkStationSlot* nativeSlot = m_.devices().workStations().find(unit);

            // A listener connection is the appliance seam for the display
            // transfer, not a permanently attached physical S/36 display.  A
            // display cannot be transferred into the machine while the IPL
            // is still building its SSP work-station topology: the target
            // machine must first be running.  A client which completed Telnet
            // negotiation during IPL used to be bound at the next MSP
            // preemption point, which delivered action-0/F7 while the IPL
            // modules were still running, so the activation was consumed as
            // IPL controller traffic rather than as the post-IPL session
            // attach.
            //
            // Retain the socket's attention edge until the dispatcher reaches
            // the first real all-tasks-waiting boundary.  Subsequent input on
            // an already transferred station remains eligible at ordinary MSP
            // preemption points; only the ownership transfer is gated here.
            if (configuredTub != 0 && !csp.isM36WorkStationTransferReady(configuredTub, autoSignOn) &&
                !csp.idleEventWait())
                continue;

            if (configuredTub != 0 && !csp.isM36WorkStationTransferReady(configuredTub, autoSignOn)) {
                bool bindPending = false;
                if (csp.beginM36WorkStationTransfer(configuredTub, autoSignOn, !pendingWsEntryUnits_.empty(),
                                                    "station " + s.id() + " negotiated-session bind", bindPending)) {
                    if (bindPending) {
                        fmt::print("station {}: session waiting for workstation controller ({} request(s) ahead)\n",
                                   s.id(), pendingWsEntryUnits_.size());
                        continue;
                    }
                }
            }
            if (configuredTub != 0 && csp.isM36WorkStationTransferReady(configuredTub, autoSignOn) &&
                s.tubAddress != configuredTub) {
                if (nativeSlot != nullptr) nativeSlot->bindTransferRenderer();
                s.tubAddress = configuredTub;
                fmt::print("station {}: session bound to TU {:06X}\n", s.id(), configuredTub);
            }

            // The action-0 response can run the dispatcher immediately and
            // dispatch a real SSP task while the MSP still carries the stop
            // latch from the preceding no-task exit.  This applies to the
            // console as well as ordinary stations.
            any = any || csp.lastM36WorkStationTransferPostedGuestWork();

            // Action 0 binds the native display and calls its power-on
            // virtual.  Its ordinary response tail scans the native
            // controller-wide configuration (whose unit is FF) and can consume
            // the resulting BA.20 activation status.  FF is not a guest IOB
            // and no guest Invite is a prerequisite here.
            if (!s.isConsole() && configuredTub != 0 && s.tubAddress == configuredTub) {
                s.takeAttentionPending();
                fmt::print("station {}: bind/powerOn complete for TU {:06X}\n", s.id(), configuredTub);
                // Binding alone is not a guest attention.  But action 0's
                // response may just have consumed BA.20 and readied a task.
                // In that case real SSP work is runnable and the driver must
                // resume it; otherwise it would strand the exact response it
                // just delivered while reporting an old idle stop.
                continue;
            }
        }

        // During IPL the socket can finish negotiating before the guest has
        // built any station TUB.  That is a pending native device-present,
        // not an unowned attention: retain the latch and let a later
        // dispatcher pull point retry once the guest topology exists.
        if (!s.isPrinter() && s.attentionPending() && s.tubAddress == 0) continue;

        if (s.isConsole() && s.tubAddress != 0) {
            const bool automaticTransfer = m_.config.listenerAutoSignOn;
            // Present the workstation once.  The IPL posts task 0019 and the
            // guest, after its replacement call returns, posts event 0029
            // back to task 0009.  That acknowledgement is guest-owned; the
            // host must not manufacture a second completion event.
            if (consoleSignOnStage_ == 0) {
                if (!s.takeAttentionPending()) continue;
                // Action 0 binds and powers on the native display.  Its
                // response may deliver F7 through the controller-wide native
                // configuration; connection readiness alone still creates no
                // SSP element, OC, TU or session object.
                consoleSignOnStage_ = 1;
                fmt::print("station {}: bind/powerOn complete for TU {:06X}\n", s.id(), s.tubAddress);
                // FAITHFUL appliance sign-on (signon_statement): front-load
                // the auto-signon STATEMENT (the stand-in that builds the
                // real sign-on JCB) and the request-state stamp at the SAME
                // idle as the device-present, so the JCB exists BEFORE
                // class-init reads the FA work space.  Posting the statement
                // at a LATER idle is too late: class-init faults on the null
                // FA work space first.
                if (automaticTransfer && m_.config.consoleSignOnStatement) {
                    // The reference also raises its class map-gate
                    // experiment switch here; SIM/36 does not carry that
                    // experiment.
                    if (m_.config.consoleSignOnRequest)
                        csp.postConsoleSignOnRequest(s.tubAddress, "station " + s.id() + " sign-on request");
                    csp.postConsoleSignOnStatement(s.tubAddress, "station " + s.id() + " sign-on statement");
                    consoleSignOnStatementPosted_ = true;
                    consoleSignOnStage_ = 2;
                    fmt::print("station {}: sign-on request/statement experiment + mapgate + auto-signon statement "
                               "front-loaded (real #CPON JCB before class-init)\n", s.id());
                } else {
                    fmt::print("station {}: prompt session ready on TU {:06X}\n", s.id(), s.tubAddress);
                }
            } else if (consoleSignOnStage_ == 1 && automaticTransfer && m_.config.consoleSignOnStatement &&
                       !consoleSignOnStatementPosted_) {
                // Phase 1 (opt-in): the SIGN-ON STATEMENT element, which
                // drives the JCB-build half of the command processor
                // unforced (routing key = the console TUB address).  Posted
                // BEFORE the phase-2 job-init wake; the next idle then
                // delivers that.  Stops at the command processor's
                // console-TUB request-state gate, so it is off by default.
                s.takeAttentionPending();
                // Model the inbound sign-on REQUEST (opt-in): stamp the
                // console-TUB request state the gate reads BEFORE posting
                // the statement that drives it.
                if (m_.config.consoleSignOnRequest)
                    csp.postConsoleSignOnRequest(s.tubAddress, "station " + s.id() + " sign-on request");
                csp.postConsoleSignOnStatement(s.tubAddress, "station " + s.id() + " sign-on statement");
                consoleSignOnStatementPosted_ = true;
                fmt::print("station {}: console sign-on STATEMENT delivered (phase 1) - #MSSC router key = console "
                           "TUB {:04X} to task 0F00 (drives #CPSC -> #CPSI -> #CPRT)\n", s.id(), s.tubAddress);
                any = true;
            }
        } else if (m_.config.wsInteractive && !s.isConsole()) {
            // EXPERIMENTAL per-station present (ws_interactive).  Deliver a
            // device-present for an ordinary display, tagged by ITS OWN
            // terminal unit block so there is no cross-talk with the console:
            // the element carries this station's TUB and the present marker
            // is set on THAT block; the block is resolved from the guest's
            // own TUB+12 unit field, so W1's block is never handed to W2 or
            // vice-versa.  The owner is the command router task 0F00 (the
            // only task before the console signs on; it routes on the
            // element payload).  One present per station.
            if (wsPresented_.count(s.id()) != 0) { s.takeAttentionPending(); continue; }
            if (!s.takeAttentionPending()) continue;
            const int unit = (s.port() << 4) | s.address();
            const int tub = s.tubAddress != 0 ? s.tubAddress : csp.resolveTubByUnit(unit);
            if (tub == 0) {
                fmt::print("station {}: attention, but no #SVTUB terminal unit block for unit {:02X} yet (phase 2 has "
                           "not built it) - cannot present\n", s.id(), unit);
                continue;
            }
            s.tubAddress = tub;
            csp.raiseDeviceAttention(tub, processors::controlstorage::GuestLowStorage::kTaskBlock,
                                     "station " + s.id() + " present (ws_interactive)");
            wsPresented_.insert(s.id());
            fmt::print("station {}: EXPERIMENTAL per-station device-present delivered - own TUB {:06X} (unit {:02X}) "
                       "to router task {:04X}\n", s.id(), tub, unit,
                       processors::controlstorage::GuestLowStorage::kTaskBlock);
            any = true;
        } else if (s.takeAttentionPending()) {
            fmt::print("station {}: I/O attention, but no producer-wired owner task (only the console is wired "
                       "today; ws_interactive is off) - ignored\n", s.id());
        }
    }
    return any;
}

// Deliver the byte-exact status half of queued terminal responses.  The
// later retained Read uses the same record's field half and its
// already-owned SVC-43 element.
bool MonitorCli::deliverPendingInputStatuses(As36ControlStorageProcessor& csp)
{
    bool posted = false;
    for (auto& sp : m_.stations()) {
        devices::VirtualWorkstation& s = *sp;
        if (s.isPrinter() || s.tubAddress == 0 || !s.attentionPending()) continue;
        if (csp.deliverWorkStationInputStatus(s.tubAddress)) {
            s.takeAttentionPending();
            posted = true;
        }
    }
    return posted;
}

// Guest-thread half of a display power-off.  Returns true when guest work
// was posted (a failed operation woke its owner) so the driver resumes the
// machine.
bool MonitorCli::powerOffStation(As36ControlStorageProcessor& csp, devices::VirtualWorkstation& s)
{
    const int unit = (s.port() << 4) | s.address();
    const int tub = s.tubAddress != 0 ? s.tubAddress : csp.resolveConfiguredTubByUnit(unit);
    const std::string call = "station " + s.id() + " powered off";
    int failed = 0;
    bool posted = false;
    while (csp.failPendingWorkStationOperation(unit, call)) {
        failed++;
        posted = true;
    }
    s.powerOff();
    csp.deactivateWorkStationUnit(unit);
    const bool released = csp.endM36WorkStationTransfer(tub, call);
    wsPresented_.erase(s.id());
    fmt::print("station {}: client gone - display powered off; {} retained operation(s) failed with status 02/03 "
               "(device not attached){}\n", s.id(), failed,
               released ? fmt::format("; TU {:06X} transfer released, the next client is a new power-on", tub) : "");
    return posted;
}

// The state the boot and IPL verbs reset: the console sign-on stages, the
// per-station present set and the queued work-station entries.
void MonitorCli::resetHostEventState()
{
    consoleSignOnStage_ = 0;
    consoleSignOnStatementPosted_ = false;
    wsPresented_.clear();
    pendingWsEntryUnits_.clear();
}

}  // namespace sim36::monitor
