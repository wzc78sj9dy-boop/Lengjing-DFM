#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lengjing::game::data {

struct ThreatObjectInfo {
    std::string_view classMarker;
    std::string_view displayName;
    float radiusCentimeters = 0.0f;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

inline constexpr std::array<ThreatObjectInfo, 34> kThreatObjects{{
    {"FireArrowActor", "电击箭矢", 300.0f, 255, 0, 0, 255},
    {"AbilityBullet_AirCannon", "虎蹲炮", 500.0f, 255, 0, 0, 255},
    {"ThrowableProjectileDefault", "手雷", 500.0f, 255, 0, 0, 255},
    {"IncendiaryArea", "复合型燃烧弹", 400.0f, 255, 0, 0, 255},
    {"AntiTankGrenade", "手炮", 500.0f, 255, 0, 0, 255},
    {"SuppressionMine", "声波陷阱", 200.0f, 255, 0, 0, 255},
    {"IntelligentSmokeActor", "智能烟雾弹体", 400.0f, 180, 180, 180, 255},
    {"IntelligenSmoke_Projectile", "智能烟雾陷阱", 400.0f, 180, 180, 180, 255},
    {"SentryHive_MP", "哨兵母巢陷阱", 300.0f, 255, 165, 0, 255},
    {"SentrySpider_MP", "哨兵蜘蛛", 250.0f, 255, 165, 0, 255},
    {"NetSpider_MP", "巡猎蜘蛛", 250.0f, 255, 165, 0, 255},
    {"ProjectileC4", "磁吸炸弹", 500.0f, 255, 0, 0, 255},
    {"C303FlashGrenade2", "突破型闪光弹", 400.0f, 255, 255, 0, 255},
    {"AbilityBullet_C303_knifeDrone2", "旋刃飞行器", 300.0f, 255, 0, 0, 255},
    {"ElectricShockBomb_Bullet", "钻墙电刺", 300.0f, 0, 191, 255, 255},
    {"RopeRoot", "紧急回避装置", 200.0f, 0, 255, 0, 255},
    {"AbilityBullet_C101_Smoke", "蜂巢科技烟雾弹", 400.0f, 180, 180, 180, 255},
    {"Inherit_SmokeWallUAS_IceLand", "烟幕", 400.0f, 180, 180, 180, 255},
    {"BlindSmokeActor", "致盲毒雾", 400.0f, 128, 0, 128, 255},
    {"AbilityBullet_C102Adrenaline", "肾上腺素激活", 300.0f, 0, 255, 0, 255},
    {"Zoya_Swarms_Mobile", "流莹集群系统", 400.0f, 255, 165, 0, 255},
    {"SkillSmokeActor", "遥控烟雾", 400.0f, 180, 180, 180, 255},
    {"AbilityBullet_C103_MedicalDrone", "纳米医疗粉尘", 300.0f, 0, 255, 0, 255},
    {"C401_Drone", "声波震慑", 300.0f, 255, 255, 0, 255},
    {"AbilityThrowBlocking_Ground", "速凝掩体", 300.0f, 0, 191, 255, 255},
    {"GuidedCruiseMissile", "巡飞弹", 500.0f, 255, 0, 0, 255},
    {"WeaponBullet_FireArrow", "电击箭矢", 300.0f, 255, 0, 0, 255},
    {"WeaponBullet_ProxSensorArrow", "侦察箭矢", 300.0f, 255, 255, 0, 255},
    {"EMPGrenade", "脉冲手雷", 400.0f, 0, 191, 255, 255},
    {"BionicBird", "猎鹰无人机", 300.0f, 255, 255, 0, 255},
    {"BarbedWireArea", "刀片刺网手雷", 400.0f, 255, 0, 0, 255},
    {"Hook_HitBox", "多功能钩爪枪", 300.0f, 255, 165, 0, 255},
    {"AbilityBullet_C202_FlashDrone", "闪光巡飞器", 400.0f, 255, 255, 0, 255},
    {"AbilityBullet_C202_DataKnife", "数据飞刀", 300.0f, 0, 191, 255, 255},
}};

inline const ThreatObjectInfo* FindThreatObject(std::string_view className) noexcept {
    for (const ThreatObjectInfo& info : kThreatObjects) {
        if (className.find(info.classMarker) != std::string_view::npos) {
            return &info;
        }
    }
    return nullptr;
}

}  // namespace lengjing::game::data
