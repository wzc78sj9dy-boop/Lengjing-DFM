#include "test_support.h"

#include "auth/RemoteAuth.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {

class FakeAuthGateway final : public lengjing::auth::AuthGateway {
public:
    lengjing::auth::AuthLoginResult Login(
        std::string_view,
        std::string_view) override {
        ++loginCalls;
        return loginResult;
    }

    lengjing::auth::AuthCallResult Heartbeat(
        std::string_view,
        std::string_view) override {
        ++heartbeatCalls;
        return heartbeatResult;
    }

    lengjing::auth::AuthVersionResult GetLatestVersion() override {
        ++versionCalls;
        return versionResult;
    }

    lengjing::auth::AuthVariableResult GetVariableByCard(
        std::string_view,
        std::string_view valueId,
        std::string_view valueName) override {
        ++variableCalls;
        lastValueId = valueId;
        lastValueName = valueName;
        return variableResult;
    }

    void CancelPendingRequests() noexcept override {
        ++cancelCalls;
    }

    lengjing::auth::AuthLoginResult loginResult{
        true, {}, "STATE_FOR_TEST", "2099-12-31 23:59:59"};
    lengjing::auth::AuthCallResult heartbeatResult{true, {}};
    lengjing::auth::AuthVersionResult versionResult{
        true, {}, "1.0.0"};
    lengjing::auth::AuthVariableResult variableResult{};
    std::atomic_int loginCalls{0};
    std::atomic_int heartbeatCalls{0};
    std::atomic_int versionCalls{0};
    std::atomic_int variableCalls{0};
    std::atomic_int cancelCalls{0};
    std::string lastValueId;
    std::string lastValueName;
};

class BlockingHeartbeatGateway final : public lengjing::auth::AuthGateway {
public:
    explicit BlockingHeartbeatGateway(bool honorCancellation = true)
        : honorCancellation_(honorCancellation) {}

    lengjing::auth::AuthLoginResult Login(
        std::string_view,
        std::string_view) override {
        return {true, {}, "BLOCKING_STATE", "2099-12-31 23:59:59"};
    }

    lengjing::auth::AuthCallResult Heartbeat(
        std::string_view,
        std::string_view) override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return cancelled_; });
        exited_ = true;
        condition_.notify_all();
        return {false, "cancelled"};
    }

    lengjing::auth::AuthVersionResult GetLatestVersion() override {
        return {true, {}, "1.0.0"};
    }

    lengjing::auth::AuthVariableResult GetVariableByCard(
        std::string_view,
        std::string_view,
        std::string_view) override {
        return {};
    }

    void CancelPendingRequests() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (honorCancellation_) cancelled_ = true;
        condition_.notify_all();
    }

    void Release() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

    bool WaitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this] { return entered_; });
    }

    bool Exited() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return exited_;
    }

    bool WaitUntilExited(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this] { return exited_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool cancelled_ = false;
    bool exited_ = false;
    bool honorCancellation_ = true;
};

class BlockingVariableGateway final : public lengjing::auth::AuthGateway {
public:
    explicit BlockingVariableGateway(std::string value)
        : value_(std::move(value)) {}

    lengjing::auth::AuthLoginResult Login(
        std::string_view,
        std::string_view) override {
        return {true, {}, "VARIABLE_STATE", "2099-12-31 23:59:59"};
    }

    lengjing::auth::AuthCallResult Heartbeat(
        std::string_view,
        std::string_view) override {
        return {true, {}};
    }

    lengjing::auth::AuthVersionResult GetLatestVersion() override {
        return {true, {}, "1.0.0"};
    }

    lengjing::auth::AuthVariableResult GetVariableByCard(
        std::string_view,
        std::string_view,
        std::string_view) override {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] { return released_; });
        return {true, {}, value_};
    }

    void CancelPendingRequests() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        condition_.notify_all();
    }

    bool WaitUntilEntered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout,
                                   [this] { return entered_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::string value_;
    bool entered_ = false;
    bool released_ = false;
};

