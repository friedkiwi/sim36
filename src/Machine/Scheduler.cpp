#include "Machine/Scheduler.h"

#include <algorithm>

namespace sim36::machine {

void Scheduler::at(long long when, const std::string& label, std::function<void()> action)
{
    if (when < now_) when = now_;
    Event e{when, seq_++, label, std::move(action)};
    heap_.push_back(std::move(e));
    std::stable_sort(heap_.begin(), heap_.end(), [](const Event& a, const Event& b) {
        return a.at != b.at ? a.at < b.at : a.seq < b.seq;
    });
}

void Scheduler::runUntil(long long until)
{
    while (!heap_.empty() && heap_.front().at <= until) {
        Event e = std::move(heap_.front());
        heap_.erase(heap_.begin());
        now_ = e.at;
        e.action();
    }
    if (now_ < until) now_ = until;
}

bool Scheduler::restoreClock(long long now)
{
    if (!heap_.empty()) return false;
    now_ = now;
    seq_ = 0;
    return true;
}

}  // namespace sim36::machine
