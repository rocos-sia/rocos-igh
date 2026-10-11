#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include <ecrt.h>
#include "slave_config.hpp"

namespace rocos {

// Owns only preallocated check descriptors. IgH owns the request handles.
// prepare() runs before activation; step() never allocates, waits or logs.
class PdoVerification {
public:
    enum class Result { Pending, Complete, Failed };
    enum class Failure {
        None, NotPrepared, SetIndex, StartRead, Read, Size, Value,
        RequestTimeout, StartupTimeout, BusUnhealthy, Write, PreopState,
        PreopTimeout, Clock
    };
    enum class Field { AssignmentCount, Assignment, MappingCount, MappingEntry, InitCommand };
    struct Check {
        std::uint16_t slave_position;
        std::uint8_t sync_index;
        std::uint16_t index;
        std::uint8_t subindex;
        std::uint8_t size;
        std::uint32_t expected;
        Field field;
        std::size_t slave_index;
    };
    struct Error {
        Failure failure{Failure::None};
        Check check{};
        std::uint32_t actual{0};
        std::size_t actual_size{0};
        int api_error{0};
        std::uint32_t abort_code{0};
    };

    static constexpr std::uint32_t kRequestTimeoutMs = 1000;
    bool prepare(const StaticSlaveConfig &config,
                 const std::vector<ec_slave_config_t *> &slaves, std::string &error);
    // Blocking SDOs: init commands (once, in order), then PDO writes/readback.
    // Initialization only, before ecrt_master_activate().
    // timeout_ms is shared by all slaves/attempts; an in-flight IgH ioctl cannot
    // be cancelled by this budget. Its result is checked again after return.
    bool configurePreop(ec_master_t *master, std::uint32_t timeout_ms,
                        std::ostream &log, std::string &error,
                        std::uint32_t max_attempts = 5U);
    Result step(std::int64_t monotonic_ns) noexcept;
    void abort(Failure failure) noexcept;
    void reset() noexcept;
    bool started() const noexcept { return started_; }
    bool complete() const noexcept { return result_ == Result::Complete; }
    const Error &error() const noexcept { return error_; }
    // Call only outside the realtime path (initialization or after cyclic exit).
    void printError(std::ostream &out) const;

private:
    bool compare(const std::uint8_t *data, std::size_t size) noexcept;
    void fail(Failure failure, int api_error = 0,
              std::uint32_t actual = 0, std::size_t actual_size = 0) noexcept;
    struct InitWrite { std::uint16_t position; InitCommandConfig command; };
    std::vector<InitWrite> init_writes_;
    std::vector<Check> checks_;
    std::vector<ec_sdo_request_t *> requests_;
    std::size_t cursor_{0};
    std::int64_t request_deadline_ns_{0};
    bool started_{false};
    bool prepared_{false};
    bool busy_{false};
    Result result_{Result::Pending};
    Error error_{};
};

} // namespace rocos
