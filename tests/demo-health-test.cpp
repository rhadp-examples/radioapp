#include "demo-health.hpp"
#include "engine-demo-memory.hpp"
#include "radio-client-state.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace std::literals::chrono_literals;

int main() {
    demo_health::Message outgoing;
    outgoing.cpu = demo_health::Level::Degraded;
    outgoing.ram = demo_health::Level::Failed;
    outgoing.cpu_metrics_valid = true;
    outgoing.ram_metrics_valid = true;
    outgoing.sequence = 0x01020304;
    outgoing.cpu_share_centi_percent = 3175;
    outgoing.cpu_wall_ms = 945;
    outgoing.cpu_off_ms = 645;
    outgoing.cpu_overrun_ms = 605;
    outgoing.cpu_deadline_misses = 15;
    outgoing.ram_requested_bytes = 8ULL * 1024 * 1024 * 1024;
    outgoing.ram_allocated_bytes = 0;
    outgoing.ram_allocation_failures = 3;

    auto wire = demo_health::encode(outgoing);
    assert(wire.size() == demo_health::kPayloadSize);
    assert(wire[0] == demo_health::kVersion);
    assert(wire[4] == 1 && wire[5] == 2 && wire[6] == 3 && wire[7] == 4);
    demo_health::Message incoming;
    assert(demo_health::decode(wire.data(), wire.size(), incoming));
    assert(incoming.sequence == outgoing.sequence);
    assert(incoming.cpu == outgoing.cpu);
    assert(incoming.ram == outgoing.ram);
    assert(incoming.cpu_share_centi_percent == 3175);
    assert(incoming.cpu_overrun_ms == 605);
    assert(incoming.ram_requested_bytes == outgoing.ram_requested_bytes);
    assert(incoming.ram_allocation_failures == 3);
    assert(!demo_health::decode(wire.data(), wire.size() - 1, incoming));
    assert(!demo_health::decode(nullptr, wire.size(), incoming));
    wire[0] = 2;
    assert(!demo_health::decode(wire.data(), wire.size(), incoming));
    wire[0] = demo_health::kVersion;
    wire[1] = 99;
    assert(!demo_health::decode(wire.data(), wire.size(), incoming));
    wire[1] = static_cast<std::uint8_t>(outgoing.cpu);
    wire[3] = 0x80;
    assert(!demo_health::decode(wire.data(), wire.size(), incoming));

    radio_ui::ClientState client;
    client.engine_availability(true);
    assert(client.engine.cpu.state == radio_ui::Health::Unknown);
    assert(client.engine.ram.state == radio_ui::Health::Unknown);
    client.engine_health_event(outgoing);
    assert(client.engine.cpu.state == radio_ui::Health::Degraded);
    assert(client.engine.ram.state == radio_ui::Health::Failed);
    assert(client.engine.cpu.source == radio_ui::Evidence::Telemetry);
    assert(client.engine.ram.source == radio_ui::Evidence::Telemetry);
    assert(client.engine.cpu_metrics.available);
    assert(client.engine.cpu_metrics.cpu_share_percent == 31.75);
    assert(client.engine.cpu_metrics.deadline_overrun_ms == 605.0);
    assert(client.engine.ram_metrics.requested_bytes == outgoing.ram_requested_bytes);
    assert(client.engine.ram_metrics.allocation_failures == 3);
    assert(client.engine_health_sequence == outgoing.sequence);

    client.refresh_engine_health(client.engine_health_at + 5s);
    assert(client.engine.communication == radio_ui::Communication::Stale);
    assert(client.engine.cpu.state == radio_ui::Health::Unknown);
    assert(client.engine.ram.state == radio_ui::Health::Unknown);

    outgoing.cpu = demo_health::Level::Normal;
    outgoing.ram = demo_health::Level::Normal;
    outgoing.cpu_overrun_ms = 0;
    outgoing.ram_allocated_bytes = outgoing.ram_requested_bytes;
    client.engine_health_event(outgoing);
    assert(client.engine.cpu.state == radio_ui::Health::Normal);
    assert(client.engine.ram.state == radio_ui::Health::Normal);
    assert(client.engine.communication == radio_ui::Communication::Receiving);

    client.engine_availability(false);
    assert(client.engine.cpu.state == radio_ui::Health::Unknown);
    assert(client.engine.ram.state == radio_ui::Health::Unknown);
    assert(!client.engine_health_seen);
    client.engine_health_event(outgoing);
    assert(client.engine.cpu.state == radio_ui::Health::Unknown);

    // RAM warning lasts a minimum of 15 seconds, even after recovery.
    engine_demo::demo_ram_warning_state warning;
    const auto start = engine_demo::demo_ram_warning_state::clock::now();
    warning.record_failure(start);
    warning.clear_allocation_failure();
    assert(warning.active(start + 14s));
    assert(!warning.active(start + 15s));
    warning.record_failure(start + 10s);
    warning.clear_allocation_failure();
    assert(warning.active(start + 20s));
    assert(!warning.active(start + 25s));

    // Exercise size parsing, resident allocation, and release separately.
    setenv("ENGINE_DEMO_RAM_SIZE", "8G", 1);
    assert(engine_demo::demo_ram_size_bytes() == 8ULL * 1024 * 1024 * 1024);
    setenv("ENGINE_DEMO_RAM_SIZE", "512M", 1);
    assert(engine_demo::demo_ram_size_bytes() == 512ULL * 1024 * 1024);
    setenv("ENGINE_DEMO_RAM_SIZE", "0", 1);
    assert(engine_demo::demo_ram_size_bytes() == 0);

    engine_demo::demo_ram_buffer buffer;
    std::string reason;
    std::uint64_t rolled_back = 0;
    if (engine_demo::demo_ram_has_capacity(16384, reason)) {
        assert(buffer.acquire(16384, reason, rolled_back));
        assert(rolled_back == 0);
        buffer.release();
    }
    // The large request must be rejected without touching memory.
    reason.clear();
    assert(!buffer.acquire(1024ULL * 1024 * 1024 * 1024, reason, rolled_back));
    assert(!reason.empty());
    buffer.release();

    std::cout << "demo health protocol, UI state and RAM warning tests passed\n";
}
