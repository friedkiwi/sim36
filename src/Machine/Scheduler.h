// A discrete-event scheduler over a monotonic virtual clock.
//
// Deliberately not built on callbacks with subscription order or on a
// runtime's task scheduling: determinism is a requirement, because the
// differential oracle needs reproducible runs.  Ties are broken by insertion
// sequence, never by hash order.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace sim36::machine {

class Scheduler {
public:
    struct Event {
        long long at;
        long long seq;
        std::string label;
        std::function<void()> action;
    };

    long long now() const { return now_; }
    int pending() const { return static_cast<int>(heap_.size()); }

    void at(long long when, const std::string& label, std::function<void()> action);
    void after(long long delta, const std::string& label, std::function<void()> action)
    {
        at(now_ + delta, label, std::move(action));
    }

    // Run every event due at or before `until`.
    void runUntil(long long until);

    const std::vector<Event>& peek() const { return heap_; }
    void clear() { heap_.clear(); }

    // Restore a checkpoint clock; only admitted with an empty queue.
    bool restoreClock(long long now);

private:
    std::vector<Event> heap_;   // kept sorted; small queues, clarity over asymptotics
    long long seq_ = 0;
    long long now_ = 0;
};

}  // namespace sim36::machine
