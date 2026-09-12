#include "Monitor/MachineSnapshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <stdexcept>

#include <fmt/format.h>
#include <zlib.h>

#include "Machine/Machine.h"

namespace sim36::monitor {

namespace fs = std::filesystem;
using configuration::EmulatorConfig;
using configuration::StationConfig;
using configuration::TapeConfig;
using processors::controlstorage::ActionControlElementQueue;
using processors::controlstorage::As36ControlStorageProcessor;

namespace {

const uint8_t kMagic[8] = {'S', '3', '6', 'C', 'K', 'P', 'T', 0};
constexpr int kCopyBufferBytes = 1024 * 1024;
constexpr long long kMaxMediaBytes = 1024LL * 1024 * 1024 * 4;

std::string randomHex(int digits)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::string s;
    for (int i = 0; i < digits; i++) s += "0123456789abcdef"[gen() & 15];
    return s;
}

[[noreturn]] void invalid(const std::string& what) { throw std::runtime_error(what); }

int boundedInt(int n, int lo, int hi, const std::string& what)
{
    if (n < lo || n > hi) invalid(what + " is out of range");
    return n;
}

long long boundedLong(long long n, long long lo, long long hi, const std::string& what)
{
    if (n < lo || n > hi) invalid(what + " is out of range");
    return n;
}

}  // namespace

// A gzip output stream with the .NET BinaryWriter's encodings: little-endian
// integers, one-byte booleans, strings as UTF-8 with a 7-bit-encoded byte
// length, preceded here by a presence flag for a null string.
class MachineSnapshot::Writer {
public:
    explicit Writer(const std::string& path) : file_(gzopen(path.c_str(), "wb"))
    {
        if (file_ == nullptr) invalid("cannot create " + path);
    }
    ~Writer()
    {
        if (file_ != nullptr) gzclose(file_);
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void close()
    {
        if (file_ != nullptr && gzclose(file_) != Z_OK) {
            file_ = nullptr;
            invalid("cannot finish the checkpoint stream");
        }
        file_ = nullptr;
    }

    void raw(const void* data, std::size_t length)
    {
        const char* p = static_cast<const char*>(data);
        while (length > 0) {
            unsigned chunk = static_cast<unsigned>(std::min<std::size_t>(length, 1u << 30));
            if (gzwrite(file_, p, chunk) != static_cast<int>(chunk)) invalid("checkpoint write failed");
            p += chunk;
            length -= chunk;
        }
    }
    void u8(uint8_t v) { raw(&v, 1); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v)
    {
        uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        raw(b, 2);
    }
    void i32(int32_t v)
    {
        uint32_t u = static_cast<uint32_t>(v);
        uint8_t b[4] = {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16),
                        static_cast<uint8_t>(u >> 24)};
        raw(b, 4);
    }
    void i64(int64_t v)
    {
        uint64_t u = static_cast<uint64_t>(v);
        uint8_t b[8];
        for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(u >> (8 * i));
        raw(b, 8);
    }
    void u64(uint64_t v) { i64(static_cast<int64_t>(v)); }
    // BinaryWriter.Write(string): 7-bit-encoded UTF-8 byte count, then the bytes.
    void dotnetString(const std::string& s)
    {
        std::size_t n = s.size();
        do {
            uint8_t b = static_cast<uint8_t>(n & 0x7F);
            n >>= 7;
            if (n != 0) b |= 0x80;
            u8(b);
        } while (n != 0);
        raw(s.data(), s.size());
    }
    // The reference writes a presence flag before every string so a null
    // string round-trips; SIM/36 has no null strings, so present is always
    // true.
    void string(const std::string& s)
    {
        boolean(true);
        dotnetString(s);
    }
    void ints(const std::vector<int>& v)
    {
        i32(static_cast<int32_t>(v.size()));
        for (int x : v) i32(x);
    }
    void strings(const std::vector<std::string>& v)
    {
        i32(static_cast<int32_t>(v.size()));
        for (const auto& s : v) string(s);
    }
    void bytes(const std::vector<uint8_t>& b, bool present = true)
    {
        boolean(present);
        if (!present) return;
        i32(static_cast<int32_t>(b.size()));
        raw(b.data(), b.size());
    }
    void file(const std::string& path)
    {
        bool present = !path.empty() && fs::exists(path) && fs::is_regular_file(path);
        boolean(present);
        if (!present) return;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) invalid("cannot read " + path);
        long long length = static_cast<long long>(fs::file_size(path));
        i64(length);
        copyFrom(f, length, path);
        std::fclose(f);
    }
    void directory(const std::string& path)
    {
        bool present = !path.empty() && fs::exists(path) && fs::is_directory(path);
        boolean(present);
        if (!present) return;
        fs::path root = fs::absolute(path);
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root))
            if (entry.is_regular_file()) files.push_back(fs::absolute(entry.path()));
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.string() < b.string(); });
        i32(static_cast<int32_t>(files.size()));
        for (const auto& f : files) {
            std::string rel = fs::relative(f, root).generic_string();
            string(rel);
            std::FILE* in = std::fopen(f.string().c_str(), "rb");
            if (in == nullptr) invalid("cannot read " + f.string());
            long long length = static_cast<long long>(fs::file_size(f));
            i64(length);
            copyFrom(in, length, f.string());
            std::fclose(in);
        }
    }

