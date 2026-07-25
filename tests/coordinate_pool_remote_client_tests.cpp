#include "game/native/CoordinatePoolRemoteClient.h"
#include "vendor/json.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

using namespace std::chrono_literals;
using lengjing::game::native::CoordinatePoolRemoteClient;
using lengjing::game::native::CoordinatePoolRemoteClientOptions;
using lengjing::game::native::CoordinatePoolRemoteCodeFingerprint;
using lengjing::game::native::CoordinatePoolRemoteRequest;
using lengjing::game::native::CoordinatePoolRemoteResult;
using lengjing::game::native::CoordinatePoolRemoteResultStatus;
using lengjing::game::native::CoordinatePoolRemoteSubmitStatus;

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned int index = 0; index < 4U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned int index = 0; index < 8U; ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
}

void AppendVariable(std::vector<std::uint8_t>& bytes,
                    const std::string& name,
                    std::uint32_t type = 2U) {
    AppendU32(bytes, type);
    AppendU64(bytes, name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
}

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2U) / 3U * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3U) {
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[offset]) << 16U |
            (offset + 1U < bytes.size()
                ? static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U
                : 0U) |
            (offset + 2U < bytes.size()
                ? static_cast<std::uint32_t>(bytes[offset + 2U])
                : 0U);
        encoded.push_back(kAlphabet[(value >> 18U) & 0x3FU]);
        encoded.push_back(kAlphabet[(value >> 12U) & 0x3FU]);
        encoded.push_back(offset + 1U < bytes.size()
            ? kAlphabet[(value >> 6U) & 0x3FU]
            : '=');
        encoded.push_back(offset + 2U < bytes.size()
            ? kAlphabet[value & 0x3FU]
            : '=');
    }
    return encoded;
}

std::string BuildPlanResponse(std::uint64_t base) {
    using namespace lengjing::game::native::coordinate_pool_internal;
    std::vector<std::uint8_t> expression;
    AppendU32(expression, EXPR_BINARY);
    AppendU32(expression, OP_ADD);
    AppendVariable(expression, "ring", EXPR_MEMORY);
    AppendU32(expression, EXPR_BINARY);
    AppendU32(expression, OP_ADD);
    AppendVariable(expression, "memory", EXPR_MEMORY);
    AppendVariable(expression, "captured");

    nlohmann::json data = {
        {"A", 8},
        {"B", 16},
        {"C", 48},
        {"D", base},
        {"E", base + 4U},
        {"F", ARM64_REG_X2},
        {"G", base},
        {"H", base + 8U},
        {"I", ARM64_REG_X3},
        {"J", nlohmann::json::array({
            {{"A", "memory"}, {"B", 8},
             {"C", nlohmann::json::array()}, {"D", 32}},
        })},
        {"K", nlohmann::json::array({
            {{"A", "captured"}, {"B", base + 8U},
             {"C", ARM64_REG_X4}, {"D", 0}},
        })},
        {"L", EncodeBase64(expression)},
        {"M", "ring"},
        {"N", base + 12U},
        {"P", nlohmann::json::array({
            nlohmann::json::array({base + 16U, UINT32_C(0xD503201F)}),
        })},
    };
    return nlohmann::json{{"code", 0}, {"data", std::move(data)}}.dump();
}

class ScriptedTransport final : public T3HttpTransport {
public:
    struct Call {
        std::thread::id thread;
        std::chrono::steady_clock::time_point time;
        std::string url;
        std::string contentType;
        std::string body;
    };

    explicit ScriptedTransport(std::deque<T3HttpTransportResult> responses = {})
        : responses_(std::move(responses)) {}

    T3HttpTransportResult post(const std::string& url,
                               const std::string& contentType,
                               const std::string& body) override {
        std::unique_lock<std::mutex> lock(mutex_);
        calls_.push_back({
            std::this_thread::get_id(),
            std::chrono::steady_clock::now(),
            url,
            contentType,
            body,
        });
        condition_.notify_all();
        if (throwing_) throw std::runtime_error("transport threw");
        if (blocking_) {
            condition_.wait(lock, [&] { return cancelled_; });
            postExited_ = true;
            condition_.notify_all();
            return {false, {}, "HTTP request cancelled"};
        }
        if (responses_.empty()) {
            return {false, {}, "script is exhausted"};
        }
        T3HttpTransportResult result = std::move(responses_.front());
        responses_.pop_front();
        return result;
    }

