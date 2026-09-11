#include "Processors/ControlStorage/GuestHeap.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace sim36::processors::controlstorage {

namespace {

std::string joinStrings(const std::vector<std::string>& items)
{
    std::string s;
    for (size_t i = 0; i < items.size(); i++) {
        if (i != 0) s += ", ";
        s += items[i];
    }
    return s;
}

}  // namespace

GuestHeap::GuestHeap(machine::MachineState& m, int low, int bytes, int maximumHigh, monitor::Tracer& trace)
    : m_(m), trace_(trace)
{
    if (low < kLowestFreeableAddress || (low & (kGranularity - 1)) != 0) {
        constructionError_ = "nufree refuses that base";
        return;
    }
    if (bytes <= kHeaderBytes || (bytes & (kGranularity - 1)) != 0) {
        constructionError_ = "a system queue space that small has nothing in it";
        return;
    }
    // One 64 KB page, or exactly one page starting at its own boundary.
    if ((low & 0xFFFF) != 0 && (low >> 16) != ((low + bytes - 1) >> 16)) {
        constructionError_ = "the system queue space crosses a 64 KB boundary";
        return;
    }
    low_ = low;
    initialBytes_ = bytes;
    bytes_ = bytes;
    high_ = low + bytes;
    if (maximumHigh < high_ || ((maximumHigh - high_) & 0xFFFF) != 0) {
        constructionError_ = "the system queue extension ceiling is not a whole number of 64 KB segments";
        return;
    }
    maximumHigh_ = maximumHigh;
    valid_ = true;
    // The constructor seeds the free list with (base + 112, size - 112),
    // which decomposes into one element per power of two.
    enqueue(kHeaderBytes, bytes - kHeaderBytes);
}

void GuestHeap::clearGuest(int at, int length)
{
    if (!m_.inRange(at, length)) return;
    std::memset(m_.raw() + at, 0, static_cast<size_t>(length));
}

void GuestHeap::reset()
{
    if (!valid_) return;
    clearGuest(low_, initialBytes_);
    for (auto& list : class_) list.clear();
    free_.clear();
    allocated_.clear();
    invariantFailures_.clear();
    operationSequence_ = 0;
    allocationSequence_ = 0;
    used_ = 0;
    peak_ = 0;
    bytes_ = initialBytes_;
    high_ = low_ + bytes_;
    extensionPages_ = 0;
    enqueue(kHeaderBytes, bytes_ - kHeaderBytes);
}

int GuestHeap::available() const
{
    int n = 0;
    for (const auto& e : free_) n += e.second;
    return n;
}

std::vector<GuestHeap::AllocationExtent> GuestHeap::allocatedExtents() const
{
    std::vector<AllocationExtent> answer;
    answer.reserve(allocated_.size());
    for (const LiveExtent& e : allocated_)
        answer.push_back({low_ + e.off, e.length, e.requested, e.allocation, e.owner});
    return answer;
}

std::vector<GuestHeap::FreeElement> GuestHeap::freeExtents() const
{
    std::vector<FreeElement> answer;
    for (const auto& e : free_) answer.push_back({low_ + e.first, e.second});
    std::sort(answer.begin(), answer.end(), [](const FreeElement& a, const FreeElement& b) { return a.address < b.address; });
    return answer;
}