private:
    void copyFrom(std::FILE* in, long long bytes, const std::string& what)
    {
        std::vector<char> buf(kCopyBufferBytes);
        while (bytes > 0) {
            std::size_t want = static_cast<std::size_t>(std::min<long long>(bytes, kCopyBufferBytes));
            std::size_t n = std::fread(buf.data(), 1, want, in);
            if (n == 0) invalid("short read of " + what);
            raw(buf.data(), n);
            bytes -= static_cast<long long>(n);
        }
    }
    gzFile file_;
};

class MachineSnapshot::Reader {
public:
    explicit Reader(const std::string& path) : file_(gzopen(path.c_str(), "rb"))
    {
        if (file_ == nullptr) invalid("cannot open " + path);
    }
    ~Reader()
    {
        if (file_ != nullptr) gzclose(file_);
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    void raw(void* data, std::size_t length, const std::string& what)
    {
        char* p = static_cast<char*>(data);
        while (length > 0) {
            unsigned chunk = static_cast<unsigned>(std::min<std::size_t>(length, 1u << 30));
            int n = gzread(file_, p, chunk);
            if (n <= 0) invalid("short " + what);
            p += n;
            length -= static_cast<std::size_t>(n);
        }
    }
    bool atEnd()
    {
        char c;
        return gzread(file_, &c, 1) <= 0;
    }
    uint8_t u8(const std::string& what = "checkpoint byte")
    {
        uint8_t v;
        raw(&v, 1, what);
        return v;
    }
    bool boolean() { return u8("checkpoint flag") != 0; }
    uint16_t u16()
    {
        uint8_t b[2];
        raw(b, 2, "checkpoint halfword");
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }
    int32_t i32()
    {
        uint8_t b[4];
        raw(b, 4, "checkpoint integer");
        return static_cast<int32_t>(static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                                    (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24));
    }
    int64_t i64()
    {
        uint8_t b[8];
        raw(b, 8, "checkpoint long");
        uint64_t u = 0;
        for (int i = 0; i < 8; i++) u |= static_cast<uint64_t>(b[i]) << (8 * i);
        return static_cast<int64_t>(u);
    }
    uint64_t u64() { return static_cast<uint64_t>(i64()); }
    std::string dotnetString()
    {
        std::size_t n = 0;
        int shift = 0;
        for (;;) {
            uint8_t b = u8("checkpoint string length");
            n |= static_cast<std::size_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 35) invalid("checkpoint string length is malformed");
        }
        if (n > 1024 * 1024) invalid("checkpoint string is too large");
        std::string s(n, '\0');
        if (n != 0) raw(&s[0], n, "checkpoint string");
        return s;
    }
    std::string string()
    {
        if (!boolean()) return std::string();
        return dotnetString();
    }
    std::vector<int> ints(const std::string& what)
    {
        int n = boundedInt(i32(), 0, 1024 * 1024, what + " count");
        std::vector<int> v(static_cast<std::size_t>(n));
        for (int i = 0; i < n; i++) v[static_cast<std::size_t>(i)] = i32();
        return v;
    }
    std::vector<std::string> strings(const std::string& what)
    {
        int n = boundedInt(i32(), 0, 1024 * 1024, what + " count");
        std::vector<std::string> v(static_cast<std::size_t>(n));
        for (int i = 0; i < n; i++) v[static_cast<std::size_t>(i)] = string();
        return v;
    }
    bool bytes(std::vector<uint8_t>& out, int max, const std::string& what)
    {
        if (!boolean()) return false;
        int n = boundedInt(i32(), 0, max, what + " length");
        out.resize(static_cast<std::size_t>(n));
        if (n != 0) raw(out.data(), out.size(), what);
        return true;
    }
    std::string file(const std::string& directory, const std::string& name)
    {
        if (!boolean()) return std::string();
        long long length = boundedLong(i64(), 0, kMaxMediaBytes, name + " length");
        fs::path path = fs::path(directory) / name;
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f == nullptr) invalid("cannot create " + path.string());
        copyTo(f, length);
        std::fclose(f);
        return path.string();
    }
    std::string directory(const std::string& directory, const std::string& name)
    {
        if (!boolean()) return std::string();
        fs::path root = fs::path(directory) / name;
        fs::create_directories(root);
        int count = boundedInt(i32(), 0, 100000, "tape file count");
        std::string prefix = fs::absolute(root).lexically_normal().string();
        if (prefix.empty() || prefix.back() != static_cast<char>(fs::path::preferred_separator))
            prefix += static_cast<char>(fs::path::preferred_separator);
        for (int i = 0; i < count; i++) {
            std::string rel = string();
            fs::path target = fs::absolute(root / rel).lexically_normal();
            if (target.string().compare(0, prefix.size(), prefix) != 0) invalid("unsafe tape member path");
            long long length = boundedLong(i64(), 0, kMaxMediaBytes, "tape member length");
            fs::create_directories(target.parent_path());
            std::FILE* f = std::fopen(target.string().c_str(), "wb");
            if (f == nullptr) invalid("cannot create " + target.string());
            copyTo(f, length);
            std::fclose(f);
        }
        return root.string();
    }