    void cancelPendingRequests() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        ++cancelCount_;
        condition_.notify_all();
    }

    void resetCancellation() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = false;
    }

    void SetBlocking(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        blocking_ = value;
    }

    void SetThrowing(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        throwing_ = value;
    }

    bool WaitForCalls(std::size_t count,
                      std::chrono::milliseconds timeout = 1s) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [&] { return calls_.size() >= count; });
    }

    std::vector<Call> Calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    std::size_t CancelCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelCount_;
    }

    bool PostExited() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return postExited_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<T3HttpTransportResult> responses_;
    std::vector<Call> calls_;
    std::size_t cancelCount_ = 0;
    bool blocking_ = false;
    bool cancelled_ = false;
    bool postExited_ = false;
    bool throwing_ = false;
};

CoordinatePoolRemoteClientOptions TestOptions() {
    CoordinatePoolRemoteClientOptions options;
    options.endpoint = "http://127.0.0.1:18080/decrypt/parse";
    options.initialRetryDelay = 10ms;
    options.maximumRetryDelay = 40ms;
    options.maximumTransportAttempts = 3;
    options.maximumMappingBytes = 4096;
    options.maximumCachedResults = 2;
    return options;
}

CoordinatePoolRemoteRequest MakeRequest(std::uint64_t base) {
    CoordinatePoolRemoteRequest request;
    request.key.mappingBase = base;
    request.key.rawEntry = base;
    request.deviceId = "12345";
    request.mapping.resize(256);
    for (std::size_t index = 0; index < request.mapping.size(); ++index) {
        request.mapping[index] = static_cast<std::uint8_t>(index);
    }
    request.mapping[7] = 0;
    request.mapping[8] = '\r';
    request.mapping[9] = '\n';
    request.key.mappingSize = request.mapping.size();
    request.key.codeFingerprint = CoordinatePoolRemoteCodeFingerprint(
        request.mapping.data(), request.mapping.size());
    return request;
}

bool WaitForSuccess(CoordinatePoolRemoteClient& client,
                    CoordinatePoolRemoteResult& success,
                    std::vector<CoordinatePoolRemoteResult>& failures) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        CoordinatePoolRemoteResult result;
        while (client.TryTakeResult(result)) {
            if (result.Ok()) {
                success = std::move(result);
                return true;
            }
            failures.push_back(std::move(result));
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

bool WaitForResults(CoordinatePoolRemoteClient& client,
                    std::size_t count,
                    std::vector<CoordinatePoolRemoteResult>& results) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        CoordinatePoolRemoteResult result;
        while (client.TryTakeResult(result)) {
            results.push_back(std::move(result));
        }
        if (results.size() >= count) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::string BoundaryFromContentType(const std::string& contentType) {
    constexpr std::string_view prefix =
        "multipart/form-data; boundary=";
    REQUIRE(contentType.compare(0, prefix.size(), prefix) == 0);
    return contentType.substr(prefix.size());
}

void VerifyMultipart(const ScriptedTransport::Call& call,
                     const CoordinatePoolRemoteRequest& expected) {
    REQUIRE(call.url == "http://127.0.0.1:18080/decrypt/parse");
    const std::string boundary = BoundaryFromContentType(call.contentType);
    REQUIRE(!boundary.empty());
    const std::string fileHeader =
        "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; "
        "filename=\"dump.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n";
    REQUIRE(call.body.compare(0, fileHeader.size(), fileHeader) == 0);
    REQUIRE(call.body.size() >= fileHeader.size() + expected.mapping.size());
    REQUIRE(std::memcmp(
        call.body.data() + fileHeader.size(),
        expected.mapping.data(),
        expected.mapping.size()) == 0);
    REQUIRE(call.body.find(
        "\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"entry\"\r\n\r\n" +
        std::to_string(expected.key.rawEntry) + "\r\n") !=
        std::string::npos);
    REQUIRE(call.body.find(
        "\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"base\"\r\n\r\n" +
        std::to_string(expected.key.mappingBase) + "\r\n") !=
        std::string::npos);
    REQUIRE(call.body.find(
        "\r\n--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"id\"\r\n\r\n" +
        expected.deviceId + "\r\n") != std::string::npos);
    REQUIRE(call.body.compare(
        call.body.size() - boundary.size() - 6U,
        boundary.size() + 6U,
        "--" + boundary + "--\r\n") == 0);
}

void RunRetryCacheAndMultipartTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x100000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {false, {}, "first failure"},
            {false, {}, "second failure"},
            {true, BuildPlanResponse(kBase), {}},
        });
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.IsReady());

    const CoordinatePoolRemoteRequest expected = MakeRequest(kBase);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Duplicate);

    CoordinatePoolRemoteResult success;
    std::vector<CoordinatePoolRemoteResult> failures;
    REQUIRE(WaitForSuccess(client, success, failures));
    REQUIRE(failures.size() == 2);
    REQUIRE(failures[0].status ==
        CoordinatePoolRemoteResultStatus::TransportFailed);
    REQUIRE(failures[0].attempt == 1);
    REQUIRE(failures[0].nextRetryDelay == 10ms);
    REQUIRE(failures[0].retryScheduled);
    REQUIRE(failures[1].attempt == 2);
    REQUIRE(failures[1].nextRetryDelay == 20ms);
    REQUIRE(failures[1].retryScheduled);
    REQUIRE(success.attempt == 3);
    REQUIRE(!success.retryScheduled);
    REQUIRE(success.key == expected.key);
    REQUIRE(success.mapping != nullptr);
    REQUIRE(*success.mapping == expected.mapping);
    REQUIRE(success.plan.poolPointerOffset == 16);

    const std::vector<ScriptedTransport::Call> calls = transport->Calls();
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[0].thread != std::this_thread::get_id());
    REQUIRE(calls[0].thread == calls[1].thread);
    REQUIRE(calls[1].thread == calls[2].thread);
    REQUIRE(calls[1].time - calls[0].time >= 7ms);
    REQUIRE(calls[2].time - calls[1].time >= 15ms);
    for (const auto& call : calls) VerifyMultipart(call, expected);

    const auto snapshot = client.GetSnapshot();
    REQUIRE(snapshot.ready);
    REQUIRE(!snapshot.requestActive);
    REQUIRE(!snapshot.requestPending);
    REQUIRE(snapshot.requestCount == 3);
    REQUIRE(snapshot.successCount == 1);
    REQUIRE(snapshot.failureCount == 2);
    REQUIRE(snapshot.cachedResultCount == 1);

    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Cached);
    CoordinatePoolRemoteResult cached;
    REQUIRE(client.TryGetCached(expected.key, cached));
    REQUIRE(cached.Ok());
    REQUIRE(cached.mapping != nullptr);
    REQUIRE(*cached.mapping == expected.mapping);
    REQUIRE(transport->Calls().size() == 3);

    CoordinatePoolRemoteRequest invalid = MakeRequest(kBase);
    invalid.key.mappingSize += 1;
    REQUIRE(client.Submit(std::move(invalid)) ==
        CoordinatePoolRemoteSubmitStatus::InvalidRequest);
}

void RunCancellationJoinTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x200000);
    auto transport = std::make_shared<ScriptedTransport>();
    transport->SetBlocking(true);
    {
        CoordinatePoolRemoteClient client(TestOptions(), transport);
        REQUIRE(client.Submit(MakeRequest(kBase)) ==
            CoordinatePoolRemoteSubmitStatus::Queued);
        REQUIRE(transport->WaitForCalls(1));
    }
    REQUIRE(transport->CancelCount() == 1);
    REQUIRE(transport->PostExited());
}

void RunResetReuseTest() {
    constexpr std::uint64_t kFirstBase = UINT64_C(0x210000);
    constexpr std::uint64_t kSecondBase = UINT64_C(0x220000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {true, BuildPlanResponse(kSecondBase), {}},
        });
    transport->SetBlocking(true);
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.Submit(MakeRequest(kFirstBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    REQUIRE(transport->WaitForCalls(1));
    client.Reset();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!transport->PostExited() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(transport->PostExited());
    REQUIRE(client.IsReady());

    transport->SetBlocking(false);
    REQUIRE(client.Submit(MakeRequest(kSecondBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    CoordinatePoolRemoteResult success;
    std::vector<CoordinatePoolRemoteResult> failures;
    REQUIRE(WaitForSuccess(client, success, failures));
    REQUIRE(failures.empty());
    REQUIRE(success.key.rawEntry == kSecondBase);
}

void RunResetActiveSameKeyTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x230000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {true, BuildPlanResponse(kBase), {}},
        });
    transport->SetBlocking(true);
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    REQUIRE(transport->WaitForCalls(1));

    client.Reset();
    transport->SetBlocking(false);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);

    CoordinatePoolRemoteResult success;
    std::vector<CoordinatePoolRemoteResult> failures;
    REQUIRE(WaitForSuccess(client, success, failures));
    REQUIRE(failures.empty());
    REQUIRE(success.key.rawEntry == kBase);
    REQUIRE(transport->Calls().size() == 2);
}

void RunResetClearsCacheTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x240000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {true, BuildPlanResponse(kBase), {}},
            {true, BuildPlanResponse(kBase), {}},
        });
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    CoordinatePoolRemoteResult first;
    std::vector<CoordinatePoolRemoteResult> failures;
    REQUIRE(WaitForSuccess(client, first, failures));
    REQUIRE(failures.empty());

    client.Reset();
    CoordinatePoolRemoteResult cached;
    REQUIRE(!client.TryGetCached(first.key, cached));
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);
    CoordinatePoolRemoteResult second;
    REQUIRE(WaitForSuccess(client, second, failures));
    REQUIRE(failures.empty());
    REQUIRE(transport->Calls().size() == 2);
}

void RunPlanRejectedStopsTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x250000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {true, "{\"code\":1}", {}},
            {true, BuildPlanResponse(kBase), {}},
        });
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);

    std::vector<CoordinatePoolRemoteResult> results;
    REQUIRE(WaitForResults(client, 1, results));
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].status ==
        CoordinatePoolRemoteResultStatus::PlanRejected);
    REQUIRE(results[0].attempt == 1);
    REQUIRE(!results[0].retryScheduled);
    REQUIRE(results[0].nextRetryDelay == 0ms);
    std::this_thread::sleep_for(80ms);
    REQUIRE(transport->Calls().size() == 1);
    const auto snapshot = client.GetSnapshot();
    REQUIRE(!snapshot.requestActive);
    REQUIRE(!snapshot.requestPending);
}

void RunTransportRetryLimitTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x260000);
    auto transport = std::make_shared<ScriptedTransport>(
        std::deque<T3HttpTransportResult>{
            {false, {}, "first failure"},
            {false, {}, "second failure"},
            {false, {}, "third failure"},
            {true, BuildPlanResponse(kBase), {}},
        });
    CoordinatePoolRemoteClient client(TestOptions(), transport);
    REQUIRE(client.Submit(MakeRequest(kBase)) ==
        CoordinatePoolRemoteSubmitStatus::Queued);

    std::vector<CoordinatePoolRemoteResult> results;
    REQUIRE(WaitForResults(client, 3, results));
    REQUIRE(results.size() == 3);
    for (const auto& result : results) {
        REQUIRE(result.status ==
            CoordinatePoolRemoteResultStatus::TransportFailed);
    }
    REQUIRE(results[0].attempt == 1);
    REQUIRE(results[0].retryScheduled);
    REQUIRE(results[0].nextRetryDelay == 10ms);
    REQUIRE(results[1].attempt == 2);
    REQUIRE(results[1].retryScheduled);
    REQUIRE(results[1].nextRetryDelay == 20ms);
    REQUIRE(results[2].attempt == 3);
    REQUIRE(!results[2].retryScheduled);
    REQUIRE(results[2].nextRetryDelay == 0ms);
    std::this_thread::sleep_for(80ms);
    REQUIRE(transport->Calls().size() == 3);
    const auto snapshot = client.GetSnapshot();
    REQUIRE(!snapshot.requestActive);
    REQUIRE(!snapshot.requestPending);
    REQUIRE(snapshot.requestCount == 3);
    REQUIRE(snapshot.failureCount == 3);
}

void RunInvalidOptionsTest() {
    CoordinatePoolRemoteClientOptions options = TestOptions();
    options.endpoint = "ftp://127.0.0.1/parse";
    CoordinatePoolRemoteClient client(
        options, std::make_shared<ScriptedTransport>());
    REQUIRE(!client.IsReady());
    REQUIRE(client.Submit(MakeRequest(UINT64_C(0x300000))) ==
        CoordinatePoolRemoteSubmitStatus::Stopped);
}

void RunWorkerExceptionTest() {
    constexpr std::uint64_t kBase = UINT64_C(0x400000);
    auto transport = std::make_shared<ScriptedTransport>();
    transport->SetThrowing(true);
    {
        CoordinatePoolRemoteClient client(TestOptions(), transport);
        REQUIRE(client.Submit(MakeRequest(kBase)) ==
            CoordinatePoolRemoteSubmitStatus::Queued);
        REQUIRE(transport->WaitForCalls(1));
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!client.GetSnapshot().stopped &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        const auto snapshot = client.GetSnapshot();
        REQUIRE(snapshot.stopped);
        REQUIRE(!snapshot.ready);
        REQUIRE(snapshot.requestCount == 1);
        REQUIRE(snapshot.failureCount == 1);
    }
    REQUIRE(transport->CancelCount() == 1);
}

}  // namespace

int main() {
    try {
        RunRetryCacheAndMultipartTest();
        RunCancellationJoinTest();
        RunResetReuseTest();
        RunResetActiveSameKeyTest();
        RunResetClearsCacheTest();
        RunPlanRejectedStopsTest();
        RunTransportRetryLimitTest();
        RunInvalidOptionsTest();
        RunWorkerExceptionTest();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