std::string CloudPayload() {
    return
        R"({"v":4,"b":"fedcba98765432100123456789abcdef","r":1,)"
        R"("d":[["0x21001000","0x22002000",)"
        R"(["0x23003000","0x24004000"],)"
        R"(["0x25005000","0x26006000","0x284","0x414",)"
        R"(1536,40,12288,3072],)"
        R"(["0x1a0","0x410","0x420"],)"
        R"("0x27007000","0x28008003"],)"
        R"([[["0x29009000","0x24",-24,"0xc0","0x260",)"
        R"(80,32,111],"0x123456789","0x987654321"],)"
        R"([["0x2a00a000","0x28",-32,"0xd0","0x270",)"
        R"(96,36,121]],)"
        R"([["0x2b00b000","0x14","0xd8",)"
        R"("0x13572468abcdef01"],["0x280","0x184"],)"
        R"(["0x400","0x404","0x408","0x40c","0x410","0x414",)"
        R"("0x418","0x41c","0x420","0x424","0x428","0x42c",)"
        R"("0x430","0x434","0x438","0x43c","0x440","0x444",)"
        R"("0x448","0x44c","0x450","0x454","0x458","0x45c",)"
        R"("0x460","0x464"],)"
        R"(["0x300","0x304","0x308","0x30c","0x310","0x314",)"
        R"("0x318","0x31c","0x320","0x324","0x328","0x32c",)"
        R"("0x330","0x334","0x338"],)"
        R"(["WorkerAlpha","0x9ac33041"]]]]})";
}

std::string EncodeCloudPayloadQuotes(std::string_view quoteEntity) {
    const std::string payload = CloudPayload();
    std::string encoded;
    encoded.reserve(payload.size() * 2U);
    for (const char character : payload) {
        if (character == '"') {
            encoded.append(quoteEntity);
        } else {
            encoded.push_back(character);
        }
    }
    return encoded;
}

std::string EncodeCloudPayloadQuotesMixed() {
    constexpr std::string_view quoteEntities[]{
        "&quot;", "&#34;", "&#x22;"};
    const std::string payload = CloudPayload();
    std::string encoded;
    encoded.reserve(payload.size() * 2U);
    std::size_t entityIndex = 0;
    for (const char character : payload) {
        if (character == '"') {
            encoded.append(quoteEntities[
                entityIndex++ % std::size(quoteEntities)]);
        } else {
            encoded.push_back(character);
        }
    }
    return encoded;
}

lengjing::auth::CloudRuntimeTarget RuntimeTarget() {
    return {"com.example.runtime", "libSynthetic.so"};
}

}  // namespace

