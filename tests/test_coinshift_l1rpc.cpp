#include "test.h"

#include <coinshift/l1rpc.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// The L1 client's transport against a one-connection server on 127.0.0.1 (an ephemeral port).
namespace {
struct OneShotServer {
    int fd{-1};
    uint16_t port{0};
    std::thread thr;
    std::atomic<bool> done{false};
    // reply(conn) writes the response (or not) once the request's headers came in.
    template <typename F>
    explicit OneShotServer(F reply)
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t l = sizeof a;
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &l);
        port = ntohs(a.sin_port);
        listen(fd, 1);
        thr = std::thread([this, reply] {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0) return;
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                ssize_t n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, size_t(n));
            }
            reply(c);
            close(c);
            done = true;
        });
    }
    ~OneShotServer()
    {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (thr.joinable()) thr.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }
};

void send_all(int c, const std::string& s)
{
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(c, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += size_t(n);
    }
}
} // namespace

TEST(l1rpc_call_reads_the_result)
{
    const std::string body = "{\"result\":\"regtest\",\"error\":null,\"id\":1}";
    OneShotServer srv([&](int c) {
        send_all(c, "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: " + std::to_string(body.size()) +
                        "\r\n\r\n" + body);
    });
    std::string err;
    auto r = coinshift::l1::Client{{srv.url(), "u", "p"}}.call("getblockchaininfo", UniValue(UniValue::VARR), err);
    REQUIRE(r.has_value());
    CHECK_EQ(r->get_str(), std::string("regtest"));
}

TEST(l1rpc_refuses_a_response_beyond_the_bound)
{
    OneShotServer srv([](int c) {
        send_all(c, "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\n\r\n");
        const std::string chunk(1 << 20, ' ');
        for (int i = 0; i < 70; ++i) send_all(c, chunk); // 70 MiB, over the 64 MiB bound
    });
    std::string err;
    auto r = coinshift::l1::Client{{srv.url(), "", ""}}.call("getblockchaininfo", UniValue(UniValue::VARR), err);
    CHECK(!r.has_value());
    CHECK_EQ(err, std::string("response too large"));
}

TEST(l1rpc_times_out_on_a_silent_server)
{
    OneShotServer srv([](int) { std::this_thread::sleep_for(std::chrono::seconds(12)); });
    std::string err;
    auto t0 = std::chrono::steady_clock::now();
    auto r = coinshift::l1::Client{{srv.url(), "", ""}}.call("getblockchaininfo", UniValue(UniValue::VARR), err);
    auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!r.has_value());
    CHECK(s >= 9 && s <= 11);
}

TEST(l1rpc_http_only)
{
    std::string err;
    auto r = coinshift::l1::Client{{"https://127.0.0.1:1/", "", ""}}.call("x", UniValue(UniValue::VARR), err);
    CHECK(!r.has_value());
    CHECK(err.find("unsupported URL") != std::string::npos);
}