bool GuestHeap::checkInvariants(std::vector<std::string>& failures) const
{
    failures.clear();
    std::vector<std::pair<int, int>> free(free_.begin(), free_.end());   // a map iterates in key order
    std::vector<LiveExtent> allocated(allocated_);
    std::sort(allocated.begin(), allocated.end(), [](const LiveExtent& a, const LiveExtent& b) { return a.off < b.off; });

    int freeBytes = 0, allocatedBytes = 0, previousEnd = kHeaderBytes;
    for (const auto& e : free) {
        int off = e.first, length = e.second;
        if (off < kHeaderBytes || length <= 0 || off + length > bytes_ || (off & (kGranularity - 1)) != 0 ||
            (length & (kGranularity - 1)) != 0)
            failures.push_back(fmt::format("free extent {:04X}+{:X} is outside/alignment-invalid", low_ + off, length));
        if (off < previousEnd)
            failures.push_back(fmt::format("free extent {:04X}+{:X} overlaps preceding free space ending {:04X}",
                                           low_ + off, length, low_ + previousEnd));
        previousEnd = std::max(previousEnd, off + length);
        freeBytes += length;

        int k = classIndex(length);
        if (!isPowerOfTwo(length) || k >= kClasses || (kGranularity << k) != length || class_[k].count(off) == 0)
            failures.push_back(fmt::format("free extent {:04X}+{:X} is filed in the wrong size class", low_ + off, length));
    }
    for (int k = 0; k < kClasses; k++) {
        for (int off : class_[k]) {
            auto it = free_.find(off);
            if (it == free_.end() || it->second != (kGranularity << k))
                failures.push_back(fmt::format("class {:X} entry {:04X} has no matching free extent", kGranularity << k, low_ + off));
        }
    }

    previousEnd = kHeaderBytes;
    for (const LiveExtent& e : allocated) {
        if (e.off < kHeaderBytes || e.length <= 0 || e.off + e.length > bytes_)
            failures.push_back(fmt::format("allocation #{} {:04X}+{:X} is outside the pool", e.allocation, low_ + e.off, e.length));
        if (e.off < previousEnd)
            failures.push_back(fmt::format("allocation #{} {:04X}+{:X} overlaps preceding allocation ending {:04X}",
                                           e.allocation, low_ + e.off, e.length, low_ + previousEnd));
        previousEnd = std::max(previousEnd, e.off + e.length);
        allocatedBytes += e.length;
    }

    size_t fi = 0, ai = 0;
    while (fi < free.size() && ai < allocated.size()) {
        int fs = free[fi].first, fe = fs + free[fi].second;
        int as = allocated[ai].off, ae = as + allocated[ai].length;
        if (fs < ae && as < fe)
            failures.push_back(fmt::format("free {:04X}+{:X} overlaps allocation #{} {:04X}+{:X}", low_ + fs, free[fi].second,
                                           allocated[ai].allocation, low_ + as, allocated[ai].length));
        if (fe <= ae) fi++; else ai++;
    }

    if (freeBytes != available())
        failures.push_back(fmt::format("free-list accounting says {}, Available says {}", freeBytes, available()));
    if (allocatedBytes != used_)
        failures.push_back(fmt::format("ownership accounting says {} allocated bytes, Used says {}", allocatedBytes, used_));
    if (freeBytes + allocatedBytes != capacity())
        failures.push_back(fmt::format("coverage is {} bytes, expected capacity {}", freeBytes + allocatedBytes, capacity()));
    return failures.empty();
}

std::vector<int> GuestHeap::captureCheckpoint() const
{
    // "SQS2" adds the committed span.  Earlier checkpoints carried only
    // used/peak followed by pairs and remain readable below.
    std::vector<int> v;
    v.push_back(used_);
    v.push_back(peak_);
    v.push_back(bytes_);
    v.push_back(static_cast<int>(0x53515332));
    for (const auto& e : free_) {
        v.push_back(e.first);
        v.push_back(e.second);
    }
    return v;
}

bool GuestHeap::restoreCheckpoint(const std::vector<int>& v)
{
    if (!valid_) return false;
    if (v.size() < 2 || (v.size() & 1) != 0) return false;
    size_t firstPair = 2;
    int restoredBytes = initialBytes_;
    if (v.size() >= 4 && v[3] == static_cast<int>(0x53515332)) {
        restoredBytes = v[2];
        firstPair = 4;
    }
    if (restoredBytes < initialBytes_ || restoredBytes > maximumHigh_ - low_ || ((restoredBytes - initialBytes_) & 0xFFFF) != 0)
        return false;

    for (auto& list : class_) list.clear();
    free_.clear();
    allocated_.clear();
    invariantFailures_.clear();
    operationSequence_ = 0;
    allocationSequence_ = 0;
    bytes_ = restoredBytes;
    high_ = low_ + bytes_;
    extensionPages_ = (bytes_ - initialBytes_) >> 16;
    used_ = v[0];
    peak_ = v[1];
    for (size_t i = firstPair; i < v.size(); i += 2) enqueue(v[i], v[i + 1]);
    rebuildOpaqueOwnership("restored checkpoint");
    return true;
}

