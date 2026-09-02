#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <ecrt.h>

#include "slave_config.hpp"

namespace rocos {

struct EthercatMasterTestPeer;

struct BusState {
    unsigned int responding_slaves{0};
    unsigned int al_states{0};
    bool link_up{false};
    unsigned int input_working_counter{0};
    unsigned int output_working_counter{0};
    ec_wc_state_t input_wc_state{EC_WC_ZERO};
    ec_wc_state_t output_wc_state{EC_WC_ZERO};
};

class EthercatMaster {
public:
    EthercatMaster() = default;
    ~EthercatMaster();
    EthercatMaster(const EthercatMaster &) = delete;
    EthercatMaster &operator=(const EthercatMaster &) = delete;
    EthercatMaster(EthercatMaster &&) = delete;
    EthercatMaster &operator=(EthercatMaster &&) = delete;

    bool initialize(unsigned int master_id, StaticSlaveConfig config, std::string &error);
    void receiveAndProcess() noexcept;
    void queueAndSend() noexcept;
    BusState readState() noexcept;

    std::uint8_t *inputData() noexcept;
    std::uint8_t *outputData() noexcept;
    std::size_t inputSize() const noexcept;
    std::size_t outputSize() const noexcept;
    bool initialized() const noexcept;

private:
    friend struct EthercatMasterTestPeer;

    void reset() noexcept;

    ec_master_t *master_{nullptr};
    ec_domain_t *input_domain_{nullptr};
    ec_domain_t *output_domain_{nullptr};
    std::uint8_t *input_data_{nullptr};
    std::uint8_t *output_data_{nullptr};
    std::size_t input_size_{0};
    std::size_t output_size_{0};
    bool initialized_{false};
};

}  // namespace rocos
