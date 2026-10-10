#include "pdo_verification.hpp"
#include "ethercat_master.hpp"
#include "cyclic_task.hpp"
#include "shared_memory_config.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <new>
#include <sstream>
#include <tuple>
#include <unistd.h>

namespace {
bool forbid_allocations = false;
unsigned int rt_allocations = 0;
}
void *operator new(std::size_t size) {
    if (forbid_allocations) ++rt_allocations;
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {
using V = rocos::PdoVerification;
using Key = std::tuple<unsigned int, unsigned int, unsigned int>;
struct Value { std::uint32_t value; std::size_t size; };
struct Request {
    unsigned int slave{0}, index{0}, sub{0};
    bool busy{false};
    unsigned int polls{0};
    std::array<std::uint8_t, 4> bytes{};
    std::size_t size{0};
};
std::array<Request, 2> requests;
std::map<Key, Value> dictionary;
unsigned int create_calls = 0, read_calls = 0, data_calls = 0, sends = 0;
int create_fail_at = -1, timeout_rc = 0, index_rc = 0, read_rc = 0;
bool read_error = false, forever_busy = false, unused_state = false;
bool invariant_failed = false, health_loss = false;
int health_fault = 0;
std::int64_t now_ns = 0;
std::int64_t wake_jump_ns = 0;
volatile std::sig_atomic_t *stop = nullptr;
rocos::CyclicTask *task = nullptr;
rocos::SharedMemoryConfig *ipc = nullptr;
std::uint8_t *output = nullptr;
bool saw_business_output = false;

Request &request(ec_sdo_request_t *p) { return *reinterpret_cast<Request *>(p); }
void resetFake() {
    requests = {};
    create_calls = read_calls = data_calls = sends = 0;
    create_fail_at = -1;
    timeout_rc = index_rc = read_rc = 0;
    read_error = forever_busy = unused_state = invariant_failed = health_loss = false;
    health_fault = 0;
    now_ns = wake_jump_ns = 0;
    stop = nullptr; task = nullptr; ipc = nullptr; output = nullptr;
    saw_business_output = false;
    rt_allocations = 0;
    // Independently specified device dictionary: multiple PDOs, different
    // layouts per slave, byte-sized mode, and an explicit padding entry.
    dictionary = {
        {{0,0x1c12,0},{2,1}}, {{0,0x1c12,1},{0x1600,2}}, {{0,0x1c12,2},{0x1601,2}},
        {{0,0x1600,0},{2,1}}, {{0,0x1600,1},{0x60400010,4}}, {{0,0x1600,2},{0x60600008,4}},
        {{0,0x1601,0},{1,1}}, {{0,0x1601,1},{0x607a0020,4}},
        {{0,0x1c13,0},{1,1}}, {{0,0x1c13,1},{0x1a07,2}},
        {{0,0x1a07,0},{2,1}}, {{0,0x1a07,1},{0x60410010,4}}, {{0,0x1a07,2},{0x60640020,4}},
        {{1,0x1c12,0},{1,1}}, {{1,0x1c12,1},{0x1607,2}},
        {{1,0x1607,0},{1,1}}, {{1,0x1607,1},{0x60400010,4}},
        {{1,0x1c13,0},{2,1}}, {{1,0x1c13,1},{0x1a00,2}}, {{1,0x1c13,2},{0x1a01,2}},
        {{1,0x1a00,0},{1,1}}, {{1,0x1a00,1},{0x60640020,4}},
        {{1,0x1a01,0},{1,1}}, {{1,0x1a01,1},{0x00000008,4}},
    };
}

struct Fixture {
    ec_pdo_entry_info_t rx0[2]{{0x6040,0,16},{0x6060,0,8}};
    ec_pdo_entry_info_t target{0x607a,0,32};
    ec_pdo_entry_info_t tx0[2]{{0x6041,0,16},{0x6064,0,32}};
    ec_pdo_entry_info_t pad{0,0,8};
    ec_pdo_info_t rx_pdos0[2]{{0x1600,2,rx0},{0x1601,1,&target}};
    ec_pdo_info_t tx_pdos0{0x1a07,2,tx0};
    ec_pdo_info_t rx_pdos1{0x1607,1,rx0};
    ec_pdo_info_t tx_pdos1[2]{{0x1a00,1,tx0+1},{0x1a01,1,&pad}};
    ec_sync_info_t sync0[3]{{2,EC_DIR_OUTPUT,2,rx_pdos0,EC_WD_ENABLE},
                          {3,EC_DIR_INPUT,1,&tx_pdos0,EC_WD_DISABLE},{0xff}};
    ec_sync_info_t sync1[3]{{2,EC_DIR_OUTPUT,1,&rx_pdos1,EC_WD_ENABLE},
                          {3,EC_DIR_INPUT,2,tx_pdos1,EC_WD_DISABLE},{0xff}};
    rocos::SlaveSpec slaves[2]{};
    rocos::StaticSlaveConfig config{slaves,2};
    std::vector<ec_slave_config_t *> handles{reinterpret_cast<ec_slave_config_t *>(1),
                                           reinterpret_cast<ec_slave_config_t *>(2)};
    Fixture() {
        slaves[0].position = 0; slaves[0].syncs = sync0;
        slaves[1].position = 7; slaves[1].syncs = sync1;
    }
};
V::Result finish(V &v) {
    V::Result result = V::Result::Pending;
    forbid_allocations = true;
    for (int cycle = 0; cycle < 5000 && result == V::Result::Pending; ++cycle) {
        now_ns += 1000000;
        result = v.step(now_ns);
    }
    forbid_allocations = false;
    return result;
}
}

extern "C" ec_sdo_request_t *__wrap_ecrt_slave_config_create_sdo_request(
    ec_slave_config_t *sc, std::uint16_t, std::uint8_t, std::size_t size) {
    if (size != 4) invariant_failed = true;
    if (static_cast<int>(create_calls++) == create_fail_at) return nullptr;
    const auto s = reinterpret_cast<std::uintptr_t>(sc) - 1;
    requests[s].slave = s;
    return reinterpret_cast<ec_sdo_request_t *>(&requests[s]);
}
extern "C" int __wrap_ecrt_sdo_request_timeout(ec_sdo_request_t *, std::uint32_t ms) {
    if (ms == 0) invariant_failed = true;
    return timeout_rc;
}
extern "C" int __wrap_ecrt_sdo_request_index(ec_sdo_request_t *p, std::uint16_t i, std::uint8_t s) {
    auto &r = request(p);
    if (r.busy) invariant_failed = true;
    r.index = i; r.sub = s;
    return index_rc;
}
extern "C" int __wrap_ecrt_sdo_request_read(ec_sdo_request_t *p) {
    auto &r = request(p);
    if (r.busy) invariant_failed = true;
    ++read_calls;
    r.busy = true; r.polls = 0;
    return read_rc;
}
extern "C" ec_request_state_t __wrap_ecrt_sdo_request_state(ec_sdo_request_t *p) {
    auto &r = request(p);
    if (forever_busy || r.polls++ == 0) return EC_REQUEST_BUSY;
    r.busy = false;
    if (read_error) return EC_REQUEST_ERROR;
    if (unused_state) return EC_REQUEST_UNUSED;
    const auto it = dictionary.find({r.slave,r.index,r.sub});
    if (it == dictionary.end()) return EC_REQUEST_ERROR;
    r.size = it->second.size;
    for (std::size_t i = 0; i < 4; ++i) r.bytes[i] = (it->second.value >> (8*i)) & 0xff;
    return EC_REQUEST_SUCCESS;
}
extern "C" std::size_t __wrap_ecrt_sdo_request_data_size(const ec_sdo_request_t *p) {
    return reinterpret_cast<const Request *>(p)->size;
}
extern "C" std::uint8_t *__wrap_ecrt_sdo_request_data(const ec_sdo_request_t *p) {
    ++data_calls;
    auto &r = *const_cast<Request *>(reinterpret_cast<const Request *>(p));
    if (r.busy) invariant_failed = true;
    return r.bytes.data();
}
extern "C" int __wrap_clock_gettime(clockid_t, timespec *t) {
    *t = {static_cast<time_t>(now_ns / 1000000000), static_cast<long>(now_ns % 1000000000)};
    return 0;
}
extern "C" int __wrap_clock_nanosleep(clockid_t, int, const timespec *t, timespec *) {
    now_ns = std::int64_t{t->tv_sec} * 1000000000 + t->tv_nsec + wake_jump_ns;
    wake_jump_ns = 0;
    return 0;
}
extern "C" int __wrap_ecrt_master_receive(ec_master_t *) { return 0; }
extern "C" int __wrap_ecrt_domain_process(ec_domain_t *) { return 0; }
extern "C" int __wrap_ecrt_master_state(const ec_master_t *, ec_master_state_t *s) {
    s->slaves_responding = 2; s->al_states = EC_AL_STATE_OP;
    s->link_up = !(health_loss && read_calls > 0);
    return 0;
}
extern "C" int __wrap_ecrt_domain_state(const ec_domain_t *, ec_domain_state_t *s) {
    s->working_counter = 2;
    s->wc_state = health_fault == 1 && read_calls > 0 ? EC_WC_INCOMPLETE : EC_WC_COMPLETE;
    return 0;
}
extern "C" int __wrap_ecrt_slave_config_state(const ec_slave_config_t *, ec_slave_config_state_t *s) {
    *s = {1,1,EC_AL_STATE_OP};
    if (health_fault == 2 && read_calls > 0) { s->al_state = EC_AL_STATE_SAFEOP; s->operational = 0; }
    return 0;
}
extern "C" int __wrap_ecrt_domain_queue(ec_domain_t *) { return 0; }
extern "C" int __wrap_ecrt_master_send(ec_master_t *) {
    ++sends;
    if (task != nullptr) {
        if (!task->startupReady()) {
            int value = 0;
            sem_getvalue(ipc->sem_mutex[0], &value);
            if (*output != 0 || value != 0 || ipc->ecatBus->is_authorized ||
                *static_cast<std::uint8_t *>(ipc->pdInputPtr) != 0) invariant_failed = true;
        } else {
            saw_business_output = *output == 0xab;
            *stop = 1;
        }
    }
    if (sends > 10000 && stop) *stop = 1;
    return 0;
}
extern "C" void __wrap_ecrt_release_master(ec_master_t *) {}

namespace rocos {
struct EthercatMasterTestPeer {
    static void seed(EthercatMaster &m, const Fixture &f, std::uint8_t *in, std::uint8_t *out) {
        m.initialized_ = true;
        m.master_ = reinterpret_cast<ec_master_t *>(1);
        m.input_domain_ = reinterpret_cast<ec_domain_t *>(2);
        m.output_domain_ = reinterpret_cast<ec_domain_t *>(3);
        m.input_data_ = in; m.output_data_ = out;
        m.input_size_ = m.output_size_ = 1;
        m.slave_configs_ = f.handles;
    }
};
}

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return false; } } while (false)
namespace {
bool unitTests() {
    Fixture f;
    std::string error;
    V v;
    resetFake();
    CHECK(v.prepare(f.config, f.handles, error));
    CHECK(create_calls == 2 && read_calls == 0); // No requests issued before activation.
    CHECK(finish(v) == V::Result::Complete);
    CHECK(read_calls == dictionary.size() && !invariant_failed && rt_allocations == 0);
    const auto reads = read_calls;
    CHECK(v.step(now_ns) == V::Result::Complete && read_calls == reads);

    // Corrupt every individual count, assignment and entry on both slaves.
    const auto correct = dictionary;
    for (const auto &item : correct) {
        resetFake();
        dictionary[item.first].value ^= 1;
        CHECK(v.prepare(f.config, f.handles, error));
        CHECK(finish(v) == V::Result::Failed);
        CHECK(v.error().failure == V::Failure::Value);
        CHECK(v.error().check.slave_position == (std::get<0>(item.first) ? 7 : 0));
        CHECK(v.error().check.index == std::get<1>(item.first));
        CHECK(v.error().check.subindex == std::get<2>(item.first));
        CHECK(v.error().actual == (item.second.value ^ 1));
        CHECK(v.error().check.expected == item.second.value);
        CHECK(!invariant_failed && rt_allocations == 0);
        std::ostringstream message;
        v.printError(message);
        CHECK(message.str().find("expected=") != std::string::npos);
        CHECK(message.str().find("actual=") != std::string::npos);
    }
    // Correct objects in the wrong order must also fail.
    resetFake();
    std::swap(dictionary[{0,0x1c12,1}], dictionary[{0,0x1c12,2}]);
    CHECK(v.prepare(f.config, f.handles, error));
    CHECK(finish(v) == V::Result::Failed && v.error().check.index == 0x1c12);
    resetFake();
    std::swap(dictionary[{0,0x1a07,1}], dictionary[{0,0x1a07,2}]);
    CHECK(v.prepare(f.config, f.handles, error));
    CHECK(finish(v) == V::Result::Failed && v.error().check.index == 0x1a07);
    // Check index, subindex and bit width independently, on the second slave.
    for (const auto mask : {0x00010000U, 0x00000100U, 0x00000030U}) {
        resetFake(); dictionary[{1,0x1a00,1}].value ^= mask;
        CHECK(v.prepare(f.config, f.handles, error));
        CHECK(finish(v) == V::Result::Failed && v.error().check.slave_position == 7);
        std::ostringstream message;
        v.printError(message);
        CHECK(message.str().find("expected{index=") != std::string::npos);
        CHECK(message.str().find("actual{index=") != std::string::npos);
    }

    for (std::size_t size : {0U,2U,4U,8U}) {
        resetFake(); dictionary[{0,0x1c12,0}].size = size;
        CHECK(v.prepare(f.config, f.handles, error));
        CHECK(finish(v) == V::Result::Failed && v.error().failure == V::Failure::Size);
        CHECK(data_calls == 0); // Never decode wrong-sized payloads.
    }
    for (int fault = 0; fault < 5; ++fault) {
        resetFake();
        index_rc = fault == 0 ? -EINVAL : 0;
        read_rc = fault == 1 ? -EIO : 0;
        read_error = fault == 2;
        forever_busy = fault == 3;
        unused_state = fault == 4;
        CHECK(v.prepare(f.config, f.handles, error));
        CHECK(finish(v) == V::Result::Failed && data_calls == 0);
        CHECK(!invariant_failed && rt_allocations == 0);
        const std::array<V::Failure,5> failures{V::Failure::SetIndex,V::Failure::StartRead,
            V::Failure::Read,V::Failure::RequestTimeout,V::Failure::Read};
        CHECK(v.error().failure == failures[fault]);
        v.abort(V::Failure::StartupTimeout);
        CHECK(v.error().failure == failures[fault]); // Preserve first fault.
    }
    resetFake(); create_fail_at = 1;
    CHECK(!v.prepare(f.config, f.handles, error));
    CHECK(error.find("slave[7]") != std::string::npos);
    CHECK(v.step(0) == V::Result::Failed); // Partial preparation cannot be used.
    resetFake(); timeout_rc = -EIO;
    CHECK(!v.prepare(f.config, f.handles, error));
    // A wake beyond the deadline must fail even if a response could now succeed.
    resetFake();
    CHECK(v.prepare(f.config, f.handles, error));
    CHECK(v.step(0) == V::Result::Pending);
    CHECK(v.step(2000000000LL) == V::Result::Failed);
    CHECK(v.error().failure == V::Failure::RequestTimeout && data_calls == 0);
    // An empty assignment is still read and checked.
    resetFake(); f.sync1[1].n_pdos = 0; dictionary[{1,0x1c13,0}] = {0,1};
    CHECK(v.prepare(f.config, f.handles, error));
    CHECK(finish(v) == V::Result::Complete);
    v.reset();
    CHECK(v.step(0) == V::Result::Failed && v.error().failure == V::Failure::NotPrepared);
    resetFake(); f.sync1[1].index = 0xff;
    CHECK(!v.prepare(f.config, f.handles, error));
    CHECK(error.find("slave[7]") != std::string::npos);
    return true;
}

bool cyclicTests() {
    // Real cyclic loop + fake transport/clock: client output must remain gated
    // even when OP and WCs are healthy throughout a failed verification.
    for (int scenario = 0; scenario < 9; ++scenario) {
        resetFake();
        Fixture f;
        rocos::EthercatMaster master;
        std::array<std::uint8_t,1> in{0x55}, out{0};
        rocos::EthercatMasterTestPeer::seed(master,f,in.data(),out.data());
        std::string error;
        CHECK(master.pdoVerification().prepare(f.config,f.handles,error));
        if (scenario == 1) dictionary[{1,0x1a01,1}].value = 0x60410010; // Last check.
        if (scenario == 2 || scenario == 3) forever_busy = true;
        if (scenario == 4) health_loss = true;
        if (scenario == 5) read_error = true;
        if (scenario == 6) health_fault = 1;
        if (scenario == 7) health_fault = 2;
        if (scenario == 8) read_rc = -EIO;
        rocos::SharedMemoryConfig shared(700000 + getpid()*16 + scenario);
        CHECK(shared.createSharedMemory() && shared.createPdDataMemoryProvider(1,1));
        shared.ecatBus->slave_num = 2;
        *static_cast<std::uint8_t *>(shared.pdOutputPtr) = 0xab;
        rocos::CyclicTask cyclic(master,shared,1000,scenario == 3 ? 10 : 5000);
        volatile std::sig_atomic_t stopped = 0;
        task = &cyclic; stop = &stopped; ipc = &shared; output = out.data();
        forbid_allocations = true;
        const int rc = cyclic.run(stopped);
        forbid_allocations = false;
        CHECK(!invariant_failed && rt_allocations == 0);
        CHECK(sends > 0); // Communication progressed while verifying.
        if (scenario == 0) {
            CHECK(rc == 0 && cyclic.startupReady() && saw_business_output);
            CHECK(shared.ecatBus->is_authorized);
            CHECK(*static_cast<std::uint8_t *>(shared.pdInputPtr) == 0x55);
            CHECK(sem_trywait(shared.sem_mutex[0]) == 0);
        } else {
            CHECK(rc != 0 && !cyclic.startupReady() && !saw_business_output);
            CHECK(out[0] == 0 && !shared.ecatBus->is_authorized);
            CHECK(sem_trywait(shared.sem_mutex[0]) == -1 && errno == EAGAIN);
            const std::array<V::Failure,8> failures{V::Failure::Value,V::Failure::RequestTimeout,
                V::Failure::StartupTimeout,V::Failure::BusUnhealthy,V::Failure::Read,
                V::Failure::BusUnhealthy,V::Failure::BusUnhealthy,V::Failure::StartRead};
            CHECK(master.pdoVerification().error().failure == failures[scenario-1]);
            if (scenario == 2 || scenario == 3) CHECK(rc == ETIMEDOUT);
        }
        task = nullptr; stop = nullptr; ipc = nullptr; output = nullptr;
    }
    return true;
}
}
int main() { return unitTests() && cyclicTests() ? 0 : 1; }
