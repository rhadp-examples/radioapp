#pragma once

#include <chrono>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// Independent of vsomeip and ncurses so the state model can be tested alone.
namespace radio_ui {

enum class Availability { Unknown, Available, Unavailable };
enum class Communication { Unknown, Receiving, Unavailable, Stale };
enum class ServiceKind { Radio, Engine };
enum class Health { Unknown, Normal, Degraded, Failed };
enum class Evidence { None, DemoIndicator, Telemetry };
enum class Direction { Unknown, Forward, Reverse };
enum class TextColor { Default, Red, Yellow, Magenta };

struct CpuMetrics {
    bool available = false;
    double cpu_share_percent = 0.0;
    double wall_ms = 0.0;
    double off_cpu_ms = 0.0;
    double deadline_overrun_ms = 0.0;
    unsigned deadline_misses = 0;
};

struct RamMetrics {
    bool available = false;
    std::uint64_t requested_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    unsigned allocation_failures = 0;
};

struct HealthAxis {
    Health state = Health::Unknown;
    Evidence source = Evidence::None;
};

struct ServiceStatus {
    Availability availability = Availability::Unknown;
    Communication communication = Communication::Unknown;
    HealthAxis cpu;
    HealthAxis ram;
    CpuMetrics cpu_metrics;
    RamMetrics ram_metrics;
    bool has_message = false;
    std::chrono::steady_clock::time_point last_message{};

    void observe_message() {
        has_message = true;
        if (availability != Availability::Unavailable)
            communication = Communication::Receiving;
        last_message = std::chrono::steady_clock::now();
    }

    void on_availability(bool available) {
        const Availability next = available ? Availability::Available : Availability::Unavailable;
        if (availability != next &&
            (!available || availability == Availability::Unavailable)) {
            // Old telemetry is not evidence of the health of a new session.
            // Preserve initial field notifications received before discovery.
            cpu = HealthAxis{};
            ram = HealthAxis{};
            cpu_metrics = CpuMetrics{};
            ram_metrics = RamMetrics{};
            has_message = false;
            communication = available ? Communication::Unknown : Communication::Unavailable;
        }
        availability = next;
    }

    // Reserved for a future explicit SOME/IP health event from either service.
    void update_cpu(Health value, const CpuMetrics &metrics) {
        cpu = {value, Evidence::Telemetry};
        cpu_metrics = metrics;
        observe_message();
    }

    void update_ram(Health value, const RamMetrics &metrics) {
        ram = {value, Evidence::Telemetry};
        ram_metrics = metrics;
        observe_message();
    }
};

struct StyledGlyph {
    wchar_t value = L'?';
    TextColor color = TextColor::Default;
};

struct StyledText {
    std::vector<StyledGlyph> glyphs;
    unsigned colored_symbols = 0;
};

// Only interpret the SGR sequences emitted by the demo. All other escape
// sequences and terminal control characters are discarded, never executed.
inline StyledText parse_styled_text(const std::string &input) {
    StyledText result;
    TextColor color = TextColor::Default;
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '\x1b') {
            if (input.compare(i, 4, "\x1b[0m") == 0) {
                color = TextColor::Default;
                i += 4;
                continue;
            }
            if (input.compare(i, 7, "\x1b[1;31m") == 0) {
                color = TextColor::Red;
                i += 7;
                continue;
            }
            if (input.compare(i, 7, "\x1b[1;33m") == 0) {
                color = TextColor::Yellow;
                i += 7;
                continue;
            }
            if (input.compare(i, 7, "\x1b[1;35m") == 0) {
                color = TextColor::Magenta;
                i += 7;
                continue;
            }
            // Remove unsupported CSI sequences entirely.
            ++i;
            if (i < input.size() && input[i] == '[') {
                ++i;
                for (unsigned length = 0; i < input.size() && length < 32; ++length) {
                    const unsigned char byte = static_cast<unsigned char>(input[i++]);
                    if (byte >= 0x40 && byte <= 0x7e)
                        break;
                }
            }
            color = TextColor::Default;
            continue;
        }

        const unsigned char byte = static_cast<unsigned char>(input[i]);
        if (byte < 0x20 || byte == 0x7f) {
            ++i;
            continue;
        }