private:
    void copyTo(std::FILE* out, long long bytes)
    {
        std::vector<char> buf(kCopyBufferBytes);
        while (bytes > 0) {
            std::size_t want = static_cast<std::size_t>(std::min<long long>(bytes, kCopyBufferBytes));
            int n = gzread(file_, buf.data(), static_cast<unsigned>(want));
            if (n <= 0) invalid("short checkpoint media");
            if (std::fwrite(buf.data(), 1, static_cast<std::size_t>(n), out) != static_cast<std::size_t>(n))
                invalid("cannot write checkpoint media");
            bytes -= n;
        }
    }
    gzFile file_;
};

void MachineSnapshot::save(const std::string& path, const EmulatorConfig& definition, machine::Machine* machine,
                           uint32_t pendingTrace)
{
    if (definition.volumePath.empty() || !fs::exists(definition.volumePath))
        throw std::runtime_error("snapshot requires the configured fixed disk");
    if (!definition.diskettePath.empty() && !fs::exists(definition.diskettePath) &&
        (machine == nullptr || !machine->devices().diskette.hasMedium()))
        throw std::runtime_error("configured diskette is absent");
    if (machine != nullptr) {
        if (machine->scheduler.pending() != 0)
            throw std::runtime_error("snapshot save requires an empty scheduler queue; run to a device boundary first");
        if (machine->devices().pendingScreenSaveCount() != 0)
            throw std::runtime_error("snapshot save cannot cross an outstanding terminal Save Screen round trip");
        if (machine->devices().pendingPutWithInviteCount() != 0)
            throw std::runtime_error("snapshot save cannot cross an outstanding terminal PUT-with-invite round trip");
        for (auto& s : machine->stations())
            // The operator console is an in-process attachment that is
            // reconstructed with every machine, not a live TN5250
            // transport.  Only external station sessions make a native
            // checkpoint non-portable.
            if (!s->isConsole() && (s->attached() || s->pendingInput() != 0 || s->attentionPending()))
                throw std::runtime_error("snapshot save cannot capture a live TN5250 transport; detach station " +
                                         s->id() + " first");
        for (auto& p : machine->printers())
            if (p->attached())
                throw std::runtime_error("snapshot save cannot capture a live TN5250 printer transport; detach it first");
    }

    fs::path full = fs::absolute(path);
    fs::path dir = full.parent_path();
    if (!dir.empty()) fs::create_directories(dir);
    std::string temp = full.string() + ".tmp-" + randomHex(32);
    try {
        {
            Writer w(temp);
            w.raw(kMagic, sizeof kMagic);
            w.i32(kVersion);
            writeConfig(w, definition);
            w.boolean(machine != nullptr);
            w.i32(static_cast<int32_t>(machine == nullptr ? pendingTrace : machine->trace.flags));
            w.boolean(machine == nullptr ? definition.volumeReadOnly : machine->diskBackend().readOnly());
            w.boolean(machine != nullptr && machine->devices().diskette.hasMedium()
                          ? machine->devices().diskette.medium()->readOnly()
                          : definition.disketteReadOnly);
            w.boolean(machine != nullptr && machine->devices().tape.medium() != nullptr
                          ? machine->devices().tape.medium()->readOnly()
                          : definition.tape == nullptr ? false : definition.tape->readOnly);
            w.file(definition.volumePath);
            w.file(machine != nullptr && machine->devices().diskette.hasMedium()
                       ? machine->devices().diskette.medium()->path()
                       : definition.diskettePath);
            std::string tape = machine != nullptr && machine->devices().tape.medium() != nullptr
                                   ? machine->devices().tape.medium()->path()
                                   : definition.tape == nullptr ? std::string() : definition.tape->folderPath;
            w.directory(tape);
            if (machine != nullptr) writeRuntime(w, *machine);
            w.close();
        }
        fs::rename(temp, full);
    } catch (...) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw;
    }
}

