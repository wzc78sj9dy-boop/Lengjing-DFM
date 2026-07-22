#include "config/LocalConfig.h"

#include "game/ProjectileTrackingFeature.h"
#include "vendor/json.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

#ifndef LENGJING_ENABLE_ALGORITHM_COORDINATE
#define LENGJING_ENABLE_ALGORITHM_COORDINATE 0
#endif

namespace lengjing::config {
namespace {

using Json = nlohmann::json;

constexpr int kSchemaVersion = 1;

template <typename T>
T ReadNumber(const Json& object, const char* key, T fallback, T minimum, T maximum) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number()) {
        return fallback;
    }
    const T value = iterator->get<T>();
    return std::clamp(value, minimum, maximum);
}

bool ReadBool(const Json& object, const char* key, bool fallback) {
    const auto iterator = object.find(key);
    return iterator != object.end() && iterator->is_boolean()
        ? iterator->get<bool>()
        : fallback;
}

ui::RenderBackend ReadRenderBackend(
    const Json& object,
    const char* key,
    ui::RenderBackend fallback) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return fallback;
    }
    if (!iterator->is_number_integer()) {
        return ui::RenderBackend::Cpu;
    }
    switch (iterator->get<int>()) {
        case static_cast<int>(ui::RenderBackend::Cpu):
            return ui::RenderBackend::Cpu;
        case static_cast<int>(ui::RenderBackend::Vulkan):
            return ui::RenderBackend::Vulkan;
        case static_cast<int>(ui::RenderBackend::OpenGl):
            return ui::RenderBackend::OpenGl;
        default:
            return ui::RenderBackend::Cpu;
    }
}

Json SaveAimTuning(const ui::AimTuning& tuning) {
    return Json{
        {"range_pixels", tuning.rangePixels},
        {"hip_distance_meters", tuning.hipDistanceMeters},
        {"ads_distance_meters", tuning.adsDistanceMeters},
        {"hip_speed", tuning.hipSpeed},
        {"ads_speed", tuning.adsSpeed},
        {"horizontal_speed", tuning.horizontalSpeed},
        {"vertical_speed", tuning.verticalSpeed},
        {"prediction", tuning.prediction},
        {"recoil", tuning.recoil},
        {"smoothing", tuning.smoothing},
        {"hip_bone", tuning.hipBone},
        {"ads_bone", tuning.adsBone},
    };
}

void LoadAimTuning(const Json& object, ui::AimTuning& tuning) {
    tuning.rangePixels = ReadNumber(object, "range_pixels", tuning.rangePixels, 0.0f, 2000.0f);
    tuning.hipDistanceMeters = ReadNumber(object, "hip_distance_meters", tuning.hipDistanceMeters, 1.0f, 1000.0f);
    tuning.adsDistanceMeters = ReadNumber(object, "ads_distance_meters", tuning.adsDistanceMeters, 1.0f, 1000.0f);
    tuning.hipSpeed = ReadNumber(object, "hip_speed", tuning.hipSpeed, 0.0f, 300.0f);
    tuning.adsSpeed = ReadNumber(object, "ads_speed", tuning.adsSpeed, 0.0f, 300.0f);
    tuning.horizontalSpeed = ReadNumber(object, "horizontal_speed", tuning.horizontalSpeed, 0.0f, 300.0f);
    tuning.verticalSpeed = ReadNumber(object, "vertical_speed", tuning.verticalSpeed, 0.0f, 300.0f);
    tuning.prediction = ReadNumber(object, "prediction", tuning.prediction, 0.0f, 5.0f);
    tuning.recoil = ReadNumber(object, "recoil", tuning.recoil, 0.0f, 5.0f);
    tuning.smoothing = ReadNumber(object, "smoothing", tuning.smoothing, 0.0f, 500.0f);
    tuning.hipBone = ReadNumber(object, "hip_bone", tuning.hipBone, 0, 7);
    tuning.adsBone = ReadNumber(object, "ads_bone", tuning.adsBone, 0, 7);
}

