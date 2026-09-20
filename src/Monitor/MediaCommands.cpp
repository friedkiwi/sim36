// Monitor commands over the removable media: the diskette drive (insert,
// eject, dsktread, dsktwrite), the tape drive (load, unload, init, vtoc,
// files, status, the backend round-trip and the SVC 46 exercise) and
// savemain.
#include "Monitor/MonitorCli.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include <fmt/format.h>

#include "Devices/DeviceSet.h"
#include "Devices/VirtualDiskette.h"
#include "Devices/VirtualTape.h"
#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Storage/DisketteBackend.h"
#include "Storage/FolderTapeBackend.h"
#include "Storage/TapeManifest.h"

namespace sim36::monitor {

using devices::DisketteIoBlock;
using devices::NuTaIob;
using processors::controlstorage::Ecm;
using storage::FolderTapeBackend;
using storage::TapeManifest;
using storage::TapeResult;

namespace {

// int.Parse(s): optional sign, decimal digits only.
int parseInt(const std::string& s)
{
    if (s.empty()) throw MonitorError("Input string was not in a correct format.");
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) throw MonitorError("Input string was not in a correct format.");
    for (std::size_t k = i; k < s.size(); k++)
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) throw MonitorError("Input string was not in a correct format.");
    long long v = std::strtoll(s.c_str(), nullptr, 10);
    if (v > 0x7FFFFFFFLL || v < -0x80000000LL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

bool isReadOnlyFlag(const std::string& s)
{
    std::string l = toLower(s);
    return l == "ro" || l == "readonly" || l == "yes" || l == "true" || l == "1";
}

struct Score {
    int pass = 0, fail = 0;
};

void report(const std::string& what, bool ok, Score& s)
{
    fmt::print("  {:<42} {}\n", what, ok ? "PASS" : "FAIL");
    if (ok) ++s.pass; else ++s.fail;
}

std::string temporaryFolder(const char* prefix)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string name = prefix;
    for (int i = 0; i < 32; i++) name += "0123456789abcdef"[gen() & 15];
    return (std::filesystem::temp_directory_path() / name).string();
}

void removeTree(const std::string& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

std::string describeTapeLabel(const nlohmann::ordered_json& value)
{
    if (!value.is_object()) return value.is_string() ? value.get<std::string>() : value.dump();
    std::string sb;
    for (auto it = value.begin(); it != value.end(); ++it) {
        std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        if (val.empty()) continue;
        if (!sb.empty()) sb += "  ";
        sb += fmt::format("{}={}", it.key(), val);
    }
    return sb.empty() ? std::string("(blank)") : sb;
}

constexpr int kDiskReadBuffer = 0x20000;

}  // namespace

void MonitorCli::diskette(const std::vector<std::string>& a)
{
    auto& drive = m_.devices().diskette;
    std::string what = a.size() > 1 ? toLower(a[1]) : "show";

    if (what == "eject") {
        fmt::print("{}\n", drive.eject() ? "diskette ejected" : "the drive is already empty");
        return;
    }

    if (what == "insert") {
        if (a.size() < 3) {
            fmt::print("diskette insert <image>\n");
            return;
        }
        std::string why;
        auto d = storage::DisketteBackend::open(a[2], m_.config.disketteReadOnly, why);
        if (!d) {
            // Refused, with the reason, rather than mounted with a guessed
            // layout: a diskette whose geometry is invented reads plausible
            // rubbish.
            fmt::print("REFUSED {}: {}\n", a[2], why);
            return;
        }
        drive.insert(std::move(d));
        what = "show";
    }

    if (what != "show") {
        fmt::print("diskette [insert <image>|eject]\n");
        return;
    }

    if (!drive.hasMedium()) {
        fmt::print("diskette drive: EMPTY (a normal machine state - a disk IPL never needs one)\n");
        return;
    }
    const auto& g = drive.medium()->geometry();
    fmt::print("diskette drive: {}{}\n", drive.medium()->path(), drive.medium()->readOnly() ? " (read-only)" : "");
    fmt::print("  volume      {}   owner {}\n", g.volumeId(), g.ownerId());
    fmt::print("  media       {}-inch (label track {} x {} B)\n", g.isEightInch() ? "8" : "5.25",
               storage::DisketteGeometry::kLabelSectorsPerTrack, g.labelSectorBytes());
    fmt::print("  data area   {} x {} B per track\n", g.dataSectorsPerTrack(), g.dataSectorBytes());
    fmt::print("  geometry    {} cylinders x {} head(s) = {} bytes\n", g.cylinders(), g.heads(), g.totalBytes());
    fmt::print("  reads {} ({} record(s))   writes {} ({} record(s))\n", drive.readsIssued(), drive.recordsRead(),
               drive.writesIssued(), drive.recordsWritten());
}

void MonitorCli::disketteRead(const std::vector<std::string>& a)
{
    if (a.size() < 4) {
        fmt::print("dsktread <cyl> <head> <record> [count]\n");
        return;
    }
    int c = parseInt(a[1]);
    int h = parseInt(a[2]);
    int r = parseInt(a[3]);
    int count = a.size() > 4 ? parseInt(a[4]) : 1;

    auto& drive = m_.devices().diskette;
    // N comes from the MEDIUM, not from the caller: the record length code
    // has to match how the track is recorded, and the volume is what knows.
    int n = 0;
    if (drive.hasMedium()) {
        int bytes = drive.medium()->geometry().trackSectorBytes(c, h);
        n = bytes == 1024 ? 3 : bytes == 512 ? 2 : bytes == 256 ? 1 : 0;
    }

    constexpr int iob = 0x0600;
    auto& st = m_.state;
    for (int i = 0; i <= DisketteIoBlock::kOffRecordCount; i++) st.writeByte(iob + i, 0);

    st.writeByte(iob + DisketteIoBlock::kOffCommand, DisketteIoBlock::kCommandReadDataControlAm);
    st.writeByte(iob + DisketteIoBlock::kOffCommandModifier,
                 static_cast<uint8_t>(DisketteIoBlock::kModifierChrnx | DisketteIoBlock::kModifierReturnOnNotReady | n));
    st.writeAddr24(iob + DisketteIoBlock::kOffDataBuffer, kDiskReadBuffer);
    st.writeByte(iob + DisketteIoBlock::kOffCylinder, static_cast<uint8_t>(c));
    st.writeByte(iob + DisketteIoBlock::kOffHead, static_cast<uint8_t>(h));
    st.writeByte(iob + DisketteIoBlock::kOffRecord, static_cast<uint8_t>(r));
    st.writeByte(iob + DisketteIoBlock::kOffRecordLength, static_cast<uint8_t>(n));
    st.writeByte(iob + DisketteIoBlock::kOffRecordCount, static_cast<uint8_t>(count - 1));

    if (issueDeviceSvc(0x41, iob) < 0) return;
    fmt::print("completion  {:02X}\n", st.readByte(iob + Ecm::kOffCompletion));

    if (drive.hasLastRead()) {
        fmt::print("-- first 128 bytes read from C/H/R {}/{}/{} --\n", c, h, r);
        fmt::print("{}", hexDump(drive.lastRead().data(), std::min<int>(128, static_cast<int>(drive.lastRead().size())), 0));
    }
}

void MonitorCli::disketteWrite(const std::vector<std::string>& a)
{
    if (a.size() < 5) {
        fmt::print("dsktwrite <cyl> <head> <record> <hex>\n");
        return;
    }
    int c = parseInt(a[1]);
    int h = parseInt(a[2]);
    int r = parseInt(a[3]);
    std::vector<uint8_t> pattern;
    if (!parseHexBytes(a, 4, pattern) || pattern.empty()) {
        fmt::print("dsktwrite: no pattern\n");
        return;
    }

    auto& drive = m_.devices().diskette;
    if (!drive.hasMedium()) {
        fmt::print("the drive is empty\n");
        return;
    }
    int bytes = drive.medium()->geometry().trackSectorBytes(c, h);
    int n = bytes == 1024 ? 3 : bytes == 512 ? 2 : bytes == 256 ? 1 : 0;

    auto& st = m_.state;
    for (int i = 0; i < bytes; i++) st.writeByte(kDiskReadBuffer + i, pattern[static_cast<std::size_t>(i) % pattern.size()]);

    constexpr int iob = 0x0600;
    for (int i = 0; i <= DisketteIoBlock::kOffRecordCount; i++) st.writeByte(iob + i, 0);
    st.writeByte(iob + DisketteIoBlock::kOffCommand, DisketteIoBlock::kCommandWriteDataControlAm);
    st.writeByte(iob + DisketteIoBlock::kOffCommandModifier,
                 static_cast<uint8_t>(DisketteIoBlock::kModifierChrnx | DisketteIoBlock::kModifierReturnOnNotReady | n));
    st.writeAddr24(iob + DisketteIoBlock::kOffDataBuffer, kDiskReadBuffer);
    st.writeByte(iob + DisketteIoBlock::kOffCylinder, static_cast<uint8_t>(c));
    st.writeByte(iob + DisketteIoBlock::kOffHead, static_cast<uint8_t>(h));
    st.writeByte(iob + DisketteIoBlock::kOffRecord, static_cast<uint8_t>(r));
    st.writeByte(iob + DisketteIoBlock::kOffRecordLength, static_cast<uint8_t>(n));
    st.writeByte(iob + DisketteIoBlock::kOffRecordCount, 0);

    if (issueDeviceSvc(0x41, iob) < 0) return;
    fmt::print("completion  {:02X}\n", st.readByte(iob + Ecm::kOffCompletion));
}

void MonitorCli::tape(const std::vector<std::string>& a)
{
    auto& drive = m_.devices().tape;
    std::string what = a.size() > 1 ? toLower(a[1]) : "status";

    if (what == "load") {
        if (a.size() < 3) {
            fmt::print("tape load <dir> [ro]\n");
            return;
        }
        bool ro = a.size() > 3 && isReadOnlyFlag(a[3]);
        std::string why;
        auto back = FolderTapeBackend::open(a[2], ro, why);
        if (!back) {
            // Refused, with the reason, rather than mounted blind: the same
            // answer `diskette insert` gives a bad container.
            fmt::print("REFUSED {}: {}\n", a[2], why);
            return;
        }
        std::string path = back->path();
        std::string volumeId = back->volumeId();
        drive.load(std::move(back));
        fmt::print("mounted {}{}, volume {}\n", path, ro ? " (read-only)" : "",
                   volumeId.empty() ? std::string("(unlabeled)") : volumeId);
        return;
    }
    if (what == "unload") {
        bool ok;
        try {
            ok = drive.unload();
        } catch (const std::exception& e) {
            fmt::print("{}\n", e.what());
            return;
        }
        fmt::print("{}\n", ok ? "tape unloaded (writes flushed)" : "the drive is already empty");
        return;
    }
    if (what == "init") {
        if (a.size() < 4) {
            fmt::print("tape init <dir> <volid> [owner]\n");
            return;
        }
        std::string owner = a.size() > 4 ? a[4] : "S36REFEMU";
        std::string why;
        if (!FolderTapeBackend::init(a[2], a[3], owner, why)) {
            fmt::print("REFUSED {}: {}\n", a[2], why);
            return;
        }
        fmt::print("initialised tape folder {}: volume {}, owner {} (one VOL1 label, then a tape mark)\n", a[2],
                   storage::TapeLabel::normaliseVolumeId(a[3]), owner);
        return;
    }
    if (what == "vtoc") {
        tapeVtoc();
        return;
    }
    if (what == "files") {
        tapeFiles();
        return;
    }
    if (what == "status") {
        tapeStatus();
        return;
    }
    storage::ITapeBackend* medium = drive.medium();
    if (what == "position") {
        if (!medium) { fmt::print("REFUSED: the tape drive is empty\n"); return; }
        fmt::print("tape position: {}\n", medium->readPosition().toString());
        return;
    }
    if (what == "rewind") {
        if (!medium || !medium->loaded()) { fmt::print("REFUSED: the tape drive is not ready\n"); return; }
        medium->rewind();
        fmt::print("tape rewind: Ok; {}\n", medium->readPosition().toString());
        return;
    }
    if (what == "space") {
        if (a.size() != 4 || (toLower(a[2]) != "block" && toLower(a[2]) != "file")) {
            fmt::print("tape space <block|file> <count>\n");
            return;
        }
        if (!medium || !medium->loaded()) { fmt::print("REFUSED: the tape drive is not ready\n"); return; }
        const int count = parseInt(a[3]);
        int spaced = 0;
        const bool files = toLower(a[2]) == "file";
        TapeResult result = files ? medium->spaceFiles(count, spaced) : medium->spaceRecords(count, spaced);
        fmt::print("tape space {} {}: {}, moved {}; {}\n", files ? "file" : "block", count,
                   storage::tapeResultName(result), spaced, medium->readPosition().toString());
        return;
    }
    if (what == "mark") {
        if (a.size() > 3) { fmt::print("tape mark [count]\n"); return; }
        if (!medium || !medium->loaded()) { fmt::print("REFUSED: the tape drive is not ready\n"); return; }
        const int count = a.size() == 3 ? parseInt(a[2]) : 1;
        if (count < 1) { fmt::print("REFUSED: tape mark count must be positive\n"); return; }
        int written = 0;
        TapeResult result = TapeResult::Ok;
        for (; written < count; ++written) {
            result = medium->writeTapeMark();
            if (result != TapeResult::Ok) break;
        }
        fmt::print("tape mark {}: {}, wrote {}; {}\n", count, storage::tapeResultName(result), written,
                   medium->readPosition().toString());
        return;
    }
    fmt::print("tape [load <dir> [ro] | unload | init <dir> <volid> [owner] | status | position | rewind | "
               "space <block|file> <count> | mark [count] | vtoc | files]\n");
}

void MonitorCli::tapeStatus()
{
    auto& drive = m_.devices().tape;
    if (drive.medium() == nullptr) {
        fmt::print("tape drive: EMPTY (a normal machine state - a System/36 runs for long stretches with no tape in the "
                   "drive)\n");
    } else if (!drive.hasCartridge()) {
        // A cartridge is present but spun down: distinct from an empty drive.
        fmt::print("tape drive: {} loaded but not ready\n", drive.medium()->path());
    } else {
        const auto* med = drive.medium();
        fmt::print("tape drive: {}{}\n", med->path(), med->readOnly() ? " (read-only)" : "");
        fmt::print("  volume      {}\n", med->volumeId().empty() ? std::string("(unlabeled)") : med->volumeId());
        fmt::print("  state       loaded (spun up at load point)\n");
    }
    fmt::print("  reads {}   writes {}   control {}   unmapped {}\n", drive.readsIssued(), drive.writesIssued(),
               drive.controlOps(), drive.unmappedCommands());
}

std::unique_ptr<TapeManifest> MonitorCli::loadTapeManifest(std::string& reason)
{
    auto& drive = m_.devices().tape;
    reason.clear();
    if (drive.medium() == nullptr) {
        reason = "the tape drive is empty";
        return nullptr;
    }
    std::filesystem::path mpath = std::filesystem::path(drive.medium()->path()) / FolderTapeBackend::kManifestName;
    if (!std::filesystem::exists(mpath)) {
        reason = std::string("no ") + FolderTapeBackend::kManifestName + " in " + drive.medium()->path();
        return nullptr;
    }
    std::ifstream in(mpath, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    return TapeManifest::read(buf.str(), reason);
}

void MonitorCli::tapeVtoc()
{
    std::string reason;
    auto m = loadTapeManifest(reason);
    if (!m) {
        fmt::print("{}\n", reason);
        return;
    }
    const auto& v = m->volume;
    fmt::print("-- tape volume label (VOL1) --\n");
    fmt::print("  volume id       {}\n", v.volumeId.empty() ? std::string("(none)") : v.volumeId);
    std::string access = v.accessSecurity;
    bool blankAccess = access.find_first_not_of(' ') == std::string::npos;
    fmt::print("  access security {}\n", blankAccess ? std::string("(none)") : v.accessSecurity);
    fmt::print("  owner id        {}\n", v.ownerId.empty() ? std::string("(none)") : v.ownerId);
    fmt::print("  labeled         {}\n", v.labeled ? "True" : "False");

    bool any = false;
    for (const auto& f : m->files) {
        if (f.labels.empty()) continue;
        if (!any) {
            fmt::print("-- label groups --\n");
            any = true;
        }
        fmt::print("  file {} ({}):\n", f.sequence, f.blob);
        for (auto it = f.labels.begin(); it != f.labels.end(); ++it) {
            std::string key = it.key();
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::toupper(ch); });
            fmt::print("    {:<4} {}\n", key, describeTapeLabel(it.value()));
        }
    }
    if (!any) fmt::print("  (no HDR/EOF label groups - a freshly initialised tape carries only VOL1)\n");
}

