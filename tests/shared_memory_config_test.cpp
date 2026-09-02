#include "shared_memory_config.hpp"

#include <iostream>

int main() {
    const rocos::EcatBus bus{};
    if (bus.current_state != ECAT_STATE_INIT || bus.request_state != ECAT_STATE_OP) {
        std::cerr << "unexpected EcatBus defaults\n";
        return 1;
    }
    return 0;
}
