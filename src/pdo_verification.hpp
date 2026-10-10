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
        RequestTimeout, StartupTimeout, BusUnhealthy
    };
    enum class Field { AssignmentCount, Assignment, MappingCount, MappingEntry };
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
    };

    static constexpr std::uint32_t kRequestTimeoutMs = 1000;
    bool prepare(const StaticSlaveConfig &config,
                 const std::vector<ec_slave_config_t *> &slaves, std::string &error);
    Result step(std::int64_t monotonic_ns) noexcept;
    void abort(Failure failure) noexcept;
    void reset() noexcept;
    bool started() const noexcept { return started_; }
    bool complete() const noexcept { return result_ == Result::Complete; }
    const Error &error() const noexcept { return error_; }
    // Call only after the cyclic task has returned, outside the realtime path.
    void printError(std::ostream &out) const;

private:
    void fail(Failure failure, int api_error = 0,
              std::uint32_t actual = 0, std::size_t actual_size = 0) noexcept;
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
