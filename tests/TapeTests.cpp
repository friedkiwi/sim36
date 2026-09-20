#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "Storage/FolderTapeBackend.h"
#include "Storage/TapeBackend.h"

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
