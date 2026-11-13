#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <unistd.h>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"
#include "radio-stations.hpp"
#include "radio-demo-health.hpp"

namespace {

// Use consumed thread CPU time so that scheduling delays caused by competing
// workloads extend the wall-clock duration of this demo computation.
std::uint64_t thread_cpu_time_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

// Busy work is intentionally independent of the application's normal logic.
void demo_cpu_work(unsigned cpu_ms) {
    const std::uint64_t start_ns = thread_cpu_time_ns();
    const std::uint64_t duration_ns = static_cast<std::uint64_t>(cpu_ms) * 1000000ULL;
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;

    while (thread_cpu_time_ns() - start_ns < duration_ns) {
        for (unsigned i = 0; i < 16384; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
        }
    }

    // Make the result observable so optimizing builds retain the computation.
    static volatile std::uint64_t sink;
    sink ^= state;
}

unsigned demo_cpu_duration_ms() {
    const char *value = std::getenv("RADIO_DEMO_CPU_MS");
    if (value == nullptr || *value == '\0')
        return 0;

    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (value[0] == '-' || value[0] == '+' || end == value || *end != '\0'
            || errno != 0 || parsed > 5000UL) {
        std::cerr << "RADIO: Invalid RADIO_DEMO_CPU_MS (expected 0..5000): "
                  << value << std::endl;
        return 0;
    }
    return static_cast<unsigned>(parsed);
}

// Configure the size of the resident memory working set used by the RAM demo.
// Supported units are K, M, and G (binary multiples), e.g. 512M or 8G.
// An unset or zero value disables the RAM demo.
std::uint64_t demo_ram_size_bytes() {
    const char *value = std::getenv("RADIO_DEMO_RAM_SIZE");
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
        std::cerr << "RADIO: Invalid RADIO_DEMO_RAM_SIZE (use e.g. 512M or 8G): "
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

// Keep one committed working set for the lifetime of a song. Release the
// previous song's pages before acquiring the next working set. Every system
// page is touched, otherwise Linux overcommit could leave an 8G buffer with
// almost no physical memory footprint.
class demo_ram_buffer {
public:
    void release() {
        chunks_.clear();
        allocated_bytes_ = 0;
    }

    std::uint64_t allocated_bytes() const {
        return allocated_bytes_;
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

// This is a synthetic display fault, not a detection of real network corruption.
// Allow 10% plus 10 ms of scheduling overhead before reporting a missed budget.
// The number of inserted symbols rises gradually with the wall-clock overrun.
unsigned demo_garbage_level(unsigned cpu_ms, std::chrono::steady_clock::duration elapsed) {
    if (cpu_ms == 0)
        return 0;

    const double wall_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    const double tolerance_ms = static_cast<double>(cpu_ms) * 0.10 + 10.0;
    const double late_ms = wall_ms - static_cast<double>(cpu_ms) - tolerance_ms;
    if (late_ms <= 0.0)
        return 0;

    const double relative_delay = late_ms / static_cast<double>(cpu_ms);
    const double scaled = 12.0 * relative_delay / (relative_delay + 1.5);
    return std::min(12u, 1u + static_cast<unsigned>(scaled));
}

// Track wall-clock delays independently of the process-wide %CPU reported by
// monitoring tools. Off-CPU time can include preemption, throttling and other
// stalls; it should not be interpreted as an exact scheduler latency metric.
class demo_cpu_metrics {
public:
    void record(unsigned target_cpu_ms, const radio_demo::CpuSample &sample,
                std::chrono::steady_clock::duration mutex_wait,
                unsigned garbage_level) {
        const double wall_ms = sample.wall_ms;
        const double actual_cpu_ms = sample.cpu_ms;
        const double off_cpu_ms = sample.off_cpu_ms;
        const double cpu_share_percent = sample.cpu_share_percent;
        const double deadline_ms = sample.deadline_ms;
        const double overrun_ms = sample.overrun_ms;
        const double mutex_wait_ms = to_ms(mutex_wait);

        ++samples_;
        if (overrun_ms > 0.0)
            ++misses_;
        off_cpu_sum_ms_ += off_cpu_ms;
        off_cpu_max_ms_ = std::max(off_cpu_max_ms_, off_cpu_ms);
        overrun_max_ms_ = std::max(overrun_max_ms_, overrun_ms);
        garbage_max_ = std::max(garbage_max_, garbage_level);

        const auto now = std::chrono::steady_clock::now();
        // Report immediately on the first sample and then approximately once
        // per second, even if no garbage level change has occurred.
        if (!first_report_ && now - last_report_at_ < std::chrono::seconds(1))
            return;

        std::ostringstream message;
        message << std::fixed << std::setprecision(1)
                << "RADIO: CPU demo metrics: target_cpu=" << target_cpu_ms
                << "ms actual_cpu=" << actual_cpu_ms
                << "ms wall=" << wall_ms
                << "ms off_cpu=" << off_cpu_ms
                << "ms cpu_share=" << cpu_share_percent
                << "% deadline=" << deadline_ms
                << "ms overrun=" << overrun_ms
                << "ms mutex_wait=" << mutex_wait_ms
                << "ms garbage=" << garbage_level << "/12"
                << " | window samples=" << samples_
                << " misses=" << misses_
                << " avg_off_cpu=" << (off_cpu_sum_ms_ / samples_)
                << "ms max_off_cpu=" << off_cpu_max_ms_
                << "ms max_overrun=" << overrun_max_ms_
                << "ms max_garbage=" << garbage_max_ << "/12";
        std::cout << message.str() << std::endl;

        first_report_ = false;
        last_report_at_ = now;
        samples_ = 0;
        misses_ = 0;
        off_cpu_sum_ms_ = 0.0;
        off_cpu_max_ms_ = 0.0;
        overrun_max_ms_ = 0.0;
        garbage_max_ = 0;
    }

private:
    static double to_ms(std::chrono::steady_clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    bool first_report_ = true;
    std::chrono::steady_clock::time_point last_report_at_{};
    unsigned samples_ = 0;
    unsigned misses_ = 0;
    double off_cpu_sum_ms_ = 0.0;
    double off_cpu_max_ms_ = 0.0;
    double overrun_max_ms_ = 0.0;
    unsigned garbage_max_ = 0;
};

// Insert ANSI-colored garbage at UTF-8 character boundaries. The payload is
// capped below 256 bytes because the demo's existing string serializer uses
// an 8-bit loop counter. Plain metadata remains unchanged when level is zero.
std::string demo_corrupt_text(const std::string &original, unsigned level, unsigned seed) {
    if (level == 0 || original.empty())
        return original;

    constexpr std::size_t max_payload_bytes = 240;
    constexpr std::size_t bytes_per_symbol = 12; // ANSI color + ASCII symbol + reset
    if (original.size() >= max_payload_bytes)
        return original;

    const std::size_t count = std::min<std::size_t>(
            level, (max_payload_bytes - original.size()) / bytes_per_symbol);
    if (count == 0)
        return original;

    const char *const colors[] = {"\x1b[1;31m", "\x1b[1;35m", "\x1b[1;33m"};
    const char symbols[] = {'#', '@', '%', '&', '?', '!', '*', '~'};
    const char *const reset = "\x1b[0m";

    // Collect ends of UTF-8 code points to avoid inserting escape sequences
    // in the middle of a multibyte character.
    std::vector<std::size_t> boundaries;
    for (std::size_t pos = 0; pos < original.size();) {
        ++pos;
        while (pos < original.size()
                && (static_cast<unsigned char>(original[pos]) & 0xc0) == 0x80)
            ++pos;
        boundaries.push_back(pos);
    }

    std::string result;
    result.reserve(original.size() + count * bytes_per_symbol);
    std::size_t previous = 0;
    std::size_t inserted = 0;
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
        result.append(original, previous, boundaries[i] - previous);
        previous = boundaries[i];

        // Distribute the extra characters across the full title/artist.
        while (inserted < count
                && 1 + inserted * boundaries.size() / count == i + 1) {
            result += colors[(seed + static_cast<unsigned>(inserted)) % 3];
            result += symbols[(seed + static_cast<unsigned>(inserted)) % sizeof(symbols)];
            result += reset;
            ++inserted;
        }
    }
    return result;
}

} // namespace

class radio_service {
public:
    radio_service() :
            app(vsomeip::runtime::get()->create_application("Radio")),
            main_blocked(false),
            main_running(true),
            is_offered(false),
            engine_available(false),
            verbose(false),
            next_volume(50),
            next_radio_station(0),
            next_playing(false),
            next_reversed(false),
            main_thread(std::bind(&radio_service::run, this)) {
    }

    bool init() {
        std::lock_guard<std::mutex> its_lock(main_mutex);

        if (!app->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }

        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SET_PLAYING_METHOD_ID,
                std::bind(&radio_service::on_message_set_playing, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_IS_PLAYING_METHOD_ID,
                std::bind(&radio_service::on_message_is_playing, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_GET_VOLUME_METHOD_ID,
                std::bind(&radio_service::on_message_get_volume, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_CHANGE_VOLUME_METHOD_ID,
                std::bind(&radio_service::on_message_change_volume, this,
                          std::placeholders::_1));
        app->register_message_handler(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SWITCH_STATION_METHOD_ID,
                std::bind(&radio_service::on_message_switch_station, this,
                          std::placeholders::_1));

        std::set<vsomeip::eventgroup_t> its_groups_radio;
        its_groups_radio.insert(RADIO_EVENTGROUP_ID);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_VOLUME_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_STATION_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_SONG_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_ARTIST_EVENT_ID,
                its_groups_radio,
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);
        app->offer_event(
                RADIO_SERVICE_ID,
                RADIO_INSTANCE_ID,
                RADIO_HEALTH_EVENT_ID,
                std::set<vsomeip::eventgroup_t>{RADIO_HEALTH_EVENTGROUP_ID},
                vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);

        app->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                std::bind(&radio_service::on_engine_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        app->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(ENGINE_EVENTGROUP_ID);
        app->request_event(
                ENGINE_SERVICE_ID,
                ENGINE_INSTANCE_ID,
                ENGINE_REVERSE_EVENT_ID,
                its_groups,
                vsomeip::event_type_e::ET_FIELD);
        app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);

        app->register_message_handler(
                ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID,
                std::bind(&radio_service::on_engine_reverse_event, this,
                        std::placeholders::_1));

        main_blocked = true;
        main_condition.notify_one();
        return true;
    }

    void start() {
        set_playing(true);
        app->start();
    }

    void stop() {
        main_running = false;
        main_blocked = true;
        main_condition.notify_one();
        app->clear_all_handler();
        app->unsubscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
        app->release_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_REVERSE_EVENT_ID);
        app->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
        stop_offer();
        main_thread.join();
        app->stop();
    }

    void offer() {
        app->offer_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
        is_offered = true;
    }

    void stop_offer() {
        app->stop_offer_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
    }

    void run() {
        uint32_t current_volume = next_volume;
        bool currently_playing = false;
        bool current_reversed = false;
        uint32_t saved_volume;
        bool next_volume_is_auto = false;
        bool volume_is_auto = false;
        uint32_t current_station = 0xffffffff;
        const radio_station_info_t *station_info = NULL;
        uint32_t current_song = 0;
        bool goto_next_song = true;
        std::chrono::time_point<std::chrono::system_clock> next_song_at;
        const unsigned demo_cpu_ms = demo_cpu_duration_ms();
        const char *lock_env = std::getenv("RADIO_DEMO_HOLD_MUTEX");
        const bool demo_hold_mutex = lock_env != nullptr && lock_env[0] == '1'
                                   && lock_env[1] == '\0';
        unsigned garbage_level = 0;
        bool garbage_level_changed = false;
        demo_cpu_metrics demo_metrics;
        const std::uint64_t demo_ram_bytes = demo_ram_size_bytes();
        demo_ram_buffer ram_working_set;
        demo_ram_warning_state ram_warning;
        bool ram_warning_displayed = false;
        radio_demo::HealthTracker health_tracker(demo_cpu_ms, demo_ram_bytes);
        auto next_health_report = std::chrono::steady_clock::now();
        std::uint32_t health_sequence = 0;

        std::unique_lock<std::mutex> its_lock(main_mutex);
        std::cout << "RADIO: Started main thread" << std::endl;

        // Wait until init() has completed before offering the service.
        while (!main_blocked)
            main_condition.wait(its_lock);

        if (demo_cpu_ms > 0) {
            std::cout << "RADIO: CPU demo enabled (" << demo_cpu_ms
                      << " ms thread CPU time per cycle, mutex "
                      << (demo_hold_mutex ? "held" : "released")
                      << "; metrics approximately every 1 s)" << std::endl;
        } else {
            std::cout << "RADIO: CPU demo disabled; set RADIO_DEMO_CPU_MS > 0 "
                      << "to enable workload delay metrics" << std::endl;
        }

        if (demo_ram_bytes > 0) {
            std::cout << "RADIO: RAM demo enabled ("
                      << demo_ram_bytes / (1024ULL * 1024ULL)
                      << " MiB per song, 256 MiB safety headroom)" << std::endl;
        } else {
            std::cout << "RADIO: RAM demo disabled; set RADIO_DEMO_RAM_SIZE=8G "
                      << "to enable per-song allocation" << std::endl;
        }

        offer();

        // The health field also serves as a one-second heartbeat. In normal
        // operation no journal parsing or metadata inference is required.
        const auto publish_health = [&](bool force) {
            const auto now = std::chrono::steady_clock::now();
            if (!force && now < next_health_report)
                return;
            health_tracker.refresh_ram(ram_working_set.allocated_bytes(),
                                       ram_warning.active(now));
            auto health = health_tracker.snapshot(now);
            // Change the sequence even if the measured values remain stable.
            health.sequence = ++health_sequence;
            auto payload = vsomeip::runtime::get()->create_payload();
            payload->set_data(demo_health::encode(health));
            app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_HEALTH_EVENT_ID, payload);
            next_health_report = now + std::chrono::seconds(1);
        };
        publish_health(true);

        while (main_running) {
            bool display = false;
            bool station_changed = false;

            if (next_reversed != current_reversed) {
                if (next_reversed) {
                    saved_volume = current_volume;
                    next_volume = 30;
                    next_volume_is_auto = true;
                    std::cout << "RADIO: Lowering volume due to reverse" << std::endl;
                } else {
                    if (volume_is_auto) {
                        next_volume = saved_volume;
                        std::cout << "RADIO: Restoring volume due to cancelled reverse" << std::endl;
                    }
                }
                current_reversed = next_reversed;
            }

            if (next_volume != current_volume) {
                current_volume = next_volume;
                volume_is_auto = next_volume_is_auto;
                next_volume_is_auto = false;
                display = true;

                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_VOLUME_EVENT_ID, payload_with_arg (current_volume));
            }

            if (next_radio_station != current_station) {
                current_station = next_radio_station;
                goto_next_song = true;
                station_info = get_radio_stations(current_station);
                display = true;
                station_changed = true;
            }

            if (next_playing) {
                if (!currently_playing) {
                    std::cout << "RADIO: Started playing" << std::endl;
                    goto_next_song = true;
                }
                currently_playing = true;

                if (goto_next_song) {
                    auto old = current_song;
                    do {
                        current_song = rand() % station_info->n_songs;
                    } while (old == current_song);
                    goto_next_song = false;
                    display = true;

                    if (demo_ram_bytes > 0) {
                        std::string failure_reason;
                        std::uint64_t partially_allocated_bytes = 0;
                        const auto allocation_started = std::chrono::steady_clock::now();

                        // Memory allocation can take seconds for large buffers.
                        // Do not hold the radio state mutex while touching pages.
                        its_lock.unlock();
                        const bool acquired = ram_working_set.acquire(
                                demo_ram_bytes, failure_reason, partially_allocated_bytes);
                        const auto allocation_elapsed = std::chrono::steady_clock::now()
                                - allocation_started;
                        its_lock.lock();

                        std::ostringstream ram_log;
                        ram_log << std::fixed << std::setprecision(1)
                                << "RADIO: RAM demo: requested="
                                << demo_ram_bytes / (1024.0 * 1024.0)
                                << "MiB result=" << (acquired ? "OK" : "FAILED")
                                << " duration="
                                << std::chrono::duration<double, std::milli>(
                                           allocation_elapsed).count()
                                << "ms";
                        if (!acquired)
                            ram_log << " allocated_before_rollback="
                                    << partially_allocated_bytes / (1024.0 * 1024.0)
                                    << "MiB reason=" << failure_reason;
                        std::cout << ram_log.str() << std::endl;

                        if (!acquired) {
                            // Every failure restarts the minimum display hold,
                            // even if the competing process has already exited.
                            ram_warning.record_failure(std::chrono::steady_clock::now());
                            std::cout << "RADIO: RAM demo: warning held for at least 15s "
                                         "after the latest allocation failure"
                                      << std::endl;
                        }
                        health_tracker.record_ram_allocation(
                                acquired, ram_working_set.allocated_bytes(),
                                ram_warning.active(std::chrono::steady_clock::now()));
                        // Publish the allocation result immediately, before a
                        // potentially long CPU workload can delay the warning.
                        publish_health(true);

                        // An intervening station or playback request makes the
                        // just-prepared song obsolete. Preserve any RAM warning
                        // that was already triggered while retrying the request.
                        if (!next_playing || next_radio_station != current_station) {
                            ram_working_set.release();
                            publish_health(true);
                            goto_next_song = true;
                            continue;
                        }
                        if (acquired)
                            ram_warning.clear_allocation_failure();
                    }

                    next_song_at = std::chrono::system_clock::now()
                            + 3s + (rand() % 3000) * 1ms;
                }
            } else {
                if (currently_playing)
                    std::cout << "RADIO: Paused playing" << std::endl;
                if (currently_playing)
                    ram_working_set.release();
                // Pausing stops allocation attempts, but does not cancel the
                // minimum warning duration after a prior allocation failure.
                ram_warning.clear_allocation_failure();
                currently_playing = false;
            }

            // The warning is a display state, not the result of the most recent
            // allocation alone. Do not republish station, title, or artist data
            // while it is active, including on song/station changes or when
            // CPU-induced garbage levels change.
            const bool show_ram_warning = ram_warning.active(std::chrono::steady_clock::now());
            if (show_ram_warning && !ram_warning_displayed) {
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_STATION_EVENT_ID,
                        payload_with_string(RADIO_RAM_FAILURE_MARKER));
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_SONG_EVENT_ID, payload_with_string("RAM"));
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_ARTIST_EVENT_ID, payload_with_string(""));
                ram_warning_displayed = true;
                std::cout << "RADIO: RAM demo: warning displayed" << std::endl;
            } else if (!show_ram_warning && ram_warning_displayed) {
                ram_warning_displayed = false;
                display = true;
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_STATION_EVENT_ID, payload_with_string(station_info->name));
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - ram_warning.last_failure_at());
                std::cout << "RADIO: RAM demo: warning cleared after "
                          << elapsed.count()
                          << "ms; resuming normal metadata" << std::endl;
            } else if (station_changed && !ram_warning_displayed) {
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_STATION_EVENT_ID, payload_with_string(station_info->name));
            }

            if (next_playing) {
                if ((display || garbage_level_changed) && !ram_warning_displayed) {
                    struct song_info_t *song = &station_info->songs[current_song];
                    const std::string title = demo_corrupt_text(song->name, garbage_level, current_song);
                    const std::string artist = demo_corrupt_text(song->artist, garbage_level,
                            current_song + current_station + 3);
                    app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_SONG_EVENT_ID,
                            payload_with_string(title));
                    app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_ARTIST_EVENT_ID,
                            payload_with_string(artist));
                    if (display) {
                        std::cout << "RADIO: Playing song \""
                                  << song->name << "\" by " << song->artist
                                  << " (on " << station_info->name << ") "
                                  << std::dec << current_volume << "% volume"
                                  << std::endl;
                    }
                    if (garbage_level_changed) {
                        // Confirm that the new corruption level was actually
                        // published, not merely detected by the CPU workload.
                        std::cout << "RADIO: Republished song metadata: garbage="
                                  << std::dec << garbage_level << "/12"
                                  << " title_bytes=" << title.size()
                                  << " artist_bytes=" << artist.size()
                                  << std::endl;
                    }
                    garbage_level_changed = false;
                }
            } else if (!ram_warning_displayed) {
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_SONG_EVENT_ID, payload_with_string("-off-"));
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                        RADIO_ARTIST_EVENT_ID, payload_with_string(""));
            }
            if (ram_warning_displayed)
                garbage_level_changed = false;

            if (demo_cpu_ms > 0) {
                // Releasing the mutex isolates CPU scheduling interference.
                // Holding it additionally delays SOME/IP callbacks that need
                // access to the radio state, intentionally amplifying failures.
                if (!demo_hold_mutex)
                    its_lock.unlock();

                const auto work_started = std::chrono::steady_clock::now();
                const std::uint64_t cpu_started_ns = thread_cpu_time_ns();
                demo_cpu_work(demo_cpu_ms);
                const std::uint64_t consumed_cpu_ns = thread_cpu_time_ns() - cpu_started_ns;
                const auto work_finished = std::chrono::steady_clock::now();
                const auto work_elapsed = work_finished - work_started;

                if (!demo_hold_mutex)
                    its_lock.lock();
                const auto mutex_wait = demo_hold_mutex
                        ? std::chrono::steady_clock::duration::zero()
                        : std::chrono::steady_clock::now() - work_finished;

                // The demo deliberately corrupts outgoing display text after
                // missed computation deadlines. It does not modify source data.
                const unsigned observed_level = demo_garbage_level(demo_cpu_ms, work_elapsed);
                if (observed_level != garbage_level) {
                    garbage_level = observed_level;
                    garbage_level_changed = true;
                }
                const auto cpu_sample = health_tracker.record_cpu(
                        work_elapsed, consumed_cpu_ns, std::chrono::steady_clock::now());
                demo_metrics.record(demo_cpu_ms, cpu_sample, mutex_wait, garbage_level);

                // Keep the CPU load almost continuous, but briefly release
                // the mutex to allow state updates and normal shutdown.
                main_condition.wait_for(its_lock, 2ms);

                // Preserve song changes despite replacing the original wait.
                if (next_playing && std::chrono::system_clock::now() >= next_song_at)
                    goto_next_song = true;
                publish_health(false);
            } else {
                // While the RAM warning is visible, wake periodically so its
                // 15-second hold can end without waiting for another song.
                // This also works when playback is paused and CPU load is off.
                if (ram_warning_displayed) {
                    main_condition.wait_for(its_lock, 250ms);
                    if (next_playing && std::chrono::system_clock::now() >= next_song_at)
                        goto_next_song = true;
                } else if (!next_playing) {
                    // Continue the health heartbeat while playback is paused.
                    main_condition.wait_until(its_lock, next_health_report);
                } else {
                    // Wake for either a song transition or the health heartbeat.
                    // The two deadlines use different clocks, so compare their
                    // remaining durations rather than their time points.
                    const auto song_wait = std::chrono::duration_cast<
                            std::chrono::steady_clock::duration>(
                            next_song_at - std::chrono::system_clock::now());
                    const auto health_wait = next_health_report -
                            std::chrono::steady_clock::now();
                    const auto wait = std::min(song_wait, health_wait);
                    if (wait > std::chrono::steady_clock::duration::zero())
                        main_condition.wait_for(its_lock, wait);
                    if (next_playing && std::chrono::system_clock::now() >= next_song_at)
                        goto_next_song = true;
                }
                publish_health(false);
            }
        }
    }

    void on_message_set_playing(const std::shared_ptr<vsomeip::message> &_request) {
        bool arg = get_arg0(_request) != 0;
        bool was_playing;

        if (verbose)
            std::cout << "RADIO: Received set_playing "
                      << arg
                      << " from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        was_playing = set_playing (arg);

        std::shared_ptr<vsomeip::message> its_response = response (_request, was_playing ? 1 : 0);
        app->send(its_response);
    }

    void on_message_is_playing(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received is_playing "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        std::shared_ptr<vsomeip::message> its_response = response (_request, get_playing () ? 1 : 0);
        app->send(its_response);
    }

    void on_message_change_volume(const std::shared_ptr<vsomeip::message> &_request) {
        int32_t arg = (int32_t)get_arg0(_request);
        uint32_t result;

        if (verbose)
            std::cout << "RADIO: Received change_volume "
                      << arg
                      << " from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        result = change_volume (arg);

        std::shared_ptr<vsomeip::message> its_response = response (_request, result);
        app->send(its_response);
    }

    void on_message_get_volume(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received get_volume "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        uint32_t volume = get_volume();

        std::shared_ptr<vsomeip::message> its_response = response (_request, volume);
        app->send(its_response);
    }

    void on_message_switch_station(const std::shared_ptr<vsomeip::message> &_request) {
        if (verbose)
            std::cout << "RADIO: Received switch_station "
                      << "from Client/Session ["
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_client() << "/"
                      << std::setw(4) << std::setfill('0') << std::hex << _request->get_session() << "] "
                      << std::endl;

        switch_station();

        std::shared_ptr<vsomeip::message> its_response = response (_request, 0);
        app->send(its_response);
    }

    void on_engine_reverse_event(const std::shared_ptr<vsomeip::message> &_event) {
        bool reversed = get_arg0(_event) != 0;
        if (verbose)
            std::cout << "RADIO: Received event: "
                      << (reversed ? "engine reversed" : "engine forward")
                      << std::endl;
        set_reversed(reversed);
    }

    void on_engine_availability(vsomeip::service_t _service, vsomeip::instance_t _instance, bool _is_available) {
        std::cout << "Engine Service is "
                << (_is_available ? "available." : "NOT available.")
                << std::endl;

        std::lock_guard<std::mutex> its_lock(main_mutex);
        engine_available = _is_available;
    }

