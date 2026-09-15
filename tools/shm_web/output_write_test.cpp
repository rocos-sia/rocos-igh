// Exercise the real backend with anonymous mappings; no EtherCAT or named IPC.
#define main shm_web_server_main
#include "shm_web_server.cpp"
#undef main

#include <stdexcept>

#define check(ok) do { if (!(ok)) throw std::runtime_error("output write check failed at line " + std::to_string(__LINE__)); } while (false)

void setup(AppState &st, int id) {
    st.master_id = id;
    st.demo = true;
    st.connected = true;
    st.cfg = std::make_unique<SharedMemoryConfig>(id);
    st.cfg->ecm_size_ = sizeof(EcatBus);
    st.cfg->ecatBus = static_cast<EcatBus *>(mmap(nullptr, sizeof(EcatBus), PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    check(st.cfg->ecatBus != MAP_FAILED);
    populateDemoBus(*st.cfg->ecatBus);
    st.cfg->pd_output_size_ = 13;
    st.cfg->pdOutputPtr = mmap(nullptr, 13, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(st.cfg->pdOutputPtr != MAP_FAILED);
}

std::string request(AppState &st, const std::string &headers, const std::string &body, bool fragmented = false) {
    int fds[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    std::thread worker(handleConnection, fds[1], std::ref(st), std::string("test"));
    const auto head = "POST /api/output HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
    const auto sent = send(fds[0], head.data(), head.size(), 0);
    if (sent < 0) std::perror("test send");
    check(sent == static_cast<ssize_t>(head.size()));
    if (fragmented) {
        for (char c : body) check(send(fds[0], &c, 1, 0) == 1);
    } else if (!body.empty()) {
        check(send(fds[0], body.data(), body.size(), 0) == static_cast<ssize_t>(body.size()));
    }
    shutdown(fds[0], SHUT_WR);
    std::string result;
    char buf[1024];
    ssize_t n;
    while ((n = recv(fds[0], buf, sizeof(buf), 0)) > 0) result.append(buf, n);
    close(fds[0]);
    worker.join();
    return result;
}

int main() {
    ignoreSigpipe();
    AppState st, other;
    setup(st, 910001);
    setup(other, 910002);
    const std::string prefix = "910001 0 ";
    check(writeOutput(st, prefix + "24698 0 0 4 78563412").empty()); // 607A
    check(hexBytes(st.cfg->pdOutputPtr, 0, 13) == "78563412000000000000000000");
    check(hexBytes(other.cfg->pdOutputPtr, 0, 13) == "00000000000000000000000000");
    check(writeOutput(st, prefix + "24689 0 8 2 feff").empty()); // 6071
    check(writeOutput(st, prefix + "24672 0 12 1 08").empty()); // 6060
    const auto before = hexBytes(st.cfg->pdOutputPtr, 0, 13);
    for (const auto &body : {
        "910002 0 24698 0 0 4 00000000", // wrong master
        "910001 1 24698 0 0 4 00000000", // wrong slave
        "910001 0 24641 0 0 2 0000", // IN 6041
        "910001 0 24698 1 0 4 00000000", // wrong subindex
        "910001 0 24698 0 1 4 00000000", // wrong offset
        "910001 0 24698 0 0 2 0000", // wrong width
        "910001 0 24698 0 0 4 zzzzzzzz", // invalid hex
        "910001 0 24698 0 0 4 0000", // truncated data
        "910001 0 24698 0 0 4 00000000 extra",
        "910001 0 24698 0 -1 4 00000000",
        "910001 0 24698 0 4294967296 4 00000000",
    }) check(!writeOutput(st, body).empty());
    st.cfg->pd_output_size_ = 3;
    check(!writeOutput(st, prefix + "24698 0 0 4 00000000").empty());
    st.cfg->pd_output_size_ = 13;
    st.connected = false;
    check(!writeOutput(st, prefix + "24698 0 0 4 00000000").empty());
    st.connected = true;
    st.demo = false;
    check(!writeOutput(st, prefix + "24698 0 0 4 00000000").empty());
    st.demo = true;
    check(hexBytes(st.cfg->pdOutputPtr, 0, 13) == before);
    const std::string body = prefix + "24698 0 0 4 01000000";
    const auto length = "Content-Length: " + std::to_string(body.size()) + "\r\n";
    check(request(st, length + "X-Rocos-Write: 1\r\nOrigin: http://localhost\r\n", body, true).find("200 OK") != std::string::npos);
    check(hexBytes(st.cfg->pdOutputPtr, 0, 4) == "01000000");
    check(request(st, length, body).find("400 Bad Request") != std::string::npos);
    check(request(st, length + "X-Rocos-Write: 1\r\nOrigin: http://foreign\r\n", body).find("403 ") != std::string::npos);
    check(request(st, length + length + "X-Rocos-Write: 1\r\n", body).find("400 ") != std::string::npos);
    check(request(st, length + "X-Rocos-Write: 1\r\nTransfer-Encoding: chunked\r\n", body).find("400 ") != std::string::npos);
    check(request(st, length + "X-Rocos-Write: 1\r\n", "short").find("400 ") != std::string::npos);
    std::cout << "OUT write validation and HTTP tests passed\n";
}
