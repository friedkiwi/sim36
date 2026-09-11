// A SIMH-style monitor over a constructed machine.  Inspection is a
// first-class feature rather than a debug aid: every structure the machine
// defines should be dumpable by name.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Machine/Machine.h"

namespace sim36::monitor {

struct SelfTestResult {
    int passed = 0;
    int failed = 0;
};

// Runs the manual's worked examples against a machine state and processor,
// printing one PASS/FAIL line per vector and the totals.
SelfTestResult runSelfTest(machine::MachineState& m, processors::MainStorageProcessor& msp);

class MonitorCli {
public:
    explicit MonitorCli(machine::Machine& m) : m_(m) {}

    // Route an already canonicalised, registry-approved command to its
    // machine-state implementation.  The session is the sole public
    // dispatcher.
    void executeTokens(const std::vector<std::string>& a);

    bool executionActive() const { return false; }
    void stopExecutionForTeardown() {}

    // A hex dump in the monitor's format: address, 16 bytes, EBCDIC text.
    static std::string hexDump(const uint8_t* b, int len, int baseAddr);

private:
    void show(const std::vector<std::string>& a);
    void showVtoc(const std::vector<std::string>& a);
    void showLibrary(const std::vector<std::string>& a);
    void dumpSector(const std::vector<std::string>& a);
    void dump(const std::vector<std::string>& a);
    void setRegister(const std::vector<std::string>& a);
    void setTrace(const std::vector<std::string>& a);
    void ipl(const std::vector<std::string>& a);
    void boot();
    void load(const std::vector<std::string>& a);
    void loadFile(const std::vector<std::string>& a);
    void diskRead(const std::vector<std::string>& a);
    void disassemble(const std::vector<std::string>& a);
    void step(const std::vector<std::string>& a);
    long long driveMachine(long long cap);
    void breakCommand(const std::vector<std::string>& a);
    void watch(const std::vector<std::string>& a);
    void installWatchReporter();
    void poke(const std::vector<std::string>& a);
    void patch(const std::vector<std::string>& a);
    void findMemory(const std::vector<std::string>& a);
    void addressMap(const std::vector<std::string>& a);
    void selfTest();
    static bool parseHexBytes(const std::vector<std::string>& a, std::size_t from, std::vector<uint8_t>& out);

    machine::Machine& m_;
};

}  // namespace sim36::monitor