Json Serialize(const ui::UiModel& model) {
    Json profiles = Json::array();
    for (const ui::AimTuning& profile : model.aim.weaponProfiles) {
        profiles.push_back(SaveAimTuning(profile));
    }

    Json containers = Json::array();
    for (bool enabled : model.loot.containerKinds) {
        containers.push_back(enabled);
    }

    Json randomBones = Json::array();
    for (float weight : model.aim.randomBoneWeights) {
        randomBones.push_back(weight);
    }

    Json aim{
        {"enabled", model.aim.enabled},
        {"cover", model.aim.missMode},
        {"cover_mode", model.aim.coverMode},
        {"weapon_profiles_enabled", model.aim.weaponProfilesEnabled},
        {"weapon_profile", model.aim.weaponProfileIndex},
        {"weapon_profiles", std::move(profiles)},
        {"defaults", SaveAimTuning(model.aim.defaults)},
        {"ignore_bots", model.aim.ignoreBots},
        {"ignore_downed", model.aim.ignoreDowned},
        {"persistent_lock", model.aim.persistentLock},
        {"curved_motion", model.aim.curvedMotion},
        {"require_visibility", model.aim.requireVisibility},
        {"enforce_fov", model.aim.enforceFov},
        {"enforce_distance", model.aim.enforceDistance},
        {"draw_range", model.aim.drawRange},
        {"draw_target_ray", model.aim.drawTargetRay},
        {"trigger_mode", model.aim.triggerMode},
        {"target_algorithm", model.aim.targetAlgorithm},
        {"random_bone_weights", std::move(randomBones)},
        {"input_mode", static_cast<int>(model.aim.inputMode)},
        {"show_touch_area", model.aim.showTouchArea},
        {"touch_range", model.aim.touchRange},
        {"touch_x", model.aim.touchX},
        {"touch_y", model.aim.touchY},
    };
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        aim["trajectory_tracking"] = model.aim.trajectoryTracking;
        aim["reject_target_state"] = model.aim.rejectTargetState;
        aim["reject_dead_target"] = model.aim.rejectDeadTarget;
        aim["player_dead_box"] = model.aim.playerDeadBox;
        aim["robot_dead_box"] = model.aim.robotDeadBox;
        aim["hit_percentage"] = model.aim.hitPercentage;
#endif

    Json root{
        {"schema_version", kSchemaVersion},
        {"runtime", {
            {"game_version", model.runtime.gameVersionIndex},
            {"driver", model.runtime.driverIndex},
        }},
        {"visual", {
            {"enabled", model.visual.enabled},
            {"player_count", model.visual.playerCount},
            {"anti_flicker", model.visual.antiFlicker},
            {"model_geometry", model.visual.modelGeometry},
            {"visibility_color", model.visual.visibilityColor},
            {"coordinate_decrypt", model.visual.coordinateDecrypt},
            {"box", model.visual.box},
            {"snapline", model.visual.snapline},
            {"skeleton", model.visual.skeleton},
            {"distance", model.visual.distance},
            {"player_name", model.visual.playerName},
            {"health", model.visual.health},
            {"offscreen_warning", model.visual.offscreenWarning},
            {"operator_name", model.visual.operatorName},
            {"held_weapon", model.visual.heldWeapon},
            {"armor_level", model.visual.armorLevel},
            {"armor_durability", model.visual.armorDurability},
            {"downed_player", model.visual.downedPlayer},
            {"filter_bots", model.visual.filterBots},
            {"nearby_enemy", model.visual.nearbyEnemy},
            {"battlefield_mode", model.visual.battlefieldMode},
            {"combat_mode", model.visual.combatMode},
            {"crosshair", model.visual.crosshair},
            {"throwable_warning", model.visual.throwableWarning},
            {"throwable_trajectory", model.visual.throwableTrajectory},
            {"aim_warning", model.visual.aimWarning},
            {"aim_warning_ray", model.visual.aimWarningRay},
            {"player_view_ray", model.visual.playerViewRay},
            {"debug_info", model.visual.debugInfo},
            {"class_name_debug", model.visual.classNameDebug},
            {"draw_distance_meters", model.visual.drawDistanceMeters},
            {"warning_size", model.visual.warningSize},
            {"warning_distance_meters", model.visual.warningDistanceMeters},
            {"skeleton_distance_meters", model.visual.skeletonDistanceMeters},
            {"crosshair_size", model.visual.crosshairSize},
            {"crosshair_thickness", model.visual.crosshairThickness},
            {"line_thickness", model.visual.lineThickness},
            {"font_scale", model.visual.fontScale},
        }},
        {"loot", {
            {"enabled", model.loot.enabled},
            {"player_box", model.loot.playerBox},
            {"bot_box", model.loot.botBox},
            {"password", model.loot.password},
            {"containers", model.loot.containers},
            {"box_contents", model.loot.boxContents},
            {"container_contents", model.loot.containerContents},
            {"high_value_list", model.loot.highValueList},
            {"container_kinds", std::move(containers)},
            {"item_distance_meters", model.loot.itemDistanceMeters},
            {"minimum_item_value", model.loot.minimumItemValue},
            {"minimum_item_rarity", model.loot.minimumItemRarity},
            {"container_distance_meters", model.loot.containerDistanceMeters},
            {"minimum_container_value", model.loot.minimumContainerValue},
            {"minimum_container_rarity", model.loot.minimumContainerRarity},
            {"list_limit", model.loot.listLimit},
            {"minimum_list_value", model.loot.minimumListValue},
            {"minimum_list_rarity", model.loot.minimumListRarity},
        }},
        {"radar", {
            {"overlay", model.radar.overlay},
            {"mini_map", model.radar.miniMap},
            {"big_map", model.radar.bigMap},
            {"show_self", model.radar.showSelf},
            {"mini_map_bots", model.radar.miniMapBots},
            {"big_map_bots", model.radar.bigMapBots},
            {"overlay_x", model.radar.overlayX},
            {"overlay_y", model.radar.overlayY},
            {"overlay_size", model.radar.overlaySize},
            {"overlay_range_meters", model.radar.overlayRangeMeters},
            {"map_offset_x", model.radar.mapOffsetX},
            {"map_offset_y", model.radar.mapOffsetY},
            {"map_font_size", model.radar.mapFontSize},
            {"map_dot_size", model.radar.mapDotSize},
        }},
        {"aim", std::move(aim)},
        {"system", {
            {"frame_limit", model.system.frameLimitIndex},
            {"render_backend", static_cast<int>(model.system.renderBackend)},
            {"auto_scroll_logs", model.system.autoScrollLogs},
            {"toast_notifications", model.system.toastNotifications},
        }},
    };
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    root["visual"]["algorithm_decrypt"] = model.visual.algorithmDecrypt;
#endif
    return root;
}

