#include "pdo_verification.hpp"

#include <cerrno>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace rocos {

bool PdoVerification::prepare(const StaticSlaveConfig &config,
                              const std::vector<ec_slave_config_t *> &slaves,
                              std::string &error) {
    reset();
    error.clear();
    if (config.slaves == nullptr || config.slave_count == 0 ||
        slaves.size() != config.slave_count) {
        error = "PDO verification requires all configured slaves";
        return false;
    }
    for (std::size_t s = 0; s < config.slave_count; ++s) {
        const auto &slave = config.slaves[s];
        if (slave.syncs == nullptr || slaves[s] == nullptr) {
            error = "missing PDO verification configuration for slave[" +
                    std::to_string(slave.position) + "]";
            return false;
        }
        const auto add = [&](std::uint8_t sm, std::uint16_t index, std::uint8_t sub,
                             std::uint8_t size, std::uint32_t value, Field field) {
            checks_.push_back({slave.position, sm, index, sub, size, value, field, s});
        };
        // Validate the entire assignment, including count, then each PDO in order.
        unsigned int sync_mask = 0;
        for (const ec_sync_info_t *sm = slave.syncs; sm->index != 0xff; ++sm) {
            if (sm->index != 2 && sm->index != 3) continue;
            if ((sync_mask & (1U << sm->index)) != 0 ||
                (sm->n_pdos != 0 && sm->pdos == nullptr)) {
                error = "invalid PDO verification Sync Manager for slave[" +
                        std::to_string(slave.position) + "]";
                return false;
            }
            sync_mask |= 1U << sm->index;
            if (sm->n_pdos > 255) {
                error = "PDO assignment count exceeds SDO subindex range for slave[" +
                        std::to_string(slave.position) + "]";
                return false;
            }
            const auto assignment = static_cast<std::uint16_t>(0x1c10 + sm->index);
            add(sm->index, assignment, 0, 1, sm->n_pdos, Field::AssignmentCount);
            for (unsigned int p = 0; p < sm->n_pdos; ++p) {
                const auto &pdo = sm->pdos[p];
                add(sm->index, assignment, static_cast<std::uint8_t>(p + 1),
                    2, pdo.index, Field::Assignment);
                if (pdo.n_entries > 255 || (pdo.n_entries != 0 && pdo.entries == nullptr)) {
                    error = "invalid PDO mapping entries for slave[" +
                            std::to_string(slave.position) + "]";
                    return false;
                }
                add(sm->index, pdo.index, 0, 1, pdo.n_entries, Field::MappingCount);
                for (unsigned int e = 0; e < pdo.n_entries; ++e) {
                    const auto &entry = pdo.entries[e];
                    const std::uint32_t mapping = (std::uint32_t{entry.index} << 16) |
                        (std::uint32_t{entry.subindex} << 8) | entry.bit_length;
                    add(sm->index, pdo.index, static_cast<std::uint8_t>(e + 1),
                        4, mapping, Field::MappingEntry);
                }
            }
        }
        if (sync_mask != ((1U << 2) | (1U << 3))) {
            error = "PDO verification requires explicit SM2 and SM3 for slave[" +
                    std::to_string(slave.position) + "]";
            return false;
        }
        // One reusable request per slave, preallocated for the largest scalar.
        // No Complete Access: individual 8/16/32-bit objects have exact sizes.
        auto *request = ecrt_slave_config_create_sdo_request(slaves[s], 0x1c12, 0, 4);
        if (request == nullptr ||
            ecrt_sdo_request_timeout(request, kRequestTimeoutMs) != 0) {
            error = "failed to prepare PDO verification SDO request for slave[" +
                    std::to_string(slave.position) + "]";
            return false;
        }
        requests_.push_back(request);
    }
    if (checks_.empty()) {
        error = "PDO verification has no SM2/SM3 checks";
        return false;
    }
    prepared_ = true;
    return true;
}

void PdoVerification::fail(Failure failure, int api_error,
                           std::uint32_t actual, std::size_t actual_size) noexcept {
    if (result_ == Result::Failed) return; // Preserve the first failure.
    error_ = {};
    error_.failure = failure;
    if (!checks_.empty()) {
        error_.check = checks_[cursor_ < checks_.size() ? cursor_ : checks_.size() - 1];
    }
    error_.actual = actual;
    error_.actual_size = actual_size;
    error_.api_error = api_error;
    result_ = Result::Failed;
}

void PdoVerification::abort(Failure failure) noexcept {
    fail(failure);
}

