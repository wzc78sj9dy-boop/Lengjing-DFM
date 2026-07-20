#include "test_support.h"

#include "t3/t3sdk.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
using TestSocket = SOCKET;
constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;
void CloseTestSocket(TestSocket value) {
    if (value != kInvalidTestSocket) closesocket(value);
}
#else
using TestSocket = int;
constexpr TestSocket kInvalidTestSocket = -1;
void CloseTestSocket(TestSocket value) {
    if (value != kInvalidTestSocket) close(value);
}
#endif

class LocalHttpServer final {
public:
    enum class Mode {
        Capture,
        Stall,
    };

    explicit LocalHttpServer(Mode mode) : mode_(mode) {
#if defined(_WIN32)
        REQUIRE(WSAStartup(MAKEWORD(2, 2), &winsockData_) == 0);
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ != kInvalidTestSocket);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0);
        REQUIRE(listen(listener_, 1) == 0);

#if defined(_WIN32)
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                            &length) == 0);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { Serve(); });
    }

    ~LocalHttpServer() {
        Join();
        CloseTestSocket(listener_);
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    unsigned short Port() const noexcept { return port_; }

    void Join() {
        if (worker_.joinable()) worker_.join();
    }

    const std::string& CapturedBody() const noexcept { return capturedBody_; }

private:
    void Serve() {
        sockaddr_in peer{};
#if defined(_WIN32)
        int peerLength = sizeof(peer);
#else
        socklen_t peerLength = sizeof(peer);
#endif
        const TestSocket client = accept(
            listener_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (client == kInvalidTestSocket) return;

        if (mode_ == Mode::Stall) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            CloseTestSocket(client);
            return;
        }

        std::string request;
        std::size_t expectedBytes = 0;
        char buffer[1024];
        for (;;) {
            const int received = recv(
                client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<std::size_t>(received));

            const std::size_t headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos && expectedBytes == 0) {
                constexpr const char* kLengthHeader = "Content-Length: ";
                const std::size_t lengthStart = request.find(kLengthHeader);
                if (lengthStart == std::string::npos) break;
                const std::size_t valueStart =
                    lengthStart + std::strlen(kLengthHeader);
                const std::size_t valueEnd = request.find("\r\n", valueStart);
                if (valueEnd == std::string::npos) break;
                expectedBytes = headerEnd + 4 + static_cast<std::size_t>(
                    std::stoull(request.substr(valueStart,
                                               valueEnd - valueStart)));
            }
            if (expectedBytes != 0 && request.size() >= expectedBytes) break;
        }

        const std::size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            capturedBody_ = request.substr(headerEnd + 4);
        }
        constexpr char response[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
            "Connection: close\r\n\r\nok";
        std::size_t sent = 0;
        while (sent < sizeof(response) - 1) {
            const int result = send(
                client, response + sent,
                static_cast<int>(sizeof(response) - 1 - sent), 0);
            if (result <= 0) break;
            sent += static_cast<std::size_t>(result);
        }
        CloseTestSocket(client);
    }

    Mode mode_;
    TestSocket listener_ = kInvalidTestSocket;
    unsigned short port_ = 0;
    std::thread worker_;
    std::string capturedBody_;
#if defined(_WIN32)
    WSADATA winsockData_{};
#endif
};

std::string LocalUrl(unsigned short port, const char* path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

}  // namespace

void RunAuthTransportTests() {
    using namespace std::chrono;
    using namespace std::chrono_literals;

    T3HttpTransportOptions invalid;
    invalid.receiveTimeoutMilliseconds = 0;
    REQUIRE(!invalid.isValid());
    invalid.receiveTimeoutMilliseconds = 5000;
    invalid.requestTimeoutMilliseconds = 0;
    REQUIRE(!invalid.isValid());

    T3HttpTransportOptions options;
    options.connectTimeoutMilliseconds = 250;
    options.sendTimeoutMilliseconds = 250;
    options.receiveTimeoutMilliseconds = 50;
    options.requestTimeoutMilliseconds = 1000;
    auto transport = createT3DefaultHttpTransport(options);
    REQUIRE(transport != nullptr);

    const T3HttpTransportResult invalidScheme = transport->post(
        "ftp://127.0.0.1/private", "text/plain", "sensitive");
    REQUIRE(!invalidScheme.success);
    REQUIRE(invalidScheme.error == "HTTP request URL is invalid");

    transport->cancelPendingRequests();
    const T3HttpTransportResult cancelled = transport->post(
        "http://127.0.0.1:1/cancelled", "text/plain", "value");
    REQUIRE(!cancelled.success);
    REQUIRE(cancelled.error == "HTTP request cancelled");
    transport->resetCancellation();

    {
        LocalHttpServer server(LocalHttpServer::Mode::Stall);
        const auto start = steady_clock::now();
        const T3HttpTransportResult result = transport->post(
            LocalUrl(server.Port(), "/timeout"), "text/plain", "value");
        const auto elapsed = duration_cast<milliseconds>(
            steady_clock::now() - start);
        REQUIRE(!result.success);
        REQUIRE(result.error == "HTTP receive timed out");
        REQUIRE(elapsed < 200ms);
    }

    {
        options.receiveTimeoutMilliseconds = 1000;
        options.requestTimeoutMilliseconds = 75;
        transport = createT3DefaultHttpTransport(options);
        LocalHttpServer server(LocalHttpServer::Mode::Stall);
        const auto start = steady_clock::now();
        const T3HttpTransportResult result = transport->post(
            LocalUrl(server.Port(), "/deadline"), "text/plain", "value");
        const auto elapsed = duration_cast<milliseconds>(
            steady_clock::now() - start);
        REQUIRE(!result.success);
        REQUIRE(result.error == "HTTP request deadline exceeded");
        REQUIRE(elapsed < 200ms);
    }

    {
        options.sendTimeoutMilliseconds = 250;
        options.receiveTimeoutMilliseconds = 1000;
        options.requestTimeoutMilliseconds = 1500;
        transport = createT3DefaultHttpTransport(options);
        LocalHttpServer server(LocalHttpServer::Mode::Stall);
        T3HttpTransportResult result;
        const auto start = steady_clock::now();
        std::thread request([&] {
            result = transport->post(
                LocalUrl(server.Port(), "/cancel"), "text/plain", "value");
        });
        std::this_thread::sleep_for(20ms);
        transport->cancelPendingRequests();
        request.join();
        const auto elapsed = duration_cast<milliseconds>(
            steady_clock::now() - start);
        REQUIRE(!result.success);
        REQUIRE(result.error == "HTTP request cancelled");
        REQUIRE(elapsed < 200ms);
        transport->resetCancellation();
    }

    {
        options.receiveTimeoutMilliseconds = 1000;
        options.requestTimeoutMilliseconds = 2000;
        transport = createT3DefaultHttpTransport(options);
        LocalHttpServer server(LocalHttpServer::Mode::Capture);
        const std::string body(512U * 1024U, 'x');
        const T3HttpTransportResult result = transport->post(
            LocalUrl(server.Port(), "/capture"),
            "application/octet-stream", body);
        REQUIRE(result.success);
        REQUIRE(result.body == "ok");
        server.Join();
        REQUIRE(server.CapturedBody() == body);
    }
}