private:
    bool set_playing (bool new_playing) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        bool old = new_playing;

        if (next_playing != new_playing) {
            next_playing = new_playing;
            main_condition.notify_one();
        }

        return old;
    }

    bool get_playing (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        return next_playing;
    }

    uint32_t change_volume (int32_t delta_volume) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        int32_t new_volume;

        new_volume = (int32_t)next_volume + delta_volume;
        if (new_volume < 0)
            new_volume = 0;
        else {
            if (new_volume > 100)
                new_volume = 100;
        }

        if (next_volume != new_volume) {
            next_volume = new_volume;
            main_condition.notify_one();
        }

        return new_volume;
    }

    uint32_t get_volume (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        return next_volume;
    }

    void switch_station (void) {
        std::unique_lock<std::mutex> its_lock(main_mutex);

        next_radio_station = (next_radio_station + 1) % n_radio_stations();
        main_condition.notify_one();
    }

    void set_reversed (bool new_reversed) {
        std::unique_lock<std::mutex> its_lock(main_mutex);
        next_reversed = new_reversed;
        main_condition.notify_one();
    }

    std::shared_ptr< vsomeip::application > app;

    std::mutex main_mutex;
    std::condition_variable main_condition;
    bool main_blocked;
    std::atomic<bool> main_running;
    bool is_offered;
    bool engine_available;

    bool verbose;

    uint32_t next_volume;
    uint32_t next_radio_station;
    bool next_playing;
    bool next_reversed;

    // Start the worker only after all state and synchronization are initialized.
    std::thread main_thread;
};

radio_service *its_radio_ptr(nullptr);
static void handle_signal(int _signal) {
    if (its_radio_ptr != nullptr &&
        (_signal == SIGINT || _signal == SIGTERM))
        its_radio_ptr->stop();
}

int main() {
    radio_service its_radio;

    its_radio_ptr = &its_radio;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!its_radio.init()) {
        return 1;
    }

    its_radio.start();

    // Work around dlt-daemon timeout on exit
    _exit(0);
}
