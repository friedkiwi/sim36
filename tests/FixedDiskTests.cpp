// The fixed-disk device over a synthetic volume: IOB decode, the A0/A4
// completions, A1 reads into real and translated buffers, A2 writes, A3 scans.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "Devices/IoBlock.h"
#include "Devices/VirtualFixedDisk.h"
#include "Machine/MachineState.h"
#include "Monitor/Tracer.h"
#include "Processors/ControlStorage/ActionControlElement.h"
#include "Storage/DiskBackend.h"
#include "Storage/ByteOrder.h"
#include "Storage/Ebcdic.h"
#include "Storage/Library.h"
#include "Storage/Vtoc.h"

using namespace sim36;
using devices::IoBlock;
using devices::VirtualFixedDisk;
using processors::controlstorage::Ecm;
using storage::DiskBackend;

namespace {

struct Volume {
    std::filesystem::path path;
    explicit Volume(int sectors)
    {
        path = std::filesystem::temp_directory_path() /
               ("sim36-disk-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".img");
        std::vector<uint8_t> img(static_cast<std::size_t>(sectors) * DiskBackend::kSectorBytes, 0);
        for (int s = 0; s < sectors; ++s)
            for (int i = 0; i < DiskBackend::kSectorBytes; ++i)
                img[static_cast<std::size_t>(s) * DiskBackend::kSectorBytes + static_cast<std::size_t>(i)] =
                    static_cast<uint8_t>((s * 7 + i) & 0xFF);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    }
    ~Volume() { std::filesystem::remove(path); }
};

void buildIob(machine::MachineState& m, int iob, int command, int modifier, int sector0, int count, int buffer)
{
    for (int i = 0; i < 48; ++i) m.writeByte(iob + i, 0);
    m.writeByte(iob + IoBlock::kOffCommand, static_cast<uint8_t>(command));
    m.writeByte(iob + IoBlock::kOffCommandModifier, static_cast<uint8_t>(modifier));
    m.writeAddr24(iob + IoBlock::kOffDiskSector, sector0 + 1);   // 1-based on the wire
    m.writeAddr24(iob + IoBlock::kOffDiskCount, count - 1);
    m.writeAddr24(iob + IoBlock::kOffDataBuffer, buffer);
}

}  // namespace

TEST_CASE("fixed disk: command 00, A0 and A4 complete without a transfer")
{
    Volume v(100);
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    DiskBackend d(v.path.string(), storage::VolumeMode::ReadOnly);
    VirtualFixedDisk disk(m, d, t);
    const int iob = 0x600;
    for (int command : {VirtualFixedDisk::kCommandCompleteDefault,
                        VirtualFixedDisk::kCommandComplete,
                        VirtualFixedDisk::kCommandCompleteAlt}) {
        buildIob(m, iob, command, 0, 5, 1, 0x2000);
        CHECK(disk.execute(iob, 0));
        CHECK(Ecm::isComplete(m, iob));
        CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x40);
        CHECK(m.readAddr24(iob + IoBlock::kOffDiskSectorWork) == 6);
    }
    CHECK(disk.readsIssued() == 0);
}

TEST_CASE("fixed disk: A1 reads sectors into a real buffer and leaves the work sector on the last one")
{
    Volume v(100);
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    DiskBackend d(v.path.string(), storage::VolumeMode::ReadOnly);
    VirtualFixedDisk disk(m, d, t);
    const int iob = 0x600;
    buildIob(m, iob, VirtualFixedDisk::kCommandRead, 0, 10, 2, 0x2000);
    REQUIRE(disk.execute(iob, 0));
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x40);
    CHECK(m.readByte(0x2000) == static_cast<uint8_t>((10 * 7) & 0xFF));
    CHECK(m.readByte(0x2100) == static_cast<uint8_t>((11 * 7) & 0xFF));
    CHECK(m.readAddr24(iob + IoBlock::kOffDiskSectorWork) == 12);   // 1-based last sector
    CHECK(disk.sectorsRead() == 2);
    CHECK(disk.lastReadSector() == 10);
    CHECK(disk.lastRead().size() == 512);
    // A 1-based sector of zero is a resident page, not an error.
    buildIob(m, iob, VirtualFixedDisk::kCommandRead, 0, -1, 1, 0x3000);
    CHECK(disk.execute(iob, 0));
    CHECK(m.readByte(0x3000) == 0);
    // Past the end of the volume is refused with completion 4.
    buildIob(m, iob, VirtualFixedDisk::kCommandRead, 0, 99, 2, 0x3000);
    CHECK_FALSE(disk.execute(iob, 0));
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x44);
}

