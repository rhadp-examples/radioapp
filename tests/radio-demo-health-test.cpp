#include "demo-health.hpp"
#include "radio-demo-health.hpp"
#include "radio-client-state.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std::literals::chrono_literals;

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double observed, double expected, double tolerance = 0.02) {
    return std::fabs(observed - expected) < tolerance;
}

} // namespace

int main() {
    using radio_demo::HealthTracker;
    using demo_health::Level;
    using radio_ui::Evidence;
    using radio_ui::Health;

    const auto now = HealthTracker::clock::now();
    HealthTracker tracker(300, 8ULL * 1024 * 1024 * 1024);
    auto health = tracker.snapshot(now);
    check(health.cpu == Level::Unknown && health.ram == Level::Unknown,
          "No measurements must not be reported as healthy");
    check(!health.cpu_metrics_valid && !health.ram_metrics_valid,
          "Initial metrics must be invalid");
    check(health.ram_requested_bytes == 8ULL * 1024 * 1024 * 1024,
          "RAM target should be known even before the first allocation");

    auto sample = tracker.record_cpu(900ms, 300000000ULL, now);
    check(near(sample.cpu_share_percent, 100.0 / 3.0),
          "CPU share must use thread CPU time divided by wall time");
    check(near(sample.off_cpu_ms, 600.0), "Off-CPU time mismatch");
    check(near(sample.overrun_ms, 560.0), "Deadline calculation mismatch");
    health = tracker.snapshot(now);
    check(health.cpu == Level::Degraded && health.cpu_metrics_valid,
          "Deadline miss must set CPU to degraded");
    check(health.cpu_share_centi_percent == 3333, "CPU percentage wire units mismatch");
    check(health.cpu_wall_ms == 900 && health.cpu_off_ms == 600 &&
          health.cpu_overrun_ms == 560 && health.cpu_deadline_misses == 1,
          "CPU metrics mismatch");

    check(tracker.snapshot(now + 1999ms).cpu == Level::Degraded,
          "Short CPU warning hold should persist");
    check(tracker.snapshot(now + 2s).cpu == Level::Normal,
          "CPU warning should clear after two seconds without new misses");
    tracker.record_cpu(300ms, 300000000ULL, now + 2500ms);
    check(tracker.snapshot(now + 2500ms).cpu == Level::Normal,
          "On-time CPU work must report normal");
    check(tracker.snapshot(now + 2500ms).cpu_deadline_misses == 1,
          "Deadline miss counter must be cumulative");

    tracker.record_ram_allocation(false, 0, true);
    health = tracker.snapshot(now);
    check(health.ram == Level::Failed && health.ram_metrics_valid &&
          health.ram_allocated_bytes == 0 && health.ram_allocation_failures == 1,
          "RAM allocation failure must be reported");
    tracker.record_ram_allocation(true, 128ULL * 1024 * 1024, true);
    check(tracker.snapshot(now).ram == Level::Failed,
          "Successful RAM allocation must not cancel the warning hold");
    tracker.refresh_ram(128ULL * 1024 * 1024, false);
    check(tracker.snapshot(now).ram == Level::Normal,
          "RAM status must recover after the warning hold");
    tracker.refresh_ram(0, false);
    check(tracker.snapshot(now).ram_allocated_bytes == 0,
          "Releasing the buffer must update allocated RAM");
    check(tracker.snapshot(now).ram_allocation_failures == 1,
          "RAM failure count must be cumulative");

    demo_health::Message wire_message = tracker.snapshot(now + 3s);
    wire_message.sequence = 17;
    auto wire = demo_health::encode(wire_message);
    demo_health::Message roundtrip;
    check(demo_health::decode(wire.data(), wire.size(), roundtrip),
          "Radio health payload must round-trip through the shared protocol");
    check(roundtrip.sequence == 17 && roundtrip.cpu == Level::Normal &&
          roundtrip.ram == Level::Normal && roundtrip.cpu_metrics_valid &&
          roundtrip.ram_metrics_valid,
          "Radio health payload fields differ after decoding");

    radio_ui::ClientState client;
    client.radio_availability(true);
    client.engine_availability(true);
    client.station_event("Radio Los Santos", "__RADIO_DEMO_RAM_FAILURE__");
    client.title_event("Title\x1b[1;31m#\x1b[0m");
    check(client.radio.cpu.source == Evidence::DemoIndicator &&
          client.radio.cpu.state == Health::Degraded,
          "Prior to telemetry, colored metadata should remain a demo indicator");

    wire_message.cpu = Level::Normal;
    wire_message.ram = Level::Failed;
    wire_message.cpu_overrun_ms = 0;
    wire_message.ram_allocation_failures = 1;
    client.radio_health_event(wire_message);
    check(client.radio.cpu.source == Evidence::Telemetry &&
          client.radio.cpu.state == Health::Normal,
          "Explicit radio CPU telemetry must override the demo indicator");
    check(client.radio.ram.state == Health::Failed && client.ram_warning,
          "Explicit radio RAM failure must be visible");
    check(client.radio.cpu_metrics.available && client.radio.ram_metrics.available,
          "Radio metrics must be populated from a health event");
    check(client.radio_health_seen && client.radio_health_sequence == 17,
          "Radio health heartbeat and sequence must be recorded");

    client.title_event("Title\x1b[1;35m!\x1b[0m");
    client.station_event("Radio Los Santos", "__RADIO_DEMO_RAM_FAILURE__");
    check(client.radio.cpu.source == Evidence::Telemetry &&
          client.radio.ram.source == Evidence::Telemetry,
          "Metadata updates must not override explicit telemetry");
    wire_message.ram = Level::Normal;
    client.radio_health_event(wire_message);
    check(!client.ram_warning, "RAM warning must clear after explicit recovery");
    client.station_event("__RADIO_DEMO_RAM_FAILURE__", "__RADIO_DEMO_RAM_FAILURE__");
    check(!client.ram_warning,
          "A delayed old RAM marker must not override newer recovery telemetry");
    client.station_event("Radio Los Santos", "__RADIO_DEMO_RAM_FAILURE__");

    client.refresh_radio_health(client.radio_health_at + 5s);
    check(client.radio.cpu_metrics.available == false &&
          client.radio.ram_metrics.available == false,
          "Stale health metrics must be invalidated");
    check(client.radio.cpu.state == Health::Degraded &&
          client.radio.cpu.source == Evidence::DemoIndicator,
          "Stale health must fall back to colored metadata, not normal");
    check(client.radio.ram.source == Evidence::DemoIndicator,
          "Stale radio RAM health should fall back to the station marker");

    client.radio_health_event(wire_message);
    check(client.radio.cpu.source == Evidence::Telemetry &&
          client.radio.cpu_metrics.available,
          "Fresh radio health should recover after stale telemetry");
    client.radio_availability(false);
    check(!client.radio_health_seen && !client.radio.cpu_metrics.available,
          "Radio disconnection must clear health state");
    client.radio_health_event(wire_message);
    check(!client.radio_health_seen,
          "Radio health events must be ignored while unavailable");

    // Engine health handling is unchanged and independent of the radio state.
    client.engine_health_event(wire_message);
    check(client.engine_health_seen && !client.radio_health_seen,
          "Radio and Engine health lifetimes must be independent");

    std::cout << "radio health sampling, SOME/IP encoding and UI integration tests passed\n";
}