PdoVerification::Result PdoVerification::step(std::int64_t now_ns) noexcept {
    if (result_ != Result::Pending) return result_;
    if (!prepared_) {
        fail(Failure::NotPrepared);
        return result_;
    }
    started_ = true;
    const auto &check = checks_[cursor_];
    auto *request = requests_[check.slave_index];
    if (!busy_) {
        const int index_rc = ecrt_sdo_request_index(request, check.index, check.subindex);
        if (index_rc != 0) {
            fail(Failure::SetIndex, index_rc);
            return result_;
        }
        const int read_rc = ecrt_sdo_request_read(request);
        if (read_rc != 0) {
            fail(Failure::StartRead, read_rc);
            return result_;
        }
        request_deadline_ns_ = now_ns + std::int64_t{kRequestTimeoutMs} * 1000000;
        busy_ = true;
        return result_;
    }
    // Wall time bounds even a perpetually BUSY request or late scheduler wake.
    if (now_ns >= request_deadline_ns_) {
        fail(Failure::RequestTimeout);
        return result_;
    }
    const auto state = ecrt_sdo_request_state(request);
    if (state == EC_REQUEST_BUSY) return result_;
    if (state != EC_REQUEST_SUCCESS) {
        fail(Failure::Read);
        return result_;
    }
    const auto size = ecrt_sdo_request_data_size(request);
    // Do not access the request buffer if IgH reports an unexpected size.
    if (!compare(size == check.size ? ecrt_sdo_request_data(request) : nullptr, size))
        return result_;
    busy_ = false;
    if (++cursor_ == checks_.size()) result_ = Result::Complete;
    return result_;
}

bool PdoVerification::compare(const std::uint8_t *data, std::size_t size) noexcept {
    const auto &check = checks_[cursor_];
    if (size != check.size) {
        fail(Failure::Size, 0, 0, size);
        return false;
    }
    if (data == nullptr) {
        fail(Failure::Read);
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value |= std::uint32_t{data[i]} << (8 * i);
    if (value != check.expected) {
        fail(Failure::Value, 0, value, size);
        return false;
    }
    return true;
}

bool PdoVerification::configurePreop(ec_master_t *master, std::uint32_t timeout_ms,
                                      std::ostream &log, std::string &error,
                                      std::uint32_t max_attempts) {
    error.clear();
    if (max_attempts == 0U) {
        error = "PDO configuration attempts must be positive";
        return false;
    }
    const auto report = [&] {
        std::ostringstream message;
        printError(message);
        error = message.str();
        return false;
    };
    if (!prepared_ || master == nullptr || started_) {
        fail(Failure::NotPrepared);
        return report();
    }
    const auto now = [&]() -> std::int64_t {
        timespec ts{};
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            fail(Failure::Clock, -errno);
            return 0;
        }
        return std::int64_t{ts.tv_sec} * 1000000000 + ts.tv_nsec;
    };
    const auto deadline = now() + std::int64_t{timeout_ms} * 1000000;
    if (result_ == Result::Failed) return report();
    const auto guard = [&] {
        ec_slave_info_t info{};
        const auto &c = checks_[cursor_];
        const int rc = ecrt_master_get_slave(master, c.slave_position, &info);
        if (rc != 0 || info.al_state != EC_AL_STATE_PREOP || info.error_flag != 0) {
            fail(Failure::PreopState, rc, info.al_state, info.error_flag);
            return false;
        }
        if (now() >= deadline) fail(Failure::PreopTimeout);
        return result_ != Result::Failed;
    };
    // Both assignments must be disabled before touching a PDO mapping. Restore
    // mapping counts before enabling assignments. Retry the whole slave so a
    // partial write can never be accepted as a complete configuration.
    const Field phases[]{Field::AssignmentCount, Field::MappingCount,
                         Field::MappingEntry, Field::MappingCount,
                         Field::Assignment, Field::AssignmentCount};
    for (std::size_t begin = 0; begin < checks_.size();) {
        std::size_t end = begin + 1;
        while (end < checks_.size() &&
               checks_[end].slave_index == checks_[begin].slave_index) ++end;
        bool verified = false;
        for (std::uint64_t attempt = 1; attempt <= max_attempts; ++attempt) {
            result_ = Result::Pending;
            error_ = {};
            for (unsigned int phase = 0; phase < 6 && result_ == Result::Pending; ++phase) {
                for (cursor_ = begin; cursor_ < end; ++cursor_) {
                    const auto &c = checks_[cursor_];
                    if (c.field != phases[phase]) continue;
                    if (!guard()) break;
                    const auto value = phase < 2 ? 0U : c.expected;
                    std::uint8_t data[4]{};
                    for (std::size_t i = 0; i < c.size; ++i)
                        data[i] = static_cast<std::uint8_t>(value >> (8 * i));
                    std::uint32_t abort_code = 0;
                    const int rc = ecrt_master_sdo_download(master, c.slave_position,
                        c.index, c.subindex, data, c.size, &abort_code);
                    if (!guard()) break; // Reject late results and state changes.
                    if (rc != 0) {
                        fail(Failure::Write, rc, value, c.size);
                        error_.abort_code = abort_code;
                        break;
                    }
                }
            }
            if (result_ == Result::Pending) {
                for (cursor_ = begin; cursor_ < end; ++cursor_) {
                    const auto &c = checks_[cursor_];
                    if (!guard()) break;
                    std::uint8_t data[4]{};
                    std::size_t size = 0;
                    std::uint32_t abort_code = 0;
                    const int rc = ecrt_master_sdo_upload(master, c.slave_position,
                        c.index, c.subindex, data, sizeof(data), &size, &abort_code);
                    if (!guard()) break;
                    if (rc != 0) {
                        fail(Failure::Read, rc);
                        error_.abort_code = abort_code;
                        break;
                    }
                    if (!compare(data, size)) break;
                }
            }
            if (result_ == Result::Pending) {
                log << "PREOP PDO configuration verified: slave[position="
                    << checks_[begin].slave_position << "] attempt=" << attempt << "/" << max_attempts << '\n';
                verified = true;
                break;
            }
            log << "PREOP PDO attempt=" << attempt << "/" << max_attempts << ' ';
            printError(log);
            if (error_.failure == Failure::PreopState ||
                error_.failure == Failure::PreopTimeout || error_.failure == Failure::Clock)
                return report(); // Never retry writes outside healthy PREOP.
        }
        if (!verified) return report();
        begin = end;
    }
    // PREOP success does not grant readiness: IgH may rewrite PDOs on activation.
    // Keep all asynchronous requests unused and verify again in the startup loop.
    cursor_ = 0;
    result_ = Result::Pending;
    error_ = {};
    return true;
}

