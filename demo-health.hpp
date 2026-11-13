#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Demo-only SOME/IP health field shared by the engine service and radio client.
// The fixed 48-byte v1 payload uses network byte order for all integer fields.
// Unknown means the corresponding demo has not reported any measurement.
namespace demo_health {

enum class Level : std::uint8_t {
    Unknown = 0,
    Normal = 1,
    Degraded = 2,
    Failed = 3
};

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kPayloadSize = 48;
constexpr std::uint8_t kCpuMetricsValid = 0x01;
constexpr std::uint8_t kRamMetricsValid = 0x02;

struct Message {
    Level cpu = Level::Unknown;
    Level ram = Level::Unknown;
    bool cpu_metrics_valid = false;
    bool ram_metrics_valid = false;
    std::uint32_t sequence = 0;
    std::uint32_t cpu_share_centi_percent = 0;  // Percent * 100, e.g. 32.50% -> 3250.
    std::uint32_t cpu_wall_ms = 0;
    std::uint32_t cpu_off_ms = 0;
    std::uint32_t cpu_overrun_ms = 0;
    std::uint32_t cpu_deadline_misses = 0;       // Cumulative since service start.
    std::uint64_t ram_requested_bytes = 0;
    std::uint64_t ram_allocated_bytes = 0;
    std::uint32_t ram_allocation_failures = 0;   // Cumulative since service start.
};

inline void write_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline void write_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

inline std::uint32_t read_u32(const std::uint8_t *bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

inline std::uint64_t read_u64(const std::uint8_t *bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

inline std::vector<std::uint8_t> encode(const Message &message) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kPayloadSize);
    bytes.push_back(kVersion);
    bytes.push_back(static_cast<std::uint8_t>(message.cpu));
    bytes.push_back(static_cast<std::uint8_t>(message.ram));
    bytes.push_back(static_cast<std::uint8_t>(
            (message.cpu_metrics_valid ? kCpuMetricsValid : 0) |
            (message.ram_metrics_valid ? kRamMetricsValid : 0)));
    write_u32(bytes, message.sequence);
    write_u32(bytes, message.cpu_share_centi_percent);
    write_u32(bytes, message.cpu_wall_ms);
    write_u32(bytes, message.cpu_off_ms);
    write_u32(bytes, message.cpu_overrun_ms);
    write_u32(bytes, message.cpu_deadline_misses);
    write_u64(bytes, message.ram_requested_bytes);
    write_u64(bytes, message.ram_allocated_bytes);
    write_u32(bytes, message.ram_allocation_failures);
    return bytes;
}

inline bool decode(const std::uint8_t *bytes, std::size_t length, Message &message) {
    if (bytes == nullptr || length != kPayloadSize || bytes[0] != kVersion ||
        bytes[1] > static_cast<std::uint8_t>(Level::Failed) ||
        bytes[2] > static_cast<std::uint8_t>(Level::Failed) ||
        (bytes[3] & ~(kCpuMetricsValid | kRamMetricsValid)) != 0)
        return false;

    Message decoded;
    decoded.cpu = static_cast<Level>(bytes[1]);
    decoded.ram = static_cast<Level>(bytes[2]);
    decoded.cpu_metrics_valid = (bytes[3] & kCpuMetricsValid) != 0;
    decoded.ram_metrics_valid = (bytes[3] & kRamMetricsValid) != 0;
    decoded.sequence = read_u32(bytes + 4);
    decoded.cpu_share_centi_percent = read_u32(bytes + 8);
    decoded.cpu_wall_ms = read_u32(bytes + 12);
    decoded.cpu_off_ms = read_u32(bytes + 16);
    decoded.cpu_overrun_ms = read_u32(bytes + 20);
    decoded.cpu_deadline_misses = read_u32(bytes + 24);
    decoded.ram_requested_bytes = read_u64(bytes + 28);
    decoded.ram_allocated_bytes = read_u64(bytes + 36);
    decoded.ram_allocation_failures = read_u32(bytes + 44);
    message = decoded;
    return true;
}

} // namespace demo_health
