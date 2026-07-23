#include "test_support.h"

#include "platform/CoordinateDebugLog.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using lengjing::platform::CoordinateDebugLogSessionPolicy;
using lengjing::platform::CoordinateDebugLogTransition;

void RunCoordinateDebugLogTests() {
    using Clock = CoordinateDebugLogSessionPolicy::Clock;
    const Clock::time_point start{};

    CoordinateDebugLogSessionPolicy policy;
    REQUIRE(policy.UpdateRequested(false, start) ==
            CoordinateDebugLogTransition::None);
    REQUIRE(policy.UpdateRequested(true, start) ==
            CoordinateDebugLogTransition::Started);
    REQUIRE(policy.IsActive(start + std::chrono::seconds(29)));
    REQUIRE(policy.UpdateRequested(true, start + std::chrono::seconds(29)) ==
            CoordinateDebugLogTransition::None);
    REQUIRE(!policy.IsActive(start + std::chrono::seconds(30)));
    REQUIRE(policy.UpdateRequested(true, start + std::chrono::seconds(31)) ==
            CoordinateDebugLogTransition::None);

    REQUIRE(policy.UpdateRequested(false, start + std::chrono::seconds(31)) ==
            CoordinateDebugLogTransition::Stopped);
    REQUIRE(policy.UpdateRequested(true, start + std::chrono::seconds(32)) ==
            CoordinateDebugLogTransition::Started);
    REQUIRE(policy.BytesWritten() == 0);
    policy.RecordBytes(CoordinateDebugLogSessionPolicy::kMaximumBytes - 1);
    REQUIRE(policy.IsActive(start + std::chrono::seconds(33)));
    policy.RecordBytes(1);
    REQUIRE(!policy.IsActive(start + std::chrono::seconds(33)));
    REQUIRE(policy.UpdateRequested(
                true, start + std::chrono::seconds(34)) ==
            CoordinateDebugLogTransition::None);
    REQUIRE(!policy.IsActive(start + std::chrono::seconds(34)));

    const std::filesystem::path logPath =
        std::filesystem::temp_directory_path() /
        "lengjing_coordinate_debug_test.txt";
    std::remove(logPath.string().c_str());
    REQUIRE(lengjing::platform::ConfigureCoordinateDebugLog(
        logPath.string().c_str(), "test"));
    lengjing::platform::UpdateCoordinateDebugLogSession(true);
    lengjing::platform::CoordinateDebugLogPrint("first-session\n");
    lengjing::platform::UpdateCoordinateDebugLogSession(true);
    lengjing::platform::CoordinateDebugLogPrint("first-session-tail\n");
    lengjing::platform::UpdateCoordinateDebugLogSession(false);
    {
        std::ifstream firstInput(logPath, std::ios::binary);
        const std::string firstContents{
            std::istreambuf_iterator<char>(firstInput),
            std::istreambuf_iterator<char>()};
        REQUIRE(firstContents.find("first-session\n") != std::string::npos);
        REQUIRE(firstContents.find("first-session-tail") !=
                std::string::npos);
    }
    lengjing::platform::UpdateCoordinateDebugLogSession(true);
    lengjing::platform::CoordinateDebugLogPrint("second-session\n");
    lengjing::platform::UpdateCoordinateDebugLogSession(false);
    lengjing::platform::CoordinateDebugLogPrint("after-stop\n");

    std::ifstream input(logPath, std::ios::binary);
    const std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    REQUIRE(contents.find("[coordinate-debug-start] schema=4") !=
            std::string::npos);
    REQUIRE(contents.find("duration_seconds=30") != std::string::npos);
    REQUIRE(contents.find("max_bytes=33554432") != std::string::npos);
    REQUIRE(contents.find("second-session") != std::string::npos);
    REQUIRE(contents.find("first-session") == std::string::npos);
    REQUIRE(contents.find("after-stop") == std::string::npos);
    std::remove(logPath.string().c_str());
}