MachineSnapshot::Loaded MachineSnapshot::load(const std::string& path)
{
    fs::path full = fs::absolute(path);
    std::string media = full.string() + ".media-" + randomHex(32);
    fs::create_directories(media);
    try {
        Reader r(full.string());
        uint8_t magic[sizeof kMagic];
        r.raw(magic, sizeof magic, "header");
        if (std::memcmp(magic, kMagic, sizeof kMagic) != 0) invalid("not a SIM/36 checkpoint");
        int version = r.i32();
        if (version != kVersion) invalid("unsupported checkpoint version " + std::to_string(version));
        Loaded answer;
        answer.config = readConfig(r);
        answer.powered = r.boolean();
        answer.trace = static_cast<uint32_t>(r.i32());
        answer.config.volumeReadOnly = r.boolean();
        answer.config.disketteReadOnly = r.boolean();
        bool tapeReadOnly = r.boolean();
        answer.config.volumePath = r.file(media, "disk0.img");
        answer.config.diskettePath = r.file(media, "diskette0.img");
        std::string tape = r.directory(media, "tape0");
        if (answer.config.tape == nullptr && !tape.empty()) answer.config.tape = std::make_unique<TapeConfig>();
        if (answer.config.tape != nullptr) {
            answer.config.tape->folderPath = tape;
            answer.config.tape->readOnly = tapeReadOnly;
        }
        if (answer.powered) answer.runtime = std::make_unique<RuntimeState>(readRuntime(r));
        // The stream must end exactly here: the trailer is checked by the
        // decompressor before the record is accepted.
        if (!r.atEnd()) invalid("trailing checkpoint data");
        answer.mediaDirectory = media;
        return answer;
    } catch (...) {
        std::error_code ec;
        fs::remove_all(media, ec);
        throw;
    }
}