void Apply(const Json& root, ui::UiModel& model) {
    if (root.value("schema_version", 0) != kSchemaVersion) {
        return;
    }

    const Json& runtime = root.value("runtime", Json::object());
    model.runtime.gameVersionIndex = ReadNumber(runtime, "game_version", model.runtime.gameVersionIndex, 0, 2);
    model.runtime.driverIndex = ReadNumber(
        runtime, "driver", model.runtime.driverIndex, 0, 2);

    const Json& visual = root.value("visual", Json::object());
#define LOAD_VISUAL_BOOL(field, key) model.visual.field = ReadBool(visual, key, model.visual.field)
    LOAD_VISUAL_BOOL(enabled, "enabled");
    LOAD_VISUAL_BOOL(playerCount, "player_count");
    LOAD_VISUAL_BOOL(antiFlicker, "anti_flicker");
    LOAD_VISUAL_BOOL(modelGeometry, "model_geometry");
    LOAD_VISUAL_BOOL(visibilityColor, "visibility_color");
    LOAD_VISUAL_BOOL(coordinateDecrypt, "coordinate_decrypt");
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    LOAD_VISUAL_BOOL(algorithmDecrypt, "algorithm_decrypt");
#else
    model.visual.algorithmDecrypt = false;
#endif
    LOAD_VISUAL_BOOL(box, "box");
    LOAD_VISUAL_BOOL(snapline, "snapline");
    LOAD_VISUAL_BOOL(skeleton, "skeleton");
    LOAD_VISUAL_BOOL(distance, "distance");
    LOAD_VISUAL_BOOL(playerName, "player_name");
    LOAD_VISUAL_BOOL(health, "health");
    LOAD_VISUAL_BOOL(offscreenWarning, "offscreen_warning");
    LOAD_VISUAL_BOOL(operatorName, "operator_name");
    LOAD_VISUAL_BOOL(heldWeapon, "held_weapon");
    LOAD_VISUAL_BOOL(armorLevel, "armor_level");
    LOAD_VISUAL_BOOL(armorDurability, "armor_durability");
    LOAD_VISUAL_BOOL(downedPlayer, "downed_player");
    LOAD_VISUAL_BOOL(filterBots, "filter_bots");
    LOAD_VISUAL_BOOL(nearbyEnemy, "nearby_enemy");
    LOAD_VISUAL_BOOL(battlefieldMode, "battlefield_mode");
    LOAD_VISUAL_BOOL(combatMode, "combat_mode");
    LOAD_VISUAL_BOOL(crosshair, "crosshair");
    LOAD_VISUAL_BOOL(throwableWarning, "throwable_warning");
    LOAD_VISUAL_BOOL(throwableTrajectory, "throwable_trajectory");
    LOAD_VISUAL_BOOL(aimWarning, "aim_warning");
    LOAD_VISUAL_BOOL(aimWarningRay, "aim_warning_ray");
    LOAD_VISUAL_BOOL(playerViewRay, "player_view_ray");
    LOAD_VISUAL_BOOL(debugInfo, "debug_info");
    LOAD_VISUAL_BOOL(classNameDebug, "class_name_debug");
#undef LOAD_VISUAL_BOOL
    model.visual.drawDistanceMeters = ReadNumber(visual, "draw_distance_meters", model.visual.drawDistanceMeters, 0, 2000);
    model.visual.warningSize = ReadNumber(visual, "warning_size", model.visual.warningSize, 0.0f, 1000.0f);
    model.visual.warningDistanceMeters = ReadNumber(visual, "warning_distance_meters", model.visual.warningDistanceMeters, 0.0f, 2000.0f);
    model.visual.skeletonDistanceMeters = ReadNumber(visual, "skeleton_distance_meters", model.visual.skeletonDistanceMeters, 0.0f, 1000.0f);
    model.visual.crosshairSize = ReadNumber(visual, "crosshair_size", model.visual.crosshairSize, 1.0f, 500.0f);
    model.visual.crosshairThickness = ReadNumber(visual, "crosshair_thickness", model.visual.crosshairThickness, 0.5f, 20.0f);
    model.visual.lineThickness = ReadNumber(visual, "line_thickness", model.visual.lineThickness, 0.5f, 20.0f);
    model.visual.fontScale = ReadNumber(visual, "font_scale", model.visual.fontScale, 0.2f, 4.0f);

    const Json& loot = root.value("loot", Json::object());
#define LOAD_LOOT_BOOL(field, key) model.loot.field = ReadBool(loot, key, model.loot.field)
    LOAD_LOOT_BOOL(enabled, "enabled");
    LOAD_LOOT_BOOL(playerBox, "player_box");
    LOAD_LOOT_BOOL(botBox, "bot_box");
    LOAD_LOOT_BOOL(password, "password");
    LOAD_LOOT_BOOL(containers, "containers");
    LOAD_LOOT_BOOL(boxContents, "box_contents");
    LOAD_LOOT_BOOL(containerContents, "container_contents");
    LOAD_LOOT_BOOL(highValueList, "high_value_list");
#undef LOAD_LOOT_BOOL
    const auto containerIterator = loot.find("container_kinds");
    if (containerIterator != loot.end() && containerIterator->is_array()) {
        const std::size_t count = std::min(containerIterator->size(), model.loot.containerKinds.size());
        for (std::size_t index = 0; index < count; ++index) {
            if ((*containerIterator)[index].is_boolean()) {
                model.loot.containerKinds[index] = (*containerIterator)[index].get<bool>();
            }
        }
    }
    model.loot.itemDistanceMeters = ReadNumber(loot, "item_distance_meters", model.loot.itemDistanceMeters, 0, 2000);
    model.loot.minimumItemValue = ReadNumber(loot, "minimum_item_value", model.loot.minimumItemValue, 0, 10000000);
    model.loot.minimumItemRarity = ReadNumber(loot, "minimum_item_rarity", model.loot.minimumItemRarity, 1, 6);
    model.loot.containerDistanceMeters = ReadNumber(loot, "container_distance_meters", model.loot.containerDistanceMeters, 0, 2000);
    model.loot.minimumContainerValue = ReadNumber(loot, "minimum_container_value", model.loot.minimumContainerValue, 0, 10000000);
    model.loot.minimumContainerRarity = ReadNumber(loot, "minimum_container_rarity", model.loot.minimumContainerRarity, 1, 6);
    model.loot.listLimit = ReadNumber(loot, "list_limit", model.loot.listLimit, 1, 50);
    model.loot.minimumListValue = ReadNumber(loot, "minimum_list_value", model.loot.minimumListValue, 0, 10000000);
    model.loot.minimumListRarity = ReadNumber(loot, "minimum_list_rarity", model.loot.minimumListRarity, 1, 6);

    const Json& radar = root.value("radar", Json::object());
#define LOAD_RADAR_BOOL(field, key) model.radar.field = ReadBool(radar, key, model.radar.field)
    LOAD_RADAR_BOOL(overlay, "overlay");
    LOAD_RADAR_BOOL(miniMap, "mini_map");
    LOAD_RADAR_BOOL(bigMap, "big_map");
    LOAD_RADAR_BOOL(showSelf, "show_self");
    LOAD_RADAR_BOOL(miniMapBots, "mini_map_bots");
    LOAD_RADAR_BOOL(bigMapBots, "big_map_bots");
#undef LOAD_RADAR_BOOL
    model.radar.overlayX = ReadNumber(radar, "overlay_x", model.radar.overlayX, -4000.0f, 4000.0f);
    model.radar.overlayY = ReadNumber(radar, "overlay_y", model.radar.overlayY, -4000.0f, 4000.0f);
    model.radar.overlaySize = ReadNumber(radar, "overlay_size", model.radar.overlaySize, 50.0f, 1000.0f);
    model.radar.overlayRangeMeters = ReadNumber(radar, "overlay_range_meters", model.radar.overlayRangeMeters, 10.0f, 5000.0f);
    model.radar.mapOffsetX = ReadNumber(radar, "map_offset_x", model.radar.mapOffsetX, -2000.0f, 2000.0f);
    model.radar.mapOffsetY = ReadNumber(radar, "map_offset_y", model.radar.mapOffsetY, -2000.0f, 2000.0f);
    model.radar.mapFontSize = ReadNumber(radar, "map_font_size", model.radar.mapFontSize, 0.0f, 80.0f);
    model.radar.mapDotSize = ReadNumber(radar, "map_dot_size", model.radar.mapDotSize, 0.0f, 80.0f);

    const Json& aim = root.value("aim", Json::object());
    model.aim.enabled = ReadBool(aim, "enabled", model.aim.enabled);
    const bool legacyCover =
        ReadBool(aim, "miss_mode", model.aim.missMode);
    model.aim.missMode = ReadBool(aim, "cover", legacyCover);
    model.aim.coverMode = ReadNumber(
        aim, "cover_mode", model.aim.coverMode, 0, 1);
    model.aim.weaponProfilesEnabled = ReadBool(aim, "weapon_profiles_enabled", model.aim.weaponProfilesEnabled);
    model.aim.weaponProfileIndex = ReadNumber(aim, "weapon_profile", model.aim.weaponProfileIndex, 0, static_cast<int>(ui::kWeaponProfileCount - 1));
    const auto profilesIterator = aim.find("weapon_profiles");
    if (profilesIterator != aim.end() && profilesIterator->is_array()) {
        const std::size_t count = std::min(profilesIterator->size(), model.aim.weaponProfiles.size());
        for (std::size_t index = 0; index < count; ++index) {
            if ((*profilesIterator)[index].is_object()) {
                LoadAimTuning((*profilesIterator)[index], model.aim.weaponProfiles[index]);
            }
        }
    }
    const auto defaultsIterator = aim.find("defaults");
    if (defaultsIterator != aim.end() && defaultsIterator->is_object()) {
        LoadAimTuning(*defaultsIterator, model.aim.defaults);
    }
    model.aim.ignoreBots = ReadBool(aim, "ignore_bots", model.aim.ignoreBots);
    model.aim.ignoreDowned = ReadBool(aim, "ignore_downed", model.aim.ignoreDowned);
    model.aim.persistentLock = ReadBool(aim, "persistent_lock", model.aim.persistentLock);
    model.aim.curvedMotion = ReadBool(aim, "curved_motion", model.aim.curvedMotion);
    model.aim.requireVisibility = ReadBool(aim, "require_visibility", model.aim.requireVisibility);
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        model.aim.trajectoryTracking = ReadBool(
            aim, "trajectory_tracking", model.aim.trajectoryTracking);
        model.aim.rejectTargetState = ReadBool(
            aim, "reject_target_state", model.aim.rejectTargetState);
        model.aim.rejectDeadTarget = ReadBool(
            aim, "reject_dead_target", model.aim.rejectDeadTarget);
        model.aim.playerDeadBox = ReadBool(
            aim, "player_dead_box", model.aim.playerDeadBox);
        model.aim.robotDeadBox = ReadBool(
            aim, "robot_dead_box", model.aim.robotDeadBox);
        model.aim.hitPercentage = ReadNumber(
            aim, "hit_percentage", model.aim.hitPercentage, 0, 100);
#else
        const ui::AimSettings defaults{};
        model.aim.trajectoryTracking = false;
        model.aim.rejectTargetState = defaults.rejectTargetState;
        model.aim.rejectDeadTarget = defaults.rejectDeadTarget;
        model.aim.playerDeadBox = defaults.playerDeadBox;
        model.aim.robotDeadBox = defaults.robotDeadBox;
        model.aim.hitPercentage = defaults.hitPercentage;
#endif
    model.aim.enforceFov = ReadBool(aim, "enforce_fov", model.aim.enforceFov);
    model.aim.enforceDistance = ReadBool(aim, "enforce_distance", model.aim.enforceDistance);
    model.aim.drawRange = ReadBool(aim, "draw_range", model.aim.drawRange);
    model.aim.drawTargetRay = ReadBool(aim, "draw_target_ray", model.aim.drawTargetRay);
    model.aim.triggerMode = ReadNumber(aim, "trigger_mode", model.aim.triggerMode, 0, 2);
    model.aim.targetAlgorithm = ReadNumber(aim, "target_algorithm", model.aim.targetAlgorithm, 0, 1);
    const auto randomIterator = aim.find("random_bone_weights");
    if (randomIterator != aim.end() && randomIterator->is_array()) {
        const std::size_t count = std::min(randomIterator->size(), model.aim.randomBoneWeights.size());
        for (std::size_t index = 0; index < count; ++index) {
            if ((*randomIterator)[index].is_number()) {
                model.aim.randomBoneWeights[index] = std::clamp((*randomIterator)[index].get<float>(), 0.0f, 100.0f);
            }
        }
    }
    const auto inputModeIterator = aim.find("input_mode");
    model.aim.inputMode = ui::AimInputMode::ReadOnly;
    if (inputModeIterator != aim.end() && inputModeIterator->is_number_integer()) {
        const int inputMode = inputModeIterator->get<int>();
        if (inputMode >= static_cast<int>(ui::AimInputMode::ReadOnly) &&
            inputMode <= static_cast<int>(ui::AimInputMode::KernelGyroscope)) {
            model.aim.inputMode = static_cast<ui::AimInputMode>(inputMode);
        } else {
            model.aim.inputMode = ui::AimInputMode::ReadOnly;
        }
    }
    model.aim.showTouchArea = ReadBool(aim, "show_touch_area", model.aim.showTouchArea);
    model.aim.touchRange = ReadNumber(aim, "touch_range", model.aim.touchRange, 10.0f, 2000.0f);
    model.aim.touchX = ReadNumber(aim, "touch_x", model.aim.touchX, -4000.0f, 8000.0f);
    model.aim.touchY = ReadNumber(aim, "touch_y", model.aim.touchY, -4000.0f, 8000.0f);

    const Json& system = root.value("system", Json::object());
    model.system.frameLimitIndex = ReadNumber(system, "frame_limit", model.system.frameLimitIndex, 0, 6);
    model.system.renderBackend = ReadRenderBackend(
        system, "render_backend", model.system.renderBackend);
    model.system.autoScrollLogs = ReadBool(system, "auto_scroll_logs", model.system.autoScrollLogs);
    model.system.toastNotifications = ReadBool(system, "toast_notifications", model.system.toastNotifications);
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

}  // namespace

