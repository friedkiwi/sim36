// Monitor commands over the control storage processor's task, module and
// allocation state: whereis, tasklist, mapstate, sqsstate, modules,
// modstorage, residency, smf, allocchain, breakm, xferid, xferterm,
// nuptermscan, actions, timers and `show ptt`.  All of them read guest
// storage and the processor's own bookkeeping; none writes guest state.
#include "Monitor/MonitorCli.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <fmt/format.h>

#include "Monitor/CommandRegistry.h"
#include "Monitor/SimulatorSession.h"
#include "Processors/ControlStorage/GuestLowStorage.h"
#include "Processors/ControlStorage/MapParameterList.h"
#include "Processors/ControlStorage/ProgramBlock.h"
#include "Processors/ControlStorage/TaskBlock.h"
#include "Processors/ControlStorage/TransientArea.h"

namespace sim36::monitor {

using processors::controlstorage::As36ControlStorageProcessor;
using processors::controlstorage::GuestLowStorage;
using processors::controlstorage::JobControlBlock;
using processors::controlstorage::LoadedMember;
using processors::controlstorage::MapTable;
using processors::controlstorage::ProgramBlock;
using processors::controlstorage::RequestBlock;
using processors::controlstorage::StorageBlock;
using processors::controlstorage::SvcRequest;
using processors::controlstorage::TaskBlock;
using processors::controlstorage::TransientArea;

namespace {

int parseHex32(const std::string& s)
{
    std::string h = s;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
    if (h.empty()) throw MonitorError("Could not find any recognizable digits.");
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw MonitorError("Could not find any recognizable digits.");
    if (h.size() > 8) throw MonitorError("Value was either too large or too small for an Int32.");
    unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    if (v > 0x7FFFFFFFUL) throw MonitorError("Value was either too large or too small for an Int32.");
    return static_cast<int>(v);
}

bool memberNameMatches(const std::string& member, const std::string& wanted)
{
    if (member.empty()) return false;
    std::string m = toLower(member), w = toLower(wanted);
    return m.find(w) != std::string::npos;
}

}  // namespace

bool MonitorCli::canReadGuest(int address, int length) const
{
    return address > 0 && length >= 0 && address <= m_.state.backingBytes() - length;
}

std::vector<MonitorCli::RequestFrame> MonitorCli::requestBlockChain(int taskBlock)
{
    auto& csp = m_.nativeControlStorage();
    std::vector<RequestFrame> result;
    std::set<int> seen;
    int rb = m_.state.readAddr24(taskBlock + TaskBlock::kOffRequestBlock);
    while (rb != 0 && result.size() < 256 && seen.insert(rb).second) {
        if (!canReadGuest(rb, RequestBlock::kOffProgramBlock + 3) || m_.state.readHalf(rb) != GuestLowStorage::kEyeRequestBlock)
            break;
        int pb = m_.state.readAddr24(rb + RequestBlock::kOffProgramBlock);
        int iar = m_.state.readHalf(rb + RequestBlock::kOffIar);
        LoadedMember member;
        int offset = 0;
        bool attributed = csp.tryProgramBlockMember(pb, iar, member, offset);
        int previous = m_.state.readAddr24(rb + RequestBlock::kOffPrevious);
        RequestFrame f;
        f.depth = static_cast<int>(result.size());
        f.requestBlock = rb;
        f.previous = previous;
        f.programBlock = pb;
        f.resumeIar = iar;
        f.attributed = attributed;
        f.member = attributed ? member.name : std::string();
        f.offset = offset;
        result.push_back(f);
        rb = previous;
    }
    return result;
}

std::string MonitorCli::moduleOwners(int programBlock)
{
    auto& csp = m_.nativeControlStorage();
    std::vector<std::string> owners;
    std::set<int> seen;
    int tb = m_.state.readAddr24(GuestLowStorage::queueHeader(39));
    while (tb != 0 && seen.size() < 4096 && seen.insert(tb).second) {
        if (!TaskBlock::isTaskBlock(m_.state, tb)) break;
        for (const auto& frame : requestBlockChain(tb))
            if (frame.programBlock == programBlock)
                owners.push_back(fmt::format("{}{:04X}/{:06X}@{}", tb == csp.currentTaskBlock() ? "*" : "",
                                             m_.state.readHalf(tb + TaskBlock::kOffTaskId), tb, frame.depth));
        tb = m_.state.readAddr24(tb + TaskBlock::kOffQueue39Link);
    }
    if (owners.empty()) return "-";
    std::string joined;
    for (std::size_t i = 0; i < owners.size(); i++) joined += (i ? "," : "") + owners[i];
    return joined;
}

std::vector<std::string> MonitorCli::moduleOwnerRows(int programBlock)
{
    auto& csp = m_.nativeControlStorage();
    std::vector<std::string> owners;
    std::set<int> seen;
    int tb = m_.state.readAddr24(GuestLowStorage::queueHeader(39));
    while (tb != 0 && seen.size() < 4096 && seen.insert(tb).second) {
        if (!TaskBlock::isTaskBlock(m_.state, tb)) break;
        for (const auto& frame : requestBlockChain(tb))
            if (frame.programBlock == programBlock)
                owners.push_back(fmt::format("task {:04X}/{:06X}{}; rb {:06X}; depth {}; resume {:04X}",
                                             m_.state.readHalf(tb + TaskBlock::kOffTaskId), tb,
                                             tb == csp.currentTaskBlock() ? " (current)" : "", frame.requestBlock,
                                             frame.depth, frame.resumeIar));
        tb = m_.state.readAddr24(tb + TaskBlock::kOffQueue39Link);
    }
    return owners;
}

void MonitorCli::whereIs(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    int task = csp.currentTaskBlock();
    int iar = m_.state.msp.iar;
    if (a.size() == 2) {
        iar = parseHex32(a[1]);
    } else if (a.size() >= 3) {
        task = parseHex32(a[1]);
        iar = parseHex32(a[2]);
    }
    std::string where = csp.describeActiveMember(task, iar);
    if (where.empty())
        fmt::print("task {:04X} at IAR {:04X}: no member attributed (no recorded transfer, or pre-SVC-10 state)\n", task, iar);
    else
        fmt::print("task {:04X} at IAR {:04X}: {}\n", task, iar, where);
}

void MonitorCli::taskList(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    int current = csp.currentTaskBlock();
    int ready = m_.state.readAddr24(GuestLowStorage::queueHeader(39));

    std::vector<int> tasks;
    int at = ready, guard = 0;
    while (at != 0 && guard++ < 4096 && std::find(tasks.begin(), tasks.end(), at) == tasks.end()) {
        if (m_.state.readHalf(at) != GuestLowStorage::kEyeTaskBlock) break;
        tasks.push_back(at);
        at = m_.state.readAddr24(at + TaskBlock::kOffQueue39Link);
    }

    if (a.size() >= 2) {
        if (equalsIgnoreCase(a[1], "current")) {
            if (current == 0 || std::find(tasks.begin(), tasks.end(), current) == tasks.end()) {
                fmt::print("no current queue-39 task\n");
                return;
            }
            taskDetail(current, current);
            return;
        }
        int want = parseHex32(a[1]);
        int tb = 0;
        for (int t : tasks)
            if (t == want || m_.state.readHalf(t + TaskBlock::kOffTaskId) == want) {
                tb = t;
                break;
            }
        if (tb == 0) {
            fmt::print("no task with id/address {:X}\n", want);
            return;
        }
        taskDetail(tb, current);
        return;
    }

    fmt::print("{} task(s) on queue 39  (* = current)\n", tasks.size());
    fmt::print("  tb     id    pri  tb+4 tb+5  rb      state\n");
    for (int tb : tasks) {
        uint8_t st = m_.state.readByte(tb + TaskBlock::kOffState);
        uint8_t st2 = m_.state.readByte(tb + TaskBlock::kOffStat2);
        std::string state = (st & 0x80) != 0 ? fmt::format("waiting on {:02X}", st2) : std::string("ready");
        fmt::print("{} {:04X}  {:04X}  {:02X}   {:02X}   {:02X}    {:06X}  {}\n", tb == current ? "*" : " ", tb,
                   m_.state.readHalf(tb + TaskBlock::kOffTaskId), m_.state.readByte(tb + TaskBlock::kOffPriority), st, st2,
                   m_.state.readAddr24(tb + TaskBlock::kOffRequestBlock), state);
    }
}

void MonitorCli::taskDetail(int tb, int current)
{
    auto& st = m_.state;
    fmt::print("task block {:04X}{}\n", tb, tb == current ? "  (CURRENT)" : "");
    fmt::print("  +0  eyecatcher  {:04X} {}\n", st.readHalf(tb), st.readHalf(tb) == GuestLowStorage::kEyeTaskBlock ? "(TB)" : "(NOT TB)");
    fmt::print("  +2  task id     {:04X}\n", st.readHalf(tb + TaskBlock::kOffTaskId));
    uint8_t s4 = st.readByte(tb + TaskBlock::kOffState);
    fmt::print("  +4  state       {:02X}  ({}{}{})\n", s4, (s4 & 0x80) != 0 ? "waiting " : "runnable ",
               (s4 & 0x10) != 0 ? "long-wait " : "", (s4 & 0x02) != 0 ? "ecs " : "");
    fmt::print("  +5  TB_STAT2    {:02X}  (event-wait conditions)\n", st.readByte(tb + TaskBlock::kOffStat2));
    fmt::print("  +7  priority    {:02X}\n", st.readByte(tb + TaskBlock::kOffPriority));
    fmt::print("  +17 return ACE  {:06X}\n", st.readAddr24(tb + TaskBlock::kOffReturnAce));
    fmt::print("  +24 term state  {:02X}; +32 status {:02X}; +40 disposition {:02X}\n",
               st.readByte(tb + TaskBlock::kOffTerminationState), st.readByte(tb + TaskBlock::kOffStatus), st.readByte(tb + 40));
    fmt::print("  +48 dependency  {:02X}; +52/+60 depth/pass {:02X}/{:02X}; +62 MIC {:04X}\n",
               st.readByte(tb + TaskBlock::kOffTerminationDependencyFlags), st.readByte(tb + TaskBlock::kOffTerminationDepth),
               st.readByte(tb + TaskBlock::kOffTerminationPass), st.readHalf(tb + TaskBlock::kOffMic));
    fmt::print("  +130/+150 nupterm wait halfwords {:04X}/{:04X}\n", st.readHalf(tb + TaskBlock::kOffTerminationWait130),
               st.readHalf(tb + TaskBlock::kOffTerminationWait150));
    fmt::print("  +65 request blk {:06X}\n", st.readAddr24(tb + TaskBlock::kOffRequestBlock));
    int jcb = st.readAddr24(tb + TaskBlock::kOffJobControlBlock);
    fmt::print("  +21 JCB pointer {:06X}\n", jcb);
    if (jcb != 0 && canReadGuest(jcb, JobControlBlock::kOffRegionPages + 1)) {
        uint8_t js = st.readByte(jcb + JobControlBlock::kOffStatus);
        fmt::print("    JCB+35 status {:02X}  (built={}, work-spaces={})\n", js,
                   (js & JobControlBlock::kStatusBuilt) != 0 ? "yes" : "no",
                   (js & JobControlBlock::kStatusWorkSpacesPresent) != 0 ? "yes" : "no");
        fmt::print("    JCB+36 class-state {:02X}; current-task {:06X}; JCBWSWA {:06X}\n",
                   st.readByte(jcb + JobControlBlock::kOffClassState), st.readAddr24(jcb + JobControlBlock::kOffCurrentTask),
                   st.readAddr24(jcb + JobControlBlock::kOffWorkSpaceDiskAddress));
        fmt::print("    region current/limit {}/{} page(s); ceiling {}; growth {:04X}\n",
                   JobControlBlock::currentPages(st, jcb), JobControlBlock::regionPages(st, jcb),
                   st.readByte(jcb + JobControlBlock::kOffRegionCeiling), st.readHalf(jcb + JobControlBlock::kOffGrowthCounter));
    }
    fmt::print("  +45 complete Q  {:06X}\n", st.readAddr24(tb + TaskBlock::kOffCompleteQueue));
    fmt::print("  +69 work base   {:06X}\n", st.readAddr24(tb + TaskBlock::kOffWorkBase));
    int rb = st.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (rb != 0) {
        fmt::print("  rb+24 resume IAR {:04X}  (where it runs when dispatched)\n", st.readHalf(rb + 24));
        fmt::print("  rb+36 saved WR6 {:04X}  (event-type wait filter)\n", st.readHalf(rb + 36));
    }
    fmt::print("  request-block chain (0 = active frame):\n");
    for (const auto& frame : requestBlockChain(tb))
        fmt::print("    [{}] rb {:06X} -> {:06X}; pb {:06X}; resume {:04X}; {}\n", frame.depth, frame.requestBlock, frame.previous,
                   frame.programBlock, frame.resumeIar,
                   frame.attributed ? fmt::format("{}+{:04X}", frame.member, frame.offset) : std::string("(unattributed)"));
}

// Decode the mapping owned by one task's active request block: unlike
// `show atr` this identifies the block behind every logical range, and it
// works for a parked task whose ATR file is not live.
void MonitorCli::mapState(const std::vector<std::string>& a)
{
    if (a.size() > 2) {
        fmt::print("usage: mapstate [current|task-id|task-address]\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    int current = csp.currentTaskBlock();
    int want = -1;
    if (a.size() == 2 && !equalsIgnoreCase(a[1], "current")) want = parseHex32(a[1]);

    int tb = 0;
    std::set<int> seen;
    for (int at = st.readAddr24(GuestLowStorage::queueHeader(39)); at != 0 && seen.insert(at).second;
         at = st.readAddr24(at + TaskBlock::kOffQueue39Link)) {
        if (!TaskBlock::isTaskBlock(st, at)) break;
        if ((want < 0 && at == current) || want == at || want == st.readHalf(at + TaskBlock::kOffTaskId)) {
            tb = at;
            break;
        }
    }
    if (tb == 0) {
        if (want < 0) fmt::print("no current queue-39 task\n");
        else fmt::print("no task with id/address {:X}\n", want);
        return;
    }

    int rb = st.readAddr24(tb + TaskBlock::kOffRequestBlock);
    if (rb == 0 || !canReadGuest(rb, RequestBlock::kOffProgramBlock + 3) || st.readHalf(rb) != GuestLowStorage::kEyeRequestBlock) {
        fmt::print("mapstate: task {:06X} has no valid active request block\n", tb);
        return;
    }
    int pb = st.readAddr24(rb + RequestBlock::kOffProgramBlock);
    if (pb == 0 || !canReadGuest(pb, ProgramBlock::kBytes)) {
        fmt::print("mapstate: request block {:06X} has no readable program block\n", rb);
        return;
    }

    int count = MapTable::count(st, rb);
    int table = MapTable::base(st, rb, pb);
    int end = MapTable::blockEnd(st, rb);
    fmt::print("mapstate task {:06X} id {:04X}{}: rb {:06X}, pb {:06X}, {} entr{}, table {:06X}..{:06X}\n", tb,
               st.readHalf(tb + TaskBlock::kOffTaskId), tb == current ? " (current)" : "", rb, pb, count,
               count == 1 ? "y" : "ies", table, end);

    for (int i = 0; i < count; i++) {
        int entry = table + i * MapTable::kEntryBytes;
        if (entry + MapTable::kEntryBytes > end || !canReadGuest(entry, MapTable::kEntryBytes)) {
            fmt::print("  [{}] {:06X}: outside the request block\n", i, entry);
            break;
        }
        int first = st.readByte(entry + MapTable::kOffStartPage);
        int pages = st.readByte(entry + MapTable::kOffPages);
        int displacement = st.readHalf(entry + MapTable::kOffDisplacement);
        int block = st.readAddr24(entry + MapTable::kOffBlock);
        uint16_t eye = canReadGuest(block, 2) ? st.readHalf(block) : static_cast<uint16_t>(0);
        const char* kind = eye == GuestLowStorage::kEyeSystemBlock ? "SB" : eye == GuestLowStorage::kEyeProgramBlock ? "PB" : "??";
        std::string detail = eye == GuestLowStorage::kEyeSystemBlock && canReadGuest(block, StorageBlock::kOffSizePages + 2)
                                 ? fmt::format(" type {:02X}, {} page(s)", st.readByte(block + StorageBlock::kOffType),
                                               st.readHalf(block + StorageBlock::kOffSizePages))
                                 : std::string();
        fmt::print("  [{}] {:06X}: logical pages {}..{} -> {} {:06X} from object page {}{}\n", i, entry, first,
                   first + std::max(0, pages - 1), kind, block, displacement, detail);
    }

    fmt::print("  live ATR 08..0B:");
    for (int page = 8; page <= 11; page++) fmt::print(" {:04X}", st.atr[machine::MachineState::kAtrTaskGroup0 + page]);
    fmt::print("{}\n", tb == current ? "" : "  (current task, not selected task)");
}

// Read-only ownership view over the system queue space: the allocator's
// journal is host diagnostic metadata recording what the assign and free
// operations did.
void MonitorCli::systemQueueSpaceState(const std::vector<std::string>& a)
{
    if (a.size() > 2) {
        fmt::print("usage: sqsstate [all|hex-address]\n");
        return;
    }
    auto& heap = m_.nativeControlStorage().heap();
    std::vector<std::string> failures;
    bool valid = heap.checkInvariants(failures);
    auto allocated = heap.allocatedExtents();
    auto free = heap.freeExtents();
    fmt::print("system queue {:04X}..{:04X}: {}/{} used, {} free; {} allocation fragment(s), {} free extent(s), op {}; {}\n",
               heap.low(), heap.high(), heap.used(), heap.capacity(), heap.available(), allocated.size(), free.size(),
               heap.operationSequence(), valid ? std::string("invariants OK") : fmt::format("{} invariant failure(s)", failures.size()));
    for (const auto& failure : failures) fmt::print("  INVALID: {}\n", failure);
    for (const auto& failure : heap.invariantFailures()) fmt::print("  HISTORY: {}\n", failure);

    bool all = a.size() == 2 && equalsIgnoreCase(a[1], "all");
    int address = -1;
    if (a.size() == 2 && !all) address = parseHex32(a[1]);
    if (!all && address < 0) return;

    for (const auto& e : allocated) {
        if (!all && !(e.address <= address && address < e.address + e.length)) continue;
        fmt::print("  allocated {:04X}..{:04X} ({:X} bytes), allocation #{}, request {:X}{}\n", e.address, e.address + e.length - 1,
                   e.length, e.allocation, e.requested, e.owner.empty() ? std::string() : ", " + e.owner);
    }
    for (const auto& e : free) {
        if (!all && !(e.address <= address && address < e.address + e.length)) continue;
        fmt::print("  free      {:04X}..{:04X} ({:X} bytes)\n", e.address, e.address + e.length - 1, e.length);
    }
    if (!all && address >= 0) {
        bool found = false;
        for (const auto& e : allocated) found |= e.address <= address && address < e.address + e.length;
        for (const auto& e : free) found |= e.address <= address && address < e.address + e.length;
        if (!found) fmt::print("  {:04X} is the reserved heap header or an uncovered gap\n", address);
    }
}

void MonitorCli::modules(const std::vector<std::string>& a)
{
    if (a.size() > 2) {
        fmt::print("usage: modules [active|loaded|<name>]\n");
        return;
    }
    std::string selector = a.size() == 2 ? a[1] : std::string();
    bool activeOnly = !selector.empty() && equalsIgnoreCase(selector, "active");
    bool loadedOnly = !selector.empty() && equalsIgnoreCase(selector, "loaded");
    std::string name = activeOnly || loadedOnly ? std::string() : selector;
    if (!loadedOnly) modulesActive(name);
    if (!activeOnly) modulesLoaded(name);
}

void MonitorCli::moduleStorage(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("usage: modstorage\n");
        return;
    }
    for (const auto& row : m_.nativeControlStorage().moduleStorageDiagnostics()) fmt::print("{}\n", row);
}

void MonitorCli::modulesActive(const std::string& name)
{
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    std::vector<std::string> rows;
    int matchedTasks = 0;
    std::set<int> seen;
    int at = st.readAddr24(GuestLowStorage::queueHeader(39));
    for (int guard = 0; at != 0 && guard++ < 4096 && seen.insert(at).second;) {
        if (!TaskBlock::isTaskBlock(st, at)) break;
        int tb = at;
        int rb = st.readAddr24(tb + TaskBlock::kOffRequestBlock);
        int pb = csp.getActiveProgramBlock(tb);
        int iar = rb == 0 ? 0 : st.readHalf(rb + RequestBlock::kOffIar);
        auto frames = requestBlockChain(tb);
        LoadedMember member;
        int offset = 0;
        bool attributed = csp.tryActiveMember(tb, iar, member, offset);
        bool matches = name.empty();
        if (!matches)
            for (const auto& f : frames)
                if (f.attributed && memberNameMatches(f.member, name)) matches = true;
        if (matches) {
            matchedTasks++;
            std::string where = attributed ? fmt::format("{}+{:04X}", member.name, offset) : std::string("(unattributed)");
            int jcb = st.readAddr24(tb + TaskBlock::kOffJobControlBlock);
            std::string jstate = jcb != 0 && canReadGuest(jcb, JobControlBlock::kOffClassState + 1)
                                     ? fmt::format("{:06X}:{:02X}/{:02X}", jcb, st.readByte(jcb + JobControlBlock::kOffStatus),
                                                   st.readByte(jcb + JobControlBlock::kOffClassState))
                                     : std::string("------:--/--");
            rows.push_back(fmt::format("{} {:06X} {:04X}  {:02X}/{:02X}  {:06X} {:06X} {:04X}  {:<16} {}",
                                       tb == csp.currentTaskBlock() ? "*" : " ", tb, st.readHalf(tb + TaskBlock::kOffTaskId),
                                       st.readByte(tb + TaskBlock::kOffState), st.readByte(tb + TaskBlock::kOffStat2), rb, pb, iar,
                                       jstate, where));
            for (const auto& frame : frames)
                rows.push_back(fmt::format("    rb[{}] {:06X} -> {:06X}  pb {:06X} resume {:04X}  {}", frame.depth,
                                           frame.requestBlock, frame.previous, frame.programBlock, frame.resumeIar,
                                           frame.attributed ? fmt::format("{}+{:04X}", frame.member, frame.offset)
                                                            : std::string("(unattributed)")));
        }
        at = st.readAddr24(tb + TaskBlock::kOffQueue39Link);
    }

    fmt::print("active modules: {} queue-39 task(s){}  (* = current)\n", matchedTasks, name.empty() ? "" : " matching " + name);
    fmt::print("  tb     id    state  rb     pb     resume  jcb:status/class member+offset\n");
    for (const auto& row : rows) fmt::print("{}\n", row);
}

void MonitorCli::modulesLoaded(const std::string& name)
{
    auto& csp = m_.nativeControlStorage();
    auto modules = csp.getLoadedProgramBlocks();
    modules.erase(std::remove_if(modules.begin(), modules.end(),
                                 [&](const auto& p) { return !name.empty() && !memberNameMatches(p.name, name); }),
                  modules.end());
    std::stable_sort(modules.begin(), modules.end(), [](const auto& x, const auto& y) {
        std::string a = toLower(x.name), b = toLower(y.name);
        if (a != b) return a < b;
        return x.programBlock < y.programBlock;
    });

    fmt::print("loaded modules: {} program block(s){}\n", modules.size(), name.empty() ? "" : " matching " + name);
    fmt::print("  pb     name   extent  logical physical pb-sector attr pages ready owners(id/tb@rb-depth)\n");
    for (const auto& p : modules)
        fmt::print("  {:06X} {:<5}  {:>6}  {:06X}  {:06X}  {:06X}    {:02X}   {}/{} {}  {}\n", p.programBlock, p.name,
                   p.extentSector, p.logicalBase, p.physicalBase, p.programBlockSector, p.attribute, p.pagesReady, p.pageCount,
                   p.ready ? "yes" : "no", moduleOwners(p.programBlock));
}

// Exact, read-only residency and ownership query.  An SSP load member and an
// SVC-50 control-storage transient are separate forms: only the former can
// own a guest program block.
void MonitorCli::residency(const std::vector<std::string>& a)
{
    if (a.size() != 3) {
        fmt::print("usage: residency program <exact-name> | residency transient <hex-id>\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();

    if (equalsIgnoreCase(a[1], "transient")) {
        const std::string& h = a[2];
        bool hex = !h.empty() && h.size() <= 8;
        for (char c : h)
            if (!std::isxdigit(static_cast<unsigned char>(c))) hex = false;
        long id = hex ? std::strtol(h.c_str(), nullptr, 16) : -1;
        if (!hex || id < 0 || id > 0xFF) {
            fmt::print("residency: transient id must be hex 00-FF\n");
            return;
        }
        if (id > TransientArea::kMaxId) {
            fmt::print("transient {:02X}: OUT OF RANGE (SVC 50 error 55); not an SSP program and has no program block\n", id);
            return;
        }
        const char* body = TransientArea::isErrorStub(static_cast<uint8_t>(id)) ? "SLIC error-table entry"
                           : TransientArea::hasEmulatedBody(static_cast<uint8_t>(id)) ? "implemented"
                                                                                       : "missing";
        fmt::print("transient {:02X} ({}): native control-storage ID; not an SSP program and has no program block\n", id,
                   TransientArea::name(static_cast<uint8_t>(id)));
        fmt::print("  emulator body {}; transient area busy {}; queue depth {}\n", body, csp.transients().busy() ? "yes" : "no",
                   csp.transients().queueDepth());
        return;
    }

    if (!equalsIgnoreCase(a[1], "program")) {
        fmt::print("usage: residency program <exact-name> | residency transient <hex-id>\n");
        return;
    }

    const std::string& wanted = a[2];
    auto all = csp.getLoadedProgramBlocks();
    std::vector<decltype(all)::value_type> matches;
    for (const auto& p : all)
        if (equalsIgnoreCase(p.name, wanted)) matches.push_back(p);
    std::sort(matches.begin(), matches.end(), [](const auto& x, const auto& y) { return x.programBlock < y.programBlock; });
    if (matches.empty()) {
        fmt::print("program {}: NOT RESIDENT - no loader-attributed program block (this says nothing about whether the member "
                   "exists on disk)\n",
                   wanted);
        return;
    }
    fmt::print("program {}: RESIDENT in {} loader-attributed program block(s)\n", matches[0].name, matches.size());
    for (const auto& p : matches) {
        fmt::print("  pb {:06X}: extent {}; logical {:06X}; physical {:06X}; pages {}/{}; ready {}; attr {:02X}\n", p.programBlock,
                   p.extentSector, p.logicalBase, p.physicalBase, p.pagesReady, p.pageCount, p.ready ? "yes" : "no", p.attribute);
        auto owners = moduleOwnerRows(p.programBlock);
        if (owners.empty())
            fmt::print("    owners: none (resident but no queue-39 RB frame refers to it)\n");
        else
            for (const auto& owner : owners) fmt::print("    owner {}\n", owner);
    }
}

// `smf status [task]` joins the three independent parts of the measurement
// contract without changing any of them; the action verbs drive the native
// system-measurement action.
void MonitorCli::systemMeasurement(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    std::string verb = a.size() > 1 ? toLower(a[1]) : "status";
    uint8_t action;
    if (verb == "status") {
        if (a.size() > 3) {
            fmt::print("usage: smf status [task-id-or-address-hex]\n");
            return;
        }
        printSystemMeasurementStatus(a.size() == 3 ? parseHex32(a[2]) : -1);
        return;
    } else if (verb == "size1") {
        action = 1;
    } else if (verb == "start") {
        action = 2;
    } else if (verb == "retrieve") {
        action = 3;
    } else if (verb == "stop") {
        action = 4;
    } else if (verb == "size5") {
        action = 5;
    } else {
        fmt::print("usage: smf [status [id|addr]|size1|size5|start|retrieve|stop]\n");
        return;
    }
    bool accepted = csp.applySystemMeasurementAction(action, "monitor smf " + verb);
    fmt::print("$NUSMF action {} {}; native SM sizing {}\n", action, accepted ? "accepted" : "rejected",
               csp.systemMeasurementEnabled() ? "initialized" : "not initialized");
}

void MonitorCli::printSystemMeasurementStatus(int selector)
{
    auto& csp = m_.nativeControlStorage();
    auto& st = m_.state;
    uint8_t action = st.readByte(0x08B4);
    fmt::print("system measurement state (read-only)\n");
    fmt::print("  guest 08B4={:02X}; 08B4.80={} (SSP-owned smfSize/smfStop input)\n", action, (action & 0x80) != 0 ? "set" : "clear");
    fmt::print("  emulator NuEmul+03D5.80 analogue={} (native nuptask allocation gate)\n",
               csp.systemMeasurementEnabled() ? "set" : "clear");
    fmt::print("  tb     id    tb+49   base    eye  length result\n");

    int matched = 0;
    std::set<int> seen;
    int tb = st.readAddr24(GuestLowStorage::queueHeader(39));
    for (int guard = 0; tb != 0 && guard++ < 4096 && seen.insert(tb).second;) {
        if (!TaskBlock::isTaskBlock(st, tb)) break;
        int id = st.readHalf(tb + TaskBlock::kOffTaskId);
        if (selector < 0 || selector == tb || selector == id) {
            printTaskMeasurementStatus(tb, id);
            matched++;
        }
        tb = st.readAddr24(tb + TaskBlock::kOffQueue39Link);
    }
    if (selector >= 0 && matched == 0)
        fmt::print("  no task with id/address {:X}\n", selector);
    else
        fmt::print("  {} task measurement record(s)\n", matched);
}

void MonitorCli::printTaskMeasurementStatus(int tb, int id)
{
    constexpr int kRecordLength = 64;
    constexpr uint16_t kEyeSm = 0xE2D4;   // "SM"
    auto& st = m_.state;
    int pointer = st.readAddr24(tb + TaskBlock::kOffMeasurementBlock);
    if (pointer == 0) {
        fmt::print("  {:06X} {:04X}  000000  ------  ---- ----   absent\n", tb, id);
        return;
    }
    // TB+49 is also the chained-allocation queue head: every element is
    // represented by the LAST byte of its trailing 3-byte link, with the
    // rounded length at +1..+2.  An SM record is merely the first possible
    // element, with the fixed length 64 and eye "SM".
    if (pointer < 2 || pointer > st.backingBytes() - 3) {
        fmt::print("  {:06X} {:04X}  {:06X}  ------  ---- ----   invalid queue pointer\n", tb, id, pointer);
        return;
    }
    uint16_t length = st.readHalf(pointer + 1);
    int recordBase = pointer + 3 - length;
    bool allocationShape = length >= 16 && (length & 15) == 0 && recordBase >= 0 && recordBase <= st.backingBytes() - length;
    if (!allocationShape) {
        fmt::print("  {:06X} {:04X}  {:06X}  ------  ---- {:04X}   invalid allocation cell\n", tb, id, pointer, length);
        return;
    }
    uint16_t eye = st.readHalf(recordBase);
    const char* result = eye == kEyeSm && length == kRecordLength ? "valid SM/64" : "allocation head (no SM)";
    fmt::print("  {:06X} {:04X}  {:06X}  {:06X}  {:04X} {:04X}   {}\n", tb, id, pointer, recordBase, eye, length, result);
}

// Decode the variable-sized allocation queue rooted in TB+49..51, whose
// elements are represented by the last byte of their trailing link.
void MonitorCli::allocationChain(const std::vector<std::string>& a)
{
    if (a.size() != 2) {
        fmt::print("usage: allocchain <task-id-or-address-hex>\n");
        return;
    }
    auto& st = m_.state;
    int want = parseHex32(a[1]);
    int tb = 0;
    std::set<int> seenTasks;
    int at = st.readAddr24(GuestLowStorage::queueHeader(39));
    for (int guard = 0; at != 0 && guard++ < 4096 && seenTasks.insert(at).second;) {
        if (!TaskBlock::isTaskBlock(st, at)) break;
        if (at == want || st.readHalf(at + TaskBlock::kOffTaskId) == want) {
            tb = at;
            break;
        }
        at = st.readAddr24(at + TaskBlock::kOffQueue39Link);
    }
    if (tb == 0) {
        fmt::print("no task with id/address {:X}\n", want);
        return;
    }

    int pointer = st.readAddr24(tb + TaskBlock::kOffMeasurementBlock);
    fmt::print("task {:06X} id {:04X}: TB+49..51 allocation head {:06X}\n", tb, st.readHalf(tb + TaskBlock::kOffTaskId), pointer);
    fmt::print("  cell    base    size eye next    result\n");
    std::set<int> seen;
    for (int guard = 0; pointer != 0 && guard++ < 4096;) {
        if (!seen.insert(pointer).second) {
            fmt::print("  {:06X}  ------  ---- ---- ------  cycle\n", pointer);
            return;
        }
        if (pointer < 2 || pointer > st.backingBytes() - 3) {
            fmt::print("  {:06X}  ------  ---- ---- ------  outside storage\n", pointer);
            return;
        }
        int next = st.readAddr24(pointer - 2);
        uint16_t size = st.readHalf(pointer + 1);
        int block = pointer + 3 - size;
        bool valid = size >= 16 && (size & 15) == 0 && block >= 0 && block <= st.backingBytes() - size && (block & 15) == 0;
        uint16_t eye = valid ? st.readHalf(block) : static_cast<uint16_t>(0);
        if (valid)
            fmt::print("  {:06X}  {:06X}  {:04X} {:04X} {:06X}  {}\n", pointer, block, size, eye, next,
                       eye == 0xE2D4 && size == 64 ? "SM" : "allocation");
        else
            fmt::print("  {:06X}  ------  {:04X} ---- {:06X}  invalid size/base\n", pointer, size, next);
        if (!valid) return;
        pointer = next;
    }
    if (pointer == 0) fmt::print("  end\n");
    else fmt::print("  stopped after 4096 elements\n");
}

// `breakm <member> <offset> [name] | breakm list | breakm clear <member> <offset>`.
// A member not loaded yet is the normal case for anything that appears
// mid-run, so the breakpoint is deferred and arms itself the first time the
// loader attributes the member.
void MonitorCli::breakMember(const std::vector<std::string>& a)
{
    if (a.size() == 1 || equalsIgnoreCase(a[1], "list")) {
        auto bps = m_.msp().memberBreakpoints();
        std::stable_sort(bps.begin(), bps.end(), [](const auto& x, const auto& y) {
            std::string p = toLower(x.member), q = toLower(y.member);
            if (p != q) return p < q;
            return x.offset < y.offset;
        });
        if (bps.empty()) {
            fmt::print("no member breakpoints\n");
            return;
        }
        fmt::print("{} member breakpoint(s):\n", bps.size());
        for (const auto& bp : bps)
            fmt::print("  {}+{:04X} -> IAR {:04X}{}\n", bp.member, bp.offset, bp.address,
                       bp.description.empty() ? std::string() : "  " + bp.description);
        return;
    }

    bool clear = equalsIgnoreCase(a[1], "clear");
    std::size_t memberArg = clear ? 2 : 1;
    std::size_t offsetArg = clear ? 3 : 2;
    if (a.size() <= offsetArg) {
        fmt::print("usage: breakm <member> <offset> [name] | breakm list | breakm clear <member> <offset>\n");
        return;
    }
    std::string memberName = a[memberArg];
    int offset = parseHex32(a[offsetArg]);
    if (offset < 0 || offset > 0xFFFF) {
        fmt::print("breakm: offset must be 0000-FFFF\n");
        return;
    }
    std::string description;
    for (std::size_t i = offsetArg + 1; i < a.size(); i++) description += (i > offsetArg + 1 ? " " : "") + a[i];

    auto& csp = m_.nativeControlStorage();
    auto all = csp.getLoadedProgramBlocks();
    std::vector<decltype(all)::value_type> matches;
    for (const auto& p : all)
        if (memberNameMatches(p.name, memberName)) matches.push_back(p);
    if (matches.empty()) {
        if (!clear) {
            pendingMemberBreaks_.push_back({memberName, offset, description});
            fmt::print("breakm: '{}' is not loaded yet - breakpoint PENDING at {}+{:04X}; it arms when the loader attributes the "
                       "member\n",
                       memberName, memberName, offset);
        } else {
            fmt::print("breakm: member '{}' is not loader-attributed\n", memberName);
        }
        return;
    }
    int logicalBase = matches[0].logicalBase;
    for (const auto& p : matches)
        if (p.logicalBase != logicalBase) {
            fmt::print("breakm: member '{}' has multiple logical bases; cannot resolve one IAR\n", matches[0].name);
            return;
        }
    int address = logicalBase + offset;
    if (address > 0xFFFF) {
        fmt::print("breakm: {}+{:04X} is outside the 16-bit IAR\n", matches[0].name, offset);
        return;
    }
    if (clear) {
        if (m_.msp().clearMemberBreakpoint(static_cast<uint16_t>(address), matches[0].name))
            fmt::print("cleared member breakpoint {}+{:04X} at IAR {:04X}\n", matches[0].name, offset, address);
        else
            fmt::print("no member breakpoint {}+{:04X}\n", matches[0].name, offset);
        return;
    }
    m_.msp().setMemberBreakpoint(static_cast<uint16_t>(address), static_cast<uint16_t>(offset), matches[0].name, description);
    fmt::print("member breakpoint set at {}+{:04X} -> IAR {:04X}{}\n", matches[0].name, offset, address,
               description.empty() ? std::string() : " (" + description + ")");
}

// Arm any deferred member breakpoint whose member has since been loaded.
void MonitorCli::armPendingMemberBreaks()
{
    if (pendingMemberBreaks_.empty()) return;
    auto& csp = m_.nativeControlStorage();
    for (std::size_t i = pendingMemberBreaks_.size(); i-- > 0;) {
        PendingMemberBreak p = pendingMemberBreaks_[i];
        auto all = csp.getLoadedProgramBlocks();
        std::vector<decltype(all)::value_type> m;
        for (const auto& x : all)
            if (memberNameMatches(x.name, p.member)) m.push_back(x);
        if (m.empty()) continue;
        int logicalBase = m[0].logicalBase;
        bool ambiguous = false;
        for (const auto& x : m)
            if (x.logicalBase != logicalBase) ambiguous = true;
        if (ambiguous) continue;
        int address = logicalBase + p.offset;
        if (address > 0xFFFF) {
            fmt::print("breakm: {}+{:04X} is outside the 16-bit IAR; dropped\n", m[0].name, p.offset);
            pendingMemberBreaks_.erase(pendingMemberBreaks_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        m_.msp().setMemberBreakpoint(static_cast<uint16_t>(address), static_cast<uint16_t>(p.offset), m[0].name, p.description);
        fmt::print("breakm: pending breakpoint armed at {}+{:04X} -> IAR {:04X}\n", m[0].name, p.offset, address);
        pendingMemberBreaks_.erase(pendingMemberBreaks_.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void MonitorCli::transferById(const std::vector<std::string>& a)
{
    if (a.size() != 2) {
        fmt::print("xferid <hex-id>\n");
        return;
    }
    int id = parseHex32(a[1]);
    if (id < 0 || id > 0xFF) throw MonitorError("Value was either too large or too small for an unsigned byte.");
    auto& csp = m_.nativeControlStorage();
    if (csp.currentRequestBlock() == 0) {
        fmt::print("no current request block - `boot` the machine first\n");
        return;
    }
    SvcRequest req;
    req.r = 0x04;
    req.q = 0;
    req.inline1 = static_cast<uint8_t>(id);
    req.dispatch = m_.controlStorage().classify(0x04);
    bool ok = m_.controlStorage().svc(req);
    fmt::print("EXPERIMENT xferid {:02X}: SVC 04 from current task -> {}; live XR1={:04X} XR2={:04X}\n", id,
               ok ? "transferred" : "rejected", m_.state.msp.xr1, m_.state.msp.xr2);
}

void MonitorCli::transferTerminationContinuation(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("xferterm\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    if (csp.currentRequestBlock() == 0) {
        fmt::print("no current request block - `boot` the machine first\n");
        return;
    }
    bool ok = csp.enterTerminationContinuationExperiment();
    fmt::print("EXPERIMENT xferterm: nupterm internal slot 4 -> {}; no CTE request object was synthesized\n",
               ok ? "transferred" : "rejected");
}

void MonitorCli::terminationDependencyScan(const std::vector<std::string>& a)
{
    if (a.size() != 1) {
        fmt::print("nuptermscan\n");
        return;
    }
    auto& csp = m_.nativeControlStorage();
    if (csp.currentTaskBlock() == 0) {
        fmt::print("no current task block - `boot` the machine first\n");
        return;
    }
    int dying = csp.currentTaskBlock();
    int immediate = 0;
    int selected = csp.scanTerminationDependentsExperiment(immediate);
    fmt::print("EXPERIMENT nuptermscan: dying task {:04X}; selected {}, immediate {}, deferred {}\n", dying, selected, immediate,
               selected - immediate);
}

void MonitorCli::actions()
{
    m_.nativeControlStorage().actionControllerReport([](const std::string& line) { fmt::print("{}\n", line); });
}

// Read the native timer table, or explicitly service its due handlers as a
// diagnostic.
void MonitorCli::timers(const std::vector<std::string>& a)
{
    auto& csp = m_.nativeControlStorage();
    if (a.size() > 2 || (a.size() == 2 && !equalsIgnoreCase(a[1], "service"))) {
        fmt::print("usage: timers [service]\n");
        return;
    }
    if (a.size() == 2) {
        int expired = 0;
        bool readied = csp.serviceDueNativeTimers(expired);
        fmt::print("serviced {} due native timer callback(s); SSP task readied={}\n", expired, readied ? "yes" : "no");
    }
    auto timers = csp.nativeTimerState();
    fmt::print("native timers: {}; next due in {} ms\n", timers.size(), csp.millisecondsUntilNextNativeTimer());
    fmt::print("  TRB    TB     RB     ctl key type initial remaining due-ms expiries disposition\n");
    for (const auto& t : timers)
        fmt::print("  {:06X} {:06X} {:06X}  {:02X}  {:02X}   {:X}  {:>7} {:>9} {:>6} {:>8} {}\n", t.trb, t.taskBlock, t.requestBlock,
                   t.control, t.key, t.type, t.originalInterval, t.remainingUnits, t.millisecondsUntilDue, t.expirationCount,
                   t.expiryDisposition);
}

// The per-request-block ATR files: `show atr` prints the live registers;
// this prints who OWNS what.
void MonitorCli::showPtt()
{
    auto& csp = m_.nativeControlStorage();
    auto& pool = csp.translationFiles();
    fmt::print("ATR files: {} constructed, {} free\n", pool.constructed(), pool.freeCount());
    for (const auto& f : pool.files()) {
        for (int first = 0; first < processors::controlstorage::NuPtt::kAtrCount; first += 8) {
            std::string line = first == 0 ? fmt::format("  file {:04X}  owner {:04X} ", f->handle(), f->owner)
                                          : fmt::format("                    +{:02X} ", first);
            for (int i = first; i < std::min(first + 8, processors::controlstorage::NuPtt::kAtrCount); i++)
                line += fmt::format(" {:04X}", f->atr[i]);
            fmt::print("{}\n", line);
        }
    }
}

}  // namespace sim36::monitor
