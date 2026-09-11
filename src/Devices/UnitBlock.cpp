#include "Devices/UnitBlock.h"

#include <fmt/format.h>

namespace sim36::devices {

namespace {

void dumpJobAssociation(std::FILE* w, machine::MachineState& m, int block)
{
    int jobTask = m.readAddr24(block + UnitBlock::kOffOwningJobTask);
    int jcb = m.readAddr24(block + UnitBlock::kOffJobControlBlock);
    bool taskWellFormed = jobTask != 0 && jobTask + 1 < m.backingBytes() && m.readHalf(jobTask) == 0xE3C2;   // "TB"
    fmt::print(w, "  +62 job task     {:06X}  {}\n", jobTask,
               jobTask == 0 ? "none"
               : taskWellFormed ? "owning interactive job task (\"TB\")"
                                : "set, but no \"TB\" eyecatcher at target");
    fmt::print(w, "  +65 JCB          {:06X}  {}\n", jcb, jcb == 0 ? "none" : "job control block");
    const char* verdict;
    if (jobTask != 0)
        verdict = "yes - an interactive job owns this station (Cmd1 Resume-job legal)";
    else if (jcb != 0)
        verdict = "NO - JCB retained but owning task cleared: the job terminated. Cmd1 Resume-job would be refused (SYS-7212)";
    else
        verdict = "NO - no owning job task and no JCB: no interactive job was initiated for this station. Cmd1 Resume-job "
                  "would be refused (SYS-7212)";
    fmt::print(w, "  resumable job    {}\n", verdict);
}

void dumpCpetTable(std::FILE* w, machine::MachineState& m, int block)
{
    int table = m.readAddr24(block + UnitBlock::kOffCpetTablePointer);
    if (table == 0) {
        fmt::print(w, "  CPET table       NULL; not walked\n");
        return;
    }
    // The table holds at most twenty 16-byte entries with FF at byte zero of
    // the following slot: scan the twenty entry starts plus that terminator.
    for (int slot = 0; slot <= UnitBlock::kCpetMaximumEntries; slot++) {
        long long start = static_cast<long long>(table) + static_cast<long long>(slot) * UnitBlock::kCpetEntrySize;
        if (start < 0 || start >= m.backingBytes()) {
            fmt::print(w, "  CPET table       TRUNCATED before slot {} (+{:04X})\n", slot, slot * UnitBlock::kCpetEntrySize);
            return;
        }
        if (m.readByte(static_cast<int>(start)) == 0xFF) {
            fmt::print(w, "  CPET table       {} x 16-byte entries; FF terminator at slot {} (+{:04X}); VALID\n", slot, slot,
                       slot * UnitBlock::kCpetEntrySize);
            return;
        }
        if (start + UnitBlock::kCpetEntrySize > m.backingBytes()) {
            fmt::print(w, "  CPET table       TRUNCATED in entry {} (+{:04X})\n", slot, slot * UnitBlock::kCpetEntrySize);
            return;
        }
    }
    fmt::print(w, "  CPET table       INVALID: no FF terminator in 21 16-byte slots\n");
}

void dumpPointer(std::FILE* w, machine::MachineState& m, int block, int leftmost, int rightmost, const char* role)
{
    int pointer = m.readAddr24(block + leftmost);
    std::string target;
    if (pointer == 0)
        target = "NULL";
    else if (pointer < 0 || pointer + 1 >= m.backingBytes())
        target = "outside guest main storage";
    else
        target = fmt::format("target eye {:04X}", m.readHalf(pointer));
    fmt::print(w, "  +{:02X} ptr [{:02X}..{:02X}] {:06X}  {}; {}\n", rightmost, leftmost, rightmost, pointer, target, role);
}

}  // namespace

void UnitBlock::dump(std::FILE* w, machine::MachineState& m, int block)
{
    fmt::print(w, "unit block {:06X}{}\n", block, isWellFormed(m, block) ? " (TU)" : " - NO \"TU\" EYECATCHER");
    fmt::print(w, "  +07 flags        {:02X} {}\n", m.readByte(block + kOffFlags),
               isSignedOn(m, block) ? "(signed on)" : "(not signed on)");
    fmt::print(w, "  +0A class        {:02X}\n", m.readByte(block + kOffClass));
    fmt::print(w, "  +27 state        {:02X}\n", m.readByte(block + kOffState));
    fmt::print(w, "  +28 queue header {:04X}\n", m.readHalf(block + kOffQueueHeader));
    fmt::print(w, "  +2A status       {:02X}\n", m.readByte(block + kOffStatus));
    fmt::print(w, "  +2D chain        {:06X}\n", m.readAddr24(block + kOffChain));
    dumpJobAssociation(w, m, block);
    fmt::print(w, "  +90..+9F raw     ");
    for (int i = 0x90; i <= 0x9F; i++) {
        if (i != 0x90) fmt::print(w, " ");
        fmt::print(w, "{:02X}", m.readByte(block + i));
    }
    fmt::print(w, "\n");
    dumpPointer(w, m, block, kOffActiveSessionPointer, kActiveSessionPointerRightmost, "active-session/replacement");
    dumpPointer(w, m, block, kOffPointerEnding95, kPointerEnding95Rightmost, "role unresolved; overlaps CPET pointer byte 0");
    dumpPointer(w, m, block, kOffCpetTablePointer, kCpetTablePointerRightmost, "CPET table");
    dumpCpetTable(w, m, block);
    fmt::print(w, "  +99..+9A half    {:04X}\n", m.readHalf(block + 0x99));
}

}  // namespace sim36::devices