TEST_CASE("fixed disk: a translated buffer is resolved through task group 0 page by page")
{
    Volume v(100);
    machine::MachineState m(64 * 1024, 1024 * 1024);
    monitor::Tracer t;
    DiskBackend d(v.path.string(), storage::VolumeMode::ReadOnly);
    VirtualFixedDisk disk(m, d, t);
    m.atr[machine::MachineState::kAtrTaskGroup0 + 1] = 0x0030;   // logical 0x0800 -> real 0x18000
    m.atr[machine::MachineState::kAtrTaskGroup0 + 2] = 0x0007;   // logical 0x1000 -> real 0x03800
    const int iob = 0x600;
    buildIob(m, iob, VirtualFixedDisk::kCommandRead, 0, 3, 8, 0x800F00);   // 2 KB crossing the page boundary
    REQUIRE(disk.execute(iob, 0));
    CHECK(m.readByte(0x18700) == static_cast<uint8_t>((3 * 7) & 0xFF));   // logical 0F00 is page 1 offset 700
    CHECK(m.readByte(0x03800) == static_cast<uint8_t>(((3 + 1) * 7 + 0) & 0xFF));   // second sector's byte 0 lands on page 2
    // An unmapped page refuses the whole transfer.
    m.atr[machine::MachineState::kAtrTaskGroup0 + 2] = machine::MachineState::kAtrProtect;
    buildIob(m, iob, VirtualFixedDisk::kCommandRead, 0, 3, 8, 0x800F00);
    CHECK_FALSE(disk.execute(iob, 0));
}

TEST_CASE("fixed disk: A2 writes, fills and wraps; read-only volumes refuse")
{
    Volume v(100);
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    const int iob = 0x600;
    {
        DiskBackend d(v.path.string(), storage::VolumeMode::ReadOnly);
        VirtualFixedDisk disk(m, d, t);
        buildIob(m, iob, VirtualFixedDisk::kCommandWrite, 0, 1, 1, 0x2000);
        CHECK_FALSE(disk.execute(iob, 0));
        CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x44);
    }
    {
        DiskBackend d(v.path.string(), storage::VolumeMode::Overlay);
        VirtualFixedDisk disk(m, d, t);
        for (int i = 0; i < 256; ++i) m.writeByte(0x2000 + i, 0xAA);
        buildIob(m, iob, VirtualFixedDisk::kCommandWrite, 0, 1, 1, 0x2000);
        REQUIRE(disk.execute(iob, 0));
        uint8_t back[256];
        REQUIRE(d.readSector(1, back));
        CHECK(back[5] == 0xAA);
        // Fill: byte 13 is the fill character, not a buffer address.
        buildIob(m, iob, VirtualFixedDisk::kCommandWrite, IoBlock::kModifierFill, 2, 2, 0x5B0000);
        REQUIRE(disk.execute(iob, 0));
        REQUIRE(d.readSector(3, back));
        CHECK(back[0] == 0x5B);
        // Wrap: one 256-byte area is the source of every sector.
        for (int i = 0; i < 256; ++i) m.writeByte(0x2000 + i, 0x77);
        buildIob(m, iob, VirtualFixedDisk::kCommandWrite, IoBlock::kModifierWrap, 10, 3, 0x2000);
        REQUIRE(disk.execute(iob, 0));
        REQUIRE(d.readSector(12, back));
        CHECK(back[100] == 0x77);
        CHECK(disk.sectorsWritten() == 6);
    }
}

