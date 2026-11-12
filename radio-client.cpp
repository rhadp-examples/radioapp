#define _XOPEN_SOURCE_EXTENDED 1

#include <ncursesw/curses.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>

#include <vsomeip/vsomeip.hpp>

#include "radio-client-state.hpp"
#include "services.hpp"

namespace {

using radio_ui::Availability;
using radio_ui::ClientState;
using radio_ui::Communication;
using radio_ui::Direction;
using radio_ui::Evidence;
using radio_ui::Health;
using radio_ui::ServiceStatus;
using radio_ui::StyledText;
using radio_ui::TextColor;

constexpr int kRefreshMs = 100;
constexpr int kMonitorMinRows = 16;

std::shared_ptr<vsomeip::application> app;
std::mutex state_mutex;
ClientState state;
std::atomic<bool> running{true};

// SOME/IP callbacks never call ncurses: the UI thread owns the terminal.
template <typename F>
void update_state(F update) {
    std::lock_guard<std::mutex> lock(state_mutex);
    update(state);
}

void on_volume_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::uint32_t value = get_arg0(event);
    update_state([&](ClientState &s) { s.volume_event(value); });
}

void on_song_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) { s.title_event(value); });
}

void on_artist_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) { s.artist_event(value); });
}

void on_station_message(const std::shared_ptr<vsomeip::message> &event) {
    const std::string value = get_arg0_string(event);
    update_state([&](ClientState &s) {
        s.station_event(value, RADIO_RAM_FAILURE_MARKER);
    });
}

void on_radio_availability(vsomeip::service_t, vsomeip::instance_t, bool available) {
    update_state([&](ClientState &s) { s.radio_availability(available); });
}

void on_engine_availability(vsomeip::service_t, vsomeip::instance_t, bool available) {
    update_state([&](ClientState &s) { s.engine_availability(available); });
}

void on_engine_reverse_event(const std::shared_ptr<vsomeip::message> &event) {
    const bool reversed = get_arg0(event) != 0;
    update_state([&](ClientState &s) { s.reverse_event(reversed); });
}

void request_radio_events() {
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_VOLUME_EVENT_ID, on_volume_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_STATION_EVENT_ID, on_station_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_SONG_EVENT_ID, on_song_message);
    app->register_message_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                  RADIO_ARTIST_EVENT_ID, on_artist_message);

    const std::set<vsomeip::eventgroup_t> groups{RADIO_EVENTGROUP_ID};
    for (vsomeip::event_t id : {RADIO_VOLUME_EVENT_ID, RADIO_STATION_EVENT_ID,
                                RADIO_SONG_EVENT_ID, RADIO_ARTIST_EVENT_ID}) {
        app->request_event(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, id, groups,
                           vsomeip::event_type_e::ET_FIELD);
    }
    app->subscribe(RADIO_SERVICE_ID, RADIO_INSTANCE_ID, RADIO_EVENTGROUP_ID);
}

void request_engine_events() {
    app->register_message_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                                  ENGINE_REVERSE_EVENT_ID, on_engine_reverse_event);
    const std::set<vsomeip::eventgroup_t> groups{ENGINE_EVENTGROUP_ID};
    app->request_event(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                       ENGINE_REVERSE_EVENT_ID, groups, vsomeip::event_type_e::ET_FIELD);
    app->subscribe(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID, ENGINE_EVENTGROUP_ID);
}

bool setup_vsomeip() {
    app = vsomeip::runtime::get()->create_application("Radio client");
    if (!app || !app->init())
        return false;
    app->register_availability_handler(RADIO_SERVICE_ID, RADIO_INSTANCE_ID,
                                       on_radio_availability);
    app->register_availability_handler(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID,
                                       on_engine_availability);
    request_radio_events();
    request_engine_events();
    app->request_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
    app->request_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);
    return true;
}

enum ColorPair {
    PairNormal = 0,
    PairGood = 1,
    PairWarning = 2,
    PairError = 3,
    PairMagenta = 4,
    PairMuted = 5,
    PairHeading = 6
};

