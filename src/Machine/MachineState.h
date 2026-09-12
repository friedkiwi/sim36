// The shared state both processors reach.
//
// The CSP and MSP do not exchange messages: the CSP reads and writes MSP
// registers directly, cycle steal reaches all four address spaces, and the
// CSP has its own entry in the MSP's PACT file (A7 = PCSP), whose bit 0 is
// physically the same flip-flop as CMR bit 7.  So one explicit state object
// is the faithful model.
//
// Guest storage access is exception-free: an access outside main storage
// records a fault (`faulted()`) and reads as zero or writes nothing; address
// translation returns status.  The monitor checks bounds itself and reports
// them in the reference's words.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Machine/MspRegisters.h"

namespace sim36::monitor { class Tracer; }

namespace sim36::machine {

// A translated access to a page whose ATR is protected (bits 0-2 set).  On
// real hardware this is a level 5 interrupt to the control processor when
// raised by the main storage program.
struct StorageProtection {
    uint16_t address = 0;
    bool forWrite = false;
    std::string message() const;
};

class MachineState {
public:
    static constexpr int kSectorBytes = 256;
    static constexpr int kPageBytes = 2048;
    static constexpr int kPageShift = 11;   // 2 KB pages

    // 128 address translation registers: 64 program level, 8 PACT, 56 I/O.
    // Each holds 9 bits; 0x1FF marks the page protected.
    uint16_t atr[128] = {};

    // Bits 0-2, IBM numbering: the invalid/protect field.  Any of them on
    // when the ATR is used raises a storage exception.  SY31-9035 10-670.
    static constexpr uint16_t kAtrInvalidMask = 0xE000;
    // Bits 3-15: the page frame.  The manual documents nine bits and a
    // 20-bit real address; the 5360 routes thirteen.
    static constexpr uint16_t kAtrPageFrameMask = 0x1FFF;
    // What SSP writes to protect a page: hex FFFF.
    static constexpr uint16_t kAtrProtect = 0xFFFF;

    // The 128 ATRs share one system address space with the PACT registers,
    // and the base of each group is its system address minus 0x80
    // (SY31-9035 10-650): 0x80-0x9F task group 1 (base 0), 0xA0-0xA7 the
    // PACT bytes, 0xA8-0xBF I/O ATRs 40-63, 0xC0-0xDF task group 0 (base
    // 64), 0xE0-0xFF I/O ATRs 0-31 (base 96).
    static constexpr int kAtrTaskGroup0 = 64;
    static constexpr int kAtrTaskGroup1 = 0;

    // The address bits of a PACT prefix: four, because concatenation forms
    // a 20-bit address (SA21-9436 1-28).  Bits 1-3 carry flags, not address.
    static constexpr uint8_t kPactAddressBits = 0x0F;
    static constexpr uint8_t kPactFlagBits = 0x70;

    MspRegisters msp;
    long long cycles = 0;   // virtual time in MSP instruction units

    // Construct the guest-visible installed store and its host backing.  The
    // Advanced/36 keeps a wider address space than the 1 MB SSP reports as
    // installed; keeping the two sizes distinct lets CSP-owned queue space and
    // translated module frames live in that backing without advertising them
    // to SSP as installed storage.
    MachineState(int mainStorageBytes, int backingBytes);
    explicit MachineState(int mainStorageBytes) : MachineState(mainStorageBytes, mainStorageBytes) {}

    int installedMainStorageBytes() const { return installedBytes_; }
    int backingBytes() const { return static_cast<int>(mainStorage_.size()); }
    uint8_t* raw() { return mainStorage_.data(); }
    const uint8_t* raw() const { return mainStorage_.data(); }

    // ---- guest storage access ------------------------------------------
    bool inRange(int addr, int len) const
    {
        return addr >= 0 && len >= 0 && static_cast<long long>(addr) + len <= static_cast<long long>(mainStorage_.size());
    }
    bool faulted() const { return faulted_; }
    void clearFault() { faulted_ = false; }
    std::string faultMessage() const;

    uint8_t readByte(int addr);
    void writeByte(int addr, uint8_t v);
    uint16_t readHalf(int addr);
    void writeHalf(int addr, uint16_t v);
    // A 24-bit guest address stored as three big-endian bytes: ECM+2,
    // ACE+13, ACE+16, IOB fields.
    int readAddr24(int addr);
    void writeAddr24(int addr, int v);
    void read(int addr, uint8_t* dst, int len);
    void write(int addr, const uint8_t* src, int len);

