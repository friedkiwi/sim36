#include "Machine/Machine.h"
#include "Storage/FolderTapeBackend.h"
#include "Storage/DisketteBackend.h"
#include "Configuration/ConfigError.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include <fmt/format.h>

namespace sim36::machine {

using configuration::CspKind;
using configuration::EmulatorConfig;

namespace {

std::string sizeText(long long bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) return std::to_string(bytes / (1024LL * 1024 * 1024)) + "G";
    if (bytes >= 1024LL * 1024) return std::to_string(bytes / (1024LL * 1024)) + "M";
    if (bytes >= 1024) return std::to_string(bytes / 1024) + "K";
    return std::to_string(bytes) + "B";
}

// `readonly` is ReadOnly ALONE: overlay accepts writes and holds them.
const char* modeSuffix(storage::VolumeMode mode)
{
    if (mode == storage::VolumeMode::ReadOnly) return " readonly";
    if (mode == storage::VolumeMode::Overlay) return " overlay";
    return "";
}

bool equalsIgnoreCaseRole(const std::string& a, const char* b)
{
    std::string x = a, y = b;
    for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return x == y;
}

std::string bareName(std::string s)
{
    while (!s.empty() && s[0] == '#') s.erase(s.begin());
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Machine::Machine(const EmulatorConfig& cfg, const SessionBackends* sessionBackends)
    : config(cfg),
      // A real Advanced/36 dump is 16 MB even though this SSP model describes
      // 1 MB of installed main storage: the extra backing is the host's CSP
      // address space and translated-page store, not installed storage.
      state(cfg.mainStorageKb * 1024,
            cfg.cspKind() == CspKind::Virtual ? 16 * 1024 * 1024 : cfg.mainStorageKb * 1024)
{
    state.attachSrcTracer(&trace);
    disk_ = std::make_unique<storage::DiskBackend>(
        cfg.volumePath, cfg.volumeOverlay ? storage::VolumeMode::Overlay
                        : cfg.volumeReadOnly ? storage::VolumeMode::ReadOnly : storage::VolumeMode::ReadWrite);
    devices_ = std::make_unique<devices::DeviceSet>(state, *disk_, trace);

    // A diskette in the drive at power-on, if the definition names an image.
    // The geometry is NOT configured, it is read off the medium, so this
    // either works or says exactly which check failed.
    if (!cfg.diskettePath.empty()) {
        std::string why;
        auto d = storage::DisketteBackend::open(cfg.diskettePath, cfg.disketteReadOnly, why);
        if (!d) throw configuration::ConfigError(cfg.diskettePath, 0, "cannot be used as a diskette: " + why);
        devices_->diskette.insert(std::move(d));
    }
    // A tape in the drive at power-on, if a tape device declared a folder:
    // the container is opened and mounted here, and a bad folder is a
    // configuration error the operator should see, not a silent empty drive.
    if (cfg.tape && !cfg.tape->folderPath.empty()) {
        std::string why;
        auto t = storage::FolderTapeBackend::open(cfg.tape->folderPath, cfg.tape->readOnly, why);
        if (!t) throw configuration::ConfigError(cfg.tape->folderPath, 0, "cannot be used as a tape: " + why);
        devices_->tape.load(std::move(t));
    }

    // A model whose CSP kind is microcode needs a CSP interpreter and the
    // microcode volume set for that model and stage.
    if (cfg.cspKind() != CspKind::Virtual)
        throw std::runtime_error(fmt::format(
            "model {} needs a microcode control storage processor, which is not "
            "implemented: it would require a CSP interpreter and the microcode "
            "volume set for that model and stage. See "
            "docs/s36/machine-models-and-startup.md", cfg.model));
    csp_ = std::make_unique<processors::controlstorage::As36ControlStorageProcessor>(state, config, *devices_, *disk_, trace);
    auto* csp = csp_.get();
    state.stateDescriber = [csp] { return csp->describeSrcState(); };
    state.checkStateDescriber = [csp] { return csp->describeCheckState(); };
    // Module-filtered breakpoints (`break member <name>`, `breakm`) resolve
    // the active member for the current IAR and task, so they survive the
    // load-0x1000 aliasing.
    csp->mainStorage().memberResolver = [csp, this] {
        return csp->describeActiveMember(csp->currentTaskBlock(), state.msp.iar);
    };
    csp->mainStorage().memberNameResolver = [csp, this] {
        processors::controlstorage::LoadedMember member;
        int offset;
        return csp->tryActiveMember(csp->currentTaskBlock(), state.msp.iar, member, offset) ? member.name
                                                                                              : std::string();
    };

    // The session normally supplies persistent host backends so listeners
    // and clients can exist before and across machine lifetimes; a direct
    // machine user gets machine-owned backends.  The guest-facing station
    // model above that seam is owned by this machine.
    auto signal = [this] { signalNativeEvent(); };
    for (const auto& s : cfg.stations) {
        // The slot's type is fixed here, once, and it reaches both ends: the
        // six-byte configuration record `82` reports, and the listener that
        // decides which 5250 client it will serve.
        if (s.isPrinter()) {
            host::PrinterBackend* pb = nullptr;
            if (sessionBackends != nullptr) {
                auto it = sessionBackends->find(s.id());
                if (it == sessionBackends->end())
                    throw std::runtime_error("station " + s.id() + " has no session listener");
                pb = dynamic_cast<host::PrinterBackend*>(it->second.get());
            } else {
                pb = new host::PrinterBackend(s.listenHost, s.listenPort, "printer " + s.id(), &trace, signal);
            }
            if (pb == nullptr) throw std::runtime_error("station " + s.id() + " listener type does not match printer role");
            pb->bindMachine(&trace, signal);
            auto p = std::make_unique<devices::VirtualPrinter>(s, *pb, trace, sessionBackends == nullptr);
            devices_->addPrinter(*p);
            printers_.push_back(std::move(p));
        } else {
            host::WorkstationBackend* backend = nullptr;
            if (sessionBackends != nullptr) {
                auto it = sessionBackends->find(s.id());
                if (it == sessionBackends->end())
                    throw std::runtime_error("station " + s.id() + " has no session listener");
                backend = dynamic_cast<host::WorkstationBackend*>(it->second.get());
            } else {
                backend = new host::WorkstationBackend(s.listenHost, s.listenPort, "station " + s.id(), &trace, signal);
            }
            if (backend == nullptr)
                throw std::runtime_error("station " + s.id() + " listener type does not match display role");
            // A display backend keeps the chassis' listener trace: only the
            // printer backend is rebound to the machine's tracer, so a display
            // session's record traces stay silent under `trace ws`.
            // Work-station address 0.0 is the console on every 5250 machine:
            // it has to be a terminal, and it has to be present for the IPL
            // to complete.  So `role console` attaches to this emulator's
            // operator interface instead of opening a listener: present from
            // power-on and never racing six telnet sessions for acquisition.
            if (equalsIgnoreCaseRole(s.role, "console")) backend->attachConsole();
            auto ws = std::make_unique<devices::VirtualWorkstation>(s, *backend, trace, sessionBackends == nullptr);
            devices_->addStation(*ws);
            stations_.push_back(std::move(ws));
        }
    }
}

Machine::~Machine()
{
    // The station models go first: a machine-owned backend is deleted by
    // its model, and a session-owned one is merely unbound.
    stations_.clear();
    printers_.clear();
}

devices::VirtualWorkstation* Machine::findStation(const std::string& id)
{
    for (auto& s : stations_)
        if (s->id() == id) return s.get();
    return nullptr;
}

devices::VirtualPrinter* Machine::findPrinter(const std::string& id)
{
    for (auto& p : printers_)
        if (p->id() == id) return p.get();
    return nullptr;
}

void Machine::startListeners()
{
    if (config.stationMultiplex) {
        // The multiplexer itself is owned by the monitor chassis and outlives
        // the machine, so a client can attach before power-on and still be
        // present when the guest IPLs.  What belongs to the machine is only
        // this: with one port serving every display station, the per-station
        // listeners are NOT opened, and a station that also declares a
        // `listen` says so rather than quietly not existing.
        for (auto& s : stations_)
            if (!s->isConsole() && s->backend().port() != 0)
                fmt::print("station {}: per-station listener disabled (terminal multiplex is on)\n", s->id());
        // Printers are untouched: a different 5250 family with different
        // record framing, and multiplexed printer attachment is not
        // implemented.
        for (auto& p : printers_) p->start();
        return;
    }
    for (auto& s : stations_) s->start();
    for (auto& p : printers_) p->start();
}

std::vector<host::MultiplexStationView> Machine::multiplexStations()
{
    std::vector<devices::VirtualWorkstation*> ordered;
    for (auto& s : stations_) ordered.push_back(s.get());
    std::stable_sort(ordered.begin(), ordered.end(), [](const devices::VirtualWorkstation* x, const devices::VirtualWorkstation* y) {
        return x->port() != y->port() ? x->port() < y->port() : x->address() < y->address();
    });
    std::vector<host::MultiplexStationView> view;
    int n = 0;
    for (auto* s : ordered) {
        n++;
        host::MultiplexStationView v;
        v.number = n;
        v.id = s->id();
        v.isConsole = s->isConsole();
        v.available = s->isConsole() ? !s->backend().consoleClientAttached() : !s->backend().attached();
        v.backend = &s->backend();
        view.push_back(v);
    }
    return view;
}

void Machine::signalNativeEvent()
{
    {
        std::lock_guard<std::mutex> lock(nativeEventGate_);
        nativeEventSet_ = true;
    }
    nativeEvent_.notify_one();
}

bool Machine::waitForNativeEvent(int milliseconds)
{
    std::unique_lock<std::mutex> lock(nativeEventGate_);
    if (milliseconds < 0) {
        nativeEvent_.wait(lock, [this] { return nativeEventSet_; });
    } else if (!nativeEvent_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return nativeEventSet_; })) {
        return false;
    }
    // Auto-reset: one wait consumes one signal.
    nativeEventSet_ = false;
    return true;
}

