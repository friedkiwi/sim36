#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "Configuration/ConfigError.h"
#include "Configuration/EmulatorConfig.h"
#include "Configuration/IplSourceTable.h"
#include "Monitor/CommandRegistry.h"
#include "Monitor/ConfigurationRenderer.h"
#include "Monitor/SimulatorSession.h"
#include "Monitor/Tracer.h"
#include "Storage/DiskBackend.h"

using namespace sim36::configuration;
using sim36::monitor::ConfigurationRenderer;

namespace {

struct EmptyVolume {
    std::filesystem::path path;

    EmptyVolume()
    {
        path = std::filesystem::temp_directory_path() /
               ("sim36-media-default-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".img");
        std::vector<uint8_t> image(9000 * sim36::storage::DiskBackend::kSectorBytes, 0);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
    }

    ~EmptyVolume() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("config: the default definition validates once a volume and console exist")
{
    EmulatorConfig c;
    CHECK_FALSE(c.volumeReadOnly);
    CHECK_FALSE(c.disketteReadOnly);
    CHECK_THROWS_AS(c.validate("t"), ConfigError);
    c.volumePath = "x.img";
    CHECK_THROWS_AS(c.validate("t"), ConfigError);   // no console
    c.applyDefaultStationsIfNoneDeclared();
    REQUIRE(c.stations.size() == 7);
    CHECK(c.stations[6].listenPort == 2306);
    CHECK_NOTHROW(c.validate("t"));
    c.mainStorageKb = 2048;
    CHECK_THROWS_AS(c.validate("t"), ConfigError);
}

TEST_CASE("media attach without a mode resets fixed disk and diskette to writable")
{
    EmptyVolume volume;
    sim36::monitor::SimulatorSession session;
    const std::string quotedPath = ConfigurationRenderer::quoteArgument(volume.path.string());

    session.execute("attach disk0 " + quotedPath + " ro");
    CHECK(session.definition().volumeReadOnly);
    session.execute("attach disk0 " + quotedPath);
    CHECK_FALSE(session.definition().volumeReadOnly);
    CHECK_FALSE(session.definition().volumeOverlay);
    session.execute("attach disk0 " + quotedPath + " overlay");
    CHECK(session.definition().volumeOverlay);
    session.execute("attach disk0 " + quotedPath);
    CHECK_FALSE(session.definition().volumeReadOnly);
    CHECK_FALSE(session.definition().volumeOverlay);

    session.execute("attach diskette0 " + quotedPath + " ro");
    CHECK(session.definition().disketteReadOnly);
    session.execute("attach diskette0 " + quotedPath);
    CHECK_FALSE(session.definition().disketteReadOnly);
}

TEST_CASE("config: machine identity is independent of the CSP implementation")
{
    EmulatorConfig c;
    c.volumePath = "x.img";
    c.applyDefaultStationsIfNoneDeclared();

    c.model = "5363";
    c.cspType = "advanced36";
    CHECK(c.systemCustomize1() == 0x8B);
    CHECK(c.cspKind() == CspKind::Virtual);
    CHECK_NOTHROW(c.validate("t"));

    c.model = "5364";
    CHECK(c.systemCustomize1() == 0x8D);
    CHECK_NOTHROW(c.validate("t"));

    c.model = "5360-s3";
    CHECK_THROWS_AS(c.validate("t"), ConfigError);
}

TEST_CASE("config: printers need a printer device code")
{
    EmulatorConfig c;
    c.volumePath = "x.img";
    c.applyDefaultStationsIfNoneDeclared();
    StationConfig p;
    p.port = 1; p.address = 0; p.role = "printer";
    c.stations.push_back(p);
    CHECK_THROWS_AS(c.validate("t"), ConfigError);
    c.stations.back().deviceCode = "11";
    c.stations.back().deviceCodeGiven = true;
    CHECK_THROWS_AS(c.validate("t"), ConfigError);   // a display's code
    c.stations.back().deviceCode = "PB";
    CHECK_NOTHROW(c.validate("t"));
}

TEST_CASE("ipl source: disk requests no reload; attended sets bit 0x80")
{
    CHECK_FALSE(IplSourceTable::requestsReload("disk"));
    CHECK(IplSourceTable::requestsReload("tape"));
    CHECK(IplSourceTable::encode("disk", "unattend") == 0x00);
    CHECK(IplSourceTable::encode("tape", "attended") == 0x88);
    CHECK(IplSourceTable::encode("0x14", "unattend") == 0x14);
    CHECK_THROWS_AS(IplSourceTable::encode("cassette", "unattend"), ConfigError);
    std::string n;
    CHECK(IplSourceTable::tryNormalizeType("Attended", n));
    CHECK(n == "attend");
    CHECK_FALSE(IplSourceTable::tryNormalizeType("mystery", n));
}

TEST_CASE("renderer: replay quoting")
{
    CHECK(ConfigurationRenderer::quoteArgument("plain") == "plain");
    CHECK(ConfigurationRenderer::quoteArgument("") == "\"\"");
    CHECK(ConfigurationRenderer::quoteArgument("a b") == "\"a b\"");
    CHECK(ConfigurationRenderer::quoteArgument("x\\y") == "\"x\\\\y\"");
    CHECK(ConfigurationRenderer::quoteArgument("say \"hi\"") == "\"say \\\"hi\\\"\"");
}

TEST_CASE("renderer: replay is sorted by station and puts the endpoint before enable")
{
    EmulatorConfig c;
    StationConfig s2; s2.port = 0; s2.address = 2;
    StationConfig s0; s0.port = 0; s0.address = 0; s0.role = "console";
    c.stations = {s2, s0};
    std::string replay = ConfigurationRenderer::renderReplay(c);
    CHECK(replay.find("set terminal multiplex listen 127.0.0.1:2300\nset terminal multiplex on\n") != std::string::npos);
    CHECK(replay.find("set station 0.0 role console") < replay.find("set station 0.2 role display"));
}

TEST_CASE("trace flags: parse and render as the reference's enum did")
{
    using namespace sim36::monitor;
    CHECK(traceFlagsToString(0) == "None");
    CHECK(traceFlagsToString(parseTraceFlags("msp,svc")) == "Msp, Svc");
    CHECK(traceFlagsToString(parseTraceFlags("all,defer")) == "All, Defer");
    CHECK(traceFlagsToString(parseTraceFlags("all")) == "All");
    CHECK(traceFlagsToString(TraceDisk | TraceAce) == "Disk, Ace");
    CHECK_THROWS(parseTraceFlags("bogus"));
}

TEST_CASE("ordinary trace toggles are host controls but guest-owned trace forms are not")
{
    using sim36::monitor::CommandRegistry;
    CHECK(CommandRegistry::isHostControl({"trace"}));
    CHECK(CommandRegistry::isHostControl({"trace", "ws"}));
    CHECK(CommandRegistry::isHostControl({"trace", "off"}));
    CHECK_FALSE(CommandRegistry::isHostControl({"trace", "member", "MSPID"}));
    CHECK_FALSE(CommandRegistry::isHostControl({"trace", "workstation", "W1", "lifecycle", "on"}));
}
