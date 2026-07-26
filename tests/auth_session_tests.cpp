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
    std::ostringstream stream;
    stream
        << R"({"schema_version":3,"build_id":"fedcba98765432100123456789abcdef","revision":1,)"
        << R"("layout":{"name_pool":"0x21001000","world":"0x22002000",)"
        << R"("geometry_instances":["0x23003000","0x24004000"],)"
        << R"("actor_records":{"tagged_container":"0x25005000",)"
        << R"("plain_array":"0x26006000","plain_root":"0x284",)"
        << R"("plain_mesh":"0x414","encrypted_record_count":1536,)"
        << R"("plain_record_stride":40,"maximum_plain_count":12288,)"
        << R"("fallback_plain_count":3072},)"
        << R"("actor_subject":{"root":"0x1a0","mesh":"0x410",)"
        << R"("alternate_root":"0x420"},)"
        << R"("tracking_matrix_root":"0x27007000",)"
        << R"("component_position_flag":"0x28008003"},)"
        << R"("decrypt":{"mode1":{"pool":{"root_rva":"0x29009000",)"
        << R"("bridge_offset":"0x24","context_offset":-24,)"
        << R"("entry_offset":"0xc0","component_key_offset":"0x260",)"
        << R"("entry_stride":80,"pool_head_skip":32,)"
        << R"("ring_refresh_frames":111},"pacga_data":"0x123456789",)"
        << R"("pacga_modifier":"0x987654321"},)"
        << R"("mode2":{"pool":{"root_rva":"0x2a00a000",)"
        << R"("bridge_offset":"0x28","context_offset":-32,)"
        << R"("entry_offset":"0xd0","component_key_offset":"0x270",)"
        << R"("entry_stride":96,"pool_head_skip":36,)"
        << R"("ring_refresh_frames":121}},)"
        << R"("execution":{"discovery":{"root_offset":"0x2b00b000",)"
        << R"("pointer_offset":"0x14","entry_offset":"0xd8",)"
        << R"("return_stub_magic":"0x13572468abcdef01"},)"
        << R"("result":{"slot_offset":"0x280",)"
        << R"("position_offset":"0x184"},)"
        << R"("hook_offsets":{"subject_load":"0x400",)"
        << R"("callback_entry":"0x404","callback_return":"0x408",)"
        << R"("callback_index":"0x40c","callback_copy_prepare":"0x410",)"
        << R"("callback_copy_after":"0x414","table_pointer":"0x418",)"
        << R"("table_value":"0x41c","lock":"0x420",)"
        << R"("lock_return":"0x424","first_call":"0x428",)"
        << R"("first_return":"0x42c","external_call":"0x430",)"
        << R"("external_return":"0x434","primary_gate_write":"0x438",)"
        << R"("alternate_gate_write":"0x43c","gate_probe":"0x440",)"
        << R"("record_count":"0x444","target_key":"0x448",)"
        << R"("ring_setup":"0x44c","ring_probe":"0x450",)"
        << R"("ring_hit":"0x454","dispatch":"0x458",)"
        << R"("dispatch_return":"0x45c","result_prepare":"0x460",)"
        << R"("result":"0x464"},)"
        << R"("field_offsets":{"context_expected":"0x300",)"
        << R"("stack_prior_gate":"0x304",)"
        << R"("stack_primary_gate_source":"0x308",)"
        << R"("stack_gate_flag":"0x30c",)"
        << R"("stack_gate_snapshot_a":"0x310",)"
        << R"("stack_gate_snapshot_b":"0x314",)"
        << R"("stack_ring_mid":"0x318",)"
        << R"("object_position":"0x31c",)"
        << R"("stack_capture_a":"0x320",)"
        << R"("stack_capture_b":"0x324",)"
        << R"("stack_capture_c":"0x328",)"
        << R"("stack_capture_d":"0x32c",)"
        << R"("capture_field":"0x330",)"
        << R"("stack_pool_selector":"0x334",)"
        << R"("context_pool_table":"0x338"},)"
        << R"("context":{"thread_name":"WorkerAlpha",)"
        << R"("oracle_opcode":"0x9ac33041"}}}})";
    return stream.str();
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
