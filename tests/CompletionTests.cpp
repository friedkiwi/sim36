// Tab completion: the registry supplies the vocabulary for each argument
// position and the console filters it by the partial word under the cursor,
// adding directory entries where a host path is accepted.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Host/Console.h"
#include "Monitor/CommandRegistry.h"

using namespace sim36;

namespace {

host::Console::Completer registryCompleter()
{
    return [](const std::vector<std::string>& preceding) {
        monitor::CommandRegistry::Completion c = monitor::CommandRegistry::complete(preceding);
        return host::Console::Completion{c.words, c.paths};
    };
}

std::vector<std::string> complete(const std::string& input)
{
    return host::Console::candidates(input, registryCompleter());
}

bool has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST_CASE("an empty line completes to every command name")
{
    std::vector<std::string> all = complete("");
    CHECK(all.size() == monitor::CommandRegistry::all().size());
    CHECK(has(all, "help"));
    CHECK(has(all, "listener-auto-signon"));
}

TEST_CASE("a partial verb is filtered case-insensitively")
{
    std::vector<std::string> v = complete("SNA");
    REQUIRE(v.size() == 1);
    CHECK(v[0] == "snapshot");
    CHECK(complete("zzz").empty());
}

TEST_CASE("argument positions complete their keywords")
{
    CHECK(has(complete("show "), "status"));
    CHECK(has(complete("show "), "cpu"));
    CHECK(complete("show st") == std::vector<std::string>{"status", "storage"});
    CHECK(complete("set machine ipl-") == std::vector<std::string>{"ipl-type", "ipl-source"});
    CHECK(complete("set station 1.0 ") == std::vector<std::string>{"role", "device-code", "listen", "signon-at-ipl"});
    CHECK(complete("set station 1.0 role ") == std::vector<std::string>{"console", "display", "printer"});
    CHECK(complete("listener-auto-signon o") == std::vector<std::string>{"on", "off"});
    CHECK(complete("b ") == std::vector<std::string>{"list", "clear"});  // alias resolves
    CHECK(complete("help ").empty());
    CHECK(complete("nosuch ").empty());
}

TEST_CASE("path positions offer directory entries, directories with a trailing slash")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "sim36-completion-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "startup.sim") << "help\n";
    std::ofstream(dir / "other.txt") << "";

    const std::string base = dir.string() + "/";
    std::vector<std::string> v = complete("do " + base + "s");
    CHECK(v == std::vector<std::string>{base + "startup.sim", base + "sub/"});
    CHECK(complete("attach disk0 " + base + "o") == std::vector<std::string>{base + "other.txt"});
    CHECK(complete("snapshot save " + base + "sub/").empty());
    CHECK(complete("save config " + base + "nothing").empty());
    // Keywords and paths coexist in `save config`.
    CHECK(has(complete("save config st"), "stdout"));
    // Non-path positions never touch the file system.
    CHECK(complete("show " + base).empty());
    fs::remove_all(dir);
}