int palette_color(TextColor color) {
    switch (color) {
    case TextColor::Red: return PairError;
    case TextColor::Yellow: return PairWarning;
    case TextColor::Magenta: return PairMagenta;
    case TextColor::Default: return PairNormal;
    }
    return PairNormal;
}

class Terminal {
public:
    bool initialize() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            std::cerr << "radio-client requires an interactive terminal (TTY)\n";
            return false;
        }
        std::setlocale(LC_ALL, "");
        if (MB_CUR_MAX == 1)
            std::setlocale(LC_CTYPE, "C.UTF-8");
        if (!std::getenv("TERM"))
            setenv("TERM", "xterm", 0);

        // Use a dedicated copy of stdout for ncurses. Library messages can
        // subsequently go to a log file without corrupting the display.
        const int screen_fd = dup(STDOUT_FILENO);
        if (screen_fd < 0)
            return false;
        screen_output_ = fdopen(screen_fd, "w");
        if (!screen_output_) {
            close(screen_fd);
            return false;
        }
        setvbuf(screen_output_, nullptr, _IONBF, 0);
        screen_ = newterm(nullptr, screen_output_, stdin);
        if (!screen_)
            return false;
        set_term(screen_);
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        wtimeout(stdscr, kRefreshMs);
        curs_set(0);
        set_escdelay(25);
        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(PairGood, COLOR_GREEN, -1);
            init_pair(PairWarning, COLOR_YELLOW, -1);
            init_pair(PairError, COLOR_RED, -1);
            init_pair(PairMagenta, COLOR_MAGENTA, -1);
            init_pair(PairMuted, COLOR_CYAN, -1);
            init_pair(PairHeading, COLOR_WHITE, -1);
            colors_ = true;
        }
        redirect_console_logs();
        return true;
    }

    ~Terminal() {
        if (screen_) {
            set_term(screen_);
            endwin();
            delscreen(screen_);
        }
        if (screen_output_)
            fclose(screen_output_);
        if (saved_stdout_ >= 0) {
            fflush(stdout);
            dup2(saved_stdout_, STDOUT_FILENO);
            close(saved_stdout_);
        }
        if (saved_stderr_ >= 0) {
            fflush(stderr);
            dup2(saved_stderr_, STDERR_FILENO);
            close(saved_stderr_);
        }
    }

    int read_key(wint_t &key) const {
        return get_wch(&key);
    }

    void render(const ClientState &s, bool monitor) const {
        erase();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        if (rows < 6 || cols < 28) {
            draw_text(0, 0, cols - 1, "Resize terminal (min 28x6)", PairWarning, true);
            refresh();
            return;
        }
        const int content_width = cols - 2;
        if (s.radio.availability == Availability::Unavailable) {
            draw_text(0, 1, content_width, "RADIO UNAVAILABLE", PairError, true);
            draw_text(1, 1, content_width, "Waiting for radio service", PairMuted);
            draw_text(2, 1, content_width, "Last title: " + s.last_good_title, PairMuted);
        } else if (s.radio.availability == Availability::Unknown) {
            draw_text(0, 1, content_width, "RADIO", PairHeading, true);
            draw_text(1, 1, content_width, "Connecting to radio service...", PairMuted);
        } else if (s.ram_warning) {
            draw_text(0, 1, content_width, "RADIO / MEMORY WARNING", PairError, true);
            draw_text(1, 1, content_width, "!  R A M  !", PairError, true, true);
            draw_text(2, 1, content_width, "Memory allocation failed", PairError);
        } else {
            draw_text(0, 1, content_width,
                      s.station_received ? s.station : "Radio station: --", PairHeading, true);
            draw_text(1, 1, content_width,
                      s.title_received ? s.title : "Waiting for song...", PairNormal, true);
            draw_text(2, 1, content_width,
                      s.artist_received ? s.artist : "", PairMuted);
        }
        draw_volume(3, cols, s);
        draw_status(4, cols, s);
        draw_text(5, 1, content_width,
                  "+/- Volume   SPACE Station   ESC Power   M Monitor   Q Quit", PairMuted);
        if (monitor)
            draw_monitor(s, rows, cols);
        refresh();
    }

