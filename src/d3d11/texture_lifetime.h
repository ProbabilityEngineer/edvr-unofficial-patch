#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace edvr::texture_lifetime {

// Diagnostic accounting, not physical GPU allocation accounting. Stable,
// bounded descriptor slots; overflow still contributes to complete totals.
struct Key {
    bool edvr = false; // caller inside EDVR's image, otherwise external
    std::array<uint32_t, 11> desc{};
    bool operator==(const Key& other) const {
        return edvr == other.edvr && desc == other.desc;
    }
};

struct Counts {
    uint64_t created = 0, destroyed = 0, live = 0, peak = 0;
    uint64_t bytes = 0, peakBytes = 0;
    void add(uint64_t size) {
        ++created;
        if (++live > peak) peak = live;
        bytes += size;
        if (bytes > peakBytes) peakBytes = bytes;
    }
    void remove(uint64_t size) {
        ++destroyed;
        --live;
        bytes -= size;
    }
};

struct Group {
    Key key{};
    Counts counts{};
};

constexpr unsigned kDescriptors = 64;
constexpr unsigned kGroups = kDescriptors + 2; // one overflow per caller class
constexpr unsigned kInvalid = kGroups;
constexpr uint64_t kMaxTags = 65536; // bounds diagnostic object overhead

struct Snapshot {
    std::array<Group, kGroups> groups{};
    std::array<Counts, 2> totals{};
    unsigned used = 0;
    uint64_t observed = 0, failed = 0, capped = 0, pending = 0;
};

// Four largest absolute changes, then four largest remaining live groups.
// No allocations, and at most eight descriptor lines per reporting interval.
inline std::array<unsigned, 8> reportGroups(
    const Snapshot& now, const std::array<uint64_t, kGroups>& previous) {
    std::array<unsigned, 8> result{};
    result.fill(kInvalid);
    std::array<bool, kGroups> selected{};
    for (unsigned n = 0; n < result.size(); ++n) {
        uint64_t best = 0;
        unsigned slot = kInvalid;
        for (unsigned i = 0; i < kGroups; ++i) {
            if (selected[i]) continue;
            const uint64_t bytes = now.groups[i].counts.bytes;
            const uint64_t change = bytes > previous[i] ? bytes - previous[i] : previous[i] - bytes;
            const uint64_t score = n < 4 ? change : bytes;
            if (score > best) { best = score; slot = i; }
        }
        result[n] = slot;
        if (slot != kInvalid) selected[slot] = true;
    }
    return result;
}

class Ledger {
public:
    Ledger() { data_.groups[kDescriptors + 1].key.edvr = true; }

    unsigned reserve(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++data_.observed;
        if (data_.totals[0].live + data_.totals[1].live + data_.pending >= kMaxTags) {
            ++data_.capped;
            return kInvalid;
        }
        ++data_.pending;
        for (unsigned i = 0; i < data_.used; ++i)
            if (data_.groups[i].key == key) return i;
        if (data_.used < kDescriptors) {
            const unsigned i = data_.used++;
            data_.groups[i].key = key;
            return i;
        }
        return kDescriptors + unsigned(key.edvr);
    }

    void attached(unsigned slot, uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        --data_.pending;
        auto& group = data_.groups[slot];
        group.counts.add(bytes);
        data_.totals[unsigned(group.key.edvr)].add(bytes);
    }
    void failed() {
        std::lock_guard<std::mutex> lock(mutex_);
        --data_.pending;
        ++data_.failed;
    }
    void destroyed(unsigned slot, uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& group = data_.groups[slot];
        group.counts.remove(bytes);
        data_.totals[unsigned(group.key.edvr)].remove(bytes);
    }
    Snapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_;
    }

private:
    std::mutex mutex_;
    Snapshot data_{};
};

} // namespace edvr::texture_lifetime
