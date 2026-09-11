// A raw System/36 volume image: 256-byte sectors, 10-sector blocks.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace sim36::storage {

// How writes to a volume are treated.
enum class VolumeMode {
    ReadWrite,   // writes reach the file
    ReadOnly,    // writes are REFUSED; an IPL legitimately writes, so it fails
    Overlay,     // writes are accepted and held in memory, never committed
};

class DiskBackend {
public:
    static constexpr int kSectorBytes = 256;
    static constexpr int kSectorsPerBlock = 10;

    // Opens the image.  Throws std::runtime_error when the file cannot be
    // opened or a read-write attach finds another emulator's lock.
    DiskBackend(const std::string& path, VolumeMode mode);
    DiskBackend(const std::string& path, bool readOnly)
        : DiskBackend(path, readOnly ? VolumeMode::ReadOnly : VolumeMode::ReadWrite) {}
    ~DiskBackend();
    DiskBackend(const DiskBackend&) = delete;
    DiskBackend& operator=(const DiskBackend&) = delete;

    const std::string& path() const { return path_; }
    VolumeMode mode() const { return mode_; }
    bool readOnly() const { return mode_ != VolumeMode::ReadWrite; }
    bool isOverlay() const { return mode_ == VolumeMode::Overlay; }
    int overlaySectors() const { return static_cast<int>(overlay_.size()); }
    long long sectorCount() const { return sectorCount_; }
    long long blockCount() const { return sectorCount_ / kSectorsPerBlock; }

    // Sector I/O.  False when the sector is outside the volume, the volume is
    // read-only (writes), or the host I/O failed; the guest core sees status,
    // never an exception.
    bool readSector(long long sector, uint8_t* dst) const;
    bool readSector(long long sector, std::vector<uint8_t>& dst) const;
    bool writeSector(long long sector, const uint8_t* src);

    // The sectors written since attach and held only in memory (overlay mode).
    const std::map<long long, std::vector<uint8_t>>& overlay() const { return overlay_; }

private:
    void acquireWriteLock();
    void releaseWriteLock();

    std::string path_;
    VolumeMode mode_;
    std::FILE* file_ = nullptr;
    long long sectorCount_ = 0;
    std::map<long long, std::vector<uint8_t>> overlay_;
    std::string lockPath_;
};

}  // namespace sim36::storage