void Machine::readVtocs()
{
    systemVtoc_ = storage::Vtoc::readSystem(*disk_);
    userVtoc_ = storage::Vtoc::readUser(*disk_);
    vtocsRead_ = true;
}

const storage::VtocEntry* Machine::find(const std::string& name) const
{
    const std::string want = bareName(name);
    for (const auto& l : systemVtoc_)
        if (bareName(l.name) == want) return &l;
    for (const auto& l : userVtoc_)
        if (bareName(l.name) == want) return &l;
    return nullptr;
}

std::vector<std::string> Machine::mediaLines() const
{
    std::vector<std::string> lines;
    lines.push_back("Drive 1: " + std::filesystem::path(disk_->path()).filename().string() + "  " +
                    sizeText(disk_->sectorCount() * storage::DiskBackend::kSectorBytes) + modeSuffix(disk_->mode()));
    if (const storage::DisketteBackend* diskette = devices_->diskette.medium())
        lines.push_back("Diskette: " + std::filesystem::path(diskette->path()).filename().string() + "  " +
                        sizeText(diskette->geometry().totalBytes()) + (diskette->readOnly() ? " readonly" : ""));
    if (const storage::ITapeBackend* tape = devices_->tape.medium()) {
        std::string path = tape->path();
        while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
        lines.push_back("Tape: " + std::filesystem::path(path).filename().string() + "  " + tape->volumeId() +
                        (tape->readOnly() ? " readonly" : ""));
    }
    return lines;
}