long long GuestHeap::liveAllocationAt(int at, int bytes) const
{
    int off = at - low_;
    for (const LiveExtent& e : allocated_)
        if (e.off == off && e.length == bytes) return e.allocation;
    return 0;
}

// ---- size classes ---------------------------------------------------------

int GuestHeap::classIndex(int bytes)
{
    // Stopping at kClasses rather than doubling for ever is this emulator's,
    // and it changes nothing: every index past the last class means the same
    // thing, a refusal.
    int k = 0;
    for (int c = kGranularity; c < bytes && k < kClasses; c <<= 1) k++;
    return k;
}

int GuestHeap::roundedSize(int bytes)
{
    if (bytes <= 0) return 0;
    int c = kGranularity << classIndex(bytes);
    if (c >= bytes + 256) return (bytes + 255) & ~255;
    return c;
}

// ---- allocation -----------------------------------------------------------

int GuestHeap::allocate(int bytes, const std::string& owner)
{
    long long operation = ++operationSequence_;
    lastRefusal_.clear();
    if (!valid_ || bytes <= 0) return 0;
    int first = classIndex(bytes);
    if (first >= kClasses) {
        lastRefusal_ = fmt::format("the system queue space cannot assign {} byte(s): that is above the "
                                   "{}-byte largest size class, so searchHeap starts past its own limit "
                                   "(initHeapInfo c1873e58)",
                                   bytes, kLargestClass);
        trace_.csp("sqs: {}", lastRefusal_);
        return 0;
    }

    int size = roundedSize(bytes);
    int answer = allocateFromLists(bytes, owner, operation, first, size);
    if (answer != 0) return answer;

    // On a miss the heap grows by one segment and the search is retried
    // exactly once.
    if (extendHeap()) {
        answer = allocateFromLists(bytes, owner, operation, first, size);
        if (answer != 0) return answer;
    }

    trace_.csp("sqs: {} bytes refused, {} of {} used, {} free", size, used_, capacity(), available());
    // Aggregate free space is actively misleading for this allocator: the
    // search starts at the request's power-of-two class and never combines
    // smaller free elements to satisfy one allocation.  Always report the
    // exact search domain when an allocation fails.
    int largest = 0;
    std::vector<std::string> classes;
    for (int k = 0; k < kClasses; k++) {
        int count = static_cast<int>(class_[k].size());
        if (count == 0) continue;
        int bytesInClass = kGranularity << k;
        largest = bytesInClass;
        classes.push_back(fmt::format("{}:{}", bytesInClass, count));
    }
    std::string classList = classes.empty() ? std::string("none") : joinStrings(classes);
    lastRefusal_ = fmt::format("the system queue space refused {} byte(s) (carve {}): {} of {} used, "
                               "{} free but the largest free element is {} and searchHeap starts at "
                               "class {} and never combines smaller ones; populated classes [{}]; "
                               "the pool is {:06X}..{:06X}, {}",
                               bytes, size, used_, capacity(), available(), largest, kGranularity << first, classList, low_,
                               high_ - 1,
                               high_ >= maximumHigh_
                                   ? fmt::format("already grown to NuEmul's measured ceiling {:06X} "
                                                 "(c1872840-c1872868)",
                                                 maximumHigh_)
                                   : fmt::format("extendable to {:06X}", maximumHigh_));
    trace_.csp("sqs: searchHeap started at class {} for request {} (carve {}); "
               "largest free element {}; populated classes [{}]",
               kGranularity << first, bytes, size, largest, classList);
    return 0;
}

