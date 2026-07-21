#include "test_support.h"

#include "config/LocalConfig.h"
#include "game/aim/CoverSelectionPolicy.h"
#include "render/render_types.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

void RunCoverSelectionPolicyTests() {
    using lengjing::game::VisibilityState;
    using lengjing::game::aim::CoverPointSelection;
    using lengjing::game::aim::SelectCoverPoint;

    std::array<bool, 15> valid{};
    valid.fill(true);
    std::array<VisibilityState, 15> visibility{};
    visibility.fill(VisibilityState::Occluded);

    visibility[0] = VisibilityState::Visible;
    visibility[1] = VisibilityState::Visible;
    auto selected = SelectCoverPoint(0, 0, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 0);
    REQUIRE(selected.visibility == VisibilityState::Visible);

    visibility[0] = VisibilityState::Unavailable;
    selected = SelectCoverPoint(0, 0, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 1);

    visibility[1] = VisibilityState::Occluded;
    visibility[2] = VisibilityState::Visible;
    visibility[0] = VisibilityState::Visible;
    selected = SelectCoverPoint(0, 1, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 0);

    visibility[0] = VisibilityState::Unavailable;
    selected = SelectCoverPoint(0, 1, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 2);

    visibility[2] = VisibilityState::Occluded;
    visibility[12] = VisibilityState::Visible;
    selected = SelectCoverPoint(0, 0, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 12);

    visibility[0] = VisibilityState::Visible;
    visibility[1] = VisibilityState::Visible;
    selected = SelectCoverPoint(0, 1, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 1);

    visibility[0] = VisibilityState::Occluded;
    selected = SelectCoverPoint(1, 0, valid, visibility);
    REQUIRE(!selected.selected);
    REQUIRE(
        selected.pointIndex == CoverPointSelection::kUnavailableIndex);
    REQUIRE(selected.visibility == VisibilityState::Unavailable);

    visibility[0] = VisibilityState::Visible;
    visibility[1] = VisibilityState::Occluded;
    visibility[2] = VisibilityState::Visible;
    selected = SelectCoverPoint(1, 1, valid, visibility);
    REQUIRE(!selected.selected);

    visibility[1] = VisibilityState::Visible;
    selected = SelectCoverPoint(1, 7, valid, visibility);
    REQUIRE(selected.selected);
    REQUIRE(selected.pointIndex == 1);

    valid[1] = false;
    selected = SelectCoverPoint(1, 1, valid, visibility);
    REQUIRE(!selected.selected);

    lengjing::SkeletonVisual skeleton{};
    skeleton.colorByVisibility = true;
    skeleton.selectedJoint = 1;
    skeleton.joints = {
        lengjing::BoneJoint{
            ImVec2{}, true, VisibilityState::Visible},
        lengjing::BoneJoint{
            ImVec2{}, true, VisibilityState::Occluded},
        lengjing::BoneJoint{
            ImVec2{}, true, VisibilityState::Unavailable},
    };
    skeleton.links.push_back(lengjing::BoneLink{0, 1});
    REQUIRE(
        skeleton.joints[skeleton.links[0].first].visibility ==
        VisibilityState::Visible);
    REQUIRE(
        skeleton.joints[skeleton.links[0].second].visibility ==
        VisibilityState::Occluded);
    REQUIRE(skeleton.selectedJoint == 1);
}

std::filesystem::path TestPath() {
    return std::filesystem::temp_directory_path() / "lengjing_config_test.json";
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

}  // namespace

