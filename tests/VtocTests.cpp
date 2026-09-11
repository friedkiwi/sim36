// Disk-layer tests over a synthetic volume the test builds itself, so they
// pass with no licensed image present.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "Storage/ByteOrder.h"
#include "Storage/DiskBackend.h"
#include "Storage/Ebcdic.h"
#include "Storage/Vtoc.h"

using namespace sim36::storage;

namespace {

struct TempVolume {
    std::filesystem::path path;
    explicit TempVolume(const std::vector<uint8_t>& image)
    {
        path = std::filesystem::temp_directory_path() /
               ("sim36-vtoc-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".img");
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    }
    ~TempVolume() { std::filesystem::remove(path); }
};

void putLabel(std::vector<uint8_t>& img, long long sector, const char* name, uint8_t type,
              uint32_t extent, uint32_t alloc)
{
    uint8_t* e = img.data() + sector * DiskBackend::kSectorBytes;
    e[0] = 0xC6; e[1] = 0xF1;            // "F1"
    e[2] = type;
    auto n = Ebcdic::fromAscii(name);
    for (std::size_t i = 0; i < 8; ++i) e[4 + i] = i < n.size() ? n[i] : 0x40;
    putBe24(e + 0x27, extent);
    putBe24(e + 0x2A, alloc);
}

// A volume whose VOL1 points the VTOC at sector 9000, with the first label
// nine FF-filled sectors later, the way real volumes are laid out.
std::vector<uint8_t> synthetic(int sectors, int vtocPointer, int labelSector)
{
    std::vector<uint8_t> img(static_cast<std::size_t>(sectors) * DiskBackend::kSectorBytes, 0);
    uint8_t* vol1 = img.data() + Vtoc::kVolumeLabelSector * DiskBackend::kSectorBytes;
    vol1[0] = 0xE5; vol1[1] = 0xD6; vol1[2] = 0xD3; vol1[3] = 0xF1;   // "VOL1"
    putBe24(vol1 + 0x0D, static_cast<uint32_t>(vtocPointer));
    for (int s = vtocPointer; s < labelSector; ++s)
        std::fill_n(img.data() + static_cast<std::size_t>(s) * DiskBackend::kSectorBytes, DiskBackend::kSectorBytes, 0xFF);
    putLabel(img, labelSector, "#LIBRARY", 0x08, 75441, 100070);
    putLabel(img, labelSector + 1, "#SYSWORK", 0x40, 8191, 720);
    // User VTOC two blocks after the system one.
    putLabel(img, labelSector + 20, "PAYROLL", 0x80, 200000, 500);
    return img;
}

}  // namespace

TEST_CASE("vtoc: the system VTOC is found through VOL1, not assumed")
{
    TempVolume v(synthetic(12000, 9000, 9009));
    DiskBackend disk(v.path.string(), VolumeMode::ReadOnly);
    CHECK(disk.sectorCount() == 12000);
    CHECK(Vtoc::systemSectorOf(&disk) == 9009);
    auto system = Vtoc::readSystem(disk);
    REQUIRE(system.size() == 2);
    CHECK(system[0].name == "#LIBRARY");
    CHECK(system[0].type == VtocEntryType::Library);
    CHECK(system[0].extentSector == 75441);
    CHECK(system[0].allocatedSectors == 100070);
    CHECK(system[0].endSector() == 175511);
    CHECK(system[0].toString() == "#LIBRARY  Library            75441..175511    100070 sectors");
    auto user = Vtoc::readUser(disk);
    REQUIRE(user.size() == 1);
    CHECK(user[0].name == "PAYROLL");
    CHECK(user[0].type == VtocEntryType::IndexedFile);
}

TEST_CASE("vtoc: a volume without VOL1 falls back to the reference layout")
{
    std::vector<uint8_t> img(static_cast<std::size_t>(9000) * DiskBackend::kSectorBytes, 0);
    TempVolume v(img);
    DiskBackend disk(v.path.string(), VolumeMode::ReadOnly);
    CHECK(Vtoc::systemSectorOf(&disk) == Vtoc::kDefaultSystemVtocBlock * DiskBackend::kSectorsPerBlock);
    CHECK(Vtoc::readSystem(disk).empty());
}

TEST_CASE("disk backend: overlay holds writes in memory and read-only refuses them")
{
    TempVolume v(synthetic(9000, 8300, 8309));
    uint8_t sector[DiskBackend::kSectorBytes];
    std::fill_n(sector, DiskBackend::kSectorBytes, 0x5A);
    {
        DiskBackend disk(v.path.string(), VolumeMode::Overlay);
        CHECK(disk.writeSector(100, sector));
        CHECK(disk.overlaySectors() == 1);
        uint8_t back[DiskBackend::kSectorBytes];
        REQUIRE(disk.readSector(100, back));
        CHECK(back[0] == 0x5A);
        CHECK_FALSE(disk.readSector(9000, back));
        CHECK_FALSE(disk.writeSector(-1, sector));
    }
    {
        DiskBackend disk(v.path.string(), VolumeMode::ReadOnly);
        uint8_t back[DiskBackend::kSectorBytes];
        REQUIRE(disk.readSector(100, back));
        CHECK(back[0] == 0x00);   // the file was never touched
        CHECK_FALSE(disk.writeSector(100, sector));
    }
    {
        DiskBackend disk(v.path.string(), VolumeMode::ReadWrite);
        CHECK(std::filesystem::exists(v.path.string() + ".lock"));
        CHECK_THROWS(DiskBackend(v.path.string(), VolumeMode::ReadWrite));
        CHECK(disk.writeSector(100, sector));
    }
    CHECK_FALSE(std::filesystem::exists(v.path.string() + ".lock"));
    DiskBackend disk(v.path.string(), VolumeMode::ReadOnly);
    uint8_t back[DiskBackend::kSectorBytes];
    REQUIRE(disk.readSector(100, back));
    CHECK(back[0] == 0x5A);
}

TEST_CASE("disk backend: a missing image is refused by name")
{
    CHECK_THROWS(DiskBackend("/nonexistent/sim36.img", VolumeMode::ReadOnly));
}
