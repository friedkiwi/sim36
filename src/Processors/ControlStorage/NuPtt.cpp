#include "Processors/ControlStorage/NuPtt.h"

#include <algorithm>

namespace sim36::processors::controlstorage {

void NuPttPool::reset()
{
    all_.clear();
    byHandle_.clear();
    free_.clear();
}

std::vector<int> NuPttPool::captureCheckpoint() const
{
    std::vector<int> v;
    v.push_back(static_cast<int>(all_.size()));
    for (const auto& p : all_) {
        v.push_back(p->owner);
        for (uint16_t a : p->atr) v.push_back(a);
    }
    v.push_back(static_cast<int>(free_.size()));
    // Serialised top first, as a stack enumerates.
    for (auto it = free_.rbegin(); it != free_.rend(); ++it) v.push_back((*it)->handle());
    return v;
}

bool NuPttPool::restoreCheckpoint(const std::vector<int>& v)
{
    reset();
    if (v.empty()) return false;
    size_t at = 0;
    int count = v[at++];
    if (count < 0 || count > 65536 || at + static_cast<size_t>(count) * (1 + NuPtt::kAtrCount) >= v.size()) return false;
    for (int i = 0; i < count; i++) {
        auto p = std::make_unique<NuPtt>(i * NuPtt::kBytes);
        p->owner = v[at++];
        for (int n = 0; n < NuPtt::kAtrCount; n++) p->atr[n] = static_cast<uint16_t>(v[at++]);
        byHandle_[p->handle()] = p.get();
        all_.push_back(std::move(p));
    }
    int free = v[at++];
    if (free < 0 || at + static_cast<size_t>(free) != v.size()) { reset(); return false; }
    // Serialised top first; push in reverse to retain the next allocation.
    for (int i = free - 1; i >= 0; i--) {
        auto it = byHandle_.find(v[at + i]);
        if (it == byHandle_.end() || it->second->owner != 0) { reset(); return false; }
        free_.push_back(it->second);
    }
    return true;
}

NuPtt* NuPttPool::allocate(int rb)
{
    NuPtt* p;
    if (!free_.empty()) {
        // Pop the head, stamp the owner, clear the image.
        p = free_.back();
        free_.pop_back();
        std::fill(std::begin(p->atr), std::end(p->atr), static_cast<uint16_t>(0));
    } else {
        // Construct a new object; the handle is the offset from the pool base.
        auto made = std::make_unique<NuPtt>(static_cast<int>(all_.size()) * NuPtt::kBytes);
        p = made.get();
        byHandle_[p->handle()] = p;
        all_.push_back(std::move(made));
    }
    p->owner = rb;
    return p;
}

bool NuPttPool::free(int handle, int rb)
{
    NuPtt* p = owned(handle, rb);
    if (p == nullptr) return false;
    p->owner = 0;
    free_.push_back(p);
    return true;
}

void NuPttPool::rebaseFrames(uint16_t oldFrame, uint16_t newFrame, int pages)
{
    const uint16_t limit = static_cast<uint16_t>(oldFrame + pages);
    for (const auto& p : all_) {
        for (uint16_t& a : p->atr) {
            // FFFF and the other architected invalid encodings are not real
            // page frames and must remain untouched.
            if ((a & 0xE000) == 0 && a >= oldFrame && a < limit)
                a = static_cast<uint16_t>(newFrame + (a - oldFrame));
        }
    }
}

NuPtt* NuPttPool::owned(int handle, int rb)
{
    auto it = byHandle_.find(handle);
    if (it == byHandle_.end()) return nullptr;
    return it->second->owner == rb ? it->second : nullptr;
}

}  // namespace sim36::processors::controlstorage