void RunAuthSessionTests() {
    using namespace std::chrono_literals;
    using namespace lengjing::auth;

    {
        FakeAuthGateway gateway;
        REQUIRE(CheckCloudVersion(gateway, "1.0.0") ==
                CloudVersionStatus::Current);
        REQUIRE(gateway.versionCalls.load() == 1);

        gateway.versionResult.version = "1.0.1";
        REQUIRE(CheckCloudVersion(gateway, "1.0.0") ==
                CloudVersionStatus::UpdateRequired);

        gateway.versionResult = {false, "network error", {}};
        REQUIRE(CheckCloudVersion(gateway, "1.0.0") ==
                CloudVersionStatus::CheckFailed);
        REQUIRE(CheckCloudVersion(gateway, {}) ==
                CloudVersionStatus::CheckFailed);
    }

    {
        std::string payload = "&quot;&amp;&#34;&#x22;";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::Success);
        REQUIRE(payload == "\"&\"\"");

        payload = "&amp;quot;";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::Success);
        REQUIRE(payload == "\"");

        payload = "&amp;amp;quot;";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::UnsupportedEntity);
        REQUIRE(payload.empty());

        payload = "&amp;lt;";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::UnsupportedEntity);
        REQUIRE(payload.empty());

        payload = "&lt;";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::UnsupportedEntity);
        REQUIRE(payload.empty());

        payload = "&#34";
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::UnsupportedEntity);
        REQUIRE(payload.empty());
    }

    {
        std::string payload(kMaximumCloudLayoutPayloadBytes, 'x');
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::Success);
        REQUIRE(payload.size() == kMaximumCloudLayoutPayloadBytes);

        payload.assign(kMaximumCloudLayoutPayloadBytes + 1U, 'x');
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::OutputTooLarge);
        REQUIRE(payload.empty());

        payload.clear();
        payload.reserve(kMaximumCloudLayoutPayloadBytes * 6U);
        for (std::size_t index = 0;
             index < kMaximumCloudLayoutPayloadBytes; ++index) {
            payload.append("&quot;");
        }
        REQUIRE(payload.size() ==
                kMaximumCloudLayoutPayloadBytes * 6U);
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::Success);
        REQUIRE(payload.size() == kMaximumCloudLayoutPayloadBytes);

        payload.clear();
        payload.reserve(kMaximumEncodedCloudVariablePayloadBytes);
        for (std::size_t index = 0;
             index < kMaximumCloudLayoutPayloadBytes; ++index) {
            payload.append("&amp;quot;");
        }
        REQUIRE(payload.size() ==
                kMaximumEncodedCloudVariablePayloadBytes);
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::Success);
        REQUIRE(payload.size() == kMaximumCloudLayoutPayloadBytes);

        payload.assign(
            kMaximumEncodedCloudVariablePayloadBytes + 1U, 'x');
        REQUIRE(DecodeCloudVariablePayload(payload) ==
                CloudVariablePayloadDecodeStatus::InputTooLarge);
        REQUIRE(payload.empty());
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        AuthSession session;
        REQUIRE(session.Login(gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST"));
        REQUIRE(session.IsValid());
        REQUIRE(!session.ExitRequested());
        REQUIRE(session.ExpiresAt() == "2099-12-31 23:59:59");

        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult missing =
            session.RefreshCloudLayout(store);
        REQUIRE(missing.status == CloudLayoutStatus::NotConfigured);
        REQUIRE(gateway->variableCalls.load() == 0);
        REQUIRE(store.Snapshot() == nullptr);
        session.Stop();
        REQUIRE(session.State() == AuthState::Stopped);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        AuthSessionOptions options;
        options.heartbeatInterval = 1ms;
        options.startHeartbeat = false;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        std::this_thread::sleep_for(5ms);
        REQUIRE(gateway->heartbeatCalls.load() == 0);
        REQUIRE(session.StartHeartbeat());
        REQUIRE(session.StartHeartbeat());
        for (int attempt = 0;
             attempt < 200 && gateway->heartbeatCalls.load() == 0;
             ++attempt) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(gateway->heartbeatCalls.load() > 0);
        session.Stop();
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {true, {}, CloudPayload()};
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::Published);
        REQUIRE(gateway->variableCalls.load() == 1);
        REQUIRE(gateway->lastValueId == "VALUE_ID");
        REQUIRE(gateway->lastValueName == "VALUE_NAME");
        REQUIRE(store.Snapshot()->revision == 1);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {
            true, {}, EncodeCloudPayloadQuotesMixed()};
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::Published);
        REQUIRE(store.Snapshot() != nullptr);
        REQUIRE(store.Snapshot()->revision == 1);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {
            true, {}, "&lt;" + EncodeCloudPayloadQuotesMixed()};
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::InvalidJson);
        REQUIRE(update.detail ==
                "cloud variable payload contains an unsupported HTML entity");
        REQUIRE(store.Snapshot() == nullptr);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {
            true, {}, EncodeCloudPayloadQuotes("&amp;quot;")};
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::Published);
        REQUIRE(store.Snapshot() != nullptr);
        REQUIRE(store.Snapshot()->revision == 1);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {
            true, {}, EncodeCloudPayloadQuotes("&amp;amp;quot;")};
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::InvalidJson);
        REQUIRE(store.Snapshot() == nullptr);
    }

    {
        auto gateway = std::make_shared<BlockingVariableGateway>(
            CloudPayload());
        AuthSessionOptions options;
        options.coordinateSuiteVariable = {
            "CALL_CODE", "VALUE_ID", "VALUE_NAME"};
        options.heartbeatInterval = std::chrono::hours(1);
        options.stopTimeout = 250ms;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeTarget());
        CloudLayoutUpdateResult update;
        std::thread refresh([&] {
            update = session.RefreshCloudLayout(store);
        });
        const bool entered = gateway->WaitUntilEntered(250ms);
        session.Stop();
        refresh.join();
        REQUIRE(entered);
        REQUIRE(update.status == CloudLayoutStatus::SessionInvalid);
        REQUIRE(store.Snapshot() == nullptr);
        REQUIRE(session.State() == AuthState::Stopped);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->heartbeatResult = {false, "HEARTBEAT_REJECTED"};
        AuthSessionOptions options;
        options.heartbeatInterval = 1ms;
        options.maximumHeartbeatFailures = 2;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        for (int attempt = 0; attempt < 200 && !session.ExitRequested();
             ++attempt) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(session.ExitRequested());
        REQUIRE(!session.IsValid());
        REQUIRE(gateway->heartbeatCalls.load() >= 2);
        REQUIRE(session.LastError() == "HEARTBEAT_REJECTED");
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->loginResult = {false, "LOGIN_REJECTED", {}, {}};
        AuthSession session;
        REQUIRE(!session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST"));
        REQUIRE(session.ExitRequested());
        REQUIRE(session.LastError() == "LOGIN_REJECTED");
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->loginResult = {
            false, "rejected CARD_FOR_TEST for DEVICE_FOR_TEST", {}, {}};
        AuthSession session;
        REQUIRE(!session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST"));
        REQUIRE(session.LastError().find("CARD_FOR_TEST") ==
                std::string::npos);
        REQUIRE(session.LastError().find("DEVICE_FOR_TEST") ==
                std::string::npos);
        REQUIRE(session.LastError().find("[redacted]") !=
                std::string::npos);
    }

    {
        auto gateway = std::make_shared<BlockingHeartbeatGateway>();
        AuthSessionOptions options;
        options.heartbeatInterval = 1ms;
        options.stopTimeout = 250ms;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        REQUIRE(gateway->WaitUntilEntered(250ms));
        const auto stopStart = std::chrono::steady_clock::now();
        session.Stop();
        const auto stopElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stopStart);
        REQUIRE(stopElapsed < 200ms);
        REQUIRE(gateway->Exited());
        REQUIRE(session.State() == AuthState::Stopped);
    }

    {
        auto gateway = std::make_shared<BlockingHeartbeatGateway>(false);
        AuthSessionOptions options;
        options.heartbeatInterval = 1ms;
        options.stopTimeout = 20ms;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        REQUIRE(gateway->WaitUntilEntered(250ms));
        const auto stopStart = std::chrono::steady_clock::now();
        session.Stop();
        const auto stopElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - stopStart);
        REQUIRE(stopElapsed < 150ms);
        REQUIRE(!gateway->Exited());
        gateway->Release();
        REQUIRE(gateway->WaitUntilExited(250ms));
    }

    {
        T3AuthConfig config = kDefaultT3AuthConfig;
        config.targetPackage = "com.example.runtime";
        config.targetModule = "libSynthetic.so";
        const CloudRuntimeTarget target = ResolveCloudRuntimeTarget(config);
        REQUIRE(target.IsValid());
        REQUIRE(target.packageName == "com.example.runtime");
        REQUIRE(target.moduleName == "libSynthetic.so");
    }
}