void RunConfigTests() {
    RunCoverSelectionPolicyTests();

    const std::filesystem::path path = TestPath();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);

    REQUIRE(
        lengjing::ui::UiModel{}.aim.inputMode ==
        lengjing::ui::AimInputMode::ReadOnly);

    lengjing::ui::UiModel expected;
    expected.runtime.gameVersionIndex = 2;
    expected.runtime.driverIndex = 1;
    expected.visual.modelGeometry = true;
    expected.visual.coordinateDecrypt = true;
    expected.visual.crosshair = true;
    expected.visual.playerViewRay = true;
    expected.visual.drawDistanceMeters = 873;
    expected.visual.warningSize = 777.0f;
    expected.visual.fontScale = 1.4f;
    expected.loot.enabled = true;
    expected.loot.containerContents = true;
    expected.loot.containerKinds[13] = true;
    expected.loot.minimumItemValue = 123456;
    expected.radar.overlay = true;
    expected.radar.overlayRangeMeters = 640.0f;
    expected.radar.miniMap = true;
    expected.radar.bigMap = true;
    expected.radar.showSelf = true;
    expected.radar.miniMapBots = true;
    expected.radar.bigMapBots = true;
    expected.radar.mapOffsetX = -123.0f;
    expected.radar.mapOffsetY = 321.0f;
    expected.radar.mapFontSize = 27.0f;
    expected.radar.mapDotSize = 9.0f;
    expected.aim.enabled = true;
    expected.aim.missMode = true;
    expected.aim.coverMode = 1;
    expected.aim.inputMode = lengjing::ui::AimInputMode::WriteTouch;
    expected.aim.weaponProfilesEnabled = true;
    expected.aim.weaponProfiles[4].prediction = 1.75f;
    expected.aim.weaponProfiles[4].adsDistanceMeters = 225.0f;
    expected.aim.trajectoryTracking = true;
    expected.aim.requireVisibility = true;
    expected.aim.rejectTargetState = true;
    expected.aim.rejectDeadTarget = true;
    expected.aim.playerDeadBox = false;
    expected.aim.robotDeadBox = true;
    expected.aim.enforceFov = false;
    expected.aim.enforceDistance = false;
    expected.aim.hitPercentage = 73;
    expected.aim.randomBoneWeights[8] = 17.0f;
    expected.system.frameLimitIndex = 4;
    expected.system.renderBackend = lengjing::ui::RenderBackend::Vulkan;
    expected.system.toastNotifications = false;

    lengjing::config::LocalConfig config(path.string());
    std::string error;
    REQUIRE(config.Save(expected, &error));
    REQUIRE(error.empty());
    const std::string serialized = ReadText(path);
    REQUIRE(serialized.find("algorithm_decrypt") == std::string::npos);
    REQUIRE(serialized.find("\"cover\"") != std::string::npos);
    REQUIRE(serialized.find("\"cover_mode\"") != std::string::npos);
    REQUIRE(serialized.find("miss_mode") == std::string::npos);
    REQUIRE(serialized.find("tracking_bone_preset") == std::string::npos);
    REQUIRE(serialized.find("tracking_projectile_speed") == std::string::npos);
    REQUIRE(serialized.find("tracking_gravity") == std::string::npos);
    REQUIRE(serialized.find("trajectory_tracking") == std::string::npos);
    REQUIRE(serialized.find("reject_target_state") == std::string::npos);
    REQUIRE(serialized.find("reject_dead_target") == std::string::npos);
    REQUIRE(serialized.find("player_dead_box") == std::string::npos);
    REQUIRE(serialized.find("robot_dead_box") == std::string::npos);
    REQUIRE(serialized.find("hit_percentage") == std::string::npos);

    lengjing::ui::UiModel actual;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.runtime.gameVersionIndex == expected.runtime.gameVersionIndex);
    REQUIRE(actual.runtime.driverIndex == expected.runtime.driverIndex);
    REQUIRE(actual.visual.modelGeometry == expected.visual.modelGeometry);
    REQUIRE(actual.visual.coordinateDecrypt);
    REQUIRE(actual.visual.crosshair == expected.visual.crosshair);
    REQUIRE(actual.visual.playerViewRay == expected.visual.playerViewRay);
    REQUIRE(actual.visual.drawDistanceMeters == expected.visual.drawDistanceMeters);
    REQUIRE(actual.visual.warningSize == expected.visual.warningSize);
    REQUIRE(actual.visual.fontScale == expected.visual.fontScale);
    REQUIRE(actual.loot.enabled == expected.loot.enabled);
    REQUIRE(actual.loot.containerContents == expected.loot.containerContents);
    REQUIRE(actual.loot.containerKinds[13]);
    REQUIRE(actual.loot.minimumItemValue == expected.loot.minimumItemValue);
    REQUIRE(actual.radar.overlay);
    REQUIRE(actual.radar.overlayRangeMeters == expected.radar.overlayRangeMeters);
    REQUIRE(actual.radar.miniMap);
    REQUIRE(actual.radar.bigMap);
    REQUIRE(actual.radar.showSelf);
    REQUIRE(actual.radar.miniMapBots);
    REQUIRE(actual.radar.bigMapBots);
    REQUIRE(actual.radar.mapOffsetX == expected.radar.mapOffsetX);
    REQUIRE(actual.radar.mapOffsetY == expected.radar.mapOffsetY);
    REQUIRE(actual.radar.mapFontSize == expected.radar.mapFontSize);
    REQUIRE(actual.radar.mapDotSize == expected.radar.mapDotSize);
    REQUIRE(actual.aim.enabled);
    REQUIRE(actual.aim.missMode);
    REQUIRE(actual.aim.coverMode == 1);
    REQUIRE(actual.aim.inputMode == lengjing::ui::AimInputMode::WriteTouch);
    REQUIRE(actual.aim.weaponProfilesEnabled);
    REQUIRE(actual.aim.weaponProfiles[4].prediction == 1.75f);
    REQUIRE(actual.aim.weaponProfiles[4].adsDistanceMeters == 225.0f);
    REQUIRE(!actual.aim.trajectoryTracking);
    REQUIRE(actual.aim.requireVisibility);
    REQUIRE(!actual.aim.rejectTargetState);
    REQUIRE(!actual.aim.rejectDeadTarget);
    REQUIRE(actual.aim.playerDeadBox);
    REQUIRE(!actual.aim.robotDeadBox);
    REQUIRE(!actual.aim.enforceFov);
    REQUIRE(!actual.aim.enforceDistance);
    REQUIRE(actual.aim.hitPercentage == 100);
    REQUIRE(actual.aim.randomBoneWeights[8] == 17.0f);
    REQUIRE(actual.system.frameLimitIndex == 4);
    REQUIRE(
        actual.system.renderBackend ==
        lengjing::ui::RenderBackend::Vulkan);
    REQUIRE(!actual.system.toastNotifications);

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << R"({"schema_version":1,"aim":{)"
               << R"("trajectory_tracking":true,)"
               << R"("reject_target_state":true,)"
               << R"("reject_dead_target":true,)"
               << R"("player_dead_box":false,)"
               << R"("robot_dead_box":true,)"
               << R"("hit_percentage":17}})";
    }
    actual = {};
    actual.aim.trajectoryTracking = true;
    actual.aim.rejectTargetState = true;
    actual.aim.rejectDeadTarget = true;
    actual.aim.playerDeadBox = false;
    actual.aim.robotDeadBox = true;
    actual.aim.hitPercentage = 17;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(!actual.aim.trajectoryTracking);
    REQUIRE(!actual.aim.rejectTargetState);
    REQUIRE(!actual.aim.rejectDeadTarget);
    REQUIRE(actual.aim.playerDeadBox);
    REQUIRE(!actual.aim.robotDeadBox);
    REQUIRE(actual.aim.hitPercentage == 100);

    expected.runtime.driverIndex = 0;
    REQUIRE(config.Save(expected, &error));
    actual.runtime.driverIndex = 1;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.runtime.driverIndex == 0);

    expected.runtime.driverIndex = 2;
    REQUIRE(config.Save(expected, &error));
    actual.runtime.driverIndex = 0;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.runtime.driverIndex == 2);

    expected.system.renderBackend = lengjing::ui::RenderBackend::OpenGl;
    REQUIRE(config.Save(expected, &error));
    actual.system.renderBackend = lengjing::ui::RenderBackend::Cpu;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(
        actual.system.renderBackend ==
        lengjing::ui::RenderBackend::OpenGl);

    for (const char* invalidBackend : {"-1", "3", "99", "null", "\"1\""}) {
        std::ofstream invalidRenderBackend(
            path, std::ios::binary | std::ios::trunc);
        invalidRenderBackend
            << "{\"schema_version\":1,\"system\":{\"render_backend\":"
            << invalidBackend << "}}";
        invalidRenderBackend.close();
        actual.system.renderBackend = lengjing::ui::RenderBackend::Vulkan;
        REQUIRE(config.Load(actual, &error));
        REQUIRE(
            actual.system.renderBackend ==
            lengjing::ui::RenderBackend::Cpu);
    }

    {
        std::ofstream conflicting(path, std::ios::binary | std::ios::trunc);
        conflicting <<
            R"({"schema_version":1,"runtime":{"driver":9},)"
            R"("visual":{"coordinate_decrypt":true,)"
            R"("algorithm_decrypt":true,"warning_size":1200},)"
            R"("aim":{"input_mode":9,"hit_percentage":1000,)"
            R"("tracking_bone_preset":99,)"
            R"("defaults":{"ads_distance_meters":0,)"
            R"("tracking_projectile_speed":9000,"tracking_gravity":1000}}})";
    }
    actual = {};
    error.clear();
    REQUIRE(config.Load(actual, &error));
    REQUIRE(error.empty());
    REQUIRE(actual.visual.coordinateDecrypt);
    REQUIRE(actual.visual.warningSize == 1000.0f);
    REQUIRE(actual.runtime.driverIndex == 2);
    REQUIRE(actual.aim.inputMode == lengjing::ui::AimInputMode::ReadOnly);
    REQUIRE(!actual.aim.trajectoryTracking);
    REQUIRE(actual.aim.requireVisibility);
    REQUIRE(!actual.aim.rejectTargetState);
    REQUIRE(!actual.aim.rejectDeadTarget);
    REQUIRE(actual.aim.playerDeadBox);
    REQUIRE(!actual.aim.robotDeadBox);
    REQUIRE(actual.aim.enforceFov);
    REQUIRE(actual.aim.enforceDistance);
    REQUIRE(actual.aim.hitPercentage == 100);
    REQUIRE(actual.aim.defaults.adsDistanceMeters == 1.0f);

    for (int inputMode = 0; inputMode <= 4; ++inputMode) {
        std::ofstream mapped(path, std::ios::binary | std::ios::trunc);
        mapped << "{\"schema_version\":1,\"aim\":{\"input_mode\":"
               << inputMode << "}}";
        mapped.close();
        actual = {};
        REQUIRE(config.Load(actual, &error));
        REQUIRE(
            static_cast<int>(actual.aim.inputMode) == inputMode);
    }

    for (const char* invalidInput : {"-1", "5", "9", "null", "\"1\""}) {
        std::ofstream invalidMode(path, std::ios::binary | std::ios::trunc);
        invalidMode << "{\"schema_version\":1,\"aim\":{\"input_mode\":"
                    << invalidInput << "}}";
        invalidMode.close();
        actual.aim.inputMode = lengjing::ui::AimInputMode::WriteTouch;
        REQUIRE(config.Load(actual, &error));
        REQUIRE(
            actual.aim.inputMode ==
            lengjing::ui::AimInputMode::ReadOnly);
    }

    {
        std::ofstream legacyCover(path, std::ios::binary | std::ios::trunc);
        legacyCover <<
            R"({"schema_version":1,"aim":{"miss_mode":true,"cover_mode":99}})";
    }
    actual = {};
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.aim.missMode);
    REQUIRE(actual.aim.coverMode == 1);

    {
        std::ofstream preferredCover(
            path, std::ios::binary | std::ios::trunc);
        preferredCover <<
            R"({"schema_version":1,"aim":{"miss_mode":true,"cover":false,"cover_mode":-3}})";
    }
    actual = {};
    REQUIRE(config.Load(actual, &error));
    REQUIRE(!actual.aim.missMode);
    REQUIRE(actual.aim.coverMode == 0);

    {
        std::ofstream missingMode(path, std::ios::binary | std::ios::trunc);
        missingMode << R"({"schema_version":1,"aim":{}})";
    }
    actual.aim.inputMode = lengjing::ui::AimInputMode::WriteTouch;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(
        actual.aim.inputMode ==
        lengjing::ui::AimInputMode::ReadOnly);

    const std::filesystem::path menuSource =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "jni" / "src" / "ui" / "MenuView.cpp";
    const std::string menuText = ReadText(menuSource);
    for (const char* inputModeName : {
             "只读", "写入触摸（不推荐）", "程序陀螺仪", "内核触摸", "内核陀螺仪"}) {
        REQUIRE(menuText.find(inputModeName) != std::string::npos);
    }
    REQUIRE(menuText.find("visual.aimWarningRay && visual.playerViewRay") != std::string::npos);
    REQUIRE(menuText.find("visual.playerViewRay = value;") != std::string::npos);

    const std::filesystem::path mainSource =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "jni" / "src" / "main.cpp";
    REQUIRE(ReadText(mainSource).find("内核 RPC") != std::string::npos);

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << "{invalid";
    }
    error.clear();
    REQUIRE(!config.Load(actual, &error));
    REQUIRE(!error.empty());

    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
}
