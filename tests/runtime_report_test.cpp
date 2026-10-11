#include "runtime_report.hpp"

#include <iostream>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return 1; } } while (false)

int main() {
    rocos::TimingSummary timing;
    for (double us : {1000., 800., 1200.}) timing.add(us);
    CHECK(timing.samples == 3 && timing.minimum_us == 800 && timing.maximum_us == 1200);
    CHECK(timing.average_us == 1000);
    timing = {};
    timing.add(0);
    timing.add(10);
    CHECK(timing.minimum_us == 0 && timing.average_us == 5 && timing.maximum_us == 10);

    rocos::RuntimeReportMailbox mailbox, other_master;
    rocos::RuntimeReport report{}, received{};
    CHECK(!mailbox.consume(received));
    report.total_missed = 42;
    CHECK(mailbox.publish(report));
    report.total_missed = 99;
    CHECK(!mailbox.publish(report)); // Full mailbox is not overwritten or waited on.
    CHECK(!other_master.consume(received));
    CHECK(mailbox.consume(received) && received.total_missed == 42);
    CHECK(!mailbox.consume(received));

    // Exercise ownership handoff on different threads; any torn copy violates
    // the relationships between the sequence number and the payload fields.
    constexpr std::uint64_t count = 50000;
    std::thread writer([&] {
        for (std::uint64_t i = 1; i <= count; ++i) {
            rocos::RuntimeReport next{};
            next.total_missed = i;
            next.total_wc_errors = i * 3;
            next.cycle.add(static_cast<double>(i));
            next.send.add(static_cast<double>(i + 7));
            while (!mailbox.publish(next)) std::this_thread::yield();
        }
    });
    bool valid = true;
    for (std::uint64_t i = 1; i <= count; ++i) {
        while (!mailbox.consume(received)) std::this_thread::yield();
        valid &= received.total_missed == i && received.total_wc_errors == i * 3 &&
                 received.cycle.samples == 1 && received.cycle.minimum_us == i &&
                 received.cycle.average_us == i && received.cycle.maximum_us == i &&
                 received.send.maximum_us == i + 7;
    }
    writer.join();
    CHECK(valid);
    return 0;
}