void GuestHeap::sortAllocated()
{
    std::sort(allocated_.begin(), allocated_.end(), [](const LiveExtent& a, const LiveExtent& b) { return a.off < b.off; });
}

int GuestHeap::allocateFromLists(int bytes, const std::string& owner, long long operation, int first, int size)
{
    for (int k = first; k < kClasses; k++) {
        if (class_[k].empty()) continue;
        int off = head(k);
        int len = kGranularity << k;
        dequeue(k, off);
        if (len > size) enqueue(off + size, len - size);

        used_ += size;
        if (used_ > peak_) peak_ = used_;
        long long allocation = ++allocationSequence_;
        allocated_.push_back({off, size, bytes, allocation, owner});
        sortAllocated();
        traceNewInvariantFailures(operation, "assign");
        trace_.csp("sqs: assign {} -> {} bytes at guest {:04X} from class {} "
                   "({} of {} used, peak {}; op {}, allocation #{}{})",
                   bytes, size, low_ + off, kGranularity << k, used_, capacity(), peak_, operation, allocation,
                   owner.empty() ? std::string() : ", " + owner);
        return low_ + off;
    }
    return 0;
}

bool GuestHeap::extendHeap()
{
    const int segmentBytes = 0x10000;
    if (high_ + segmentBytes > maximumHigh_) return false;

    int oldHigh = high_;
    clearGuest(oldHigh, segmentBytes);

    int off = bytes_ + kGranularity;
    bytes_ += segmentBytes;
    high_ += segmentBytes;
    extensionPages_++;
    enqueue(off, segmentBytes - kGranularity);
    trace_.csp("sqs: NuEmulatorHeap::extendHeap committed {:06X}..{:06X}; "
               "reserved the first 16 bytes and enqueued {} bytes "
               "(c1874260-c18742F8), new limit {:06X}",
               oldHigh, high_ - 1, segmentBytes - kGranularity, high_);
    return true;
}

// ---- free -----------------------------------------------------------------

void GuestHeap::free(int at, int bytes, const std::string& owner)
{
    long long operation = ++operationSequence_;
    if (!valid_) return;
    if (bytes <= 0) {
        trace_.csp("sqs: free of zero bytes at {:04X} - nufree calls nuerabt "
                   "code 52 (c18e2f74)",
                   at);
        return;
    }
    int size = bytes;

    // The native free's own validity checks.  All abort on the real machine;
    // here they are traced, because a machine stop would hide the caller
    // that is at fault.
    if ((at & (kGranularity - 1)) != 0) {
        trace_.csp("sqs: free of {:04X} is not on a 16-byte boundary - "
                   "nuerabt 52 (c18e2f98)",
                   at);
        return;
    }
    if (at < kLowestFreeableAddress || (at & 0xFFFF) == 0) {
        trace_.csp("sqs: free of {:04X} is below 8192 or on a 64 KB boundary - "
                   "nuerabt 52 (c18e2fa8, c18e2fb8)",
                   at);
        return;
    }
    if ((at >> 16) != ((at + size - 1) >> 16)) {
        trace_.csp("sqs: free of {} bytes at {:04X} crosses a 64 KB boundary - "
                   "nuerabt 52 (c18e2fc8)",
                   size, at);
        return;
    }
    if (!contains(at) || at + size > high_) {
        trace_.csp("sqs: free of {} bytes at {:04X} is outside the pool - "
                   "NuHeap::error 512 (c18851c8)",
                   size, at);
        return;
    }

    int off = at - low_;
    if (free_.count(off) != 0) {
        // The block is still on a free list, so this is a double free.
        trace_.csp("sqs: {:04X} is already free - NuHeap::error 514 (c18852bc)", at);
        return;
    }

    int end = off + size;
    size_t containingIndex = allocated_.size();
    for (size_t i = 0; i < allocated_.size(); i++) {
        const LiveExtent& e = allocated_[i];
        if (e.off <= off && end <= e.off + e.length) {
            containingIndex = i;
            break;
        }
    }
    bool contained = containingIndex < allocated_.size();
    long long containingAllocation = 0;
    if (!contained) {
        std::vector<std::string> owners;
        for (const LiveExtent& e : allocated_) {
            if (off < e.off + e.length && e.off < end)
                owners.push_back(fmt::format("#{} {:04X}+{:X}", e.allocation, low_ + e.off, e.length));
        }
        reportInvariant(operation, fmt::format("free {:04X}+{:X} is not contained in one live allocation; intersects {}", at, size,
                                               owners.empty() ? std::string("none") : joinStrings(owners)));
    } else {
        LiveExtent containing = allocated_[containingIndex];
        containingAllocation = containing.allocation;
        allocated_.erase(allocated_.begin() + static_cast<std::ptrdiff_t>(containingIndex));
        if (containing.off < off)
            allocated_.push_back({containing.off, off - containing.off, containing.requested, containing.allocation, containing.owner});
        if (end < containing.off + containing.length)
            allocated_.push_back({end, containing.off + containing.length - end, containing.requested, containing.allocation,
                                  containing.owner});
        sortAllocated();
    }

    used_ -= size;
    enqueue(off, size);
    traceNewInvariantFailures(operation, "free");
    trace_.csp("sqs: free {} -> {} bytes at guest {:04X} ({} of {} used; "
               "op {}{}{})",
               bytes, size, at, used_, capacity(), operation,
               contained ? fmt::format(", allocation #{}", containingAllocation) : std::string(),
               owner.empty() ? std::string() : ", " + owner);
}

