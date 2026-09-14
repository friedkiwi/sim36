#include "Monitor/PanicDump.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>

#include <fmt/format.h>
#include <mz.h>
#include <mz_os.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "Machine/Machine.h"
#include "Monitor/ConfigurationRenderer.h"
#include "Monitor/MachineSnapshot.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/TaskBlock.h"

namespace sim36::monitor {

namespace fs = std::filesystem;
using processors::controlstorage::As36ControlStorageProcessor;
using processors::controlstorage::GuestHeap;
using processors::controlstorage::GuestLowStorage;
using processors::controlstorage::TaskBlock;

namespace {

const char* boolText(bool v) { return v ? "True" : "False"; }

std::string utcNow(const char* format)
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, format, &tm);
    return buf;
}

// The .NET "o" round-trip format for a UTC instant, to the tick.
std::string utcNowRoundTrip()
{
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now - secs).count() / 100;
    std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return fmt::format("{}.{:07}Z", buf, ticks);
}

std::string randomHex(int digits)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string s;
    for (int i = 0; i < digits; i++) s += "0123456789abcdef"[gen() & 15];
    return s;
}

std::string safeName(const std::string& value)
{
    if (value.empty()) return "unknown";
    std::string s;
    for (char c : value)
        s += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return s;
}

// One archive, written entry by entry with deflate, as the reference's
// archive writer did.
class Archive {
public:
    explicit Archive(const std::string& path)
    {
        handle_ = mz_zip_writer_create();
        if (handle_ == nullptr) throw std::runtime_error("cannot create the panic archive writer");
        mz_zip_writer_set_compress_method(handle_, MZ_COMPRESS_METHOD_DEFLATE);
        mz_zip_writer_set_compress_level(handle_, MZ_COMPRESS_LEVEL_DEFAULT);
        if (mz_zip_writer_open_file(handle_, path.c_str(), 0, 0) != MZ_OK) {
            mz_zip_writer_delete(&handle_);
            throw std::runtime_error("cannot open the panic archive " + path);
        }
    }
    ~Archive()
    {
        if (handle_ != nullptr) {
            mz_zip_writer_close(handle_);
            mz_zip_writer_delete(&handle_);
        }
    }
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    void close()
    {
        int32_t rc = mz_zip_writer_close(handle_);
        mz_zip_writer_delete(&handle_);
        handle_ = nullptr;
        if (rc != MZ_OK) throw std::runtime_error("cannot finish the panic archive");
    }