private:
    void redirect_console_logs() {
        const char *path = std::getenv("RADIO_CLIENT_LOG_FILE");
        if (!path)
            path = "/tmp/radio-client-vsomeip.log";
        if (std::strcmp(path, "-") == 0)
            return;
        const int log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (log_fd < 0)
            return;
        saved_stdout_ = dup(STDOUT_FILENO);
        saved_stderr_ = dup(STDERR_FILENO);
        fflush(stdout);
        fflush(stderr);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
    }

    int attribute(int pair, bool bold = false, bool reverse = false) const {
        int attr = 0;
        if (colors_ && pair)
            attr |= COLOR_PAIR(pair);
        if (bold || (pair == PairError && !colors_))
            attr |= A_BOLD;
        if (reverse)
            attr |= A_REVERSE;
        return attr;
    }

    void draw_text(int row, int col, int width, const std::string &text,
                   int pair = PairNormal, bool bold = false, bool reverse = false) const {
        if (width <= 0)
            return;
        draw_styled(row, col, width, radio_ui::parse_styled_text(text),
                    pair, bold, reverse);
    }

    void draw_styled(int row, int col, int width, const StyledText &styled,
                     int base_pair, bool bold, bool reverse) const {
        int screen_rows, screen_cols;
        getmaxyx(stdscr, screen_rows, screen_cols);
        if (row < 0 || row >= screen_rows || col >= screen_cols - 1)
            return;
        const int limit = std::min(screen_cols - 1, col + width);
        int x = col;
        for (const auto &glyph : styled.glyphs) {
            const int char_width = wcwidth(glyph.value);
            if (char_width <= 0)
                continue;
            if (x + char_width > limit)
                break;
            const int color = glyph.color == TextColor::Default
                    ? base_pair : palette_color(glyph.color);
            const int attr = attribute(color, bold || glyph.color != TextColor::Default, reverse);
            wchar_t character[2] = {glyph.value, L'\0'};
            attron(attr);
            mvaddnwstr(row, x, character, 1);
            attroff(attr);
            x += char_width;
        }
    }

    void draw_volume(int row, int cols, const ClientState &s) const {
        const std::string label = "VOL ";
        draw_text(row, 1, cols - 2, label, PairMuted);
        if (!s.volume_received || s.radio.availability != Availability::Available) {
            draw_text(row, 5, cols - 6, "--", PairMuted);
            return;
        }
        const unsigned volume = std::min(100u, s.volume);
        const int bar_size = std::min(20, std::max(0, cols - 17));
        if (bar_size < 4) {
            draw_text(row, 5, cols - 6, std::to_string(volume) + "%");
            return;
        }
        const int filled = static_cast<int>((volume * bar_size + 50) / 100);
        const std::string meter = "[" + std::string(filled, '#') +
                                  std::string(bar_size - filled, '-') + "] ";
        draw_text(row, 5, cols - 6, meter + std::to_string(volume) + "%",
                  PairNormal);
    }

    void draw_status(int row, int cols, const ClientState &s) const {
        int x = 1;
        const int limit = cols - 1;
        auto token = [&](const std::string &value, int pair, bool bold = false) {
            if (x >= limit)
                return;
            draw_text(row, x, limit - x, value, pair, bold);
            x += static_cast<int>(value.size());
        };
        auto sep = [&]() { token(" | ", PairMuted); };

        if (s.engine.availability != Availability::Available ||
            s.direction == Direction::Unknown) {
            token("? DIR", PairWarning, true);
        } else if (s.direction == Direction::Reverse) {
            token("< REV", PairWarning, true);
        } else {
            token("> FWD", PairGood, true);
        }
        sep();
        draw_service_status(token, "R", s.radio);
        sep();
        draw_service_status(token, "E", s.engine);
        if (s.radio.cpu.state == Health::Degraded || s.radio.cpu.state == Health::Failed) {
            sep();
            token("R:CPU!", PairWarning, true);
        }
        if (s.radio.ram.state == Health::Degraded || s.radio.ram.state == Health::Failed) {
            sep();
            token("R:RAM!", PairError, true);
        }
        if (s.engine.cpu.state == Health::Degraded || s.engine.cpu.state == Health::Failed) {
            sep();
            token("E:CPU!", PairWarning, true);
        }
        if (s.engine.ram.state == Health::Degraded || s.engine.ram.state == Health::Failed) {
            sep();
            token("E:RAM!", PairError, true);
        }
    }

    template <typename Add>
    static void draw_service_status(Add add, const char *name, const ServiceStatus &service) {
        std::string label(name);
        if (service.availability == Availability::Available &&
            service.communication == Communication::Stale)
            add(label + ":STALE", PairWarning, true);
        else if (service.availability == Availability::Available)
            add(label + ":+", PairGood, true);
        else if (service.availability == Availability::Unavailable)
            add(label + ":OFF", PairError, true);
        else
            add(label + ":?", PairWarning, true);
    }

    static const char *direction_text(Direction value) {
        switch (value) {
        case Direction::Forward: return "FWD";
        case Direction::Reverse: return "REV";
        case Direction::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    static const char *availability_text(Availability value) {
        switch (value) {
        case Availability::Available: return "AVAILABLE";
        case Availability::Unavailable: return "UNAVAILABLE";
        case Availability::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }

    static const char *communication_text(Communication value) {
        switch (value) {
        case Communication::Unknown: return "UNKNOWN";
        case Communication::Receiving: return "EVENTS SEEN";
        case Communication::Unavailable: return "UNAVAILABLE";
        case Communication::Stale: return "STALE";
        }
        return "UNKNOWN";
    }

    static std::string health_text(const radio_ui::HealthAxis &axis) {
        const char *status = "UNKNOWN";
        switch (axis.state) {
        case Health::Normal: status = "NORMAL"; break;
        case Health::Degraded: status = "DEGRADED"; break;
        case Health::Failed: status = "FAILED"; break;
        case Health::Unknown: break;
        }
        if (axis.source == Evidence::DemoIndicator)
            return std::string(status) + " (demo)";
        return status;
    }

    static std::string age_text(const ServiceStatus &service) {
        if (!service.has_message)
            return "--";
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - service.last_message).count();
        std::ostringstream out;
        out << std::fixed << std::setprecision(1)
            << std::max<std::int64_t>(0, age) / 1000.0 << "s ago";
        return out.str();
    }

    static std::string cpu_text(const ServiceStatus &service, bool share) {
        if (!service.cpu_metrics.available)
            return "--";
        std::ostringstream out;
        out << std::fixed << std::setprecision(1);
        if (share)
            out << service.cpu_metrics.cpu_share_percent << "%";
        else
            out << service.cpu_metrics.deadline_overrun_ms << "ms";
        return out.str();
    }

    static std::string ram_text(const ServiceStatus &service) {
        if (!service.ram_metrics.available)
            return "--";
        std::ostringstream out;
        out << std::fixed << std::setprecision(0)
            << service.ram_metrics.allocated_bytes / (1024.0 * 1024.0) << " MiB";
        return out.str();
    }

    void draw_monitor(const ClientState &s, int rows, int cols) const {
        if (rows < kMonitorMinRows || cols < 54) {
            draw_text(7, 1, cols - 2, "Monitor: resize to at least 54x16", PairWarning);
            return;
        }
        draw_text(7, 1, cols - 2, "INTERFERENCE MONITOR  (M to close)", PairHeading, true);
        const int col_radio = std::max(20, cols / 3);
        const int col_engine = std::max(37, 2 * cols / 3);
        draw_text(8, col_radio, col_engine - col_radio - 2, "RADIO", PairHeading, true);
        draw_text(8, col_engine, cols - col_engine - 1, "ENGINE", PairHeading, true);
        auto line = [&](int row, const std::string &name,
                        const std::string &radio_value, const std::string &engine_value) {
            draw_text(row, 1, col_radio - 2, name, PairMuted);
            draw_text(row, col_radio, col_engine - col_radio - 2, radio_value);
            draw_text(row, col_engine, cols - col_engine - 1, engine_value);
        };
        line(9,  "Service", availability_text(s.radio.availability),
               availability_text(s.engine.availability));
        line(10, "CPU", health_text(s.radio.cpu), health_text(s.engine.cpu));
        line(11, "RAM", health_text(s.radio.ram), health_text(s.engine.ram));
        line(12, "Data link", communication_text(s.radio.communication),
             communication_text(s.engine.communication));
        line(13, "Last event", age_text(s.radio), age_text(s.engine));
        line(14, "CPU share", cpu_text(s.radio, true), cpu_text(s.engine, true));
        line(15, "Deadline overrun", cpu_text(s.radio, false), cpu_text(s.engine, false));
        if (rows >= 17)
            line(16, "Allocated RAM", ram_text(s.radio), ram_text(s.engine));
        if (rows >= 18) {
            std::string direction = direction_text(s.direction);
            if (s.direction == Direction::Unknown &&
                s.last_known_direction != Direction::Unknown)
                direction = std::string(direction_text(s.last_known_direction)) + " (last)";
            line(17, "Direction", "--", direction);
        }
        if (rows >= 20) {
            draw_text(19, 1, cols - 2,
                      "'demo' = inferred from metadata; -- = no SOME/IP telemetry", PairMuted);
        }
    }

    SCREEN *screen_ = nullptr;
    FILE *screen_output_ = nullptr;
    int saved_stdout_ = -1;
    int saved_stderr_ = -1;
    bool colors_ = false;
};

void handle_key(wint_t key, bool &monitor_visible, bool &off_requested) {
    std::uint32_t method = 0;
    std::uint32_t argument = 0;
    switch (key) {
    case L'q': case L'Q':
        running.store(false);
        return;
    case L'm': case L'M':
        monitor_visible = !monitor_visible;
        return;
    case L'+':
        method = RADIO_CHANGE_VOLUME_METHOD_ID;
        argument = 10;
        break;
    case L'-':
        method = RADIO_CHANGE_VOLUME_METHOD_ID;
        argument = static_cast<std::uint32_t>(-10);
        break;
    case L' ':
        method = RADIO_SWITCH_STATION_METHOD_ID;
        break;
    case 27:
        method = RADIO_SET_PLAYING_METHOD_ID;
        argument = off_requested ? 1 : 0;
        off_requested = !off_requested;
        break;
    default:
        return;
    }
    app->send(new_request(method, argument));
}

} // namespace