void GuestHeap::rebuildOpaqueOwnership(const std::string& owner)
{
    allocated_.clear();
    int cursor = kHeaderBytes;
    for (const auto& e : free_) {   // key order
        if (cursor < e.first)
            allocated_.push_back({cursor, e.first - cursor, e.first - cursor, ++allocationSequence_, owner});
        if (cursor < e.first + e.second) cursor = e.first + e.second;
    }
    if (cursor < bytes_) allocated_.push_back({cursor, bytes_ - cursor, bytes_ - cursor, ++allocationSequence_, owner});

    // The first 16 bytes of each dynamically committed segment belong to the
    // heap itself, not to the free lists or a guest owner.
    for (int page = 0; page < extensionPages_; page++) subtractOpaqueRange(initialBytes_ + (page << 16), kGranularity);
    std::vector<std::string> failures;
    if (!checkInvariants(failures))
        for (const std::string& failure : failures) reportInvariant(operationSequence_, failure);
}

void GuestHeap::subtractOpaqueRange(int off, int length)
{
    int end = off + length;
    size_t index = allocated_.size();
    for (size_t i = 0; i < allocated_.size(); i++)
        if (allocated_[i].off <= off && end <= allocated_[i].off + allocated_[i].length) {
            index = i;
            break;
        }
    if (index == allocated_.size()) return;
    LiveExtent containing = allocated_[index];
    allocated_.erase(allocated_.begin() + static_cast<std::ptrdiff_t>(index));
    if (containing.off < off)
        allocated_.push_back({containing.off, off - containing.off, containing.requested, containing.allocation, containing.owner});
    if (end < containing.off + containing.length)
        allocated_.push_back({end, containing.off + containing.length - end, containing.requested, containing.allocation,
                              containing.owner});
    sortAllocated();
}

void GuestHeap::traceNewInvariantFailures(long long operation, const std::string& action)
{
    std::vector<std::string> failures;
    if (checkInvariants(failures)) return;
    for (const std::string& failure : failures) reportInvariant(operation, action + ": " + failure);
}