TEST_CASE("fixed disk: A3 scan finds a key and reports equal versus satisfied")
{
    Volume v(100);
    machine::MachineState m(64 * 1024);
    monitor::Tracer t;
    DiskBackend d(v.path.string(), storage::VolumeMode::Overlay);
    VirtualFixedDisk disk(m, d, t);
    // Lay five 51-byte entries per sector with 9-byte keys, as a library
    // directory does, into sectors 20..22.
    uint8_t sector[256] = {0};
    for (int s = 0; s < 3; ++s) {
        for (int e = 0; e < 5; ++e) {
            const uint8_t key = static_cast<uint8_t>(0xC1 + s * 5 + e);   // 'A'..
            for (int k = 0; k < 9; ++k) sector[e * 51 + k] = key;
        }
        d.writeSector(20 + s, sector);
    }
    const int iob = 0x600;
    auto scanFor = [&](uint8_t key, int relation) {
        buildIob(m, iob, VirtualFixedDisk::kCommandScan, relation, 20, 3, 0x2000);
        m.writeByte(iob + IoBlock::kOffScanFirstKey, 0);
        m.writeByte(iob + IoBlock::kOffScanKeyLength, 8);
        m.writeByte(iob + IoBlock::kOffScanLastKey, 0xD4);
        m.writeByte(iob + IoBlock::kOffScanKeyGap, 0x2A);
        for (int k = 0; k < 9; ++k) m.writeByte(0x2000 + k, key);
        return disk.execute(iob, 0);
    };
    REQUIRE(scanFor(0xC8, IoBlock::kScanEqual));                 // 'H', sector 21 entry 2
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x44);
    CHECK(m.readAddr24(iob + IoBlock::kOffDiskSectorWork) == 22);   // 1-based sector 21
    CHECK(m.readByte(iob + IoBlock::kOffScanHit) == 2 * 51 + 8);
    CHECK(m.readByte(0x2000 + 51) == 0xC7);                     // the whole sector replaced the argument
    REQUIRE(scanFor(0xC8, IoBlock::kScanHighOrEqual));
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x44);
    REQUIRE(scanFor(0xC0, IoBlock::kScanHighOrEqual));         // below every key: first key satisfies
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x40);
    REQUIRE(scanFor(0xF0, IoBlock::kScanEqual));                // no hit is code 2, not an error
    CHECK(m.readByte(iob + Ecm::kOffCompletion) == 0x42);
    CHECK_FALSE(scanFor(0xC8, IoBlock::kScanId));
}

TEST_CASE("library directory: entries are relative to the extent and stop past the directory")
{
    Volume v(200);
    DiskBackend d(v.path.string(), storage::VolumeMode::Overlay);
    storage::VtocEntry lib;
    lib.name = "#RPGLIB";
    lib.type = storage::VtocEntryType::Library;
    lib.extentSector = 100;
    lib.allocatedSectors = 50;
    uint8_t sector[256] = {0};
    // Sector 101: two entries, O #AU002 at relative 10 (12 sectors, link 0800) and P PROC1.
    auto entry = [&](int e, uint8_t type, const char* name, int rel, int sectors, int link) {
        uint8_t* o = sector + e * 51;
        o[0] = type;
        auto n = storage::Ebcdic::fromAscii(name);
        for (int i = 0; i < 8; ++i) o[1 + i] = i < static_cast<int>(n.size()) ? n[i] : 0x40;
        storage::putBe24(o + 9, static_cast<uint32_t>(rel));
        o[12] = static_cast<uint8_t>(sectors);
        storage::putBe16(o + 13, static_cast<uint16_t>(link));
        o[0x13] = 0x00;
    };
    entry(0, 0xD6, "#AU002", 10, 12, 0x0800);
    entry(1, 0xD7, "PROC1", 30, 2, 0);
    entry(2, 0xD6, "BOGUS", 99, 1, 0);   // relative sector past the extent: skipped
    d.writeSector(101, sector);
    std::fill(sector, sector + 256, 0);
    d.writeSector(102, sector);           // an empty sector ends the directory
    entry(0, 0xE2, "LATE", 20, 1, 0);
    d.writeSector(103, sector);           // never reached
    auto members = storage::LibraryDirectory::read(d, lib, 64);
    REQUIRE(members.size() == 2);
    CHECK(members[0].kind == 'O');
    CHECK(members[0].name == "#AU002");
    CHECK(members[0].relativeSector == 10);
    CHECK(members[0].sectors == 12);
    CHECK(members[0].linkAddress == 0x0800);
    CHECK(members[0].absoluteSector(lib) == 110);
    CHECK(members[1].kind == 'P');
}
