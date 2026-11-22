#include <doctest/doctest.h>

#include "net/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace zk;

namespace {

// One connection on 127.0.0.1: answers 200 with a body of `size` bytes, sent in `chunk`-byte
// pieces every `everyMs`; stops sending after `sendBytes` (then holds the connection open for up
// to `holdMs`, or until it goes).
struct TrickleServer {
    int fd = -1, port = 0;
    std::atomic<bool> done{false};
    std::thread t;
    TrickleServer(size_t size, size_t sendBytes, size_t chunk, int everyMs, int holdMs) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        socklen_t len = sizeof a;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        ::listen(fd, 1);
        t = std::thread([=, this] {
            const int c = ::accept(fd, nullptr, nullptr);
            if (c < 0) return;
            char buf[2048];
            (void)::recv(c, buf, sizeof buf, 0);
            const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n";
            (void)::send(c, head.data(), head.size(), 0);
            const std::string piece(chunk, 'x');
            for (size_t sent = 0; sent < sendBytes; sent += chunk) {
                (void)::send(c, piece.data(), std::min(chunk, sendBytes - sent), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(everyMs));
            }
            for (int ms = 0; ms < holdMs && !done; ms += 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ::close(c);
        });
    }
    ~TrickleServer() {
        done = true;
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        if (t.joinable()) t.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/f"; }
};

}  // namespace

TEST_CASE("curl http: with a stall limit and no total limit, a download that keeps moving completes; a stalled one ends") {
    CurlHttp curl("");
    SUBCASE("moving") {
        TrickleServer srv(1500, 1500, 100, 100, 0);  // 1.5 s at 1 KB/s
        HttpRequest r;
        r.url = srv.url();
        r.noProxy = true;
        r.timeoutS = 0;
        r.stallSeconds = 1;
        r.stallBytesPerS = 10;
        const HttpResponse resp = curl.send(r);
        CHECK(resp.curlCode == 0);
        CHECK(resp.status == 200);
        CHECK(resp.body.size() == 1500);
    }
    SUBCASE("stalled") {
        TrickleServer srv(100000, 100, 100, 0, 10000);  // a little, then nothing
        HttpRequest r;
        r.url = srv.url();
        r.noProxy = true;
        r.timeoutS = 0;
        r.stallSeconds = 1;
        r.stallBytesPerS = 1000;
        const auto t0 = std::chrono::steady_clock::now();
        const HttpResponse resp = curl.send(r);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(resp.curlCode == 28);  // CURLE_OPERATION_TIMEDOUT
        CHECK(s < 8.0);  // the stall limit ended it, not the server closing (at 10 s)
    }
}

TEST_CASE("curl http: only a lookup that ran out of time counts as a lookup timeout") {
    CHECK(resolveTimedOut(28, "Resolving timed out after 10002 milliseconds"));
    CHECK_FALSE(resolveTimedOut(28, "Connection timed out after 10002 milliseconds"));
    CHECK_FALSE(resolveTimedOut(28, "Operation timed out after 10002 milliseconds with 0 bytes received"));
    CHECK_FALSE(resolveTimedOut(6, "Resolving timed out after 10002 milliseconds"));
    CHECK_FALSE(resolveTimedOut(28, nullptr));
}

TEST_CASE("curl http: a failed request still answers, with curl's code and error") {
    CurlHttp http("");
    HttpRequest req;
    req.url = "http://127.0.0.1:1/x";  // nothing listens on port 1
    const HttpResponse r = http.send(req);
    CHECK(r.curlCode == 7);
    CHECK_FALSE(r.curlError.empty());
}
