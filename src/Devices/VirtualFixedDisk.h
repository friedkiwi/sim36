// Fixed Disk IOS, SVC 40: the device the machine cannot boot without.
//
// Delayed and privileged, though "read operations will be regarded as an
// immediate SVC if the disk cache is active and the data to be read is found
// in the cache" (SA21-9436 3-132), which is why dispatch class is decided
// per request rather than per R-byte.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Storage/DiskBackend.h"

namespace sim36::devices {

class VirtualFixedDisk {
public:
    // The disk command set.  00, A0 and A4 are accepted and complete
    // immediately without touching the volume; A1 reads, A2 writes (with
    // the modifier C0 at +0x0B making byte 13 a fill character), A3 scans.
    static constexpr int kCommandComplete = 0xA0;
    static constexpr int kCommandCompleteAlt = 0xA4;
    static constexpr int kCommandCompleteDefault = 0x00;
    static constexpr int kCommandRead = 0xA1;
    static constexpr int kCommandWrite = 0xA2;
    static constexpr int kCommandScan = 0xA3;

    VirtualFixedDisk(machine::MachineState& m, storage::DiskBackend& volume, monitor::Tracer& trace)
        : m_(m), volume_(volume), trace_(trace) {}

    // Execute the IOB at `iob`.  False when the request was refused; the
    // completion code has been posted to the ECM either way.
    bool execute(int iob, uint8_t qByte);

    long long readsIssued() const { return readsIssued_; }
    long long sectorsRead() const { return sectorsRead_; }
    long long writesIssued() const { return writesIssued_; }
    long long sectorsWritten() const { return sectorsWritten_; }

    // The most recent transfer, so the monitor can show what a read produced.
    const std::vector<uint8_t>& lastRead() const { return lastRead_; }
    bool hasLastRead() const { return !lastRead_.empty(); }
    int lastReadSector() const { return lastReadSector_; }

private:
    using Extents = std::vector<std::pair<int, int>>;
    void advanceWorkSector(int iob, int sector, int sectors);
    bool scan(int iob, int sector, int sectors, int relation, int bufferField);
    bool write(int iob, int sector, int sectors, int modifier, int bufferField);
    bool resolveBufferExtents(int bufferField, int length, Extents& extents);
    bool extentsInStorage(const Extents& extents, int bufferField);
    void writeExtents(const Extents& extents, const std::vector<uint8_t>& data);
    void readExtents(const Extents& extents, std::vector<uint8_t>& data);
    static Extents resolveArgumentExtents(const Extents& extents, int length);
    static const char* relationName(int relation);
    static int compare(const uint8_t* image, int at, const std::vector<uint8_t>& argument);

    machine::MachineState& m_;
    storage::DiskBackend& volume_;
    monitor::Tracer& trace_;
    long long readsIssued_ = 0, sectorsRead_ = 0, writesIssued_ = 0, sectorsWritten_ = 0;
    std::vector<uint8_t> lastRead_;
    int lastReadSector_ = 0;
};

}  // namespace sim36::devices
