#include "slave_config.hpp"

#include "shared_memory_config.hpp"

#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace rocos {

#if ROCOS_IGH_BUILD_MASTER
struct LoadedSlaveConfig::Impl {
    struct PdoStorage {
        std::uint16_t index{0};
        std::vector<ec_pdo_entry_info_t> entries;
    };

    struct SlaveStorage {
        std::uint16_t id{0};
        std::string name;
        std::vector<std::string> entry_names;
        std::vector<PdoStorage> rx_storage;
        std::vector<PdoStorage> tx_storage;
        std::vector<ec_pdo_info_t> rx_pdos;
        std::vector<ec_pdo_info_t> tx_pdos;
        std::vector<ec_sync_info_t> syncs;
        std::vector<PdoEntrySpec> entries;
    };

    std::vector<SlaveStorage> storage;
    std::vector<SlaveSpec> slaves;
};

namespace {

std::size_t entryCount(const std::vector<PdoMappingConfig> &pdos) {
    std::size_t count = 0;
    for (const PdoMappingConfig &pdo : pdos) {
        count += pdo.entries.size();
    }
    return count;
}

template <typename PdoStorageVector>
void storePdos(const std::vector<PdoMappingConfig> &source,
               PdoStorageVector &target) {
    target.reserve(source.size());
    for (const PdoMappingConfig &source_pdo : source) {
        typename PdoStorageVector::value_type pdo;
        pdo.index = source_pdo.index;
        pdo.entries.reserve(source_pdo.entries.size());
        for (const PdoEntryConfig &entry : source_pdo.entries) {
            pdo.entries.push_back({entry.index, entry.sub_index, entry.bit_length});
        }
        target.push_back(std::move(pdo));
    }
}

template <typename PdoStorageVector>
void buildPdoViews(const PdoStorageVector &storage,
                   std::vector<ec_pdo_info_t> &views) {
    views.reserve(storage.size());
    for (const auto &pdo : storage) {
        views.push_back({pdo.index, static_cast<unsigned int>(pdo.entries.size()),
                         pdo.entries.data()});
    }
}

template <typename SlaveStorage>
void appendEntryViews(const std::vector<PdoMappingConfig> &pdos,
                      PdoDirection direction,
                      SlaveStorage &storage) {
    for (const PdoMappingConfig &pdo : pdos) {
        for (const PdoEntryConfig &entry : pdo.entries) {
            storage.entry_names.push_back(entry.name);
            storage.entries.push_back({nullptr, direction, entry.index, entry.sub_index,
                                       entry.bit_length, 0, 0});
        }
    }
}

}  // namespace

LoadedSlaveConfig::LoadedSlaveConfig() : impl_(std::make_unique<Impl>()) {}
LoadedSlaveConfig::~LoadedSlaveConfig() = default;
LoadedSlaveConfig::LoadedSlaveConfig(LoadedSlaveConfig &&) noexcept = default;
LoadedSlaveConfig &LoadedSlaveConfig::operator=(LoadedSlaveConfig &&) noexcept = default;

bool LoadedSlaveConfig::build(PdoBusConfig config, std::string &error) noexcept {
    try {
        error.clear();
        auto built = std::make_unique<Impl>();
        built->storage.resize(config.slaves.size());
        built->slaves.reserve(config.slaves.size());

        for (std::size_t slave_index = 0; slave_index < config.slaves.size(); ++slave_index) {
            const SlavePdoConfig &source = config.slaves[slave_index];
            Impl::SlaveStorage &target = built->storage[slave_index];
            target.id = source.id;
            target.name = source.name;

            const std::size_t total_entries = entryCount(source.rx_pdos) +
                                              entryCount(source.tx_pdos);
            target.entry_names.reserve(total_entries);
            target.entries.reserve(total_entries);
            storePdos(source.rx_pdos, target.rx_storage);
            storePdos(source.tx_pdos, target.tx_storage);
            buildPdoViews(target.rx_storage, target.rx_pdos);
            buildPdoViews(target.tx_storage, target.tx_pdos);
            appendEntryViews(source.rx_pdos, PdoDirection::Output, target);
            appendEntryViews(source.tx_pdos, PdoDirection::Input, target);

            for (std::size_t entry_index = 0; entry_index < target.entries.size(); ++entry_index) {
                target.entries[entry_index].name = target.entry_names[entry_index].c_str();
            }

            target.syncs = {
                {2, EC_DIR_OUTPUT, static_cast<unsigned int>(target.rx_pdos.size()),
                 target.rx_pdos.data(), EC_WD_ENABLE},
                {3, EC_DIR_INPUT, static_cast<unsigned int>(target.tx_pdos.size()),
                 target.tx_pdos.data(), EC_WD_DISABLE},
                {0xff},
            };
        }

        for (Impl::SlaveStorage &target : built->storage) {
            built->slaves.push_back({0, target.id, 0, 0, target.name.c_str(),
                                     target.syncs.data(), target.entries.data(),
                                     target.entries.size()});
        }

        const StaticSlaveConfig candidate{built->slaves.data(), built->slaves.size()};
        if (!validateSlaveConfig(candidate, error)) {
            return false;
        }

        impl_ = std::move(built);
        return true;
    } catch (const std::exception &exception) {
        error = std::string("failed to build slave configuration: ") + exception.what();
        return false;
    }
}