void MonitorCli::tapeFiles()
{
    std::string reason;
    auto m = loadTapeManifest(reason);
    if (!m) {
        fmt::print("{}\n", reason);
        return;
    }
    if (m->files.empty()) {
        fmt::print("the tape has no files\n");
        return;
    }
    fmt::print("-- tape files ({}) --\n", m->files.size());
    fmt::print("  seq  kind    blob         fmt  blocklen  blocks     bytes\n");
    for (const auto& f : m->files) {
        std::vector<int> lengths = f.resolveBlockLengths();
        long long total = 0;
        for (int n : lengths) total += n;
        std::string blocklen = f.hasBlockLengths ? std::string("var") : std::to_string(f.blockLength);
        fmt::print("  {:>3}  {:<6}  {:<10}   {:<3}  {:>8}  {:>6}  {:>8}\n", f.sequence, f.kind, f.blob, f.recordFormat, blocklen,
                   f.blockCount, total);
    }
}

// The tape backend round-trip: init, open, read the label, write and read a
// data file, space by record, flush and reopen.
void MonitorCli::tapeTest(const std::vector<std::string>& a)
{
    bool temp = a.size() <= 1;
    std::string dir = temp ? temporaryFolder("s36tape-") : a[1];
    Score sc;
    auto sameBytes = [](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) { return x == y; };
    try {
        std::string reason;
        bool init = FolderTapeBackend::init(dir, "TAP01", "S36REFEMU", reason);
        report("init writes a labeled tape folder", init, sc);
        if (init) {
            report("manifest.json present", std::filesystem::exists(std::filesystem::path(dir) / "manifest.json"), sc);
            report("0001.dat blob present", std::filesystem::exists(std::filesystem::path(dir) / "0001.dat"), sc);

            auto t = FolderTapeBackend::open(dir, false, reason);
            report("open indexes the folder", t != nullptr, sc);
            if (t) {
                t->load();
                report("VOL1 serial decodes as TAP01", t->volumeId() == "TAP01", sc);

                std::vector<uint8_t> blk;
                TapeResult r;
                int spaced = 0;

                t->rewind();
                r = t->readBlock(blk);
                report("first read returns a block (VOL1)", r == TapeResult::Ok && blk.size() == 80, sc);
                report("...first four bytes are EBCDIC VOL1",
                       blk.size() >= 4 && blk[0] == 0xE5 && blk[1] == 0xD6 && blk[2] == 0xD3 && blk[3] == 0xF1, sc);
                r = t->readBlock(blk);
                report("second read returns a tape mark", r == TapeResult::TapeMark, sc);
                r = t->readBlock(blk);
                report("third read returns the empty-volume tape mark", r == TapeResult::TapeMark, sc);
                r = t->readBlock(blk);
                report("fourth read returns end of data", r == TapeResult::EndOfData, sc);

                const std::vector<std::vector<uint8_t>> data = {
                    {'B', 'L', 'O', 'C', 'K', '-', 'O', 'N', 'E'},
                    {'S', 'E', 'C', 'O', 'N', 'D', '-', 'B', 'L', 'O', 'C', 'K', '-', 'l', 'o', 'n', 'g', 'e', 'r'},
                    {'3'},
                };

                t->rewind();
                r = t->spaceFiles(1, spaced);
                report("space forward one file lands past VOL1", r == TapeResult::Ok && spaced == 1, sc);
                auto pos = t->readPosition();
                report("...READ POSITION reports file 1 block 0", pos.fileNumber == 1 && pos.blockNumber == 0, sc);

                bool wrote = true;
                for (const auto& d : data)
                    if (t->writeBlock(d.data(), 0, static_cast<int>(d.size())) != TapeResult::Ok) wrote = false;
                if (t->writeTapeMark() != TapeResult::Ok) wrote = false;
                report("write three blocks and a tape mark", wrote, sc);

                t->rewind();
                t->spaceFiles(1, spaced);
                bool rb = true;
                for (const auto& d : data)
                    if (t->readBlock(blk) != TapeResult::Ok || !sameBytes(blk, d)) rb = false;
                report("data blocks read back byte-for-byte", rb, sc);
                r = t->readBlock(blk);
                report("...terminated by the tape mark", r == TapeResult::TapeMark, sc);

                t->rewind();
                t->spaceFiles(1, spaced);
                r = t->spaceRecords(2, spaced);
                report("space forward two records", r == TapeResult::Ok && spaced == 2, sc);
                r = t->spaceRecords(5, spaced);
                report("space forward stops on the tape mark", r == TapeResult::TapeMark && spaced == 1, sc);
                r = t->spaceRecords(-3, spaced);
                report("space back three records to start of file", spaced == 3, sc);

                t->unload();
                t.reset();
                auto t2 = FolderTapeBackend::open(dir, true, reason);
                report("reopen after flush", t2 != nullptr, sc);
                if (t2) {
                    t2->load();
                    t2->rewind();
                    t2->spaceFiles(1, spaced);
                    bool persisted = true;
                    for (const auto& d : data)
                        if (t2->readBlock(blk) != TapeResult::Ok || !sameBytes(blk, d)) persisted = false;
                    report("written data survived the round-trip to disk", persisted, sc);
                }
            } else {
                fmt::print("  reason: {}\n", reason);
            }
        } else {
            fmt::print("  reason: {}\n", reason);
        }
    } catch (const std::exception& e) {
        fmt::print("  EXCEPTION: {}\n", e.what());
        sc.fail++;
    }
    fmt::print("\n");
    fmt::print("  tape backend: {} passed, {} failed\n", sc.pass, sc.fail);
    if (temp) removeTree(dir);
}