std::vector<uint8_t> MachineSnapshot::writeDiagnosticRuntime(machine::Machine& machine)
{
    // The diagnostic record is the plain (uncompressed) BinaryWriter stream:
    // write it through the same encoder into a temporary file and read it
    // back, so that one encoder serves both paths.
    std::string temp = (fs::temp_directory_path() / ("sim36-runtime-" + randomHex(16))).string();
    std::vector<uint8_t> out;
    {
        Writer w(temp);
        w.i32(kVersion);
        writeRuntime(w, machine);
        w.close();
    }
    // Inflate it back to the raw record.
    gzFile f = gzopen(temp.c_str(), "rb");
    if (f != nullptr) {
        std::vector<char> buf(kCopyBufferBytes);
        for (;;) {
            int n = gzread(f, buf.data(), static_cast<unsigned>(buf.size()));
            if (n <= 0) break;
            out.insert(out.end(), buf.begin(), buf.begin() + n);
        }
        gzclose(f);
    }
    std::error_code ec;
    fs::remove(temp, ec);
    return out;
}

void MachineSnapshot::writeConfig(Writer& w, const EmulatorConfig& c)
{
    w.string(c.model);
    w.i32(c.mainStorageKb);
    w.i32(c.taskWorkAreaSectors);
    w.string(c.hostModel);
    w.i32(c.hostProcessorFeature);
    w.string(c.hostProcessorModel);
    w.string(c.iplType);
    w.string(c.iplSourceName);
    w.boolean(c.listenerAutoSignOn);
    w.boolean(c.consoleSignOnUseRouter);
    w.boolean(c.consoleSignOnStatement);
    w.boolean(c.consoleSignOnRequest);
    w.i32(c.consoleSignOnRouterKey);
    w.boolean(c.wsInteractive);
    w.boolean(c.volumeReadOnly);
    w.boolean(c.disketteReadOnly);
    w.boolean(c.tape != nullptr);
    if (c.tape != nullptr) w.boolean(c.tape->readOnly);
    w.i32(static_cast<int32_t>(c.stations.size()));
    for (const auto& s : c.stations) {
        w.i32(s.port);
        w.i32(s.address);
        w.string(s.role);
        w.string(s.deviceCode);
        w.boolean(s.deviceCodeGiven);
        w.string(s.listenHost);
        w.i32(s.listenPort);
        w.boolean(s.signOnAtIpl);
    }
}

EmulatorConfig MachineSnapshot::readConfig(Reader& r)
{
    EmulatorConfig c;
    c.model = r.string();
    c.mainStorageKb = r.i32();
    c.taskWorkAreaSectors = r.i32();
    c.hostModel = r.string();
    c.hostProcessorFeature = r.i32();
    c.hostProcessorModel = r.string();
    c.iplType = r.string();
    c.iplSourceName = r.string();
    c.listenerAutoSignOn = r.boolean();
    c.consoleSignOnUseRouter = r.boolean();
    c.consoleSignOnStatement = r.boolean();
    c.consoleSignOnRequest = r.boolean();
    c.consoleSignOnRouterKey = r.i32();
    c.wsInteractive = r.boolean();
    c.volumeReadOnly = r.boolean();
    c.disketteReadOnly = r.boolean();
    if (r.boolean()) {
        c.tape = std::make_unique<TapeConfig>();
        c.tape->readOnly = r.boolean();
    }
    int count = boundedInt(r.i32(), 0, 1024, "station count");
    for (int i = 0; i < count; i++) {
        StationConfig s;
        s.port = r.i32();
        s.address = r.i32();
        s.role = r.string();
        s.deviceCode = r.string();
        s.deviceCodeGiven = r.boolean();
        s.listenHost = r.string();
        s.listenPort = r.i32();
        s.signOnAtIpl = r.boolean();
        c.stations.push_back(s);
    }
    return c;
}