void Machine::reset()
{
    msp().reset();
    scheduler.clear();
    csp_->bringUpControlProcessor();
    csp_->iplMainProcessor();
}

}  // namespace sim36::machine

namespace sim36::machine {

bool Machine::restoreCheckpoint(const monitor::MachineSnapshot::RuntimeState& s, std::string& failure)
{
    if (s.main.size() != static_cast<std::size_t>(state.backingBytes())) {
        failure = "checkpoint main-storage size does not match its machine definition";
        return false;
    }
    scheduler.clear();
    if (!csp_->restoreCheckpointMemory(s.main, s.currentTaskBlock, s.currentRequestBlock, failure)) return false;

    for (std::size_t i = 0; i < 128 && i < s.atr.size(); i++) state.atr[i] = s.atr[i];
    state.cycles = s.cycles;
    state.restoreM36Src(s.m36Src);
    MspRegisters& r = state.msp;
    r.iar = s.iar;
    r.arr = s.arr;
    r.xr1 = s.xr1;
    r.xr2 = s.xr2;
    for (int i = 0; i < 8; i++) r.wr[i] = s.wr[i];
    r.loadPsr(s.psr);
    r.pactDir = s.pactDir;
    r.pactXr1 = s.pactXr1;
    r.pactXr2 = s.pactXr2;
    r.pactIar = s.pactIar;
    r.pactReg = s.pactReg;
    r.pactAtr = s.pactAtr;
    r.pactCsp = s.pactCsp;
    r.setPmr(s.pmr);
    r.cmr = s.cmr;
    msp().restoreCheckpoint(s.stopped, s.stopReason, s.instructions, s.atPreemptionPoint);
    scheduler.restoreClock(s.schedulerNow);
    if (!csp_->restoreCheckpoint(s.csp, failure)) return false;
    if (!devices_->restorePendingCheckpoint(s.devices, failure)) return false;

    for (const auto& saved : s.stations) {
        devices::VirtualWorkstation* ws = findStation(saved.id);
        if (ws == nullptr) {
            failure = "checkpoint station " + saved.id + " is absent";
            return false;
        }
        ws->restoreCheckpoint(saved.tubAddress, static_cast<devices::WorkstationOutputMode>(saved.outputMode),
                              static_cast<devices::PutWithInviteReadMode>(saved.inviteReadMode), saved.inviteOutstanding,
                              saved.activeReadMode, saved.outputStreams, saved.outputBytes, saved.inputRecords,
                              saved.hasLastOutput ? &saved.lastOutput : nullptr);
    }
    for (const auto& saved : s.printers) {
        devices::VirtualPrinter* printer = findPrinter(saved.id);
        if (printer == nullptr) {
            failure = "checkpoint printer " + saved.id + " is absent";
            return false;
        }
        printer->restoreCheckpoint(saved.pubAddress, saved.outputStreams, saved.outputBytes,
                                   saved.hasLastOutput ? &saved.lastOutput : nullptr);
    }

    if (s.hasTapePosition && devices_->tape.medium() != nullptr) {
        storage::ITapeBackend* tape = devices_->tape.medium();
        tape->rewind();
        int spaced;
        for (int i = 0; i < s.tapePosition.fileNumber; i++) tape->spaceFiles(1, spaced);
        if (s.tapePosition.blockNumber != 0) tape->spaceRecords(s.tapePosition.blockNumber, spaced);
    }
    failure.clear();
    return true;
}

}  // namespace sim36::machine
