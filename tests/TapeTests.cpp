#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Storage/FolderTapeBackend.h"
#include "Storage/SimhTapeBackend.h"
#include "Storage/TapeBackend.h"
#include "Storage/TapeBackendFactory.h"

using namespace sim36;

namespace {

struct TapeFolder
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("sim36-tape-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));

    TapeFolder()
    {
        std::string reason;
        REQUIRE(storage::FolderTapeBackend::init(path.string(), "TEST01", "OWNER", reason));
    }

    ~TapeFolder() { std::filesystem::remove_all(path); }

    nlohmann::ordered_json manifest() const
    {
        std::ifstream in(path / "manifest.json");
        return nlohmann::ordered_json::parse(in);
    }

    void manifest(const nlohmann::ordered_json& value)
    {
        std::ofstream out(path / "manifest.json", std::ios::trunc);
        out << value.dump(2) << '\n';
    }

    std::unique_ptr<storage::FolderTapeBackend> open(std::string& reason) const
    {
        return storage::FolderTapeBackend::open(path.string(), true, reason);
    }
};

struct TapFile
{
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("sim36-simh-tape-test-" +
                                  std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tap");

    TapFile() { std::filesystem::remove(path); }
    ~TapFile() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("folder tape rejects malformed media before load")
{
    SUBCASE("a canonical blank tape opens")
    {
        TapeFolder tape;
        std::string reason;
        CHECK(tape.open(reason) != nullptr);
    }

    SUBCASE("the format version is exact")
    {
        TapeFolder tape;
        auto m = tape.manifest();
        m["formatVersion"] = 2;
        tape.manifest(m);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("formatVersion") != std::string::npos);
    }

    SUBCASE("blob paths cannot traverse")
    {
        TapeFolder tape;
        auto m = tape.manifest();
        m["files"][0]["blob"] = "../outside.dat";
        tape.manifest(m);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("unsafe blob path") != std::string::npos);
    }

    SUBCASE("sequences are contiguous")
    {
        TapeFolder tape;
        auto m = tape.manifest();
        m["files"][1]["sequence"] = 3;
        tape.manifest(m);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("expected 2") != std::string::npos);
    }

    SUBCASE("block lengths are bounded")
    {
        TapeFolder tape;
        auto m = tape.manifest();
        m["files"][0]["blockLength"] = 0x8000;
        tape.manifest(m);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("outside 1..32767") != std::string::npos);
    }

    SUBCASE("blob sizes must match")
    {
        TapeFolder tape;
        std::filesystem::resize_file(tape.path / "0001.dat", 79);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("manifest declares 80") != std::string::npos);
    }

    SUBCASE("decoded labels are checked against bytes")
    {
        TapeFolder tape;
        auto m = tape.manifest();
        m["files"][0]["labels"]["vol1"]["ownerId"] = "NOTOWNER";
        tape.manifest(m);
        std::string reason;
        CHECK(tape.open(reason) == nullptr);
        CHECK(reason.find("decoded labels do not match") != std::string::npos);
    }
}

TEST_CASE("saved tape positions restore exactly")
{
    TapeFolder tape;
    std::string reason;
    auto writable = storage::FolderTapeBackend::open(tape.path.string(), false, reason);
    REQUIRE(writable != nullptr);
    writable->load();

    int spaced = 0;
    REQUIRE(writable->spaceFiles(1, spaced) == storage::TapeResult::Ok);
    const uint8_t one[] = {1, 2, 3};
    const uint8_t two[] = {4, 5};
    REQUIRE(writable->writeBlock(one, 0, 3) == storage::TapeResult::Ok);
    REQUIRE(writable->writeBlock(two, 0, 2) == storage::TapeResult::Ok);
    REQUIRE(writable->writeTapeMark() == storage::TapeResult::Ok);
    writable->unload();

    auto medium = storage::FolderTapeBackend::open(tape.path.string(), true, reason);
    REQUIRE(medium != nullptr);
    medium->load();

    auto verifyRestore = [&](const storage::TapePosition& wanted) {
        INFO(wanted.toString());
        REQUIRE(storage::restoreTapePosition(*medium, wanted, reason));
        CHECK(medium->readPosition().toString() == wanted.toString());
    };

    verifyRestore({0, 0, false, true, false});
    verifyRestore({1, 1, false, false, false});
    verifyRestore({1, 2, true, false, false});
    verifyRestore({2, 0, false, false, true});

    storage::TapePosition impossible{3, 0, false, false, true};
    CHECK_FALSE(storage::restoreTapePosition(*medium, impossible, reason));
    CHECK(reason.find("cannot restore tape file 3") != std::string::npos);
}

