#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <condition_variable>
#include <thread>
#include <chrono>

using namespace std::literals::chrono_literals;

#include <vsomeip/vsomeip.hpp>

#include "services.hpp"
#include "radio-stations.hpp"

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
    void record(unsigned target_cpu_ms,
                std::chrono::steady_clock::duration work_elapsed,
                std::uint64_t consumed_cpu_ns,
                std::chrono::steady_clock::duration mutex_wait,
                unsigned garbage_level) {
        const double wall_ms = to_ms(work_elapsed);
        const double actual_cpu_ms = static_cast<double>(consumed_cpu_ns) / 1000000.0;
        const double off_cpu_ms = std::max(0.0, wall_ms - actual_cpu_ms);
        const double cpu_share_percent = wall_ms > 0.0
                ? 100.0 * actual_cpu_ms / wall_ms : 0.0;
        const double deadline_ms = static_cast<double>(target_cpu_ms) * 1.10 + 10.0;
        const double overrun_ms = std::max(0.0, wall_ms - deadline_ms);
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

        offer();

        while (main_running) {
            bool display = false;

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
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_STATION_EVENT_ID, payload_with_string (station_info->name));
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
                    next_song_at = std::chrono::system_clock::now() + 3s + (rand() % 3000) * 1ms;
                    display = true;
                }

                if (display || garbage_level_changed) {
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
            } else {
                if (currently_playing)
                    std::cout << "RADIO: Paused playing" << std::endl;
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_SONG_EVENT_ID, payload_with_string ("-off-"));
                app->notify(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_ARTIST_EVENT_ID, payload_with_string (""));
                currently_playing = false;
            }

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
                demo_metrics.record(demo_cpu_ms, work_elapsed, consumed_cpu_ns,
                                    mutex_wait, garbage_level);

                // Keep the CPU load almost continuous, but briefly release
                // the mutex to allow state updates and normal shutdown.
                main_condition.wait_for(its_lock, 2ms);

                // Preserve song changes despite replacing the original wait.
                if (next_playing && std::chrono::system_clock::now() >= next_song_at)
                    goto_next_song = true;
            } else {
                // Preserve the original timing when demo mode is disabled.
                if (main_condition.wait_until(its_lock, next_song_at) == std::cv_status::timeout)
                    goto_next_song = true;
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