void MachineSnapshot::writeRuntime(Writer& w, machine::Machine& m)
{
    auto& st = m.state;
    std::vector<uint8_t> main(st.raw(), st.raw() + st.backingBytes());
    w.bytes(main);
    w.i32(128);
    for (uint16_t v : st.atr) w.u16(v);
    w.i64(st.cycles);
    w.i32(st.m36Src());
    const auto& q = st.msp;
    w.u16(q.iar);
    w.u16(q.arr);
    w.u16(q.xr1);
    w.u16(q.xr2);
    for (uint16_t v : q.wr) w.u16(v);
    w.u8(q.psr());
    w.u8(q.pactDir);
    w.u8(q.pactXr1);
    w.u8(q.pactXr2);
    w.u8(q.pactIar);
    w.u8(q.pactReg);
    w.u8(q.pactAtr);
    w.u8(q.pactCsp);
    w.u8(q.pmr());
    w.u8(q.cmr);
    w.boolean(m.msp().stopped());
    w.string(m.msp().stopReason());
    w.i64(m.msp().instructionsExecuted());
    w.boolean(m.msp().atPreemptionPoint());
    w.i64(m.scheduler.now());
    auto& csp = m.nativeControlStorage();
    w.i32(csp.currentTaskBlock());
    w.i32(csp.currentRequestBlock());
    As36ControlStorageProcessor::CheckpointState s;
    std::string failure;
    if (!csp.captureCheckpoint(s, failure)) throw std::runtime_error(failure);
    w.ints(s.pendingDeviceAcePairs);
    w.ints(s.pendingAsyncChildren);
    w.ints(s.nativeTransferContinuations);
    w.boolean(s.redispatch);
    w.i32(s.phase2SvatJobTask);
    w.boolean(s.phase2SvatDispatched);
    w.i32(s.taskIdCounter);
    w.i32(s.requestBlockBeforeAttach);
    w.boolean(s.systemMeasurementEnabled);
    w.i32(s.transferredTub);
    w.i32(s.pendingTransferredTub);
    w.boolean(s.transferredAutoSignOn);
    w.boolean(s.pendingTransferAutoSignOn);
    w.boolean(s.lastTransferPostedGuestWork);
    w.i32(s.wsDeviceStatusPending);
    w.i32(s.wsDeviceStatusDelivered);
    w.ints(s.wsPresentPairs);
    w.ints(s.deferredWsInput);
    w.u64(s.actionStatusLow);
    w.u64(s.actionStatusHigh);
    w.ints(s.actionCoverage);
    w.i32(s.sessionContentPopulated);
    w.i32(s.clssMapGateSet);
    w.i32(s.wddqWriteForced);
    w.i32(s.expAutoMapped);
    w.i32(s.expGrounded);
    w.ints(s.directAreaWords);
    w.ints(s.taskWorkAreaFree);
    w.ints(s.workSpaces);
    w.ints(s.heap);
    w.ints(s.ptt);
    w.i32(s.moduleStorageNext);
    w.ints(s.moduleStoragePairs);
    w.ints(s.workSpaceStoragePairs);
    w.ints(s.workSpaceStoragePages);
    w.ints(s.moduleStorageSizePairs);
    w.ints(s.moduleStorageFree);
    w.ints(s.loadedMemberData);
    w.strings(s.loadedMemberNames);
    w.i32(s.aces.next);
    w.ints(s.aces.free);
    w.ints(s.aces.allocated);

    devices::DeviceSet::PendingCheckpoint d = m.devices().capturePendingCheckpoint();
    w.ints(d.inputReadPairs);
    w.ints(d.inputStagingPairs);
    w.ints(d.inputResponseStatus);
    w.ints(d.pendingC1Pairs);
    w.ints(d.controllerInvites);
    w.ints(d.pendingActivationUnits);
    w.ints(d.activatedUnits);
    w.ints(d.nativeActiveUnits);
    w.ints(d.configuredUnits);
    w.ints(d.internalRendererUnits);
    w.ints(d.transferRendererUnits);

    bool tape = m.devices().tape.medium() != nullptr;
    w.boolean(tape);
    if (tape) {
        storage::TapePosition p = m.devices().tape.medium()->readPosition();
        w.i32(p.fileNumber);
        w.i32(p.blockNumber);
        w.boolean(p.atTapeMark);
        w.boolean(p.beginningOfTape);
        w.boolean(p.endOfData);
    }
    w.i32(static_cast<int32_t>(m.stations().size()));
    for (auto& ws : m.stations()) {
        w.string(ws->id());
        w.i32(ws->tubAddress);
        w.i32(static_cast<int32_t>(ws->outputMode()));
        w.i32(static_cast<int32_t>(ws->inviteReadMode()));
        w.boolean(ws->inviteOutstanding());
        w.u8(ws->savedReadMode());
        w.i64(ws->outputDataStreams());
        w.i64(ws->outputDataBytes());
        w.i64(ws->inputRecords());
        w.bytes(ws->lastOutputDataStream(), ws->hasLastOutputDataStream());
    }
    w.i32(static_cast<int32_t>(m.printers().size()));
    for (auto& p : m.printers()) {
        w.string(p->id());
        w.i32(p->pubAddress);
        w.i64(p->outputDataStreams());
        w.i64(p->outputDataBytes());
        w.bytes(p->lastOutputDataStream(), p->hasLastOutputDataStream());
    }
}

