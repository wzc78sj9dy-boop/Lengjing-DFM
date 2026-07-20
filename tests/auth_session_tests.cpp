#include "test_support.h"

#include "auth/RemoteAuth.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
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

    lengjing::auth::AuthVariableResult GetVariableByCard(
        std::string_view,
        std::string_view,
        std::string_view) override {
        ++variableCalls;
        return variableResult;
    }

    void CancelPendingRequests() noexcept override {
        ++cancelCalls;
    }

    lengjing::auth::AuthLoginResult loginResult{
        true, {}, "STATE_FOR_TEST", "2099-12-31 23:59:59"};
    lengjing::auth::AuthCallResult heartbeatResult{true, {}};
    lengjing::auth::AuthVariableResult variableResult{};
    std::atomic_int loginCalls{0};
    std::atomic_int heartbeatCalls{0};
    std::atomic_int variableCalls{0};
    std::atomic_int cancelCalls{0};
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
    return R"({"schema_version":2,"package":"com.example.runtime","module":"libUE4.so","build_id":"0123456789abcdef0123456789abcdef01234567","revision":1,"layout":{"name_pool":"0x12001000","world":"0x13002000","coordinate_replay_entry":"0x0","geometry_instances":["0x0","0x0"],"tracking_matrix_root":"0x0","component_position_flag":"0x0","actor_records":{"tagged_container":"0x0","plain_array":"0x0","plain_root":"0x0","plain_mesh":"0x0","encrypted_record_count":0,"plain_record_stride":0,"maximum_plain_count":0,"fallback_plain_count":0},"coordinate_pool":{"root_rva":"0x1a009000","bridge_offset":"0x14","context_offset":-16,"entry_offset":"0xb0","component_key_offset":"0x220","pacga_data":"0x13579bdf","pacga_modifier":"0x2468ace0","entry_stride":64,"pool_head_skip":24,"ring_refresh_frames":90}}})";
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

lengjing::auth::CloudRuntimeIdentity RuntimeIdentity() {
    return {"com.example.runtime", "libUE4.so",
            "0123456789abcdef0123456789abcdef01234567"};
}

}  // namespace

void RunAuthSessionTests() {
    using namespace std::chrono_literals;
    using namespace lengjing::auth;

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

        CloudLayoutStore store(RuntimeIdentity());
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
        gateway->variableResult = {true, {}, CloudPayload()};
        AuthSessionOptions options;
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::Published);
        REQUIRE(gateway->variableCalls.load() == 1);
        REQUIRE(store.Snapshot()->revision == 1);
    }

    {
        auto gateway = std::make_shared<FakeAuthGateway>();
        gateway->variableResult = {
            true, {}, EncodeCloudPayloadQuotesMixed()};
        AuthSessionOptions options;
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
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
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
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
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
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
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
        const CloudLayoutUpdateResult update =
            session.RefreshCloudLayout(store);
        REQUIRE(update.status == CloudLayoutStatus::InvalidJson);
        REQUIRE(store.Snapshot() == nullptr);
    }

    {
        auto gateway = std::make_shared<BlockingVariableGateway>(
            CloudPayload());
        AuthSessionOptions options;
        options.cloudVariable = {"CALL_CODE", "VALUE_ID", "VALUE_NAME"};
        options.heartbeatInterval = std::chrono::hours(1);
        options.stopTimeout = 250ms;

        AuthSession session;
        REQUIRE(session.Login(
            gateway, "CARD_FOR_TEST", "DEVICE_FOR_TEST", options));
        CloudLayoutStore store(RuntimeIdentity());
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
        config.cloudIdentity = {
            "com.example.runtime", "libUE4.so",
            "0123456789abcdef0123456789abcdef01234567"};
        const CloudRuntimeIdentity identity =
            ResolveCloudRuntimeIdentity(config);
        REQUIRE(identity.IsValid());
        REQUIRE(identity.packageName == "com.example.runtime");
        REQUIRE(identity.moduleName == "libUE4.so");
    }
}
