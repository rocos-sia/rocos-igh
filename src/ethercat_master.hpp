#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <ecrt.h>

#include "slave_config.hpp"

namespace rocos {

struct EthercatMasterTestPeer;

/**
 * @brief Value snapshot of master and domain state for one cycle.
 */
struct BusState {
    unsigned int responding_slaves{0};         ///< Number of slaves responding.
    unsigned int al_states{0};                 ///< Bitmask of application-layer states.
    bool link_up{false};                       ///< True when the physical link is up.
    unsigned int input_working_counter{0};     ///< Input domain working counter.
    unsigned int output_working_counter{0};    ///< Output domain working counter.
    ec_wc_state_t input_wc_state{EC_WC_ZERO};  ///< Input domain WC state.
    ec_wc_state_t output_wc_state{EC_WC_ZERO}; ///< Output domain WC state.
};

enum class DcErrorStage {
    None,
    ApplicationTime,
    SyncReferenceClock,
    SyncSlaveClocks,
};

struct DcError {
    DcErrorStage stage{DcErrorStage::None};
    int error_code{0};
};

const char *dcErrorStageName(DcErrorStage stage) noexcept;

/**
 * @brief RAII owner of one IgH master with separate input and output domains.
 *
 * Slave configuration and PDO registration happen in initialize(), before
 * activation. After activation only the fixed rt_safe cycle methods and the
 * domain buffer accessors may be called.
 */
class EthercatMaster {
public:
    /// @brief Default-constructs an uninitialized master.
    EthercatMaster() = default;
    /// @brief Releases the master and drops all borrowed pointers.
    ~EthercatMaster();
    // Non-copyable, non-movable: owns a master exclusively.
    EthercatMaster(const EthercatMaster &) = delete;
    EthercatMaster &operator=(const EthercatMaster &) = delete;
    EthercatMaster(EthercatMaster &&) = delete;
    EthercatMaster &operator=(EthercatMaster &&) = delete;

    /**
     * @brief Requests and configures the master for @p master_id.
     *
     * Creates one input and one output domain, configures each slave and
     * registers its PDO entries, then activates the master and caches both
     * domain buffers. Activation is not OP readiness: CyclicTask polls every
     * slave and the domain working counters before releasing client data.
     * On any failure the object is reset and @p error is set.
     *
     * @param master_id IgH master index to request.
     * @param config    Validated static slave configuration.
     * @param error     Receives a description of the first failure.
     * @param preop_timeout_ms Total PREOP readiness timeout in milliseconds (positive).
     * @return True on success.
     */
    bool initialize(unsigned int master_id,
                    StaticSlaveConfig config,
                    bool dc_enabled,
                    std::uint32_t period_us,
                    std::string &error,
                    std::uint32_t preop_timeout_ms = 5000U);
    /// @brief Receives a frame and processes both domains (rt_safe).
    void receiveAndProcess() noexcept;
    /// @brief Sets the DC application time for the current cycle (rt_safe).
    bool setApplicationTime(std::uint64_t app_time_ns) noexcept;
    /// @brief Re-queues both domains, queues DC sync, and sends datagrams (rt_safe).
    bool queueAndSend() noexcept;
    /// @brief Returns a non-allocating snapshot of master and domain state.
    BusState readState() noexcept;
    /// Queries every configured slave using rt_safe APIs; never short-circuits.
    int pollSlaves(ec_al_state_t target, bool &all_ready) noexcept;
    std::size_t slaveCount() const noexcept { return slave_configs_.size(); }

    /// @brief Returns the input-domain process-data base pointer.
    std::uint8_t *inputData() noexcept;
    /// @brief Returns the output-domain process-data base pointer.
    std::uint8_t *outputData() noexcept;
    /// @brief Returns the input-domain process-data size in bytes.
    std::size_t inputSize() const noexcept;
    /// @brief Returns the output-domain process-data size in bytes.
    std::size_t outputSize() const noexcept;
    /// @brief Returns true once initialize() has completed successfully.
    bool initialized() const noexcept;
    /// @brief Returns the last realtime DC API failure without allocation.
    DcError lastDcError() const noexcept;

private:
    friend struct EthercatMasterTestPeer;

    static bool slaveReadyForConfiguration(const ec_slave_info_t &slave_info) noexcept;
    bool waitForSlavesInPreop(const StaticSlaveConfig &config,
                              std::string &error, std::uint32_t timeout_ms);
    static bool waitForSlavesInPreop(
        const StaticSlaveConfig &config,
        std::string &error,
        const std::function<int(std::uint16_t, ec_slave_info_t &)> &query,
        const std::function<void()> &wait,
        std::size_t max_attempts,
        const std::function<bool()> &expired = {});
    void recordDcError(DcErrorStage stage, int error_code) noexcept;
    void reset() noexcept;

    std::vector<ec_slave_config_t *> slave_configs_;
    ec_master_t *master_{nullptr};
    ec_domain_t *input_domain_{nullptr};
    ec_domain_t *output_domain_{nullptr};
    std::uint8_t *input_data_{nullptr};
    std::uint8_t *output_data_{nullptr};
    std::size_t input_size_{0};
    std::size_t output_size_{0};
    bool dc_enabled_{false};
    DcError last_dc_error_{};
    bool initialized_{false};
};

}  // namespace rocos