    void bytes(const std::string& name, const uint8_t* data, std::size_t length)
    {
        mz_zip_file info{};
        info.filename = name.c_str();
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.modified_date = std::time(nullptr);
        info.version_madeby = MZ_VERSION_MADEBY;
        info.flag = MZ_ZIP_FLAG_UTF8;
        // An empty buffer is still an entry (the reference writes every
        // last-I/O buffer, present or not); the writer refuses a null pointer.
        static const uint8_t kEmpty = 0;
        const uint8_t* source = length == 0 ? &kEmpty : data;
        if (mz_zip_writer_add_buffer(handle_, const_cast<uint8_t*>(source), static_cast<int32_t>(length), &info) != MZ_OK)
            throw std::runtime_error("cannot write archive entry " + name);
    }
    void bytes(const std::string& name, const std::vector<uint8_t>& data) { bytes(name, data.data(), data.size()); }
    void text(const std::string& name, const std::string& text)
    {
        bytes(name, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

private:
    void* handle_ = nullptr;
};

void trySection(std::vector<std::string>& errors, const std::string& label, const std::function<void()>& capture)
{
    try {
        capture();
    } catch (const std::exception& ex) {
        errors.push_back(label + ": exception: " + ex.what());
    }
}

std::string join(const std::vector<std::string>& lines)
{
    std::string s;
    for (std::size_t i = 0; i < lines.size(); i++) s += (i ? "\n" : "") + lines[i];
    return s;
}

std::string renderRuntime(machine::Machine& m)
{
    const auto& r = m.state.msp;
    std::string s;
    s += fmt::format("state: {}\nstop reason: {}\ninstructions: {}\ncycles: {}\nscheduler now: {}\nSRC: {:04X}\n",
                     m.msp().stopped() ? "stopped" : "running", m.msp().stopReason(), m.msp().instructionsExecuted(),
                     m.state.cycles, m.scheduler.now(), m.state.m36Src() & 0xFFFF);
    s += fmt::format("IAR={:04X} ARR={:04X} XR1={:02X}:{:04X} XR2={:02X}:{:04X} PSR={:02X}\n", r.iar, r.arr, r.pactXr1,
                     r.xr1, r.pactXr2, r.xr2, r.psr());
    s += "WR0-7=";
    for (uint16_t wr : r.wr) s += fmt::format(" {:04X}", wr);
    s += "\n";
    s += fmt::format("PACT dir/xr1/xr2/iar/reg/atr/csp={:02X}/{:02X}/{:02X}/{:02X}/{:02X}/{:02X}/{:02X}\nPMR={:02X} CMR={:02X}\n",
                     r.pactDir, r.pactXr1, r.pactXr2, r.pactIar, r.pactReg, r.pactAtr, r.pactCsp, r.pmr(), r.cmr);
    s += (m.state.stateDescriber ? m.state.stateDescriber() : std::string("(no task description)")) + "\n";
    s += "ATR:\n";
    for (int i = 0; i < 128; i += 8) {
        s += fmt::format("{:02X}:", i);
        for (int j = 0; j < 8; j++) s += fmt::format(" {:04X}", m.state.atr[i + j]);
        s += "\n";
    }
    return s;
}

std::string renderCheckHistory(machine::Machine& m)
{
    std::vector<std::string> checks = m.state.copyCheckHistory();
    if (checks.empty()) return "No processor checks were recorded.\n";
    std::string s;
    for (std::size_t i = 0; i < checks.size(); i++) {
        s += fmt::format("===== check {}/{} =====\n", i + 1, checks.size());
        s += checks[i];
        if (checks[i].empty() || checks[i].back() != '\n') s += "\n";
    }
    return s;
}

std::string intsText(const std::vector<int>& v)
{
    std::string s;
    for (std::size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(v[i]);
    return s;
}

std::string stringsText(const std::vector<std::string>& v)
{
    std::string s;
    for (std::size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + v[i];
    return s;
}

// The checkpoint record, one public field per line in declaration order,
// which is what the reference's reflection walk printed.
std::string renderCheckpointFields(const As36ControlStorageProcessor::CheckpointState& s)
{
    std::string o;
    o += "PendingDeviceAcePairs=" + intsText(s.pendingDeviceAcePairs) + "\n";
    o += "PendingAsyncChildren=" + intsText(s.pendingAsyncChildren) + "\n";
    o += "NativeTransferContinuations=" + intsText(s.nativeTransferContinuations) + "\n";
    o += std::string("Redispatch=") + boolText(s.redispatch) + "\n";
    o += "Phase2SvatJobTask=" + std::to_string(s.phase2SvatJobTask) + "\n";
    o += std::string("Phase2SvatDispatched=") + boolText(s.phase2SvatDispatched) + "\n";
    o += "TaskIdCounter=" + std::to_string(s.taskIdCounter) + "\n";
    o += "RequestBlockBeforeAttach=" + std::to_string(s.requestBlockBeforeAttach) + "\n";
    o += std::string("SystemMeasurementEnabled=") + boolText(s.systemMeasurementEnabled) + "\n";
    o += "TransferredTub=" + std::to_string(s.transferredTub) + "\n";
    o += "PendingTransferredTub=" + std::to_string(s.pendingTransferredTub) + "\n";
    o += std::string("TransferredAutoSignOn=") + boolText(s.transferredAutoSignOn) + "\n";
    o += std::string("PendingTransferAutoSignOn=") + boolText(s.pendingTransferAutoSignOn) + "\n";
    o += std::string("LastTransferPostedGuestWork=") + boolText(s.lastTransferPostedGuestWork) + "\n";
    o += "WsDeviceStatusPending=" + std::to_string(s.wsDeviceStatusPending) + "\n";
    o += "WsDeviceStatusDelivered=" + std::to_string(s.wsDeviceStatusDelivered) + "\n";
    o += "ActionStatusLow=" + std::to_string(s.actionStatusLow) + "\n";
    o += "ActionStatusHigh=" + std::to_string(s.actionStatusHigh) + "\n";
    o += "ActionCoverage=" + intsText(s.actionCoverage) + "\n";
    o += "SessionContentPopulated=" + std::to_string(s.sessionContentPopulated) + "\n";
    o += "ClssMapGateSet=" + std::to_string(s.clssMapGateSet) + "\n";
    o += "WddqWriteForced=" + std::to_string(s.wddqWriteForced) + "\n";
    o += "ExpAutoMapped=" + std::to_string(s.expAutoMapped) + "\n";
    o += "ExpGrounded=" + std::to_string(s.expGrounded) + "\n";
    o += "WsPresentPairs=" + intsText(s.wsPresentPairs) + "\n";
    o += "DirectAreaWords=" + intsText(s.directAreaWords) + "\n";
    o += "TaskWorkAreaFree=" + intsText(s.taskWorkAreaFree) + "\n";
    o += "WorkSpaces=" + intsText(s.workSpaces) + "\n";
    o += "Heap=" + intsText(s.heap) + "\n";
    o += "Ptt=" + intsText(s.ptt) + "\n";
    o += "ModuleStorageNext=" + std::to_string(s.moduleStorageNext) + "\n";
    o += "CurrentTransientProgramBlock=" + std::to_string(s.currentTransientProgramBlock) + "\n";
    o += "ModuleStoragePairs=" + intsText(s.moduleStoragePairs) + "\n";
    o += "WorkSpaceStoragePairs=" + intsText(s.workSpaceStoragePairs) + "\n";
    o += "WorkSpaceStoragePages=" + intsText(s.workSpaceStoragePages) + "\n";
    o += "ModuleStorageSizePairs=" + intsText(s.moduleStorageSizePairs) + "\n";
    o += "ModuleStorageFree=" + intsText(s.moduleStorageFree) + "\n";
    o += "LoadedMemberData=" + intsText(s.loadedMemberData) + "\n";
    o += "LoadedMemberNames=" + stringsText(s.loadedMemberNames) + "\n";
    o += "Aces=sim36::processors::controlstorage::ActionControlElementQueue::CheckpointState\n";
    return o;
}

std::string renderCsp(machine::Machine& m)
{
    auto& csp = m.nativeControlStorage();
    std::string s = fmt::format("current task block: {:06X}\ncurrent request block: {:06X}\nlast refusal:\n{}\n\n",
                                csp.currentTaskBlock(), csp.currentRequestBlock(), csp.lastRefusal());
    As36ControlStorageProcessor::CheckpointState state;
    std::string failure;
    if (!csp.captureCheckpoint(state, failure)) throw std::runtime_error(failure);
    s += renderCheckpointFields(state);
    return s;
}

std::string renderSystemQueue(machine::Machine& m)
{
    GuestHeap& heap = m.nativeControlStorage().heap();
    std::string s;
    std::vector<std::string> failures;
    bool valid = heap.checkInvariants(failures);
    auto allocated = heap.allocatedExtents();
    auto free = heap.freeExtents();
    s += fmt::format("pool={:06X}..{:06X} used={} capacity={} available={} allocations={} free-extents={} operation={} "
                     "invariants={}\n",
                     heap.low(), heap.high() - 1, heap.used(), heap.capacity(), heap.available(), allocated.size(),
                     free.size(), heap.operationSequence(), valid ? "OK" : "FAILED");
    for (const auto& failure : failures) s += "INVALID: " + failure + "\n";
    for (const auto& failure : heap.invariantFailures()) s += "HISTORY: " + failure + "\n";

    std::map<std::string, std::array<long long, 3>> owners;
    for (const auto& e : allocated) {
        std::string owner = e.owner.empty() ? "(unlabelled)" : e.owner;
        auto& totals = owners[owner];
        totals[0]++;
        totals[1] += e.length;
        totals[2] += e.requested;
    }
    s += "\nowner summary (fragments, carved bytes, requested bytes):\n";
    for (const auto& owner : owners)
        s += fmt::format("{}\t{}\t{}\t{}\n", owner.first, owner.second[0], owner.second[1], owner.second[2]);

    s += "\nlive allocation fragments:\n";
    for (const auto& e : allocated)
        s += fmt::format("{:06X}..{:06X}\tlength={}\trequested={}\tallocation={}\t{}\n", e.address, e.address + e.length - 1,
                         e.length, e.requested, e.allocation, e.owner.empty() ? "(unlabelled)" : e.owner);
    s += "\nfree extents:\n";
    for (const auto& e : free) s += fmt::format("{:06X}..{:06X}\tlength={}\n", e.address, e.address + e.length - 1, e.length);
    return s;
}

std::string renderTasks(machine::Machine& m)
{
    int current = m.nativeControlStorage().currentTaskBlock();
    auto& st = m.state;
    int at = st.readAddr24(GuestLowStorage::queueHeader(39));
    std::set<int> seen;
    std::string s = "mark tb     id   state stat2 priority request-block next\n";
    int count = 0;
    while (at != 0 && count < 4096 && seen.insert(at).second) {
        if (!TaskBlock::isTaskBlock(st, at)) {
            s += fmt::format("invalid queue-39 node {:06X}; walk stopped\n", at);
            break;
        }
        int next = st.readAddr24(at + TaskBlock::kOffQueue39Link);
        s += fmt::format(" {}   {:06X} {:04X}   {:02X}    {:02X}      {:02X}      {:06X}       {:06X}\n",
                         at == current ? '*' : ' ', at, st.readHalf(at + TaskBlock::kOffTaskId),
                         st.readByte(at + TaskBlock::kOffState), st.readByte(at + TaskBlock::kOffStat2),
                         st.readByte(at + TaskBlock::kOffPriority), st.readAddr24(at + TaskBlock::kOffRequestBlock), next);
        count++;
        at = next;
    }
    if (at != 0 && count >= 4096) s += "queue-39 walk stopped at 4096 nodes\n";
    else if (at != 0 && seen.count(at) != 0) s += fmt::format("queue-39 loop returns to {:06X}\n", at);
    s += fmt::format("{} valid task(s) captured\n", count);
    return s;
}

std::string renderPendingFields(const devices::DeviceSet::PendingCheckpoint& d)
{
    std::string o;
    o += "InputReadPairs=" + intsText(d.inputReadPairs) + "\n";
    o += "InputStagingPairs=" + intsText(d.inputStagingPairs) + "\n";
    o += "InputResponseStatus=" + intsText(d.inputResponseStatus) + "\n";
    o += "PendingC1Pairs=" + intsText(d.pendingC1Pairs) + "\n";
    o += "ControllerInvites=" + intsText(d.controllerInvites) + "\n";
    o += "PendingActivationUnits=" + intsText(d.pendingActivationUnits) + "\n";
    o += "ActivatedUnits=" + intsText(d.activatedUnits) + "\n";
    o += "NativeActiveUnits=" + intsText(d.nativeActiveUnits) + "\n";
    o += "ConfiguredUnits=" + intsText(d.configuredUnits) + "\n";
    o += "InternalRendererUnits=" + intsText(d.internalRendererUnits) + "\n";
    o += "TransferRendererUnits=" + intsText(d.transferRendererUnits) + "\n";
    return o;
}

std::string renderDevices(machine::Machine& m)
{
    auto& d = m.devices();
    std::string s = fmt::format("unmodelled requests: {}\ndata-storage-controller requests: {}\npending controller invites: "
                                "{}\npending PUT-with-invites: {}\npending screen saves: {}\npending action-0 activations: "
                                "{}\n\n",
                                d.unmodelledRequests(), d.dataStorageControllerRequests(), d.pendingControllerInviteCount(),
                                d.pendingPutWithInviteCount(), d.pendingScreenSaveCount(), d.pendingAction0ActivationCount());
    s += renderPendingFields(d.capturePendingCheckpoint());
    s += fmt::format("\nfixed disk counters (no sectors included): reads={} sectors-read={} writes={} sectors-written={} "
                     "last-read-sector={}\n",
                     d.disk.readsIssued(), d.disk.sectorsRead(), d.disk.writesIssued(), d.disk.sectorsWritten(),
                     d.disk.lastReadSector());
    s += fmt::format("diskette counters: reads={} records-read={} writes={} records-written={} undecoded={}\n",
                     d.diskette.readsIssued(), d.diskette.recordsRead(), d.diskette.writesIssued(),
                     d.diskette.recordsWritten(), d.diskette.undecodedCommands());
    s += fmt::format("tape counters: reads={} writes={} controls={} unmapped={}\n", d.tape.readsIssued(),
                     d.tape.writesIssued(), d.tape.controlOps(), d.tape.unmappedCommands());
    return s;
}

std::string renderScheduler(machine::Machine& m)
{
    std::string s = fmt::format("now={} pending={}\n", m.scheduler.now(), m.scheduler.pending());
    for (const auto& e : m.scheduler.peek()) s += fmt::format("at={} seq={} label={}\n", e.at, e.seq, e.label);
    return s;
}

void writeFields(Archive& archive, const std::string& name, const host::ConsoleDisplay& display)
{
    std::string s = fmt::format("cursor={},{}\n", display.cursorRow(), display.cursorCol());
    for (const auto& f : display.fields())
        s += fmt::format("row={} col={} length={} ffw={:04X} attribute={:02X} input={} bypass={} mdt={} pending={}\n", f.row,
                         f.col, f.length, f.ffw < 0 ? 0xFFFFFFFFu : static_cast<unsigned>(f.ffw), f.attribute,
                         boolText(f.isInput()), boolText(f.bypass()), boolText(f.mdt), f.pending);
    archive.text(name, s);
}

void writeTerminalQueues(Archive& archive, const std::string& root, const host::WorkstationBackend::DiagnosticQueues& queues)
{
    std::string summary = fmt::format("input={}\nsave-screen={}\nhead-status-taken={}\n", queues.input.size(),
                                      queues.saveScreen.size(), boolText(queues.headStatusTaken));
    for (std::size_t i = 0; i < queues.input.size(); i++)
        summary += fmt::format("input[{}] opcode={} flags={} length={}\n", i, host::opcodeName(queues.input[i].opcode),
                               host::flagsName(queues.input[i].flags), queues.input[i].data.size());
    for (std::size_t i = 0; i < queues.saveScreen.size(); i++)
        summary += fmt::format("save-screen[{}] opcode={} flags={} length={}\n", i,
                               host::opcodeName(queues.saveScreen[i].opcode), host::flagsName(queues.saveScreen[i].flags),
                               queues.saveScreen[i].data.size());
    archive.text(root + "terminal-queues.txt", summary);
    for (std::size_t i = 0; i < queues.input.size(); i++)
        archive.bytes(root + fmt::format("pending-input-{:03}.bin", i), queues.input[i].data);
    for (std::size_t i = 0; i < queues.saveScreen.size(); i++)
        archive.bytes(root + fmt::format("pending-save-screen-{:03}.bin", i), queues.saveScreen[i].data);
}

void writeTerminalHistory(Archive& archive, const std::string& root,
                          const std::vector<host::WorkstationBackend::DiagnosticRecord>& history)
{
    std::string summary = fmt::format("retained={}\n", history.size());
    for (std::size_t i = 0; i < history.size(); i++) {
        const auto& record = history[i];
        summary += fmt::format("{:03} sequence={} direction={} disposition={} opcode={} flags={} length={}\n", i,
                               record.sequence, record.direction, record.disposition, host::opcodeName(record.opcode),
                               host::flagsName(record.flags), record.data.size());
        archive.bytes(root + fmt::format("history/{:03}-{}-{}.bin", i, record.direction, safeName(host::opcodeName(record.opcode))),
                      record.data);
    }
    archive.text(root + "terminal-history.txt", summary);
}

void writeStations(Archive& archive, machine::Machine& m)
{
    for (auto& ws : m.stations()) {
        std::string root = "stations/" + safeName(ws->id()) + "/";
        auto& b = ws->backend();
        archive.text(root + "state.txt",
                     fmt::format("id={}\nport={}\naddress={}\nrole={}\ndevice-code={}\nTUB={:06X}\nattached={}\nready={}\n"
                                 "listening={}\nendpoint={}\nterminal-type={}\ndevice-name={}\nuser-name={}\n"
                                 "attention-pending={}\npending-input={}\npending-save-screen={}\ninput-enabled={}\n"
                                 "invite-outstanding={}\noutput-mode={}\ninvite-read-mode={}\nactive-read-mode={:02X}\n"
                                 "saved-read-mode={:02X}\nrecords-sent={}\nbytes-sent={}\nrecords-received={}\n"
                                 "bytes-received={}\nrecords-dropped={}\noutput-streams={}\noutput-bytes={}\ninput-records={}\n",
                                 ws->id(), ws->port(), ws->address(), ws->role(), ws->deviceCode(), ws->tubAddress,
                                 boolText(ws->attached()), boolText(ws->ready()), boolText(b.listening()), b.endpoint(),
                                 b.terminalType(), b.deviceName(), b.userName(), boolText(ws->attentionPending()),
                                 b.pendingInput(), b.pendingSaveScreens(), boolText(b.inputEnabled()),
                                 boolText(ws->inviteOutstanding()), devices::workstationOutputModeName(ws->outputMode()),
                                 devices::putWithInviteReadModeName(ws->inviteReadMode()), ws->activeReadMode(),
                                 ws->savedReadMode(), b.recordsSent(), b.bytesSent(), b.recordsReceived(), b.bytesReceived(),
                                 b.recordsDropped(), ws->outputDataStreams(), ws->outputDataBytes(), ws->inputRecords()));
        archive.bytes(root + "last-output.bin", ws->hasLastOutputDataStream() ? ws->lastOutputDataStream() : std::vector<uint8_t>());
        std::vector<uint8_t> retained;
        ws->copyRetainedDeviceInput(retained);
        archive.bytes(root + "retained-device-input.bin", retained);
        archive.bytes(root + "device-screen-ebcdic.bin", ws->deviceDisplay().copyScreenBytes());
        archive.text(root + "device-screen.txt", ws->deviceDisplay().renderText());
        writeFields(archive, root + "device-fields.txt", ws->deviceDisplay());
        writeTerminalQueues(archive, root, b.captureDiagnosticQueues());
        writeTerminalHistory(archive, root, b.captureDiagnosticHistory());
        if (b.console() != nullptr) {
            archive.bytes(root + "console-screen-ebcdic.bin", b.console()->copyScreenBytes());
            archive.text(root + "console-screen.txt", b.console()->renderText());
            writeFields(archive, root + "console-fields.txt", *b.console());
            archive.text(root + "console-operations.txt", join(b.console()->log()) + "\n");
        }
    }
}

const char* kindName(host::StationKind kind)
{
    switch (kind) {
        case host::StationKind::Display: return "Display";
        case host::StationKind::Printer: return "Printer";
        case host::StationKind::Console: return "Console";
    }
    return "?";
}

void writeChassisStations(Archive& archive, const std::vector<host::StationBackend*>& backends)
{
    std::string s;
    for (const host::StationBackend* b : backends)
        s += fmt::format("kind={} endpoint={} listening={} attached={} ready={} terminal-type={} device-name={} user-name={} "
                         "attention={} accepted={} refused={} rejected={} sent={}/{} received={}/{} dropped={}\n",
                         kindName(b->kind()), b->endpoint(), boolText(b->listening()), boolText(b->attached()),
                         boolText(b->ready()), b->terminalType(), b->deviceName(), b->userName(),
                         boolText(b->attentionPending()), b->sessionsAccepted(), b->sessionsRefused(), b->sessionsRejected(),
                         b->recordsSent(), b->bytesSent(), b->recordsReceived(), b->bytesReceived(), b->recordsDropped());
    archive.text("stations/chassis-listeners.txt", s);
}

void writePrinters(Archive& archive, machine::Machine& m)
{
    for (auto& printer : m.printers()) {
        std::string root = "printers/" + safeName(printer->id()) + "/";
        auto& b = printer->backend();
        archive.text(root + "state.txt",
                     fmt::format("id={}\nport={}\naddress={}\nrole={}\ndevice-code={}\nPUB={:06X}\nattached={}\nready={}\n"
                                 "listening={}\nendpoint={}\nterminal-type={}\ndevice-name={}\nuser-name={}\n"
                                 "attention-pending={}\nrecords-sent={}\nbytes-sent={}\nrecords-received={}\n"
                                 "bytes-received={}\nrecords-dropped={}\noutput-streams={}\noutput-bytes={}\n",
                                 printer->id(), printer->port(), printer->address(), printer->role(), printer->deviceCode(),
                                 printer->pubAddress, boolText(printer->attached()), boolText(printer->ready()),
                                 boolText(b.listening()), b.endpoint(), b.terminalType(), b.deviceName(), b.userName(),
                                 boolText(b.attentionPending()), b.recordsSent(), b.bytesSent(), b.recordsReceived(),
                                 b.bytesReceived(), b.recordsDropped(), printer->outputDataStreams(),
                                 printer->outputDataBytes()));
        archive.bytes(root + "last-output.bin",
                      printer->hasLastOutputDataStream() ? printer->lastOutputDataStream() : std::vector<uint8_t>());
    }
}

}  // namespace

std::string PanicDump::create(const std::string& description, const std::string& reproduction,
                              const configuration::EmulatorConfig& config, machine::Machine* machine,
                              uint32_t pendingTrace, const std::vector<host::StationBackend*>& backends)
{
    std::string path = (fs::temp_directory_path() /
                        fmt::format("sim36-panic-{}-{}.zip", utcNow("%Y%m%d-%H%M%S"), randomHex(8)))
                           .string();
    try {
        {
            // Create the file first, owner-only, so guest memory is never
            // exposed through a permissive umask before byte 1 is written.
            std::FILE* touch = std::fopen(path.c_str(), "wb");
            if (touch == nullptr) throw std::runtime_error("cannot create the panic dump " + path);
            std::fclose(touch);
#ifndef _WIN32
            if (::chmod(path.c_str(), 0600) != 0)
                throw std::runtime_error("cannot make panic dump owner-only; chmod errno " + std::to_string(errno));
#else
            // Windows: the temporary directory is per user; no mode bits.
#endif
            Archive archive(path);
            std::vector<std::string> errors;
            archive.text("README.txt",
                         "SIM/36 panic dump format " + std::to_string(kFormatVersion) + "\n" + "Created UTC: " +
                             utcNowRoundTrip() + "\n" +
                             "Mounted media images, tape files and overlay sectors are NOT included.\n"
                             "The bounded last-read I/O buffers are included as operation evidence.\n"
                             "SENSITIVE: memory, screens, input and traces may contain credentials or user data.\n"
                             "runtime.bin is the versioned volatile-state record used by MachineSnapshot.\n");
            archive.text("operator/what-happened.txt", description);
            archive.text("operator/how-to-reproduce.txt", reproduction);
            trySection(errors, "config replay",
                       [&] { archive.text("config/config.sim", ConfigurationRenderer::renderReplay(config)); });
            trySection(errors, "config summary", [&] {
                archive.text("config/summary.txt", ConfigurationRenderer::renderHuman(config, machine != nullptr));
            });
            uint32_t flags = machine == nullptr ? pendingTrace : machine->trace.flags.load();
            archive.text("trace/settings.txt", fmt::format("flags={} ({})\n", static_cast<int>(flags), traceFlagsToString(flags)));
            trySection(errors, "chassis stations", [&] { writeChassisStations(archive, backends); });

            if (machine == nullptr) {
                archive.text("runtime/unavailable.txt", "No machine had been constructed when panic was entered.\n");
            } else {
                machine::Machine& m = *machine;
                // Memory is the least replaceable section and has no
                // precondition, so capture it before richer snapshots.
                trySection(errors, "main storage",
                           [&] { archive.bytes("runtime/main-storage.bin", m.state.raw(), static_cast<std::size_t>(m.state.backingBytes())); });
                trySection(errors, "native runtime",
                           [&] { archive.bytes("runtime/runtime.bin", MachineSnapshot::writeDiagnosticRuntime(m)); });
                trySection(errors, "runtime summary", [&] { archive.text("runtime/summary.txt", renderRuntime(m)); });
                trySection(errors, "processor check history",
                           [&] { archive.text("runtime/check-history.txt", renderCheckHistory(m)); });
                trySection(errors, "tasks", [&] { archive.text("runtime/tasks.txt", renderTasks(m)); });
                trySection(errors, "CSP state", [&] { archive.text("runtime/csp-state.txt", renderCsp(m)); });
                trySection(errors, "system queue ownership",
                           [&] { archive.text("runtime/system-queue-space.txt", renderSystemQueue(m)); });
                trySection(errors, "device state", [&] { archive.text("runtime/device-state.txt", renderDevices(m)); });
                trySection(errors, "last I/O buffers", [&] {
                    archive.bytes("io/fixed-disk-last-read.bin", m.devices().disk.lastRead());
                    archive.bytes("io/diskette-last-read.bin", m.devices().diskette.lastRead());
                    archive.bytes("io/tape-last-read.bin", m.devices().tape.lastRead());
                });
                trySection(errors, "scheduler", [&] { archive.text("runtime/scheduler.txt", renderScheduler(m)); });
                trySection(errors, "stations", [&] { writeStations(archive, m); });
                trySection(errors, "printers", [&] { writePrinters(archive, m); });
                trySection(errors, "deferred trace",
                           [&] { archive.text("trace/deferred-tail.txt", join(m.trace.copyDeferred()) + "\n"); });
            }
            if (!errors.empty()) archive.text("capture-errors.txt", join(errors) + "\n");
            archive.close();
        }
    } catch (...) {
        std::error_code ec;
        fs::remove(path, ec);
        throw;
    }
    return path;
}

}  // namespace sim36::monitor