StaticSlaveConfig LoadedSlaveConfig::view() noexcept {
    return {impl_->slaves.data(), impl_->slaves.size()};
}

void printLoadedSlaveConfig(const LoadedSlaveConfig &config, std::ostream &output) {
    const std::ios::fmtflags saved_flags = output.flags();
    const char saved_fill = output.fill();

    for (std::size_t slave_index = 0; slave_index < config.impl_->slaves.size();
         ++slave_index) {
        const SlaveSpec &slave = config.impl_->slaves[slave_index];
        const LoadedSlaveConfig::Impl::SlaveStorage &storage =
            config.impl_->storage[slave_index];
        output << "Slave " << slave.position << ": " << slave.name << '\n';
        output << "  Identity: vendor=0x" << std::hex << std::nouppercase
               << std::setfill('0') << std::setw(8) << slave.vendor_id
               << " product=0x" << std::setw(8) << slave.product_code << '\n';

        std::size_t entry_index = 0;
        const auto print_pdos = [&](const std::vector<LoadedSlaveConfig::Impl::PdoStorage> &pdos,
                                    const char *label,
                                    unsigned int sync_index,
                                    const char *direction) {
            for (const LoadedSlaveConfig::Impl::PdoStorage &pdo : pdos) {
                output << "  " << label << " 0x" << std::hex << std::setfill('0')
                       << std::setw(4) << pdo.index << std::dec << std::setfill(' ')
                       << " (SM" << sync_index << ", " << direction << ")\n";
                for (std::size_t pdo_entry = 0; pdo_entry < pdo.entries.size(); ++pdo_entry) {
                    const PdoEntrySpec &entry = slave.entries[entry_index++];
                    output << "    0x" << std::hex << std::setfill('0') << std::setw(4)
                           << entry.index << ':' << std::setw(2)
                           << static_cast<unsigned int>(entry.sub_index)
                           << std::dec << std::setfill(' ') << "  "
                           << static_cast<unsigned int>(entry.bit_length)
                           << " bit  offset=" << entry.offset << "  " << entry.name << '\n';
                }
            }
        };

        print_pdos(storage.rx_storage, "RxPDO", 2, "master -> slave");
        print_pdos(storage.tx_storage, "TxPDO", 3, "slave -> master");
    }

    output.flags(saved_flags);
    output.fill(saved_fill);
}
#endif

bool applyDiscoveredIdentity(SlaveSpec &slave,
                             std::uint32_t vendor_id,
                             std::uint32_t product_code,
                             std::string &error) noexcept {
    error.clear();
    if (vendor_id == 0 || product_code == 0) {
        error = "discovered SII identity is invalid";
        return false;
    }

    slave.vendor_id = vendor_id;
    slave.product_code = product_code;
    return true;
}