LocalConfig::LocalConfig(std::string path) : path_(std::move(path)) {}

const std::string& LocalConfig::Path() const noexcept {
    return path_;
}

bool LocalConfig::Load(ui::UiModel& model, std::string* error) const {
#if !LENGJING_ENABLE_ALGORITHM_COORDINATE
    model.visual.algorithmDecrypt = false;
#endif
    std::ifstream stream(path_, std::ios::binary);
    if (!stream.is_open()) {
        if (errno == ENOENT) {
            return true;
        }
        SetError(error, std::string("无法读取本地配置: ") + std::strerror(errno));
        return false;
    }

    try {
        Json root;
        stream >> root;
        if (!root.is_object()) {
            SetError(error, "本地配置格式无效");
            return false;
        }
        Apply(root, model);
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("本地配置解析失败: ") + exception.what());
        return false;
    }
}

bool LocalConfig::Save(const ui::UiModel& model, std::string* error) const {
    const std::string temporaryPath = path_ + ".tmp";
    try {
        std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            SetError(error, std::string("无法写入本地配置: ") + std::strerror(errno));
            return false;
        }
        stream << Serialize(model).dump(2) << '\n';
        stream.flush();
        if (!stream.good()) {
            SetError(error, "本地配置写入失败");
            stream.close();
            std::remove(temporaryPath.c_str());
            return false;
        }
        stream.close();

        if (std::rename(temporaryPath.c_str(), path_.c_str()) != 0) {
            std::remove(path_.c_str());
            if (std::rename(temporaryPath.c_str(), path_.c_str()) != 0) {
                SetError(error, std::string("无法替换本地配置: ") + std::strerror(errno));
                std::remove(temporaryPath.c_str());
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        std::remove(temporaryPath.c_str());
        SetError(error, std::string("本地配置保存失败: ") + exception.what());
        return false;
    }
}

}  // namespace lengjing::config