void PdoVerification::reset() noexcept {
    checks_.clear();
    requests_.clear();
    cursor_ = 0;
    request_deadline_ns_ = 0;
    started_ = false;
    prepared_ = false;
    busy_ = false;
    result_ = Result::Pending;
    error_ = {};
}

void PdoVerification::printError(std::ostream &out) const {
    if (error_.failure == Failure::None) return;
    const auto flags = out.flags();
    const auto fill = out.fill();
    const auto &c = error_.check;
    out << std::dec << "PDO verification failed at slave[position=" << c.slave_position << "] SM"
        << unsigned(c.sync_index) << (c.sync_index == 2 ? " RxPDO" : " TxPDO")
        << " object=0x" << std::hex << std::setfill('0') << std::setw(4) << c.index
        << ':' << std::setw(2) << unsigned(c.subindex) << std::dec << ' ';
    switch (c.field) {
    case Field::AssignmentCount: out << "assignment count"; break;
    case Field::Assignment: out << "assigned PDO/order"; break;
    case Field::MappingCount: out << "mapping entry count"; break;
    case Field::MappingEntry: out << "mapping entry/order"; break;
    }
    out << " expected=0x" << std::hex << c.expected << std::dec;
    switch (error_.failure) {
    case Failure::Value:
        out << " actual=0x" << std::hex << error_.actual << std::dec;
        if (c.field == Field::MappingEntry) {
            const auto entry = [&](const char *label, std::uint32_t value) {
                out << ' ' << label << "{index=0x" << std::hex << (value >> 16)
                    << ",subindex=0x" << ((value >> 8) & 0xff) << std::dec
                    << ",bits=" << (value & 0xff) << '}';
            };
            entry("expected", c.expected);
            entry("actual", error_.actual);
        }
        break;
    case Failure::Size:
        out << " size mismatch: expected_bytes=" << unsigned(c.size)
            << " actual_bytes=" << error_.actual_size; break;
    case Failure::SetIndex: out << " set request index failed"; break;
    case Failure::StartRead: out << " schedule SDO read failed"; break;
    case Failure::Read: out << " SDO read failed (no valid value)"; break;
    case Failure::RequestTimeout: out << " SDO read timed out"; break;
    case Failure::StartupTimeout: out << " total startup timeout during verification"; break;
    case Failure::BusUnhealthy: out << " verification invalidated by OP/link/WC loss"; break;
    case Failure::NotPrepared: out << " verification requests were not prepared"; break;
    case Failure::Write:
        out << " SDO write failed attempted=0x" << std::hex << error_.actual << std::dec; break;
    case Failure::PreopState:
        out << " PREOP state check failed: al_state=0x" << std::hex << error_.actual
            << std::dec << " error_flag=" << error_.actual_size; break;
    case Failure::PreopTimeout: out << " total PREOP configuration timeout"; break;
    case Failure::Clock: out << " monotonic clock query failed"; break;
    case Failure::None: break;
    }
    if (error_.api_error != 0) out << " rc=" << error_.api_error;
    if (error_.abort_code != 0) out << " SDO abort=0x" << std::hex << error_.abort_code << std::dec;
    out << '\n';
    out.flags(flags);
    out.fill(fill);
}

} // namespace rocos