void GuestHeap::reportInvariant(long long operation, const std::string& failure)
{
    std::string text = fmt::format("op {}: {}", operation, failure);
    if (std::find(invariantFailures_.begin(), invariantFailures_.end(), text) == invariantFailures_.end()) {
        if (invariantFailures_.size() == 64) invariantFailures_.erase(invariantFailures_.begin());
        invariantFailures_.push_back(text);
        trace_.csp("SQS INVARIANT VIOLATION: {}", text);
    }
}

// ---- the free lists -------------------------------------------------------

int GuestHeap::head(int k) const
{
    // Ascending: the lowest.
    return class_[k].empty() ? -1 : *class_[k].begin();
}

void GuestHeap::dequeue(int k, int off)
{
    class_[k].erase(off);
    free_.erase(off);
}

void GuestHeap::insert(int off, int size)
{
    class_[classIndex(size)].insert(off);
    free_[off] = size;
}

void GuestHeap::enqueue(int off, int size)
{
    // 1. While the length is not a power of two, absorb the block above if it
    //    is free.
    while (!isPowerOfTwo(size)) {
        auto above = free_.find(off + size);
        if (above == free_.end()) break;
        int aboveLen = above->second;
        dequeue(classIndex(aboveLen), off + size);
        size += aboveLen;
    }

    if (!isPowerOfTwo(size)) {
        // 3. Hand it back as a descending run of powers of two, each filed in
        //    its own class.  This is what the constructor's seed goes through.
        while (size >= kGranularity) {
            // The chunk is the class of size / 2 + 1, which for anything that
            // is not already a power of two is the largest power of two the
            // length still holds.
            int chunk = kGranularity << classIndex(size / 2 + 1);
            if (chunk > kLargestClass) chunk = kLargestClass;
            insert(off, chunk);
            off += chunk;
            size -= chunk;
        }
        return;
    }

    // 2. The buddy loop.  A block of size S at pool offset A has its buddy at
    //    A XOR S; they merge when the buddy is free and the same length, and
    //    the pair's address is the lower of the two.
    while (size < kLargestClass) {
        int buddy = off ^ size;
        // The buddy must lie strictly inside the pool.
        if (buddy <= 0 || buddy >= bytes_) break;
        auto it = free_.find(buddy);
        if (it == free_.end() || it->second != size) break;
        dequeue(classIndex(it->second), buddy);
        if (buddy < off) off = buddy;
        size <<= 1;
    }
    insert(off, size);
}

// ---- WorkSpaceHeap --------------------------------------------------------

int WorkSpaceHeap::allocate(int bytes)
{
    int size = round(bytes);
    for (size_t i = 0; i < free_.size(); i++) {
        if (free_[i].length < size) continue;
        int at = free_[i].at;
        if (free_[i].length == size) free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
        else free_[i] = {at + size, free_[i].length - size};
        return at;
    }
    return -1;
}

void WorkSpaceHeap::free(int at, int bytes)
{
    int size = round(bytes);
    size_t i = 0;
    while (i < free_.size() && free_[i].at < at) i++;
    free_.insert(free_.begin() + static_cast<std::ptrdiff_t>(i), Range{at, size});
    for (size_t n = 0; n + 1 < free_.size();) {
        if (free_[n].at + free_[n].length == free_[n + 1].at) {
            free_[n] = {free_[n].at, free_[n].length + free_[n + 1].length};
            free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(n + 1));
        } else n++;
    }
}

int WorkSpaceHeap::available() const
{
    int n = 0;
    for (const Range& r : free_) n += r.length;
    return n;
}

std::vector<int> WorkSpaceHeap::captureCheckpoint() const
{
    std::vector<int> v;
    for (const Range& r : free_) {
        v.push_back(r.at);
        v.push_back(r.length);
    }
    return v;
}

bool WorkSpaceHeap::restoreCheckpoint(const std::vector<int>& v)
{
    if ((v.size() & 1) != 0) return false;
    free_.clear();
    for (size_t i = 0; i < v.size(); i += 2) free_.push_back({v[i], v[i + 1]});
    return true;
}

}  // namespace sim36::processors::controlstorage