    // ---- observers --------------------------------------------------------
    // A read-only monitor observer (address, length, before, after).
    std::function<void(int, int, const std::vector<uint8_t>&, const std::vector<uint8_t>&)> onObservedWrite;
    // Storage watchpoints: every write to a watched range reports before/after.
    std::function<void(int, int, const std::vector<uint8_t>&, const std::vector<uint8_t>&)> onWatchWrite;
    void addWatch(int lo, int hi) { watches_.emplace_back(lo, hi); }
    void clearWatches() { watches_.clear(); }
    int watchCount() const { return static_cast<int>(watches_.size()); }

    // ---- address translation ---------------------------------------------
    // Resolve a 16-bit logical address for one access path.  Untranslated,
    // the PACT prefix's low four bits are concatenated with the register.
    // Translated, address bits 0-4 select one of 32 ATRs and the real
    // address is the page frame concatenated with address bits 5-15.
    bool resolve(uint16_t logical, uint8_t pact, int atrBase, bool forWrite, int& real,
                 StorageProtection* fault = nullptr) const;
    bool translate(uint16_t logical, int atrBase, bool forWrite, int& real,
                   StorageProtection* fault = nullptr) const;

    // A 24-bit device-path address is REAL unless bit 0x800000 is set, and
    // then it is translated through task group 0.  False when the page is
    // not mapped.
    bool resolveGuest24(int field, bool forWrite, int& addr) const;

    // The real extents one multi-byte transfer through the translation
    // registers occupies, as (real, count) pairs in transfer order.  Each
    // 2 KB of it is translated by its own register, so consecutive logical
    // pages are only consecutive in real storage when their ATRs name
    // adjacent frames.  The logical address wraps at 16 bits.
    bool translatedExtents(uint16_t logical, int atrBase, int length, bool forWrite,
                           std::vector<std::pair<int, int>>& extents,
                           StorageProtection* fault = nullptr) const;
    bool guest24Extents(int field, int length, bool forWrite,
                        std::vector<std::pair<int, int>>& extents) const;
    // Copy into a logical range backed by independently allocated 2 KiB
    // page frames. Zero frame entries are absent. The whole range is
    // validated before any byte is changed.
    bool writePageFrames(const std::vector<int>& frames, int displacement,
                         const uint8_t* source, int length);
    bool readGuest24Range(int field, uint8_t* destination, int length);
    bool writeGuest24Range(int field, const uint8_t* source, int length);

    // ---- the M36 system reference code -----------------------------------
    // The value the host reads as the machine's reference code: 0000 while
    // the guest runs normally.  It starts unposted (-1) so the first post is
    // itself logged.
    void attachSrcTracer(monitor::Tracer* t) { srcTrace_ = t; m36Src_ = -1; }
    int m36Src() const { return m36Src_; }
    // Post a new SRC.  Always surfaced on a real change; under `trace src`
    // also dumps the task and register state.  Never fabricate a code.
    void postM36Src(int code, const std::string& reason);
    void restoreM36Src(int code) { m36Src_ = code; }
    // Surface a check condition without inventing the four-digit value.
    void reportCheck(const std::string& kind, int classCode, const std::string& detail);
    std::vector<std::string> copyCheckHistory() const { return checkHistory_; }

    // Describes the current task and module for the SRC state dump.
    std::function<std::string()> stateDescriber;
    std::function<std::string()> checkStateDescriber;

private:
    void watched(int addr, int len);
    void watchDone();
    void captureCheck(const std::string& kind, int classCode, const std::string& detail);
    void dumpSrcState();

    std::vector<uint8_t> mainStorage_;
    int installedBytes_;
    bool faulted_ = false;
    int faultAddr_ = 0, faultLen_ = 0;
    std::vector<std::pair<int, int>> watches_;
    int pendingLo_ = 0;
    std::vector<uint8_t> pendingBefore_;
    bool pendingWatch_ = false;
    monitor::Tracer* srcTrace_ = nullptr;
    int m36Src_ = -1;
    std::vector<std::string> checkHistory_;
    static constexpr int kCheckHistoryLimit = 16;
};

}  // namespace sim36::machine
