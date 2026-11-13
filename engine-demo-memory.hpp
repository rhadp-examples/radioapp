#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace engine_demo {

// Configure the size of the resident memory working set used by the RAM demo.
// Supported units are K, M, and G (binary multiples), e.g. 512M or 8G.
// An unset or zero value disables the RAM demo.
std::uint64_t demo_ram_size_bytes() {
    const char *value = std::getenv("ENGINE_DEMO_RAM_SIZE");
    if (value == nullptr || *value == '\0')
        return 0;

    char *end = nullptr;
    errno = 0;
    const unsigned long long count = std::strtoull(value, &end, 10);
    bool valid = value[0] != '-' && value[0] != '+' && end != value && errno == 0;
    std::uint64_t multiplier = 1;
    if (valid && *end != '\0') {
        const char unit = static_cast<char>(std::toupper(static_cast<unsigned char>(*end++)));
        if (unit == 'K')
            multiplier = 1024ULL;
        else if (unit == 'M')
            multiplier = 1024ULL * 1024ULL;
        else if (unit == 'G')
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        else
            valid = false;
        if (*end == 'B' || *end == 'b')
            ++end;
        if (*end != '\0')
            valid = false;
    }
    if (!valid || count > std::numeric_limits<std::uint64_t>::max() / multiplier
            || count * multiplier > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "ENGINE: Invalid ENGINE_DEMO_RAM_SIZE (use e.g. 512M or 8G): "
                  << value << std::endl;
        return 0;
    }
    return static_cast<std::uint64_t>(count) * multiplier;
}

constexpr std::uint64_t demo_ram_headroom_bytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t demo_ram_chunk_bytes = 16ULL * 1024ULL * 1024ULL;

// Linux can overcommit virtual memory: a successful new[] does not prove that
// physical pages are available. Check both host MemAvailable and cgroup v2
// headroom before each chunk is allocated and its pages are written.
// These checks are best-effort, not an atomic guarantee against the OOM killer.
bool demo_read_number(const char *path, std::uint64_t &result) {
    std::ifstream input(path);
    std::string value;
    if (!(input >> value) || value == "max" || value.empty())
        return false;
    for (const char c : value) {
        if (c < '0' || c > '9')
            return false;
    }
    try {
        result = std::stoull(value);
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

bool demo_host_available(std::uint64_t &bytes) {
    std::ifstream input("/proc/meminfo");
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 13, "MemAvailable:") != 0)
            continue;
        std::istringstream text(line.substr(13));
        std::uint64_t available_kib = 0;
        if (!(text >> available_kib)
                || available_kib > std::numeric_limits<std::uint64_t>::max() / 1024ULL)
            return false;
        bytes = available_kib * 1024ULL;
        return true;
    }
    return false;
}

// Never intentionally consume the final 256 MiB of reported available memory.
// Cgroup limits are checked when a finite memory.max is visible. When no
// limit is exposed, the host estimate is the available fallback.
bool demo_ram_has_capacity(std::uint64_t next_chunk_bytes, std::string &reason) {
    if (next_chunk_bytes > std::numeric_limits<std::uint64_t>::max()
                           - demo_ram_headroom_bytes) {
        reason = "requested chunk exceeds supported size";
        return false;
    }
    const std::uint64_t needed = next_chunk_bytes + demo_ram_headroom_bytes;
    std::uint64_t host_bytes = 0;
    const bool host_known = demo_host_available(host_bytes);
    if (host_known && host_bytes < needed) {
        reason = "host MemAvailable below required headroom";
        return false;
    }
    std::uint64_t limit = 0;
    std::uint64_t current = 0;
    bool cgroup_known = demo_read_number("/sys/fs/cgroup/memory.max", limit)
            && demo_read_number("/sys/fs/cgroup/memory.current", current);
    if (!cgroup_known) {
        // Older distributions may still use cgroup v1 memory accounting.
        cgroup_known = demo_read_number(
                "/sys/fs/cgroup/memory/memory.limit_in_bytes", limit)
                && demo_read_number(
                "/sys/fs/cgroup/memory/memory.usage_in_bytes", current);
        // Some v1 implementations encode "no limit" with a huge number.
        if (limit >= (1ULL << 60))
            cgroup_known = false;
    }
    if (cgroup_known && (current >= limit || limit - current < needed)) {
        reason = "cgroup memory.max headroom exhausted";
        return false;
    }
    // Without either capacity estimate the OOM risk cannot be bounded.
    if (!host_known && !cgroup_known) {
        reason = "memory availability could not be determined";
        return false;
    }
    return true;
}

// Keep one committed working set for each forward/reverse driving phase.
// Release the previous phase's pages before acquiring the next working set.
// Every system
// page is touched, otherwise Linux overcommit could leave an 8G buffer with
// almost no physical memory footprint.
class demo_ram_buffer {
public:
    void release() {
        chunks_.clear();
        allocated_bytes_ = 0;
    }

    bool acquire(std::uint64_t requested, std::string &reason,
                 std::uint64_t &bytes_before_rollback) {
        release();
        bytes_before_rollback = 0;
        if (requested == 0)
            return true;
        // Fail before consuming host memory when the full request is already
        // larger than the reported capacity. Repeat the check per chunk to
        // account for other workloads consuming RAM during allocation.
        if (!demo_ram_has_capacity(requested, reason))
            return false;

        const long configured_page_size = sysconf(_SC_PAGESIZE);
        const std::size_t page_size = configured_page_size > 0
                ? static_cast<std::size_t>(configured_page_size) : 4096;
        try {
            while (allocated_bytes_ < requested) {
                const std::size_t chunk_size = static_cast<std::size_t>(
                        std::min<std::uint64_t>(requested - allocated_bytes_,
                                                 demo_ram_chunk_bytes));
                if (!demo_ram_has_capacity(chunk_size, reason))
                    break;

                std::unique_ptr<std::uint8_t[]> chunk(
                        new (std::nothrow) std::uint8_t[chunk_size]);
                if (!chunk) {
                    reason = "allocation returned nullptr";
                    break;
                }
                volatile std::uint8_t *const pages = chunk.get();
                for (std::size_t offset = 0; offset < chunk_size; offset += page_size)
                    pages[offset] = 0xA5;
                pages[chunk_size - 1] = 0xA5;

                chunks_.push_back(std::move(chunk));
                allocated_bytes_ += chunk_size;
            }
        } catch (const std::bad_alloc &) {
            reason = "allocation failed (std::bad_alloc)";
        }

        if (allocated_bytes_ == requested)
            return true;

        bytes_before_rollback = allocated_bytes_;
        release();
        return false;
    }

private:
    std::vector<std::unique_ptr<std::uint8_t[]>> chunks_;
    std::uint64_t allocated_bytes_ = 0;
};

// Keep the RAM warning visible for at least 15 seconds after the most recent
// allocation failure. A successful allocation only clears the outstanding
// failure; it cannot shorten the display hold. A new failure restarts the hold.
class demo_ram_warning_state {
public:
    using clock = std::chrono::steady_clock;

    void record_failure(clock::time_point now) {
        allocation_failed_ = true;
        visible_until_ = now + std::chrono::seconds(15);
        last_failure_at_ = now;
    }

    void clear_allocation_failure() {
        allocation_failed_ = false;
    }

    bool active(clock::time_point now) const {
        return allocation_failed_ || now < visible_until_;
    }

    clock::time_point last_failure_at() const {
        return last_failure_at_;
    }

private:
    bool allocation_failed_ = false;
    clock::time_point visible_until_{};
    clock::time_point last_failure_at_{};
};


} // namespace engine_demo