// Validates a static configuration structurally and returns the first exact
// failure in `error` (empty config is valid).
bool validateSlaveConfig(const StaticSlaveConfig &config, std::string &error) noexcept {
    try {
        error.clear();

        if (config.slave_count == 0) {
            return true;
        }
        if (config.slaves == nullptr) {
            error = "config has slave_count > 0 but slaves is null";
            return false;
        }

        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &slave = config.slaves[slave_index];
            if ((slave.vendor_id == 0) != (slave.product_code == 0)) {
                error = "slave[" + std::to_string(slave_index) + "] vendor_id and product_code must be both zero or both non-zero";
                return false;
            }
            if (slave.vendor_id == 0 && slave.alias != 0) {
                error = "slave[" + std::to_string(slave_index) + "] SII identity discovery requires alias 0";
                return false;
            }
            if (slave.name == nullptr || slave.name[0] == '\0') {
                error = "slave[" + std::to_string(slave_index) + "] name must be non-empty";
                return false;
            }
            if (slave.syncs == nullptr) {
                error = "slave[" + std::to_string(slave_index) + "] syncs must be non-null";
                return false;
            }
            if (slave.entry_count > 0 && slave.entries == nullptr) {
                error = "slave[" + std::to_string(slave_index) + "] entries must be non-null when entry_count > 0";
                return false;
            }

            std::size_t input_count = 0;
            std::size_t output_count = 0;
            for (std::size_t entry_index = 0; entry_index < slave.entry_count; ++entry_index) {
                const PdoEntrySpec &entry = slave.entries[entry_index];
                if (entry.name == nullptr || entry.name[0] == '\0') {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] name must be non-empty";
                    return false;
                }
                if (entry.bit_length == 0) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] bit_length must be > 0";
                    return false;
                }
                if ((entry.bit_length % 8U) != 0U) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] must be byte-aligned (bit_length % 8 == 0)";
                    return false;
                }
                if (entry.bit_position != 0U) {
                    error = "slave[" + std::to_string(slave_index) + "] entry[" + std::to_string(entry_index) + "] bit_position must be 0";
                    return false;
                }

                if (entry.direction == PdoDirection::Input) {
                    ++input_count;
                    if (input_count > MAX_PDINPUT_NUM) {
                        error = "slave[" + std::to_string(slave_index) + "] input entries exceed MAX_PDINPUT_NUM";
                        return false;
                    }
                } else {
                    ++output_count;
                    if (output_count > MAX_PDOUTPUT_NUM) {
                        error = "slave[" + std::to_string(slave_index) + "] output entries exceed MAX_PDOUTPUT_NUM";
                        return false;
                    }
                }
            }
        }

        return true;
    } catch (...) {
        error = "validateSlaveConfig encountered an unexpected exception";
        return false;
    }
}

// Copies the static configuration into the shared EcatBus metadata (slave and
// PDO variable names, offsets, sizes, indices) for client processes to read.
bool publishConfig(EcatBus &bus, const StaticSlaveConfig &config, std::string &error) noexcept {
    try {
        error.clear();
        bus = EcatBus{};

        if (!validateSlaveConfig(config, error)) {
            return false;
        }

        if (config.slave_count > MAX_SLAVE_NUM) {
            error = "slave count exceeds MAX_SLAVE_NUM";
            return false;
        }

        bus.slave_num = static_cast<int>(config.slave_count);
        for (std::size_t slave_index = 0; slave_index < config.slave_count; ++slave_index) {
            const SlaveSpec &source_slave = config.slaves[slave_index];
            Slave &target_slave = bus.slaves[slave_index];

            target_slave.id = static_cast<int>(slave_index);
            std::strncpy(target_slave.name, source_slave.name, MAX_SLAVE_NAME_LEN - 1);
            target_slave.name[MAX_SLAVE_NAME_LEN - 1] = '\0';

            int input_count = 0;
            int output_count = 0;
            for (std::size_t entry_index = 0; entry_index < source_slave.entry_count; ++entry_index) {
                const PdoEntrySpec &entry = source_slave.entries[entry_index];
                if (entry.offset > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
                    error = "entry offset exceeds PdVar range";
                    return false;
                }

                const unsigned int byte_size = static_cast<unsigned int>(entry.bit_length / 8U);
                if (byte_size > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
                    error = "entry size exceeds PdVar range";
                    return false;
                }
                if (entry.offset > EC_SHM_MAX_SIZE) {
                    error = "entry offset exceeds EC_SHM_MAX_SIZE";
                    return false;
                }
                if (byte_size > (EC_SHM_MAX_SIZE - entry.offset)) {
                    error = "entry offset + size exceeds EC_SHM_MAX_SIZE";
                    return false;
                }

                PdVar *target_var = nullptr;
                if (entry.direction == PdoDirection::Input) {
                    if (input_count >= MAX_PDINPUT_NUM) {
                        error = "input entries exceed MAX_PDINPUT_NUM";
                        return false;
                    }
                    target_var = &target_slave.input_vars[input_count++];
                } else {
                    if (output_count >= MAX_PDOUTPUT_NUM) {
                        error = "output entries exceed MAX_PDOUTPUT_NUM";
                        return false;
                    }
                    target_var = &target_slave.output_vars[output_count++];
                }

                std::strncpy(target_var->name, entry.name, MAX_PD_NAME_LEN - 1);
                target_var->name[MAX_PD_NAME_LEN - 1] = '\0';
                target_var->offset = static_cast<int>(entry.offset);
                target_var->size = static_cast<int>(byte_size);
                target_var->index = entry.index;
                target_var->sub_index = entry.sub_index;
            }

            target_slave.input_var_num = input_count;
            target_slave.output_var_num = output_count;
        }

        return true;
    } catch (...) {
        error = "publishConfig encountered an unexpected exception";
        return false;
    }
}

}  // namespace rocos