TEST_CASE("SIMH tape framing is validated and supports full tape semantics")
{
    TapFile file;
    const auto& path = file.path;

    std::string reason;
    auto tape = storage::SimhTapeBackend::open(path.string(), false, reason);
    REQUIRE(tape != nullptr);
    CHECK(std::filesystem::file_size(path) == 0);
    tape->load();
    CHECK(tape->readPosition().endOfData);

    const uint8_t odd[] = {0x11, 0x22, 0x33};
    const uint8_t even[] = {0x44, 0x55};
    REQUIRE(tape->writeBlock(odd, 0, 3) == storage::TapeResult::Ok);
    REQUIRE(tape->writeBlock(even, 0, 2) == storage::TapeResult::Ok);
    REQUIRE(tape->writeTapeMark() == storage::TapeResult::Ok);
    REQUIRE(tape->writeTapeMark() == storage::TapeResult::Ok);
    tape->unload();

    // 3-byte record: 4 + 3 + one pad + 4; 2-byte record: 4 + 2 + 4;
    // followed by two four-byte tape marks.
    CHECK(std::filesystem::file_size(path) == 30);
    auto reopened = storage::SimhTapeBackend::open(path.string(), true, reason);
    REQUIRE(reopened != nullptr);
    reopened->load();
    std::vector<uint8_t> actual;
    CHECK(reopened->readBlock(actual) == storage::TapeResult::Ok);
    CHECK(actual == std::vector<uint8_t>(odd, odd + 3));
    CHECK(reopened->readBlock(actual) == storage::TapeResult::Ok);
    CHECK(actual == std::vector<uint8_t>(even, even + 2));
    CHECK(reopened->readBlock(actual) == storage::TapeResult::TapeMark);
    CHECK(reopened->readBlock(actual) == storage::TapeResult::TapeMark);
    CHECK(reopened->readBlock(actual) == storage::TapeResult::EndOfData);

    reopened->rewind();
    int spaced = 0;
    CHECK(reopened->spaceRecords(2, spaced) == storage::TapeResult::Ok);
    CHECK(spaced == 2);
    CHECK(reopened->readPosition().atTapeMark);
    CHECK(reopened->spaceFiles(1, spaced) == storage::TapeResult::Ok);
    CHECK(reopened->readPosition().atTapeMark);
    CHECK(reopened->spaceFiles(-1, spaced) == storage::TapeResult::Ok);
    CHECK(reopened->readPosition().fileNumber == 0);
}

TEST_CASE("SIMH tape overwrite erases forward and malformed records are refused")
{
    TapFile file;
    const auto& path = file.path;

    std::string reason;
    auto tape = storage::SimhTapeBackend::open(path.string(), false, reason);
    REQUIRE(tape != nullptr);
    tape->load();
    const uint8_t records[] = {1, 2, 3};
    for (uint8_t record : records) REQUIRE(tape->writeBlock(&record, 0, 1) == storage::TapeResult::Ok);
    tape->rewind();
    int spaced = 0;
    REQUIRE(tape->spaceRecords(1, spaced) == storage::TapeResult::Ok);
    const uint8_t replacement[] = {9, 8};
    REQUIRE(tape->writeBlock(replacement, 0, 2) == storage::TapeResult::Ok);
    REQUIRE(tape->writeTapeMark() == storage::TapeResult::Ok);
    tape->unload();

    auto reopened = storage::SimhTapeBackend::open(path.string(), true, reason);
    REQUIRE(reopened != nullptr);
    reopened->load();
    std::vector<uint8_t> actual;
    REQUIRE(reopened->readBlock(actual) == storage::TapeResult::Ok);
    CHECK(actual == std::vector<uint8_t>{1});
    REQUIRE(reopened->readBlock(actual) == storage::TapeResult::Ok);
    CHECK(actual == std::vector<uint8_t>({9, 8}));
    CHECK(reopened->readBlock(actual) == storage::TapeResult::TapeMark);
    CHECK(reopened->readBlock(actual) == storage::TapeResult::EndOfData);

    // Corrupt the trailing length of the first record.
    {
        std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(6);
        const char bad = 2;
        io.write(&bad, 1);
    }
    CHECK(storage::SimhTapeBackend::open(path.string(), true, reason) == nullptr);
    CHECK(reason.find("marker mismatch") != std::string::npos);
}

TEST_CASE("tape backend selection is based only on directory versus file")
{
    std::string reason;
    TapeFolder folder;
    auto folderBackend = storage::openTapeBackend(folder.path.string(), true, reason);
    REQUIRE(folderBackend != nullptr);
    CHECK(dynamic_cast<storage::FolderTapeBackend*>(folderBackend.get()) != nullptr);

    TapFile blank;
    {
        std::ofstream create(blank.path, std::ios::binary);
    }
    auto fileBackend = storage::openTapeBackend(blank.path.string(), true, reason);
    REQUIRE(fileBackend != nullptr);
    CHECK(dynamic_cast<storage::SimhTapeBackend*>(fileBackend.get()) != nullptr);

    TapFile missing;
    REQUIRE_FALSE(std::filesystem::exists(missing.path));
    auto created = storage::openTapeBackend(missing.path.string(), false, reason);
    REQUIRE(created != nullptr);
    CHECK(dynamic_cast<storage::SimhTapeBackend*>(created.get()) != nullptr);
    CHECK(std::filesystem::is_regular_file(missing.path));
    CHECK(std::filesystem::file_size(missing.path) == 0);

    TapFile missingReadOnly;
    CHECK(storage::openTapeBackend(missingReadOnly.path.string(), true, reason) == nullptr);
    CHECK_FALSE(std::filesystem::exists(missingReadOnly.path));
}

TEST_CASE("SIMH initialization and inspection use backend primitives")
{
    TapFile file;
    std::string reason;
    REQUIRE(storage::initializeTape(file.path.string(), "TAP123", "REALOWNER", reason));

    auto tape = storage::openTapeBackend(file.path.string(), false, reason);
    REQUIRE(tape != nullptr);
    tape->load();
    CHECK(tape->volumeId() == "TAP123");
    int spaced = 0;
    REQUIRE(tape->spaceRecords(1, spaced) == storage::TapeResult::Ok);
    const storage::TapePosition before = tape->readPosition();

    auto catalog = storage::inspectTape(*tape, reason);
    REQUIRE(catalog != nullptr);
    CHECK(catalog->volume.volumeId == "TAP123");
    CHECK(catalog->volume.ownerId == "REALOWNER");
    REQUIRE(catalog->files.size() == 2);
    CHECK(catalog->files[0].kind == "label");
    CHECK(catalog->files[0].blockCount == 1);
    CHECK(catalog->files[1].blockCount == 0);
    CHECK(tape->readPosition().toString() == before.toString());
}
