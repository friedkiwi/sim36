#include "Machine/Machine.h"

#include <cctype>
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

std::string bareName(std::string s)
{
    while (!s.empty() && s[0] == '#') s.erase(s.begin());
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Machine::Machine(const EmulatorConfig& cfg)
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
}

Machine::~Machine() = default;

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
