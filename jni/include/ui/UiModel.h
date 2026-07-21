#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lengjing::ui {

enum class Page : std::uint8_t {
    Runtime,
    Visual,
    Loot,
    Radar,
    Aim,
    System,
};

enum class SettingsDomain : std::uint8_t {
    Runtime,
    Visual,
    Loot,
    Radar,
    Aim,
    System,
};

enum class RenderBackend : std::uint8_t {
    Cpu = 0,
    Vulkan,
    OpenGl,
};

enum class AimInputMode : std::uint8_t {
    ReadOnly = 0,
    WriteTouch = 1,
    ProgramGyroscope,
    KernelTouch,
    KernelGyroscope,
};

struct RuntimeModel {
    bool active = false;
    bool busy = false;
    bool stopping = false;
    int processId = 0;
    bool baseReady = false;
    bool coordinateRequested = false;
    bool coordinateEntryReady = false;
    bool coordinateContextReady = false;
    int coordinateThreadId = 0;
    std::uintptr_t coordinateGuestPc = 0;
    std::uint64_t coordinateContextGeneration = 0;
    std::uint64_t coordinateAttempts = 0;
    std::uint64_t coordinateSuccesses = 0;
    std::uint16_t coordinateErrorCode = 0;
    int coordinateSystemError = 0;
    float framesPerSecond = 0.0f;
    int screenWidth = 0;
    int screenHeight = 0;
    int gameVersionIndex = 0;
    int driverIndex = 0;
    RenderBackend activeRenderBackend = RenderBackend::Cpu;
    std::vector<std::string> driverOptions;
    std::string logText;
    std::string buildVersion;
};

struct VisualSettings {
    bool enabled = true;
    bool playerCount = true;
    bool antiFlicker = true;
    bool modelGeometry = false;
    bool visibilityColor = false;
    bool coordinateDecrypt = false;

    bool box = true;
    bool snapline = true;
    bool skeleton = true;
    bool distance = true;
    bool playerName = true;
    bool health = true;
    bool offscreenWarning = false;
    bool operatorName = false;
    bool heldWeapon = false;
    bool armorLevel = false;
    bool armorDurability = false;
    bool downedPlayer = false;
    bool filterBots = false;
    bool nearbyEnemy = false;
    bool battlefieldMode = false;
    bool combatMode = false;
    bool crosshair = false;
    bool throwableWarning = false;
    bool throwableTrajectory = false;
    bool aimWarning = false;
    bool aimWarningRay = false;
    bool playerViewRay = false;
    bool classNameDebug = false;

    int drawDistanceMeters = 500;
    float warningSize = 300.0f;
    float warningDistanceMeters = 200.0f;
    float skeletonDistanceMeters = 150.0f;
    float crosshairSize = 20.0f;
    float crosshairThickness = 2.0f;
    float lineThickness = 1.0f;
    float fontScale = 1.0f;
};

enum class ContainerKind : std::uint8_t {
    ComputerCase,
    LargeWeaponCrate,
    LargeToolbox,
    Clothing,
    SmallSafe,
    MissionTerminal,
    StorageBox,
    AmmoCrate,
    MountainPack,
    ReturnCapsule,
    FieldSupplyCrate,
    AviationStorage,
    Stairs,
    AdvancedStorage,
    MedicalSupplyPile,
    Computer,
    PremiumSuitcase,
    Safe,
    Vault,
    WeaponCrate,
    ParcelBox,
    Server,
    Suitcase,
    Locker,
    MedicalBag,
    Briefcase,
    Drawer,
    ToolCabinet,
    TravelBag,
    Stash,
    LabCoat,
    TrashCan,
    ContainerTruck,
    MixerTruck,
    CombinationLock,
    WireFence,
    BirdNest,
    PasswordDoor,
    ExtractionPoint,
    Switch,
    Recon,
    Count,
};

inline constexpr std::size_t kContainerKindCount =
    static_cast<std::size_t>(ContainerKind::Count);

struct LootSettings {
    bool enabled = false;
    bool playerBox = false;
    bool botBox = false;
    bool password = false;
    bool containers = false;
    bool boxContents = false;
    bool containerContents = false;
    bool highValueList = false;
    std::array<bool, kContainerKindCount> containerKinds{};

    int itemDistanceMeters = 200;
    int minimumItemValue = 10000;
    int minimumItemRarity = 1;
    int containerDistanceMeters = 200;
    int minimumContainerValue = 10000;
    int minimumContainerRarity = 1;
    int listLimit = 10;
    int minimumListValue = 50000;
    int minimumListRarity = 1;
    std::size_t customItemCount = 0;
};

struct RadarSettings {
    bool overlay = false;
    bool miniMap = false;
    bool bigMap = false;
    bool showSelf = false;
    bool miniMapBots = false;
    bool bigMapBots = false;

    float overlayX = 200.0f;
    float overlayY = 200.0f;
    float overlaySize = 250.0f;
    float overlayRangeMeters = 200.0f;
    float mapOffsetX = 0.0f;
    float mapOffsetY = 0.0f;
    float mapFontSize = 20.0f;
    float mapDotSize = 6.0f;
};

struct AimTuning {
    float rangePixels = 150.0f;
    float hipDistanceMeters = 50.0f;
    float adsDistanceMeters = 150.0f;
    float hipSpeed = 30.0f;
    float adsSpeed = 30.0f;
    float horizontalSpeed = 70.0f;
    float verticalSpeed = 70.0f;
    float prediction = 1.0f;
    float recoil = 0.1f;
    float smoothing = 20.0f;
    int hipBone = 0;
    int adsBone = 0;
};

inline constexpr std::size_t kWeaponProfileCount = 9;
inline constexpr std::size_t kRandomBoneCount = 9;

struct AimSettings {
    bool enabled = false;
    bool missMode = false;
    int coverMode = 0;
    bool weaponProfilesEnabled = false;
    int weaponProfileIndex = 8;
    std::array<AimTuning, kWeaponProfileCount> weaponProfiles{};
    AimTuning defaults{};

    bool ignoreBots = false;
    bool ignoreDowned = false;
    bool persistentLock = false;
    bool curvedMotion = false;
    bool trajectoryTracking = false;
    bool requireVisibility = true;
    bool rejectTargetState = false;
    bool rejectDeadTarget = false;
    bool playerDeadBox = true;
    bool robotDeadBox = false;
    bool enforceFov = true;
    bool enforceDistance = true;
    int hitPercentage = 100;
    bool drawRange = false;
    bool drawTargetRay = false;
    int triggerMode = 0;
    int targetAlgorithm = 0;
    std::array<float, kRandomBoneCount> randomBoneWeights{
        30.0f, 30.0f, 15.0f, 10.0f, 5.0f, 2.0f, 5.0f, 2.0f, 1.0f};

    AimInputMode inputMode = AimInputMode::ReadOnly;
    bool showTouchArea = false;
    float touchRange = 300.0f;
    float touchX = 1450.0f;
    float touchY = 380.0f;
};

struct SystemSettings {
    int frameLimitIndex = 3;
    RenderBackend renderBackend = RenderBackend::Cpu;
    bool autoScrollLogs = true;
    bool toastNotifications = true;
};

struct UiModel {
    bool visible = true;
    Page page = Page::Runtime;
    RuntimeModel runtime;
    VisualSettings visual;
    LootSettings loot;
    RadarSettings radar;
    AimSettings aim;
    SystemSettings system;
};

}  // namespace lengjing::ui
