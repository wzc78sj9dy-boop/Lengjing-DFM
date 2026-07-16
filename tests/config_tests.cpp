#include "test_support.h"

#include "config/LocalConfig.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

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
    const std::filesystem::path path = TestPath();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);

    lengjing::ui::UiModel expected;
    expected.runtime.gameVersionIndex = 2;
    expected.runtime.driverIndex = 1;
    expected.visual.modelGeometry = true;
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
    expected.aim.inputMode = lengjing::ui::AimInputMode::WriteTouch;
    expected.aim.weaponProfilesEnabled = true;
    expected.aim.weaponProfiles[4].prediction = 1.75f;
    expected.aim.randomBoneWeights[8] = 17.0f;
    expected.system.frameLimitIndex = 4;
    expected.system.toastNotifications = false;

    lengjing::config::LocalConfig config(path.string());
    std::string error;
    REQUIRE(config.Save(expected, &error));
    REQUIRE(error.empty());

    lengjing::ui::UiModel actual;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.runtime.gameVersionIndex == expected.runtime.gameVersionIndex);
    REQUIRE(actual.runtime.driverIndex == expected.runtime.driverIndex);
    REQUIRE(actual.visual.modelGeometry == expected.visual.modelGeometry);
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
    REQUIRE(actual.aim.inputMode == lengjing::ui::AimInputMode::WriteTouch);
    REQUIRE(actual.aim.weaponProfilesEnabled);
    REQUIRE(actual.aim.weaponProfiles[4].prediction == 1.75f);
    REQUIRE(actual.aim.randomBoneWeights[8] == 17.0f);
    REQUIRE(actual.system.frameLimitIndex == 4);
    REQUIRE(!actual.system.toastNotifications);

    expected.runtime.driverIndex = 0;
    REQUIRE(config.Save(expected, &error));
    actual.runtime.driverIndex = 1;
    REQUIRE(config.Load(actual, &error));
    REQUIRE(actual.runtime.driverIndex == 0);

    {
        std::ofstream conflicting(path, std::ios::binary | std::ios::trunc);
        conflicting <<
            R"({"schema_version":1,"runtime":{"driver":9},)"
            R"("visual":{"coordinate_decrypt":true,)"
            R"("algorithm_decrypt":true,"warning_size":1200},)"
            R"("aim":{"input_mode":9}})";
    }
    actual = {};
    error.clear();
    REQUIRE(config.Load(actual, &error));
    REQUIRE(error.empty());
    REQUIRE(!actual.visual.coordinateDecrypt);
    REQUIRE(actual.visual.algorithmDecrypt);
    REQUIRE(actual.visual.warningSize == 1000.0f);
    REQUIRE(actual.runtime.driverIndex == 1);
    REQUIRE(actual.aim.inputMode == lengjing::ui::AimInputMode::WriteTouch);

    const std::filesystem::path menuSource =
        std::filesystem::path(__FILE__).parent_path().parent_path() /
        "jni" / "src" / "ui" / "MenuView.cpp";
    const std::string menuText = ReadText(menuSource);
    REQUIRE(menuText.find("visual.aimWarningRay && visual.playerViewRay") != std::string::npos);
    REQUIRE(menuText.find("visual.playerViewRay = value;") != std::string::npos);

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