// SVC 46 through the control processor: read the label, hit the tape mark,
// write and read back a data file, refuse an invalid command, answer
// not-ready on an empty drive.
void MonitorCli::tapeSvc(const std::vector<std::string>& a)
{
    bool temp = a.size() <= 1;
    std::string dir = temp ? temporaryFolder("s36tapesvc-") : a[1];

    constexpr int iob = 0x0600;
    constexpr int buffer = 0x20000;
    Score sc;
    auto& st = m_.state;

    auto issue = [&](int command, int modifier, int length) -> int {
        for (int i = 0; i < 64; i++) st.writeByte(iob + i, 0);
        st.writeByte(iob + NuTaIob::kOffCommand, static_cast<uint8_t>(command));
        st.writeByte(iob + NuTaIob::kOffModifier, static_cast<uint8_t>(modifier));
        st.writeHalf(iob + NuTaIob::kOffBlockLength, static_cast<uint16_t>(length));
        st.writeByte(iob + NuTaIob::kOffDataFlag, 1);
        st.writeAddr24(iob + NuTaIob::kOffDataBuffer, buffer);
        if (issueDeviceSvc(0x46, iob) < 0) return -1;
        return st.readByte(iob + NuTaIob::kOffCompletion);
    };

    try {
        std::string reason;
        bool proceed = true;
        if (!std::filesystem::exists(std::filesystem::path(dir) / "manifest.json")) {
            bool init = FolderTapeBackend::init(dir, "TAP01", "S36REFEMU", reason);
            report("init a labeled tape folder", init, sc);
            if (!init) {
                fmt::print("  reason: {}\n", reason);
                proceed = false;
            }
        }
        if (proceed) {
            auto opened = FolderTapeBackend::open(dir, false, reason);
            report("open the tape folder", opened != nullptr, sc);
            if (!opened) {
                fmt::print("  reason: {}\n", reason);
            } else {
                storage::ITapeBackend* back = opened.get();
                auto& drive = m_.devices().tape;
                drive.load(std::move(opened));
                report("mount the cartridge on the drive", drive.hasCartridge(), sc);

                // Add one standard-labeled synthetic dataset through the
                // backend.  This keeps the SVC exercise independent of the
                // private system volume while testing command 16's actual
                // HDR1 search and positioning contract.
                auto requireTape = [](TapeResult got, TapeResult wanted, const char* operation) {
                    if (got != wanted)
                        throw std::runtime_error(fmt::format("{}: expected {}, got {}", operation,
                                                             storage::tapeResultName(wanted),
                                                             storage::tapeResultName(got)));
                };
                std::vector<uint8_t> vol1;
                requireTape(back->readBlock(vol1), TapeResult::Ok, "read initial VOL1");
                std::vector<uint8_t> hdr1(80, 0x40), hdr2(80, 0x40);
                const std::vector<uint8_t> data = {'D', 'A', 'T', 'A'};
                const uint8_t hdr1Id[] = {0xC8, 0xC4, 0xD9, 0xF1};
                const uint8_t dataSetId[] = {0xC4, 0xC9, 0xE2, 0xC3, 0xC6, 0xC9, 0xD3, 0xC5}; // DISCFILE
                const uint8_t hdr2Id[] = {0xC8, 0xC4, 0xD9, 0xF2};
                std::copy(std::begin(hdr1Id), std::end(hdr1Id), hdr1.begin());
                std::copy(std::begin(dataSetId), std::end(dataSetId), hdr1.begin() + 4);
                std::copy(std::begin(hdr2Id), std::end(hdr2Id), hdr2.begin());
                requireTape(back->writeBlock(hdr1.data(), 0, static_cast<int>(hdr1.size())), TapeResult::Ok, "write HDR1");
                requireTape(back->writeBlock(hdr2.data(), 0, static_cast<int>(hdr2.size())), TapeResult::Ok, "write HDR2");
                requireTape(back->writeTapeMark(), TapeResult::Ok, "close header file");
                requireTape(back->writeBlock(data.data(), 0, static_cast<int>(data.size())), TapeResult::Ok, "write data");
                requireTape(back->writeTapeMark(), TapeResult::Ok, "close data file");
                requireTape(back->writeTapeMark(), TapeResult::Ok, "write logical end mark");

                back->rewind();
                int c = issue(NuTaIob::kCommandReadData, 0, 0x100);
                if (c < 0) {
                    fmt::print("  not booted - the CSP has no request block\n");
                } else {
                    c = issue(NuTaIob::kCommandActivate, 0, 0x370);
                    report("native command 01 activates the loaded tape", (c & 0x0F) == NuTaIob::kCompletionOk, sc);
                    storage::TapePosition beforeSession = back->readPosition();
                    c = issue(NuTaIob::kCommandSetSession, 3, 0x370);
                    report("native command 02/03 establishes its session without movement",
                           (c & 0x0F) == NuTaIob::kCompletionOk &&
                               beforeSession.toString() == back->readPosition().toString(),
                           sc);
                    back->spaceFiles(1, c);
                    c = issue(NuTaIob::kCommandReadVolumeLabels, 3, 0x370);
                    report("native command 13 rewinds and reads VOL1",
                           (c & 0x0F) == NuTaIob::kCompletionOk && back->readPosition().fileNumber == 0 &&
                               back->readPosition().blockNumber == 1 && st.readByte(buffer) == 0xE5,
                           sc);

                    report("SVC 46 read posts complete (iob+0x06 bit 0x40)",
                           (c & Ecm::kComplete) != 0 && (c & 0x0F) == NuTaIob::kCompletionOk, sc);
                    report("...the block reached guest storage (LastRead 80 bytes)",
                           drive.hasLastRead() && drive.lastRead().size() == 80, sc);
                    report("...it is the EBCDIC VOL1 label in the guest buffer",
                           st.readByte(buffer) == 0xE5 && st.readByte(buffer + 1) == 0xD6 && st.readByte(buffer + 2) == 0xD3 &&
                               st.readByte(buffer + 3) == 0xF1,
                           sc);

                    for (int i = 0; i < 17; i++)
                        st.writeByte(buffer + i, i < static_cast<int>(std::size(dataSetId)) ? dataSetId[i] : 0x40);
                    c = issue(NuTaIob::kCommandFindDataSet, 3, 0x1E0);
                    report("native command 16 finds HDR1 and positions at the data file",
                           (c & 0x0F) == NuTaIob::kCompletionOk && back->readPosition().fileNumber == 1 &&
                               back->readPosition().blockNumber == 0 &&
                               st.readHalf(iob + NuTaIob::kOffReturnedLength) == 160 &&
                               st.readByte(buffer) == 0xC8 && st.readByte(buffer + 3) == 0xF1,
                           sc);
                    c = issue(NuTaIob::kCommandReadData, 0, 0x100);
                    report("...the next guest read returns the selected dataset's first block",
                           (c & 0x0F) == NuTaIob::kCompletionOk && st.readByte(buffer) == 'D' &&
                               st.readByte(buffer + 3) == 'A',
                           sc);

                    c = issue(NuTaIob::kCommandReadData, 0, 0x100);
                    report("SVC 46 read on the tape mark posts non-success",
                           (c & Ecm::kComplete) != 0 && (c & 0x0F) != NuTaIob::kCompletionOk, sc);

                    back->rewind();
                    int spaced = 0;
                    back->spaceFiles(1, spaced);
                    const std::string payloadText = "STAGE2-SVC46-TAPE";
                    std::vector<uint8_t> payload(payloadText.begin(), payloadText.end());
                    for (std::size_t i = 0; i < payload.size(); i++) st.writeByte(buffer + static_cast<int>(i), payload[i]);
                    c = issue(NuTaIob::kCommandWriteData, 0, static_cast<int>(payload.size()));
                    report("SVC 46 write posts complete", (c & Ecm::kComplete) != 0 && (c & 0x0F) == NuTaIob::kCompletionOk, sc);
                    back->writeTapeMark();

                    back->rewind();
                    back->spaceFiles(1, spaced);
                    for (std::size_t i = 0; i < payload.size(); i++) st.writeByte(buffer + static_cast<int>(i), 0);
                    c = issue(NuTaIob::kCommandReadData, 0, 0x100);
                    bool same = (c & Ecm::kComplete) != 0 && (c & 0x0F) == NuTaIob::kCompletionOk;
                    for (std::size_t i = 0; i < payload.size() && same; i++)
                        if (st.readByte(buffer + static_cast<int>(i)) != payload[i]) same = false;
                    report("SVC 46 read back returns the written record verbatim", same, sc);

                    back->rewind();
                    back->spaceFiles(1, spaced);
                    for (std::size_t i = 0; i < payload.size(); i++) st.writeByte(buffer + static_cast<int>(i), 0xA5);
                    storage::TapePosition beforeLong = back->readPosition();
                    c = issue(NuTaIob::kCommandReadData, 0, static_cast<int>(payload.size() - 1));
                    storage::TapePosition afterLong = back->readPosition();
                    report("an overlength block is rejected without changing guest data",
                           (c & 0x0F) == NuTaIob::kCompletionError && st.readByte(buffer) == 0xA5, sc);
                    report("...and remains positioned for a retry", beforeLong.toString() == afterLong.toString(), sc);

                    c = issue(0x32, 0, 0x100);
                    report("SVC 46 with an invalid command is refused", (c & 0x0F) == NuTaIob::kCompletionError, sc);

                    drive.unload();
                    c = issue(NuTaIob::kCommandReadData, 0, 0x100);
                    report("SVC 46 read on an empty drive answers not-ready",
                           (c & Ecm::kComplete) != 0 &&
                               st.readByte(iob + devices::DeviceSet::kOffDeviceStatus) == devices::DeviceSet::kNotInThisConfiguration,
                           sc);
                }
            }
        }
    } catch (const std::exception& e) {
        fmt::print("  EXCEPTION: {}\n", e.what());
        sc.fail++;
    }
    fmt::print("\n");
    fmt::print("  tape SVC: {} passed, {} failed\n", sc.pass, sc.fail);
    if (temp) removeTree(dir);
}

void MonitorCli::saveMain(const std::vector<std::string>& a)
{
    if (a.size() != 2) {
        fmt::print("usage: savemain <file>\n");
        return;
    }
    std::ofstream out(a[1], std::ios::binary | std::ios::trunc);
    if (!out) throw MonitorError(fmt::format("Could not find a part of the path \"{}\".", a[1]));
    out.write(reinterpret_cast<const char*>(m_.state.raw()), m_.state.backingBytes());
    fmt::print("savemain: wrote {} byte(s) of guest main storage to {}\n", m_.state.backingBytes(), a[1]);
}

}  // namespace sim36::monitor