        wchar_t glyph = L'?';
        std::mbstate_t state{};
        const std::size_t length = std::mbrtowc(&glyph, input.data() + i,
                                                  input.size() - i, &state);
        if (length == static_cast<std::size_t>(-1) ||
            length == static_cast<std::size_t>(-2) || length == 0) {
            // Skip one invalid byte, but keep the rest of a valid UTF-8 string.
            ++i;
            glyph = L'?';
        } else {
            i += length;
        }
        result.glyphs.push_back({glyph, color});
        if (color != TextColor::Default)
            ++result.colored_symbols;
    }
    return result;
}

struct ClientState {
    ServiceStatus radio;
    ServiceStatus engine;
    Direction direction = Direction::Unknown;
    Direction last_known_direction = Direction::Unknown;

    std::string station;
    std::string title;
    std::string artist;
    std::string last_good_station;
    std::string last_good_title;
    std::string last_good_artist;
    bool station_received = false;
    bool title_received = false;
    bool artist_received = false;
    bool volume_received = false;
    std::uint32_t volume = 0;
    bool ram_warning = false;
    unsigned demo_colored_symbols = 0;
    bool title_colored = false;
    bool artist_colored = false;
    std::uint64_t generation = 0;

    void cpu_health_event(ServiceKind service, Health value, const CpuMetrics &metrics) {
        (service == ServiceKind::Radio ? radio : engine).update_cpu(value, metrics);
        ++generation;
    }

    void ram_health_event(ServiceKind service, Health value, const RamMetrics &metrics) {
        (service == ServiceKind::Radio ? radio : engine).update_ram(value, metrics);
        ++generation;
    }

    void radio_availability(bool online) {
        radio.on_availability(online);
        if (!online) {
            ram_warning = false;
            station_received = title_received = artist_received = volume_received = false;
            title_colored = artist_colored = false;
            demo_colored_symbols = 0;
            station.clear();
            title.clear();
            artist.clear();
        }
        ++generation;
    }

    void engine_availability(bool online) {
        engine.on_availability(online);
        if (!online)
            direction = Direction::Unknown;
        ++generation;
    }

    void station_event(const std::string &value, const std::string &ram_marker) {
        if (radio.availability == Availability::Unavailable)
            return;
        station = value;
        station_received = true;
        radio.observe_message();
        if (value == ram_marker) {
            ram_warning = true;
            radio.ram = {Health::Failed, Evidence::DemoIndicator};
        } else {
            ram_warning = false;
            last_good_station = value;
            if (radio.ram.source == Evidence::DemoIndicator)
                radio.ram = {Health::Normal, Evidence::DemoIndicator};
        }
        ++generation;
    }

    void title_event(const std::string &value) {
        if (radio.availability == Availability::Unavailable)
            return;
        title = value;
        title_received = true;
        title_colored = parse_styled_text(value).colored_symbols != 0;
        if (!title_colored && !ram_warning)
            last_good_title = value;
        radio.observe_message();
        refresh_cpu_demo_indicator();
        ++generation;
    }

    void artist_event(const std::string &value) {
        if (radio.availability == Availability::Unavailable)
            return;
        artist = value;
        artist_received = true;
        artist_colored = parse_styled_text(value).colored_symbols != 0;
        if (!artist_colored && !ram_warning)
            last_good_artist = value;
        radio.observe_message();
        refresh_cpu_demo_indicator();
        ++generation;
    }

    void volume_event(std::uint32_t value) {
        if (radio.availability == Availability::Unavailable)
            return;
        volume = value;
        volume_received = true;
        radio.observe_message();
        ++generation;
    }

    void reverse_event(bool reversing) {
        if (engine.availability == Availability::Unavailable)
            return;
        direction = reversing ? Direction::Reverse : Direction::Forward;
        last_known_direction = direction;
        engine.observe_message();
        ++generation;
    }

private:
    void refresh_cpu_demo_indicator() {
        demo_colored_symbols =
                parse_styled_text(title).colored_symbols +
                parse_styled_text(artist).colored_symbols;
        // Metadata coloring is an indicator of the synthetic CPU demo, not
        // explicit CPU telemetry. Uncolored text does not prove CPU health.
        if (radio.cpu.source != Evidence::Telemetry)
            radio.cpu = demo_colored_symbols
                    ? HealthAxis{Health::Degraded, Evidence::DemoIndicator}
                    : HealthAxis{};
    }
};

} // namespace radio_ui
