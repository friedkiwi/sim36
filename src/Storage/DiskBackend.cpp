#include "Storage/DiskBackend.h"

#include "Storage/FileNotFoundError.h"

#include <cstring>
#include <filesystem>
#include <stdexcept>

#include <fmt/format.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define SIM36_GETPID _getpid
#else
#include <unistd.h>
#define SIM36_GETPID getpid
#endif

namespace sim36::storage {

namespace {

std::string fullPathOf(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(path, ec);
    return ec ? path : p.lexically_normal().string();
}

bool fileExists(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::fclose(f);
    return true;
}

long long fileLength(std::FILE* f)
{
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    long long n = static_cast<long long>(ftello(f));
    fseeko(f, 0, SEEK_SET);
#endif
    return n;
}

bool seekTo(std::FILE* f, long long offset)
{
#ifdef _WIN32
    return _fseeki64(f, offset, SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

}  // namespace

DiskBackend::DiskBackend(const std::string& path, VolumeMode mode)
    : path_(path), mode_(mode)
{
    if (!fileExists(path)) throw FileNotFoundError(fullPathOf(path));
    // Two emulators writing one image silently corrupt each other's
    // measurements, so a writable attach takes an explicit sidecar lock.
    // ReadOnly and Overlay never touch the file and need none.
    if (mode == VolumeMode::ReadWrite) acquireWriteLock();
    file_ = std::fopen(path.c_str(), mode == VolumeMode::ReadWrite ? "r+b" : "rb");
    if (file_ == nullptr) {
        releaseWriteLock();
        throw std::runtime_error(fmt::format("Could not open file \"{}\"", path));
    }
    long long length = fileLength(file_);
    sectorCount_ = length < 0 ? 0 : length / kSectorBytes;
}

DiskBackend::~DiskBackend()
{
    if (file_ != nullptr) std::fclose(file_);
    releaseWriteLock();
}

void DiskBackend::acquireWriteLock()
{
    const std::string lockPath = path_ + ".lock";
#ifdef _WIN32
    int fd = -1;
    _sopen_s(&fd, lockPath.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
             _SH_DENYRW, _S_IREAD | _S_IWRITE);
#else
    int fd = ::open(lockPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
#endif
    if (fd < 0) {
        std::string holder;
        std::FILE* f = std::fopen(lockPath.c_str(), "rb");
        if (f != nullptr) {
            char buf[64] = {0};
            std::size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            std::string pid(buf, n);
            while (!pid.empty() && (pid.back() == '\n' || pid.back() == '\r' || pid.back() == ' '))
                pid.pop_back();
            holder = " (held by pid " + pid + ")";
        }
        throw std::runtime_error(fmt::format(
            "{} is already attached read-write by another emulator{}. Two emulators "
            "writing one image corrupt each other's runs. Use a private copy, or attach "
            "it `overlay` to accept writes in memory without touching the file. Remove "
            "{} if no emulator is running.", path_, holder, lockPath));
    }
    const std::string pid = fmt::format("{}\n", SIM36_GETPID());
#ifdef _WIN32
    _write(fd, pid.data(), static_cast<unsigned>(pid.size()));
    _close(fd);
#else
    ssize_t written = ::write(fd, pid.data(), pid.size());
    (void)written;
    ::close(fd);
#endif
    lockPath_ = lockPath;
}

void DiskBackend::releaseWriteLock()
{
    if (lockPath_.empty()) return;
    std::remove(lockPath_.c_str());
    lockPath_.clear();
}

bool DiskBackend::readSector(long long sector, uint8_t* dst) const
{
    if (sector < 0 || sector >= sectorCount_) return false;
    auto it = overlay_.find(sector);
    if (it != overlay_.end()) {
        std::memcpy(dst, it->second.data(), kSectorBytes);
        return true;
    }
    if (!seekTo(file_, sector * kSectorBytes)) return false;
    return std::fread(dst, 1, kSectorBytes, file_) == static_cast<std::size_t>(kSectorBytes);
}

bool DiskBackend::readSector(long long sector, std::vector<uint8_t>& dst) const
{
    dst.resize(kSectorBytes);
    return readSector(sector, dst.data());
}

bool DiskBackend::writeSector(long long sector, const uint8_t* src)
{
    if (sector < 0 || sector >= sectorCount_) return false;
    if (mode_ == VolumeMode::Overlay) {
        std::vector<uint8_t>& dirty = overlay_[sector];
        dirty.assign(src, src + kSectorBytes);
        return true;
    }
    if (readOnly()) return false;
    if (!seekTo(file_, sector * kSectorBytes)) return false;
    if (std::fwrite(src, 1, kSectorBytes, file_) != static_cast<std::size_t>(kSectorBytes)) return false;
    std::fflush(file_);
    return true;
}

}  // namespace sim36::storage
