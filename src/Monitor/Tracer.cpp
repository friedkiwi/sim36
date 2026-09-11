#include "Monitor/Tracer.h"

#include <stdexcept>

#include "Monitor/CommandRegistry.h"

namespace sim36::monitor {

namespace {

struct Named { const char* name; uint32_t value; };

// Ascending value order, as the reference's enum declaration.
const Named kNames[] = {
    {"None", TraceNone}, {"Msp", TraceMsp}, {"Svc", TraceSvc}, {"Csp", TraceCsp},
    {"Disk", TraceDisk}, {"Ws", TraceWs}, {"Ace", TraceAce}, {"Sched", TraceSched},
    {"Src", TraceSrc}, {"Flow", TraceFlow}, {"Isn", TraceIsn}, {"Output", TraceOutput},
    {"All", TraceAll}, {"Defer", TraceDefer},
};

}  // namespace

uint32_t parseTraceFlags(const std::string& s)
{
    uint32_t f = TraceNone;
    std::string part;
    auto flush = [&]() {
        std::string p = toLower(part);
        // trim
        std::size_t b = 0, e = p.size();
        while (b < e && p[b] == ' ') ++b;
        while (e > b && p[e - 1] == ' ') --e;
        p = p.substr(b, e - b);
        part.clear();
        if (p.empty()) return;
        if (p == "msp") f |= TraceMsp;
        else if (p == "svc") f |= TraceSvc;
        else if (p == "csp") f |= TraceCsp;
        else if (p == "disk") f |= TraceDisk;
        else if (p == "ws") f |= TraceWs;
        else if (p == "ace") f |= TraceAce;
        else if (p == "sched") f |= TraceSched;
        else if (p == "flow") f |= TraceFlow;
        else if (p == "isn" || p == "instr") f |= TraceIsn;
        else if (p == "output" || p == "out") f |= TraceOutput;
        else if (p == "src") f |= TraceSrc;
        else if (p == "defer" || p == "deferred") f |= TraceDefer;
        else if (p == "all") f |= TraceAll;
        else throw std::invalid_argument("unknown trace class '" + p + "'");
    };
    for (char c : s) {
        if (c == ',' || c == '+' || c == ' ') flush();
        else part.push_back(c);
    }
    flush();
    return f;
}

std::string traceFlagsToString(uint32_t flags)
{
    if (flags == 0) return "None";
    for (const Named& n : kNames)
        if (n.value == flags) return n.name;
    // Decompose from the largest value down, then print in ascending order.
    std::vector<const char*> matched;
    uint32_t remaining = flags;
    const std::size_t count = sizeof kNames / sizeof kNames[0];
    // kNames is not strictly sorted by value (All precedes Defer), so sort a
    // copy by value.
    std::vector<Named> sorted(kNames, kNames + count);
    for (std::size_t i = 1; i < sorted.size(); ++i)
        for (std::size_t j = i; j > 0 && sorted[j - 1].value > sorted[j].value; --j)
            std::swap(sorted[j - 1], sorted[j]);
    for (std::size_t i = sorted.size(); i-- > 0;) {
        if (sorted[i].value == 0) continue;
        if ((remaining & sorted[i].value) == sorted[i].value) {
            matched.push_back(sorted[i].name);
            remaining &= ~sorted[i].value;
        }
    }
    if (remaining != 0) return std::to_string(flags);
    std::string s;
    for (std::size_t i = matched.size(); i-- > 0;) {
        if (!s.empty()) s += ", ";
        s += matched[i];
    }
    return s;
}

void Tracer::emit(uint32_t f, const char* tag, const std::string& msg)
{
    if (!on(f)) return;
    std::lock_guard<std::mutex> lock(gate_);
    ++lines_;
    if (on(TraceDefer)) {
        if (deferred_.empty()) deferred_.resize(kDeferredCapacity);
        deferred_[static_cast<std::size_t>(deferredNext_)] = fmt::format("{:<5} {}", tag, msg);
        deferredNext_ = (deferredNext_ + 1) % kDeferredCapacity;
        if (deferredCount_ < kDeferredCapacity) ++deferredCount_;
        return;
    }
    fmt::print(out_, "{:<5} {}\n", tag, msg);
}

void Tracer::write(const char* tag, const std::string& msg)
{
    std::lock_guard<std::mutex> lock(gate_);
    ++lines_;
    fmt::print(out_, "{:<5} {}\n", tag, msg);
}

int Tracer::deferred()
{
    std::lock_guard<std::mutex> lock(gate_);
    return deferredCount_;
}

std::vector<std::string> Tracer::copyDeferred()
{
    std::lock_guard<std::mutex> lock(gate_);
    std::vector<std::string> answer(static_cast<std::size_t>(deferredCount_));
    const int first = (deferredNext_ - deferredCount_ + kDeferredCapacity) % kDeferredCapacity;
    for (int i = 0; i < deferredCount_; ++i)
        answer[static_cast<std::size_t>(i)] = deferred_[static_cast<std::size_t>((first + i) % kDeferredCapacity)];
    return answer;
}

void Tracer::flushDeferred(const std::string& why)
{
    std::lock_guard<std::mutex> lock(gate_);
    if (deferredCount_ == 0) return;
    fmt::print(out_, "--- deferred trace: last {} line(s) before {} ---\n", deferredCount_, why);
    const int first = (deferredNext_ - deferredCount_ + kDeferredCapacity) % kDeferredCapacity;
    for (int i = 0; i < deferredCount_; ++i)
        fmt::print(out_, "{}\n", deferred_[static_cast<std::size_t>((first + i) % kDeferredCapacity)]);
    fmt::print(out_, "--- end deferred trace ---\n");
    deferredCount_ = 0;
    deferredNext_ = 0;
}

}  // namespace sim36::monitor