MachineSnapshot::RuntimeState MachineSnapshot::readRuntime(Reader& r)
{
    RuntimeState s;
    if (!r.bytes(s.main, 16 * 1024 * 1024, "main storage")) invalid("main storage is out of range");
    int atrs = boundedInt(r.i32(), 128, 128, "ATR count");
    s.atr.resize(static_cast<std::size_t>(atrs));
    for (int i = 0; i < atrs; i++) s.atr[static_cast<std::size_t>(i)] = r.u16();
    s.cycles = r.i64();
    s.m36Src = r.i32();
    s.iar = r.u16();
    s.arr = r.u16();
    s.xr1 = r.u16();
    s.xr2 = r.u16();
    for (int i = 0; i < 8; i++) s.wr[i] = r.u16();
    s.psr = r.u8();
    s.pactDir = r.u8();
    s.pactXr1 = r.u8();
    s.pactXr2 = r.u8();
    s.pactIar = r.u8();
    s.pactReg = r.u8();
    s.pactAtr = r.u8();
    s.pactCsp = r.u8();
    s.pmr = r.u8();
    s.cmr = r.u8();
    s.stopped = r.boolean();
    s.stopReason = r.string();
    s.instructions = r.i64();
    s.atPreemptionPoint = r.boolean();
    s.schedulerNow = r.i64();
    s.currentTaskBlock = r.i32();
    s.currentRequestBlock = r.i32();

    auto& c = s.csp;
    c.pendingDeviceAcePairs = r.ints("pending device ACEs");
    c.pendingAsyncChildren = r.ints("pending asynchronous children");
    c.nativeTransferContinuations = r.ints("native transfer continuations");
    c.redispatch = r.boolean();
    c.phase2SvatJobTask = r.i32();
    c.phase2SvatDispatched = r.boolean();
    c.taskIdCounter = r.i32();
    c.requestBlockBeforeAttach = r.i32();
    c.systemMeasurementEnabled = r.boolean();
    c.transferredTub = r.i32();
    c.pendingTransferredTub = r.i32();
    c.transferredAutoSignOn = r.boolean();
    c.pendingTransferAutoSignOn = r.boolean();
    c.lastTransferPostedGuestWork = r.boolean();
    c.wsDeviceStatusPending = r.i32();
    c.wsDeviceStatusDelivered = r.i32();
    c.wsPresentPairs = r.ints("workstation-present map");
    c.deferredWsInput = r.ints("deferred workstation input");
    c.actionStatusLow = r.u64();
    c.actionStatusHigh = r.u64();
    c.actionCoverage = r.ints("SVC 0B action coverage");
    c.sessionContentPopulated = r.i32();
    c.clssMapGateSet = r.i32();
    c.wddqWriteForced = r.i32();
    c.expAutoMapped = r.i32();
    c.expGrounded = r.i32();
    c.directAreaWords = r.ints("direct area");
    c.taskWorkAreaFree = r.ints("task work area");
    c.workSpaces = r.ints("work spaces");
    c.heap = r.ints("system queue heap");
    c.ptt = r.ints("PTT pool");
    c.moduleStorageNext = r.i32();
    c.moduleStoragePairs = r.ints("module storage map");
    c.workSpaceStoragePairs = r.ints("workspace storage map");
    c.workSpaceStoragePages = r.ints("workspace page storage map");
    c.moduleStorageSizePairs = r.ints("module storage sizes");
    c.moduleStorageFree = r.ints("module storage free list");
    c.loadedMemberData = r.ints("loaded member data");
    c.loadedMemberNames = r.strings("loaded member names");
    c.aces.next = r.i32();
    c.aces.free = r.ints("free ACEs");
    c.aces.allocated = r.ints("allocated ACEs");

    auto& d = s.devices;
    d.inputReadPairs = r.ints("pending input reads");
    d.inputStagingPairs = r.ints("workstation input staging pages");
    d.inputResponseStatus = r.ints("workstation response status");
    d.pendingC1Pairs = r.ints("pending workstation C1 responses");
    d.controllerInvites = r.ints("controller invites");
    d.pendingActivationUnits = r.ints("pending activation units");
    d.activatedUnits = r.ints("activated units");
    d.nativeActiveUnits = r.ints("native-active workstation units");
    d.configuredUnits = r.ints("configured workstation units");
    d.internalRendererUnits = r.ints("internal renderer units");
    d.transferRendererUnits = r.ints("transfer renderer units");

    s.hasTapePosition = r.boolean();
    if (s.hasTapePosition) {
        s.tapePosition.fileNumber = r.i32();
        s.tapePosition.blockNumber = r.i32();
        s.tapePosition.atTapeMark = r.boolean();
        s.tapePosition.beginningOfTape = r.boolean();
        s.tapePosition.endOfData = r.boolean();
    }
    int stations = boundedInt(r.i32(), 0, 1024, "station-state count");
    for (int i = 0; i < stations; i++) {
        StationState x;
        x.id = r.string();
        x.tubAddress = r.i32();
        x.outputMode = r.i32();
        x.inviteReadMode = r.i32();
        x.inviteOutstanding = r.boolean();
        x.activeReadMode = r.u8();
        x.outputStreams = r.i64();
        x.outputBytes = r.i64();
        x.inputRecords = r.i64();
        x.hasLastOutput = r.bytes(x.lastOutput, 16 * 1024 * 1024, "last station output");
        s.stations.push_back(std::move(x));
    }
    int printers = boundedInt(r.i32(), 0, 1024, "printer-state count");
    for (int i = 0; i < printers; i++) {
        PrinterState x;
        x.id = r.string();
        x.pubAddress = r.i32();
        x.outputStreams = r.i64();
        x.outputBytes = r.i64();
        x.hasLastOutput = r.bytes(x.lastOutput, 16 * 1024 * 1024, "last printer output");
        s.printers.push_back(std::move(x));
    }
    return s;
}

}  // namespace sim36::monitor
