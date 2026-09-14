// Everything the emulator learns about itself goes through here.  Tracing is
// the whole point of a reference implementation: it exists to be compared
// against, so what it did must be inspectable.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace sim36::monitor {

enum TraceFlags : uint32_t {
    TraceNone = 0,
    TraceMsp = 1u << 0,
    TraceSvc = 1u << 1,
    TraceCsp = 1u << 2,
    TraceDisk = 1u << 3,
    TraceWs = 1u << 4,
    TraceAce = 1u << 5,
    TraceSched = 1u << 6,
    TraceSrc = 1u << 7,
    // Every computed IAR jump, logged as from -> to + member.
    TraceFlow = 1u << 8,
    // Full instruction trace with post-execute register effects.
    TraceIsn = 1u << 9,
    // Work-station output provenance.
    TraceOutput = 1u << 10,
    // A MODIFIER, not a category: every enabled category is written to an
    // in-memory ring instead of the console, printed only when the machine
    // stops, so that tracing does not slow a timing-dependent IPL down.
    TraceDefer = 1u << 11,
    TraceAll = 0x7FF,
};

// Parses "msp,svc,..." (also '+' and ' ' separators).  Throws
// std::invalid_argument for an unknown class.
uint32_t parseTraceFlags(const std::string& s);

// Renders flags the way the reference's [Flags] enum did: an exact named
// value prints its name, otherwise the matched names in ascending order,
// comma-separated, and "None" for zero.
std::string traceFlagsToString(uint32_t flags);

class Tracer {
public:
    std::atomic<uint32_t> flags{TraceNone};

    void to(std::FILE* out) { out_ = out; }
    bool on(uint32_t f) const { return (flags.load(std::memory_order_relaxed) & f) != 0; }
    long long lines() const { return lines_; }

    int deferred();
    std::vector<std::string> copyDeferred();
    void flushDeferred(const std::string& why);

    // Emit a line UNCONDITIONALLY, through the same output channel.
    template <typename... Args>
    void line(const char* tag, fmt::format_string<Args...> fmtStr, Args&&... args)
    {
        write(tag, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    bool srcTrace() const { return on(TraceSrc); }
    bool flowOn() const { return on(TraceFlow); }
    bool isnOn() const { return on(TraceIsn); }
    bool outputOn() const { return on(TraceOutput); }

    template <typename... Args>
    void msp(int iar, fmt::format_string<Args...> f, Args&&... args)
    {
        if (!on(TraceMsp)) return;
        emit(TraceMsp, "msp", fmt::format("{:04X}  {}", iar, fmt::format(f, std::forward<Args>(args)...)));
    }
#define SIM36_TRACE_METHOD(name, flag, tag)                                       \
    template <typename... Args>                                                   \
    void name(fmt::format_string<Args...> f, Args&&... args)                      \
    {                                                                             \
        if (!on(flag)) return;                                                    \
        emit(flag, tag, fmt::format(f, std::forward<Args>(args)...));             \
    }
    SIM36_TRACE_METHOD(flow, TraceFlow, "flow")
    SIM36_TRACE_METHOD(isn, TraceIsn, "isn")
    SIM36_TRACE_METHOD(csp, TraceCsp, "csp")
    SIM36_TRACE_METHOD(svc, TraceSvc, "svc")
    SIM36_TRACE_METHOD(diskIo, TraceDisk, "disk")
    SIM36_TRACE_METHOD(ws, TraceWs, "ws")
    SIM36_TRACE_METHOD(output, TraceOutput, "out")
    SIM36_TRACE_METHOD(ace, TraceAce, "ace")
    SIM36_TRACE_METHOD(sched, TraceSched, "sch")
    SIM36_TRACE_METHOD(src, TraceSrc, "src")
#undef SIM36_TRACE_METHOD

private:
    void emit(uint32_t f, const char* tag, const std::string& msg);
    void write(const char* tag, const std::string& msg);

    static constexpr int kDeferredCapacity = 200000;
    std::FILE* out_ = stdout;
    long long lines_ = 0;
    // Serialised because host listeners trace from their own threads while
    // the guest is stepped from another.
    std::mutex gate_;
    std::vector<std::string> deferred_;
    int deferredNext_ = 0;
    int deferredCount_ = 0;
};

}  // namespace sim36::monitor
