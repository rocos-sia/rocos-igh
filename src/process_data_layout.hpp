#pragma once

#include "slave_config.hpp"

namespace rocos {
// Built once before the cyclic thread starts. Byte-aligned entries retain their
// domain offsets; ABI v2 rejects bit entries.
class ProcessDataLayout {
public:
    bool build(StaticSlaveConfig config, std::size_t input_domain_size,
               std::size_t output_domain_size, std::string &error) noexcept;
    std::size_t inputSize() const noexcept { return input_size_; }
    std::size_t outputSize() const noexcept { return output_size_; }
    void unpackInputs(const std::uint8_t *domain, void *shared) const noexcept;
    void packOutputs(const void *shared, std::uint8_t *domain) const noexcept;
    void reset() noexcept;

private:
    std::size_t input_size_{0}, output_size_{0};
};
} // namespace rocos
