#include "Machine/MachineState.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "Monitor/SrcCode.h"
#include "Monitor/Tracer.h"

namespace sim36::machine {

std::string StorageProtection::message() const
{
    return fmt::format("storage protection violation at {:04X} ({})", address, forWrite ? "write" : "read");
}

MachineState::MachineState(int mainStorageBytes, int backingBytes)
    : installedBytes_(mainStorageBytes)
{
    if (mainStorageBytes <= 0 || mainStorageBytes > 16 * 1024 * 1024) mainStorageBytes = 1024 * 1024;
    if (backingBytes < mainStorageBytes || backingBytes > 16 * 1024 * 1024) backingBytes = mainStorageBytes;
    installedBytes_ = mainStorageBytes;
    mainStorage_.assign(static_cast<std::size_t>(backingBytes), 0);
}

std::string MachineState::faultMessage() const
{
    return fmt::format("guest access {:06X}+{} outside {:06X} bytes of main storage",
                       faultAddr_, faultLen_, mainStorage_.size());
}

uint8_t MachineState::readByte(int addr)
{
    if (!inRange(addr, 1)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 1; return 0; }
    return mainStorage_[static_cast<std::size_t>(addr)];
}

void MachineState::writeByte(int addr, uint8_t v)
{
    if (!inRange(addr, 1)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 1; return; }
    write(addr, &v, 1);
}

uint16_t MachineState::readHalf(int addr)
{
    if (!inRange(addr, 2)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 2; return 0; }
    const uint8_t* p = mainStorage_.data() + addr;
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void MachineState::writeHalf(int addr, uint16_t v)
{
    uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    if (!inRange(addr, 2)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 2; return; }
    write(addr, b, 2);
}

int MachineState::readAddr24(int addr)
{
    if (!inRange(addr, 3)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 3; return 0; }
    const uint8_t* p = mainStorage_.data() + addr;
    return (p[0] << 16) | (p[1] << 8) | p[2];
}

void MachineState::writeAddr24(int addr, int v)
{
    uint8_t b[3] = {static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    if (!inRange(addr, 3)) { faulted_ = true; faultAddr_ = addr; faultLen_ = 3; return; }
    write(addr, b, 3);
}

void MachineState::read(int addr, uint8_t* dst, int len)
{
    if (!inRange(addr, len)) { faulted_ = true; faultAddr_ = addr; faultLen_ = len; return; }
    std::memcpy(dst, mainStorage_.data() + addr, static_cast<std::size_t>(len));
}

void MachineState::write(int addr, const uint8_t* src, int len)
{
    if (!inRange(addr, len)) { faulted_ = true; faultAddr_ = addr; faultLen_ = len; return; }
    std::vector<uint8_t> observed;
    if (onObservedWrite) observed.assign(mainStorage_.begin() + addr, mainStorage_.begin() + addr + len);
    watched(addr, len);
    std::memcpy(mainStorage_.data() + addr, src, static_cast<std::size_t>(len));
    watchDone();
    if (onObservedWrite) {
        std::vector<uint8_t> after(mainStorage_.begin() + addr, mainStorage_.begin() + addr + len);
        onObservedWrite(addr, len, observed, after);
    }
}

void MachineState::watched(int addr, int len)
{
    pendingWatch_ = false;
    if (watches_.empty() || !onWatchWrite) return;
    for (const auto& w : watches_) {
        const int lo = w.first, hi = w.second;
        if (addr > hi || addr + len - 1 < lo) continue;
        const int flo = std::max(addr, lo), fhi = std::min(addr + len - 1, hi);
        pendingBefore_.assign(mainStorage_.begin() + flo, mainStorage_.begin() + fhi + 1);
        pendingLo_ = flo;
        pendingWatch_ = true;
        return;
    }
}

void MachineState::watchDone()
{
    if (!pendingWatch_) return;
    pendingWatch_ = false;
    std::vector<uint8_t> after(mainStorage_.begin() + pendingLo_,
                               mainStorage_.begin() + pendingLo_ + static_cast<long>(pendingBefore_.size()));
    std::vector<uint8_t> before = std::move(pendingBefore_);
    pendingBefore_.clear();
    onWatchWrite(pendingLo_, static_cast<int>(before.size()), before, after);
}

bool MachineState::resolve(uint16_t logical, uint8_t pact, int atrBase, bool forWrite, int& real,
                           StorageProtection* fault) const
{
    if ((pact & MspRegisters::kPactPmrBit) == 0) {
        real = ((pact & kPactAddressBits) << 16) | logical;
        return true;
    }
    return translate(logical, atrBase, forWrite, real, fault);
}

bool MachineState::translate(uint16_t logical, int atrBase, bool forWrite, int& real,
                             StorageProtection* fault) const
{
    const int page = (logical >> 11) & 0x1F;
    const uint16_t a = atr[atrBase + page];
    // Bits 0-2 (0xE000) are the invalid/protect field; an equality check
    // against 0x01FF would miss every other invalid encoding.
    if ((a & kAtrInvalidMask) != 0) {
        if (fault != nullptr) { fault->address = logical; fault->forWrite = forWrite; }
        return false;
    }
    // The page frame is bits 3-15, a strict superset of the architected nine.
    real = ((a & kAtrPageFrameMask) << 11) | (logical & 0x7FF);
    return true;
}

bool MachineState::resolveGuest24(int field, bool forWrite, int& addr) const
{
    if ((field & 0x800000) == 0) {
        addr = field & 0x7FFFFF;
        return true;
    }
    if (translate(static_cast<uint16_t>(field), kAtrTaskGroup0, forWrite, addr)) return true;
    addr = 0;
    return false;
}

bool MachineState::translatedExtents(uint16_t logical, int atrBase, int length, bool forWrite,
                                     std::vector<std::pair<int, int>>& extents,
                                     StorageProtection* fault) const
{
    extents.clear();
    int at = logical, left = length;
    while (left > 0) {
        int real = 0;
        if (!translate(static_cast<uint16_t>(at), atrBase, forWrite, real, fault)) return false;
        const int inPage = kPageBytes - (at & (kPageBytes - 1));
        const int n = inPage < left ? inPage : left;
        if (!extents.empty() && extents.back().first + extents.back().second == real)
            extents.back().second += n;
        else
            extents.emplace_back(real, n);
        left -= n;
        at = (at + n) & 0xFFFF;
    }
    return true;
}

bool MachineState::guest24Extents(int field, int length, bool forWrite,
                                  std::vector<std::pair<int, int>>& extents) const
{
    extents.clear();
    if (length < 0) return false;
    if ((field & 0x800000) != 0)
        return translatedExtents(static_cast<uint16_t>(field), kAtrTaskGroup0, length, forWrite, extents);
    const int real = field & 0x7FFFFF;
    if (!inRange(real, length)) return false;
    if (length != 0) extents.emplace_back(real, length);
    return true;
}

bool MachineState::writePageFrames(const std::vector<int>& frames, int displacement,
                                   const uint8_t* source, int length)
{
    if (displacement < 0 || length < 0 || (length != 0 && source == nullptr)) return false;
    if (length == 0) return true;
    const long long end = static_cast<long long>(displacement) + length;
    if (end > static_cast<long long>(frames.size()) * kPageBytes) return false;

    // Validate every independently resident frame first. A bad tail must not
    // leave a prefix of an input record visible to the guest.
    int at = displacement;
    int left = length;
    while (left > 0) {
        const int page = at >> kPageShift;
        const int offset = at & (kPageBytes - 1);
        const int chunk = std::min(left, kPageBytes - offset);
        const int frame = frames[static_cast<std::size_t>(page)];
        if (frame == 0 || !inRange(frame + offset, chunk)) return false;
        at += chunk;
        left -= chunk;
    }

    at = displacement;
    int sourceOffset = 0;
    while (sourceOffset < length) {
        const int page = at >> kPageShift;
        const int offset = at & (kPageBytes - 1);
        const int chunk = std::min(length - sourceOffset, kPageBytes - offset);
        write(frames[static_cast<std::size_t>(page)] + offset, source + sourceOffset, chunk);
        at += chunk;
        sourceOffset += chunk;
    }
    return true;
}

bool MachineState::readGuest24Range(int field, uint8_t* destination, int length)
{
    std::vector<std::pair<int, int>> extents;
    if (!guest24Extents(field, length, false, extents)) return false;
    int offset = 0;
    for (const auto& e : extents) {
        read(e.first, destination + offset, e.second);
        offset += e.second;
    }
    return !faulted_;
}

bool MachineState::writeGuest24Range(int field, const uint8_t* source, int length)
{
    std::vector<std::pair<int, int>> extents;
    if (!guest24Extents(field, length, true, extents)) return false;
    int offset = 0;
    for (const auto& e : extents) {
        write(e.first, source + offset, e.second);
        offset += e.second;
    }
    return !faulted_;
}

void MachineState::captureCheck(const std::string& kind, int classCode, const std::string& detail)
{
    const MspRegisters& r = msp;
    std::string s = fmt::format("cycle={} class={} kind={}\ndetail={}\n{}\n", cycles,
                                monitor::SrcCode::deviceClass(classCode), kind, detail,
                                stateDescriber ? stateDescriber() : std::string("(no task describer)"));
    if (checkStateDescriber) s += checkStateDescriber();
    s += fmt::format("IAR={:04X} ARR={:04X} XR1={:02X}:{:04X} XR2={:02X}:{:04X} PSR={:02X}\n"
                     "PACT dir/xr1/xr2/iar/reg/atr/csp={:02X}/{:02X}/{:02X}/{:02X}/{:02X}/{:02X}/{:02X} PMR={:02X} CMR={:02X}\n",
                     r.iar, r.arr, r.pactXr1, r.xr1, r.pactXr2, r.xr2, r.psr(),
                     r.pactDir, r.pactXr1, r.pactXr2, r.pactIar, r.pactReg, r.pactAtr, r.pactCsp, r.pmr(), r.cmr);
    s += "WR0-7=";
    for (uint16_t w : r.wr) s += fmt::format(" {:04X}", w);
    s += "\nATR0-31=";
    for (int i = 0; i < 32; ++i) s += fmt::format(" {:04X}", atr[kAtrTaskGroup0 + i]);
    s += "\n";
    int physical = 0;
    if (resolve(r.iar, r.pactIar, kAtrTaskGroup0, false, physical)) {
        const int first = std::max(0, physical - 16);
        const int last = std::min(static_cast<int>(mainStorage_.size()), physical + 32);
        s += fmt::format("physical-IAR={:06X} window={:06X}-{:06X}\n", physical, first, last);
        for (int at = first; at < last; at += 16) {
            const int n = std::min(16, last - at);
            s += fmt::format("{:06X}:", at);
            for (int j = 0; j < n; ++j) s += fmt::format(" {:02X}", mainStorage_[static_cast<std::size_t>(at + j)]);
            s += "\n";
        }
    } else {
        StorageProtection f{r.iar, false};
        s += "physical-IAR unavailable: " + f.message() + "\n";
    }
    if (static_cast<int>(checkHistory_.size()) == kCheckHistoryLimit) checkHistory_.erase(checkHistory_.begin());
    checkHistory_.push_back(s);
}

void MachineState::dumpSrcState()
{
    if (srcTrace_ == nullptr || !srcTrace_->srcTrace()) return;
    const MspRegisters& r = msp;
    srcTrace_->line("src", "  {}", stateDescriber ? stateDescriber() : std::string("(no task describer)"));
    srcTrace_->line("src", "  IAR={:04X} ARR={:04X} XR1={:02X}:{:04X} XR2={:02X}:{:04X} PSR={:02X} "
                    "PACT dir/xr1/xr2={:02X}/{:02X}/{:02X}",
                    r.iar, r.arr, r.pactXr1, r.xr1, r.pactXr2, r.xr2, r.psr(), r.pactDir, r.pactXr1, r.pactXr2);
    srcTrace_->line("src", "  WR0-7={:04X} {:04X} {:04X} {:04X} {:04X} {:04X} {:04X} {:04X}",
                    r.wr[0], r.wr[1], r.wr[2], r.wr[3], r.wr[4], r.wr[5], r.wr[6], r.wr[7]);
}

void MachineState::postM36Src(int code, const std::string& reason)
{
    code &= 0xFFFF;
    if (code == m36Src_) return;
    const int old = m36Src_;
    m36Src_ = code;
    if (srcTrace_ != nullptr) {
        if (code == 0)
            srcTrace_->line("src", "SRC posted: {:04X}  {}", code, monitor::SrcCode::describe(code));
        else
            srcTrace_->line("src", "SRC posted: {:04X}  {}  (was {}; {})", code, monitor::SrcCode::describe(code),
                            old < 0 ? std::string("----") : fmt::format("{:04X}", old), reason);
        dumpSrcState();
    }
}

void MachineState::reportCheck(const std::string& kind, int classCode, const std::string& detail)
{
    captureCheck(kind, classCode, detail);
    if (srcTrace_ != nullptr) {
        srcTrace_->line("src", "SRC posted: CHECK [{}]  {}: {}  (M36 SRC stays {})",
                        monitor::SrcCode::deviceClass(classCode), kind, detail,
                        m36Src_ < 0 ? std::string("----") : fmt::format("{:04X}", m36Src_));
        dumpSrcState();
    }
}

}  // namespace sim36::machine