int main() {
    if (!setup_vsomeip()) {
        std::cerr << "Couldn't initialize the radio client\n";
        return EXIT_FAILURE;
    }

    {
        Terminal terminal;
        if (!terminal.initialize()) {
            std::cerr << "Couldn't initialize ncursesw\n";
            return EXIT_FAILURE;
        }

        std::thread vsomeip_thread([] {
            app->start();
            running.store(false);
        });

        bool monitor_visible = false;
        bool off_requested = false;
        std::uint64_t rendered_generation = static_cast<std::uint64_t>(-1);
        bool redraw = true;
        auto last_rendered = std::chrono::steady_clock::time_point{};
        while (running.load()) {
            wint_t key = 0;
            const int result = terminal.read_key(key);
            if (result == KEY_CODE_YES && key == KEY_RESIZE)
                redraw = true;
            else if (result == OK) {
                handle_key(key, monitor_visible, off_requested);
                redraw = true;
            }

            ClientState snapshot;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                snapshot = state;
            }
            const auto now = std::chrono::steady_clock::now();
            if (redraw || snapshot.generation != rendered_generation ||
                (monitor_visible && now - last_rendered >= std::chrono::seconds(1))) {
                terminal.render(snapshot, monitor_visible);
                rendered_generation = snapshot.generation;
                last_rendered = now;
                redraw = false;
            }
        }

        app->stop();
        vsomeip_thread.join();
        app->clear_all_handler();
        app->release_service(RADIO_SERVICE_ID, RADIO_INSTANCE_ID);
        app->release_service(ENGINE_SERVICE_ID, ENGINE_INSTANCE_ID);

    } // Restore the terminal before the original DLT shutdown workaround.
    std::fflush(nullptr);
    _exit(EXIT_SUCCESS);
}
