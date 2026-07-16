#include "game/GameBackend.h"
#include "game/aim/AimController.h"
#include "game/data/CustomItemCatalog.h"
#include "game/data/ItemCatalog.h"
#include "game/data/ThreatCatalog.h"
#include "game/native/CharacterPositionResolver.h"
#include "game/native/GeometryRuntime.h"
#include "game/native/HudMapProjection.h"
#include "game/native/MemoryTransport.h"
#include "game/native/PlayerTrackingPolicy.h"
#include "game/native/ProjectileSpeedReader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::game {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr std::int32_t kMaximumActorCount = 10000;
constexpr std::size_t kMaximumNameLength = 249;

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Rotation {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct CameraView {
    Vec3 location{};
    float padding = 0.0f;
    Rotation rotation{};
    float fieldOfView = 90.0f;
};
static_assert(sizeof(CameraView) == 32, "Camera view layout mismatch");

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform {
    Quaternion rotation{};
    Vec3 translation{};
    float translationPadding = 0.0f;
    Vec3 scale{1.0f, 1.0f, 1.0f};
    float scalePadding = 0.0f;
};
static_assert(sizeof(Transform) == 48, "Transform layout mismatch");

struct Matrix4 {
    float value[4][4]{};
};

struct ActorArrayHeader {
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    std::int32_t capacity = 0;
};
static_assert(sizeof(ActorArrayHeader) == 16, "Actor array layout mismatch");

struct VersionLayout {
    const char* processName = nullptr;
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
};

constexpr std::array<VersionLayout, 3> kVersionLayouts{{
    {"com.tencent.tmgp.dfm",
     0x1CDCB8C0ULL,
     0x1D0E8668ULL,
     {0x1C3C5368ULL, 0x1AF01C68ULL}},
    {"com.proxima.dfm",
     0x1D0F4800ULL,
     0x1D4115A8ULL,
     {0x1B1C0D68ULL, 0}},
    {"com.garena.game.df",
     0x1CF7A440ULL,
     0x1D2971F8ULL,
     {0x1B0669A8ULL, 0}},
}};

constexpr std::array<int, 15> kBoneIndices{
    31, 30, 1, 34, 6, 35, 7, 36, 8, 58, 62, 59, 63, 60, 64};

constexpr std::array<BoneLink, 14> kBoneLinks{{
    {0, 1},
    {1, 2},
    {1, 3},
    {3, 5},
    {5, 7},
    {1, 4},
    {4, 6},
    {6, 8},
    {2, 9},
    {9, 11},
    {11, 13},
    {2, 10},
    {10, 12},
    {12, 14},
}};

constexpr std::array<std::uint64_t, 8> kUnsupportedAimWeapons{
    900130826ULL,
    900130823ULL,
    900130827ULL,
    890130846ULL,
    950130817ULL,
    950130818ULL,
    900130825ULL,
    860130824ULL,
};

bool IsFinite(const Vec2& value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

native::GeometryPoint ToGeometryPoint(const Vec3& value) {
    return native::GeometryPoint{value.x, value.y, value.z};
}

SemanticTone ToneForVisibility(bool bot,
                               bool visibilityColor,
                               native::GeometryVisibility visibility) {
    if (!visibilityColor) {
        return bot ? SemanticTone::Caution : SemanticTone::Danger;
    }
    switch (visibility) {
        case native::GeometryVisibility::Visible:
            return SemanticTone::Danger;
        case native::GeometryVisibility::Occluded:
            return SemanticTone::Accent;
        case native::GeometryVisibility::Unavailable:
        default:
            return SemanticTone::Muted;
    }
}

bool IsFinite(const CameraView& view) {
    return IsFinite(view.location) && std::isfinite(view.rotation.pitch) &&
           std::isfinite(view.rotation.yaw) && std::isfinite(view.rotation.roll) &&
           std::isfinite(view.fieldOfView) && view.fieldOfView >= 5.0f &&
           view.fieldOfView <= 170.0f;
}

bool IsNonzero(const Vec3& value) {
    return value.x != 0.0f || value.y != 0.0f || value.z != 0.0f;
}

bool IsValidPointer(std::uintptr_t address) {
    return address >= kMinimumRemoteAddress && address < kMaximumRemoteAddress &&
           (address & 0x3ULL) == 0;
}

bool IsValidReadAddress(std::uintptr_t address) {
    return address >= kMinimumRemoteAddress && address < kMaximumRemoteAddress;
}

float Dot(const Vec3& left, const Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 Subtract(const Vec3& left, const Vec3& right) {
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

float Length(const Vec3& value) {
    return std::sqrt(Dot(value, value));
}

float HorizontalLength(const Vec3& value) {
    return std::hypot(value.x, value.y);
}

bool IsAimingAtPosition(const Vec3& actorPosition,
                        const Vec3& actorForward,
                        const Vec3& targetPosition) {
    constexpr float kHorizontalGateDegrees = 10.0f;
    constexpr float kVerticalGateDegrees = 40.0f;
    constexpr float kEpsilon = 0.0001f;
    if (!IsFinite(actorPosition) || !IsFinite(actorForward) ||
        !IsFinite(targetPosition)) {
        return false;
    }

    const Vec3 delta = Subtract(targetPosition, actorPosition);
    const float forwardLength = HorizontalLength(actorForward);
    const float horizontalDistance = HorizontalLength(delta);
    if (forwardLength <= kEpsilon || horizontalDistance <= kEpsilon) return false;

    const float cosine =
        (actorForward.x * delta.x + actorForward.y * delta.y) /
        (forwardLength * horizontalDistance);
    const float gateCosine = std::cos(kHorizontalGateDegrees * kPi / 180.0f);
    if (!std::isfinite(cosine) || cosine < gateCosine) return false;

    const float verticalAngle =
        std::atan2(std::fabs(delta.z), horizontalDistance) * 180.0f / kPi;
    return std::isfinite(verticalAngle) && verticalAngle <= kVerticalGateDegrees;
}

void AppendDetail(std::string& destination, std::string_view value) {
    if (value.empty()) return;
    if (!destination.empty()) destination += " | ";
    destination.append(value.data(), value.size());
}

bool WeaponAllowsAim(std::uint64_t weaponId) {
    return weaponId != 0 &&
        std::find(kUnsupportedAimWeapons.begin(), kUnsupportedAimWeapons.end(), weaponId) ==
            kUnsupportedAimWeapons.end();
}

std::size_t WeaponProfileFor(std::uint64_t weaponId) {
    const auto inRange = [weaponId](std::uint64_t first, std::uint64_t last) {
        return weaponId >= first && weaponId <= last;
    };
    if (inRange(18010000001ULL, 18010000100ULL) ||
        inRange(830000000ULL, 839999999ULL)) return 0;
    if (inRange(18020000001ULL, 18020000100ULL) ||
        inRange(840000000ULL, 849999999ULL)) return 1;
    if (inRange(18030000001ULL, 18030000100ULL) ||
        inRange(850000000ULL, 859999999ULL)) return 2;
    if (inRange(18040000001ULL, 18040000100ULL) ||
        inRange(860000000ULL, 869999999ULL)) return 3;
    if (inRange(18050000001ULL, 18050000100ULL) ||
        inRange(870000000ULL, 879999999ULL)) return 4;
    if (inRange(18060000001ULL, 18060000100ULL) ||
        inRange(880000000ULL, 889999999ULL)) return 5;
    if (inRange(18070000001ULL, 18070000100ULL) ||
        inRange(890000000ULL, 899999999ULL)) return 6;
    if (inRange(18150000001ULL, 18150000100ULL) ||
        inRange(970000000ULL, 979999999ULL)) return 7;
    return ui::kWeaponProfileCount - 1;
}

float ProjectileSpeedFor(std::uint64_t weaponId) {
    switch (WeaponProfileFor(weaponId)) {
        case 0: return 88000.0f;
        case 1: return 45000.0f;
        case 2: return 43000.0f;
        case 3: return 85000.0f;
        case 4: return 83500.0f;
        case 5: return 88000.0f;
        case 6: return 37500.0f;
        case 7: return 65000.0f;
        default: return 88000.0f;
    }
}

int EquipmentLevel(std::int32_t definitionId) {
    const std::string digits = std::to_string(
        definitionId < 0 ? -static_cast<std::int64_t>(definitionId) : definitionId);
    if (digits.size() < 7) return 0;
    const std::string_view code(digits.data() + 4, 3);
    if (code == "900") return 1;
    if (code == "899") return 2;
    if (code == "898") return 3;
    if (code == "897") return 4;
    if (code == "896") return 5;
    if (code == "895") return 6;
    return 0;
}

const char* OperatorName(std::int32_t id) {
    switch (id) {
        case 2100654110: return "红狼";
        case 2100654105: return "威龙";
        case 2100654106: return "骇爪";
        case 2100654107: return "蜂医";
        case 2100654109: return "牧羊人";
        case 2100654108: return "露娜";
        case 2100654115: return "乌鲁鲁";
        case 2100654116: return "蛊";
        case 2100654117: return "深蓝";
        case 2100654118: return "无名";
        case 2100654119: return "疾风";
        case 2100654120: return "银翼";
        case 2100654121: return "比特";
        case 2100654122: return "赤枭";
        case 2100654123:
        case 2100654124: return "赤枭小弟";
        case 2100654125: return "蝶";
        case 2100654126: return "回响";
        case 2100654127: return "液氮";
        default: return nullptr;
    }
}

Matrix4 TransformToMatrix(const Transform& transform) {
    Matrix4 matrix{};
    const float x2 = transform.rotation.x + transform.rotation.x;
    const float y2 = transform.rotation.y + transform.rotation.y;
    const float z2 = transform.rotation.z + transform.rotation.z;
    const float xx2 = transform.rotation.x * x2;
    const float yy2 = transform.rotation.y * y2;
    const float zz2 = transform.rotation.z * z2;
    const float yz2 = transform.rotation.y * z2;
    const float wx2 = transform.rotation.w * x2;
    const float xy2 = transform.rotation.x * y2;
    const float wz2 = transform.rotation.w * z2;
    const float xz2 = transform.rotation.x * z2;
    const float wy2 = transform.rotation.w * y2;

    matrix.value[0][0] = (1.0f - (yy2 + zz2)) * transform.scale.x;
    matrix.value[0][1] = (xy2 + wz2) * transform.scale.x;
    matrix.value[0][2] = (xz2 - wy2) * transform.scale.x;
    matrix.value[1][0] = (xy2 - wz2) * transform.scale.y;
    matrix.value[1][1] = (1.0f - (xx2 + zz2)) * transform.scale.y;
    matrix.value[1][2] = (yz2 + wx2) * transform.scale.y;
    matrix.value[2][0] = (xz2 + wy2) * transform.scale.z;
    matrix.value[2][1] = (yz2 - wx2) * transform.scale.z;
    matrix.value[2][2] = (1.0f - (xx2 + yy2)) * transform.scale.z;
    matrix.value[3][0] = transform.translation.x;
    matrix.value[3][1] = transform.translation.y;
    matrix.value[3][2] = transform.translation.z;
    matrix.value[3][3] = 1.0f;
    return matrix;
}

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
    Matrix4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int index = 0; index < 4; ++index) {
                result.value[row][column] +=
                    left.value[row][index] * right.value[index][column];
            }
        }
    }
    return result;
}

Vec3 MatrixTranslation(const Matrix4& matrix) {
    return Vec3{
        matrix.value[3][0],
        matrix.value[3][1],
        matrix.value[3][2],
    };
}

void CameraAxes(const CameraView& view, Vec3& forward, Vec3& right, Vec3& up) {
    const float pitch = view.rotation.pitch * kPi / 180.0f;
    const float yaw = view.rotation.yaw * kPi / 180.0f;
    const float roll = view.rotation.roll * kPi / 180.0f;
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);

    forward = Vec3{cosPitch * cosYaw, cosPitch * sinYaw, sinPitch};
    right = Vec3{
        sinRoll * sinPitch * cosYaw - cosRoll * sinYaw,
        sinRoll * sinPitch * sinYaw + cosRoll * cosYaw,
        -sinRoll * cosPitch,
    };
    up = Vec3{
        -(cosRoll * sinPitch * cosYaw + sinRoll * sinYaw),
        cosYaw * sinRoll - cosRoll * sinPitch * sinYaw,
        cosRoll * cosPitch,
    };
}

struct CameraPoint {
    float side = 0.0f;
    float vertical = 0.0f;
    float forward = 0.0f;
};

CameraPoint ToCameraSpace(const Vec3& world, const CameraView& view) {
    Vec3 forward{};
    Vec3 right{};
    Vec3 up{};
    CameraAxes(view, forward, right, up);
    const Vec3 delta = Subtract(world, view.location);
    return CameraPoint{Dot(delta, right), Dot(delta, up), Dot(delta, forward)};
}

bool ProjectToScreen(const Vec3& world,
                     const CameraView& view,
                     int screenWidth,
                     int screenHeight,
                     Vec2& screen,
                     CameraPoint* cameraPoint = nullptr) {
    if (!IsFinite(world) || !IsFinite(view) || screenWidth <= 1 || screenHeight <= 1) {
        return false;
    }
    const CameraPoint point = ToCameraSpace(world, view);
    if (cameraPoint != nullptr) *cameraPoint = point;
    if (!std::isfinite(point.side) || !std::isfinite(point.vertical) ||
        !std::isfinite(point.forward) || point.forward <= 0.01f) {
        return false;
    }
    const float tangent = std::tan(view.fieldOfView * kPi / 360.0f);
    if (!std::isfinite(tangent) || tangent <= 0.001f) return false;
    const float halfWidth = static_cast<float>(screenWidth) * 0.5f;
    const float halfHeight = static_cast<float>(screenHeight) * 0.5f;
    const float scale = halfWidth / tangent;
    screen.x = halfWidth + point.side * scale / point.forward;
    screen.y = halfHeight - point.vertical * scale / point.forward;
    return IsFinite(screen);
}

bool IsInsideScreen(const Vec2& point, int width, int height, float margin = 0.0f) {
    return IsFinite(point) && point.x >= -margin && point.y >= -margin &&
           point.x <= static_cast<float>(width) + margin &&
           point.y <= static_cast<float>(height) + margin;
}

std::string FormatDistance(float distanceMeters) {
    if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) return {};
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0fm", distanceMeters);
    return buffer;
}

bool IsCharacterClass(std::string_view name) {
    if (name == "NC_BP_DFMCharacter_C" ||
        name == "NC_BP_DFMCharacter_TutorialPlayerAi_C" ||
        name == "BP_RangeTargetCharacter_C") {
        return true;
    }
    return name.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
           name.rfind("NC_BP_DFMAICharacter", 0) == 0 ||
           name.find("DFMCharacter") != std::string_view::npos;
}

bool IsBotClass(std::string_view name) {
    return name.find("AI") != std::string_view::npos ||
           name.find("RangeTargetCharacter") != std::string_view::npos;
}

bool IsPickupClass(std::string_view name) {
    return name.find("Pickup_C") != std::string_view::npos ||
        name.find("PickUp_") != std::string_view::npos ||
        name.find("InventoryPickup") != std::string_view::npos ||
        name.find("InvntoryPickup") != std::string_view::npos;
}

bool IsDeadBodyClass(std::string_view name) {
    return name.find("BP_Inventory_DeadBody_C") != std::string_view::npos;
}

bool IsExtractionClass(std::string_view name) {
    return name.find("BP_FXProxy_Exit_C") != std::string_view::npos;
}

SemanticTone ToneForRarity(int rarity) {
    if (rarity >= 6) return SemanticTone::Danger;
    if (rarity >= 5) return SemanticTone::Caution;
    if (rarity >= 4) return SemanticTone::Accent;
    if (rarity >= 3) return SemanticTone::Ally;
    return rarity >= 2 ? SemanticTone::Accent : SemanticTone::Neutral;
}

bool IsKnownContainerId(std::int32_t id) {
    switch (id) {
        case 902: case 905: case 906: case 909: case 913: case 914:
        case 916: case 920: case 926: case 927: case 930: case 933:
        case 934: case 936: case 937: case 938: case 940: case 943:
        case 945: case 946: case 947: case 948: case 950: case 956:
        case 957: case 962:
            return true;
        default:
            return false;
    }
}

const char* ContainerName(std::int32_t id) {
    switch (id) {
        case 902: case 945: return "快递箱";
        case 905: return "医疗包";
        case 909: return "电脑机箱";
        case 913: return "垃圾箱";
        case 914: return "抽屉柜";
        case 916: case 947: return "衣物";
        case 920: return "保险箱";
        case 926: return "大型武器箱";
        case 927: return "旅行包";
        case 930: return "鸟窝";
        case 934: return "高级旅行箱";
        case 936: return "工具柜";
        case 937: return "服务器";
        case 938: return "手提箱";
        case 940: return "医疗物资堆";
        case 943: return "航空储物箱";
        case 946: return "藏匿物";
        case 948: return "野外物资箱";
        case 950: return "小保险箱";
        case 957: return "高级储物箱";
        default: return "容器";
    }
}

std::optional<ui::ContainerKind> ContainerKindForId(std::int32_t id) {
    using Kind = ui::ContainerKind;
    switch (id) {
        case 902: case 945: return Kind::ParcelBox;
        case 905: return Kind::MedicalBag;
        case 909: return Kind::ComputerCase;
        case 913: return Kind::TrashCan;
        case 914: return Kind::Drawer;
        case 916: case 947: return Kind::Clothing;
        case 920: return Kind::Safe;
        case 926: return Kind::LargeWeaponCrate;
        case 927: return Kind::TravelBag;
        case 930: return Kind::BirdNest;
        case 934: return Kind::PremiumSuitcase;
        case 936: return Kind::ToolCabinet;
        case 937: return Kind::Server;
        case 938: return Kind::Suitcase;
        case 940: return Kind::MedicalSupplyPile;
        case 943: return Kind::AviationStorage;
        case 946: return Kind::Stash;
        case 948: return Kind::FieldSupplyCrate;
        case 950: return Kind::SmallSafe;
        case 957: return Kind::AdvancedStorage;
        default: return std::nullopt;
    }
}

std::string Utf16ToUtf8(const std::u16string& value) {
    std::string output;
    output.reserve(value.size() * 3);
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t codePoint = value[index];
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU && index + 1 < value.size()) {
            const std::uint32_t low = value[index + 1];
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
                ++index;
            }
        }
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }
    return output;
}

}  // namespace

class NativeGameBackend final : public GameBackend {
public:
    ~NativeGameBackend() override {
        Close();
    }

    bool Open(const RuntimeOptions& options,
              RuntimeProbe& probe,
              std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseLocked();
        probe = RuntimeProbe{};
        error.clear();

        const int inputMode = static_cast<int>(options.inputMode);
        if (options.gameVersionIndex < 0 ||
            options.gameVersionIndex >= static_cast<int>(kVersionLayouts.size()) ||
            options.driverIndex < 0 || options.driverIndex >= 2 ||
            inputMode < static_cast<int>(ui::AimInputMode::WriteTouch) ||
            inputMode > static_cast<int>(ui::AimInputMode::KernelGyroscope) ||
            options.screenWidth <= 1 || options.screenHeight <= 1) {
            error = "运行参数无效";
            return false;
        }
        options_ = options;
        customItemPath_ = options.programDirectory.empty()
            ? std::string("自定义物资.txt")
            : options.programDirectory + "/自定义物资.txt";
        customItems_.Load(customItemPath_);
        layout_ = kVersionLayouts[static_cast<std::size_t>(options.gameVersionIndex)];
        processId_ = native::FindProcessId(layout_.processName);
        if (processId_ <= 0) {
            error = "未找到目标游戏进程";
            CloseLocked();
            return false;
        }

        memory_ = std::make_unique<native::MemoryTransport>();
        if (!memory_->Open(options.driverIndex, processId_, layout_.processName, error)) {
            CloseLocked();
            return false;
        }

        moduleBase_ = memory_->ModuleBase("libUE4.so");
        if (!IsValidPointer(moduleBase_)) {
            moduleBase_ = native::FindMappedModuleBase(processId_, "libUE4.so");
        }
        if (!IsValidPointer(moduleBase_)) {
            error = "无法定位游戏模块";
            CloseLocked();
            return false;
        }

        std::array<std::uint8_t, 4> signature{};
        if (!memory_->Read(moduleBase_, signature.data(), signature.size()) ||
            signature[0] != 0x7FU || signature[1] != 'E' ||
            signature[2] != 'L' || signature[3] != 'F') {
            error = "所选内存通道无法读取游戏模块";
            CloseLocked();
            return false;
        }

        if (!aimController_.Start(options.inputMode)) {
            error = "输入通道初始化失败";
            CloseLocked();
            return false;
        }
        opened_ = true;
        aimController_.SetEnabled(aimEnabled_.load(std::memory_order_acquire));
        probe.processId = processId_;
        probe.baseReady = true;
        probe.customItemCount = customItems_.Size();
        return true;
    }

    void Close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseLocked();
    }

    bool ReadFrame(const FeatureSettings& settings,
                   GameFrame& frame,
                   RuntimeProbe& probe,
                   std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::uint64_t sequence = frame.sequence;
        frame = GameFrame{};
        frame.sequence = sequence;
        error.clear();

        if (!opened_ || memory_ == nullptr || !memory_->IsOpen()) {
            error = "游戏后端尚未打开";
            return false;
        }
        probe.processId = processId_;
        probe.baseReady = IsValidPointer(moduleBase_);
        probe.customItemCount = customItems_.Size();
        if (!native::IsProcessAlive(processId_)) {
            probe = RuntimeProbe{};
            error = "目标游戏进程已结束";
            return false;
        }
        UpdateGeometryRuntime(settings);

        native::PositionReadMode positionMode =
            native::PositionReadMode::Standard;
        if (settings.visual.algorithmDecrypt) {
            positionMode = native::PositionReadMode::Indexed;
        } else if (settings.visual.coordinateDecrypt) {
            positionMode = native::PositionReadMode::Direct;
        }
        if (positionMode != positionReadMode_) {
            characterPositions_.Clear();
            positionReadMode_ = positionMode;
        }

        FrameContext context{};
        if (!BuildFrameContext(
                context,
                settings.visual.battlefieldMode,
                positionMode,
                settings.visual.antiFlicker,
                error)) {
            ResetWorldState();
            return true;
        }

        if (world_ != context.world) {
            ResetWorldState();
            world_ = context.world;
            RequestGeometryRefresh();
        }
        if (settings.visual.debugInfo) {
            const std::shared_ptr<const native::GeometrySnapshot> snapshot =
                geometryRuntime_.GetSnapshot();
            frame.geometryAvailable = geometrySnapshotReady_ &&
                snapshot != nullptr && snapshot->available;
            if (frame.geometryAvailable) {
                frame.geometryMeshCount =
                    snapshot->staticMeshes.size() +
                    snapshot->dynamicMeshes.size();
                frame.geometryTriangleCount = snapshot->triangleCount;
                frame.geometryGeneration = snapshot->generation;
            }
        }

        if (!settings.visual.antiFlicker) boneCache_.clear();

        const bool aimWarningEnabled = settings.visual.enabled &&
            (settings.visual.aimWarning || settings.visual.aimWarningRay) &&
            !(context.warfare || settings.visual.battlefieldMode);
        if (!aimWarningEnabled) aimWarningStates_.clear();
        const auto warningTime = std::chrono::steady_clock::now();

        if (settings.visual.crosshair && settings.visual.enabled) {
            CrosshairVisual crosshair{};
            crosshair.center = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                static_cast<float>(options_.screenHeight) * 0.5f);
            crosshair.gap = std::max(2.0f, settings.visual.crosshairSize * 0.25f);
            crosshair.armLength = std::max(3.0f, settings.visual.crosshairSize * 0.5f);
            crosshair.thickness = settings.visual.crosshairThickness;
            crosshair.tone = SemanticTone::Accent;
            frame.crosshair = crosshair;
        }

        if (settings.radar.overlay) {
            RadarVisual radar{};
            const float size = std::clamp(settings.radar.overlaySize, 90.0f, 800.0f);
            radar.center = ImVec2(
                settings.radar.overlayX + size * 0.5f,
                settings.radar.overlayY + size * 0.5f);
            radar.radius = size * 0.5f;
            radar.maxDistanceMeters = std::max(1.0f, settings.radar.overlayRangeMeters);
            radar.viewHeadingRadians = context.view.rotation.yaw * kPi / 180.0f;
            radar.showSelf = settings.radar.showSelf;
            frame.radar = std::move(radar);
        }

        bool hudBigMapActive = false;
        bool hudMiniMapActive = false;
        if (settings.radar.miniMap || settings.radar.bigMap) {
            auto readBytes = [this](std::uintptr_t address,
                                    void* destination,
                                    std::size_t size) {
                return memory_ != nullptr && IsValidReadAddress(address) &&
                    size <= kMaximumRemoteAddress - address &&
                    memory_->Read(address, destination, size);
            };
            auto resolveName = [this, &context](std::uint32_t index) {
                return DecodeName(static_cast<std::int32_t>(index), context.namePool);
            };
            native::HudMapReader::Refresh(
                context.localController, hudMapCache_, readBytes, resolveName);
            hudBigMapActive = settings.radar.bigMap &&
                hudMapCache_.bigMapWidget != 0 && hudMapCache_.bigMapVisible;
            hudMiniMapActive = settings.radar.miniMap &&
                hudMapCache_.miniMapWidget != 0 && !hudMapCache_.bigMapVisible;
            if (hudBigMapActive || hudMiniMapActive) {
                HudMapVisual map{};
                map.markerSize = std::clamp(settings.radar.mapDotSize, 0.0f, 80.0f);
                map.fontSize = std::clamp(settings.radar.mapFontSize, 0.0f, 80.0f);
                if (settings.radar.showSelf) {
                    const float heading = context.view.rotation.yaw * kPi / 180.0f;
                    native::HudMapProjection projected{};
                    if (hudBigMapActive) {
                        projected = native::ProjectBigMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            heading,
                            static_cast<float>(options_.screenWidth),
                            static_cast<float>(options_.screenHeight),
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            map.markerSize);
                    } else if (hudMiniMapActive) {
                        projected = native::ProjectMiniMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            heading,
                            context.mapBuildId >= 1900 && context.mapBuildId <= 2000,
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            map.markerSize);
                    }
                    if (projected.visible) {
                        HudMapMarker marker{};
                        marker.position = ImVec2(projected.marker.x, projected.marker.y);
                        marker.directionEnd = ImVec2(
                            projected.directionEnd.x, projected.directionEnd.y);
                        marker.label = "自";
                        marker.kind = RadarBlipKind::Self;
                        marker.tone = SemanticTone::Accent;
                        marker.drawDirection = true;
                        map.markers.push_back(std::move(marker));
                    }
                }
                frame.hudMap = std::move(map);
            }
        } else {
            hudMapCache_.Clear();
        }

        const bool aimActive = settings.aim.enabled &&
            aimEnabled_.load(std::memory_order_acquire) &&
            WeaponAllowsAim(context.weaponId);
        const ui::AimTuning aimTuning = ResolveAimTuning(
            settings.aim, context.weaponId);
        std::vector<AimCandidate> aimCandidates;
        if (aimActive) aimCandidates.reserve(64);
        if (settings.visual.enabled && settings.visual.modelGeometry) {
            BuildModelGeometry(context.view, frame);
        }

        const bool scanWorldObjects = settings.loot.enabled ||
            settings.loot.playerBox || settings.loot.botBox ||
            settings.loot.password || settings.loot.containers ||
            (settings.visual.enabled &&
             (settings.visual.throwableWarning ||
              settings.visual.throwableTrajectory));
        std::vector<std::uintptr_t> actorAddresses =
            CollectActorAddresses(context, scanWorldObjects);
        std::unordered_set<std::uintptr_t> seenThreats;
        if (actorAddresses.empty()) {
            error = "数据链等待：人物列表暂不可读";
            FinalizeThreatSignals(settings, frame);
            PublishAimFrame(settings.aim, aimTuning, context, aimCandidates, frame);
            PruneThreatState(seenThreats);
            const auto pruneTime = std::chrono::steady_clock::now();
            PruneBoneCache(pruneTime);
            PrunePositionCache(pruneTime);
            return true;
        }

        frame.players.reserve(std::min<std::size_t>(actorAddresses.size(), 256));
        for (const std::uintptr_t actor : actorAddresses) {
            if (!IsValidPointer(actor) || actor == context.localPawn) continue;

            std::int32_t nameIndex = -1;
            if (!ReadValue(actor + 0x1C, nameIndex) || nameIndex < 0) continue;
            const std::string className = DecodeName(nameIndex, context.namePool);
            bool character = IsCharacterClass(className);
            if (!character) {
                float compatibilityMarker = 0.0f;
                character = ReadValue(actor + 0x47C, compatibilityMarker) &&
                            compatibilityMarker == 40.0f;
            }
            if (!character) {
                ProcessWorldActor(
                    actor, className, context, settings, frame, seenThreats);
                continue;
            }

            const bool botClass = IsBotClass(className);
            const std::uintptr_t playerState = ReadPointer(actor + 0x390);
            const bool playerStateAvailable = IsValidPointer(playerState);
            if (!native::HasUsablePlayerState(playerStateAvailable, botClass)) {
                continue;
            }
            std::int32_t primaryTeam = native::kUnknownPlayerTeam;
            std::int32_t secondaryTeam = native::kUnknownPlayerTeam;
            const bool primaryTeamRead = playerStateAvailable &&
                ReadValue(playerState + 0x658, primaryTeam);
            const bool secondaryTeamRead = playerStateAvailable &&
                ReadValue(playerState + 0x65C, secondaryTeam);
            primaryTeam = native::ResolvePlayerTeam(
                primaryTeamRead, primaryTeam, botClass);
            secondaryTeam = native::ResolvePlayerTeam(
                secondaryTeamRead, secondaryTeam, botClass);
            const bool bot = botClass || primaryTeam == 0;
            const bool useSecondaryTeam = context.warfare || settings.visual.battlefieldMode;
            const std::int32_t targetTeam = useSecondaryTeam ? secondaryTeam : primaryTeam;
            const bool threatTeamsValid =
                context.localTeam >= 0 && targetTeam >= 0;
            if (context.localTeam >= 0 && targetTeam >= 0 &&
                targetTeam == context.localTeam) {
                continue;
            }

            Vec3 position{};
            const bool coordinateAvailable = ReadCharacterPosition(
                actor,
                className,
                positionMode,
                settings.visual.antiFlicker,
                position);
            HealthState health{};
            const bool healthAvailable =
                ReadHealth(actor, playerState, health);
            if (!native::IsPlayerTrackable(
                    native::PlayerTrackingData{
                        coordinateAvailable,
                        healthAvailable,
                        healthAvailable &&
                            (health.health > 0.0f || health.downed),
                    },
                    native::PlayerDirectionData{false})) {
                continue;
            }
            const Vec3 delta = Subtract(position, context.localPosition);
            const float distanceMeters = Length(delta) * 0.01f;
            const float horizontalDistanceMeters = HorizontalLength(delta) * 0.01f;
            if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) continue;
            if (!std::isfinite(horizontalDistanceMeters) ||
                horizontalDistanceMeters < 0.0f) {
                continue;
            }

            const bool combatModeActive = settings.visual.combatMode &&
                (context.firing || context.zooming);
            const bool aimEligible = aimActive && threatTeamsValid &&
                !(bot && settings.aim.ignoreBots) &&
                !(health.downed && settings.aim.ignoreDowned);
            const bool visualEligible = !health.downed || settings.visual.downedPlayer;
            if (!visualEligible && !aimEligible) continue;
            if (bot) ++frame.botCount;
            else ++frame.playerCount;
            if (settings.visual.enabled && settings.visual.nearbyEnemy &&
                threatTeamsValid && !bot &&
                health.health > 0.0f && horizontalDistanceMeters <= 50.0f) {
                ++frame.nearbyEnemyCount;
            }

            native::GeometryVisibility actorVisibility =
                native::GeometryVisibility::Unavailable;
            if (geometrySnapshotReady_ && settings.visual.enabled &&
                (settings.visual.visibilityColor ||
                 aimWarningEnabled)) {
                Vec3 visibilityTarget = position;
                visibilityTarget.z += 90.0f;
                actorVisibility = TraceGeometry(
                    context.view.location, visibilityTarget);
            }
            const SemanticTone actorTone = ToneForVisibility(
                bot, settings.visual.visibilityColor, actorVisibility);

            const bool radarInRange = threatTeamsValid && frame.radar.has_value() &&
                distanceMeters <= frame.radar->maxDistanceMeters;
            const bool hudMapEligible = threatTeamsValid && frame.hudMap.has_value();
            const bool facingNeeded = radarInRange || hudMapEligible ||
                (settings.visual.enabled && !bot &&
                 threatTeamsValid &&
                 (settings.visual.playerViewRay || aimWarningEnabled));
            Vec3 actorForward{};
            float actorHeadingRadians = 0.0f;
            const bool actorFacingValid = facingNeeded &&
                ReadActorFacing(actor, actorForward, actorHeadingRadians);
            if (radarInRange && !(bot && settings.visual.filterBots)) {
                AddRadarBlip(
                    context,
                    position,
                    bot,
                    className,
                    actorHeadingRadians,
                    actorFacingValid,
                    actorTone,
                    *frame.radar);
            }
            if (hudMapEligible) {
                const bool botAllowed = !bot ||
                    (hudBigMapActive
                        ? settings.radar.bigMapBots
                        : settings.radar.miniMapBots);
                if (botAllowed) {
                    const float heading = actorFacingValid
                        ? actorHeadingRadians
                        : 0.0f;
                    native::HudMapProjection projected{};
                    if (hudBigMapActive) {
                        projected = native::ProjectBigMap(
                            hudMapCache_,
                            position.x,
                            position.y,
                            heading,
                            static_cast<float>(options_.screenWidth),
                            static_cast<float>(options_.screenHeight),
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            frame.hudMap->markerSize);
                    } else if (hudMiniMapActive) {
                        projected = native::ProjectMiniMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            position.x,
                            position.y,
                            position.z,
                            heading,
                            context.mapBuildId >= 1900 && context.mapBuildId <= 2000,
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            frame.hudMap->markerSize);
                    }
                    if (projected.visible) {
                        HudMapMarker marker{};
                        marker.position = ImVec2(projected.marker.x, projected.marker.y);
                        marker.directionEnd = ImVec2(
                            projected.directionEnd.x, projected.directionEnd.y);
                        marker.kind = bot
                            ? RadarBlipKind::Bot
                            : RadarBlipKind::Player;
                        marker.tone = actorTone;
                        if (frame.hudMap->fontSize > 0.0f) {
                            marker.label = bot ? "AI" : ReadPlayerName(playerState);
                        }
                        marker.drawDirection = !bot && actorFacingValid;
                        frame.hudMap->markers.push_back(std::move(marker));
                    }
                }
            }

            const bool drawingInRange = settings.visual.drawDistanceMeters <= 0 ||
                distanceMeters <= static_cast<float>(settings.visual.drawDistanceMeters);
            const bool warningInRange = threatTeamsValid &&
                settings.visual.offscreenWarning &&
                settings.visual.warningSize > 0.0f &&
                horizontalDistanceMeters <= settings.visual.warningDistanceMeters;
            if (!drawingInRange && !warningInRange && !radarInRange && !aimEligible) {
                continue;
            }

            Vec2 bodyBottom{};
            Vec2 bodyTop{};
            CameraPoint cameraPoint{};
            const bool bottomProjected = ProjectToScreen(
                Vec3{position.x, position.y, position.z - 5.0f},
                context.view,
                options_.screenWidth,
                options_.screenHeight,
                bodyBottom,
                &cameraPoint);
            const bool topProjected = ProjectToScreen(
                Vec3{position.x, position.y, position.z + 205.0f},
                context.view,
                options_.screenWidth,
                options_.screenHeight,
                bodyTop);
            const bool onScreen = bottomProjected && topProjected &&
                (IsInsideScreen(bodyBottom, options_.screenWidth, options_.screenHeight) ||
                 IsInsideScreen(bodyTop, options_.screenWidth, options_.screenHeight));

            if (!onScreen && warningInRange && settings.visual.enabled &&
                !(bot && settings.visual.filterBots)) {
                OffscreenMarker marker{};
                CameraPoint directionPoint = ToCameraSpace(position, context.view);
                if (directionPoint.forward >= 0.0f) {
                    marker.direction = ImVec2(directionPoint.side, -directionPoint.vertical);
                } else {
                    marker.direction = ImVec2(-directionPoint.side, directionPoint.vertical);
                }
                marker.radiusPixels = settings.visual.warningSize;
                marker.markerScale = std::clamp(
                    settings.visual.warningSize / 300.0f, 0.35f, 2.5f);
                marker.distanceMeters = distanceMeters;
                marker.tone = bot ? SemanticTone::Caution : SemanticTone::Danger;
                frame.offscreenMarkers.push_back(std::move(marker));
            }

            bool aimWarningActive = false;
            if (aimWarningEnabled && threatTeamsValid && !bot &&
                health.health > 0.0f &&
                drawingInRange && onScreen) {
                aimWarningActive = ObserveAimWarning(
                    actor,
                    actorFacingValid && IsAimingAtPosition(
                        position, actorForward, context.localPosition) &&
                        actorVisibility != native::GeometryVisibility::Occluded,
                    frame.sequence,
                    warningTime);
                if (aimWarningActive && settings.visual.aimWarning) {
                    frame.aimWarningPlayers.push_back(ReadPlayerName(playerState));
                }
                if (aimWarningActive && settings.visual.aimWarningRay && bottomProjected) {
                    PlayerSignalVisual signal{};
                    signal.start = ImVec2(
                        static_cast<float>(options_.screenWidth) * 0.5f,
                        static_cast<float>(options_.screenHeight) - 3.0f);
                    signal.end = ImVec2(bodyBottom.x, bodyBottom.y);
                    signal.kind = PlayerSignalKind::AimWarning;
                    signal.tone = SemanticTone::Danger;
                    frame.playerSignals.push_back(std::move(signal));
                }
            }

            BoneFrame boneFrame{};
            bool boneFrameReady = false;
            if (aimEligible ||
                (settings.visual.enabled &&
                  (settings.visual.skeleton ||
                   settings.visual.visibilityColor ||
                   settings.visual.playerViewRay) &&
                  visualEligible && drawingInRange && onScreen)) {
                boneFrameReady = ReadBoneFrame(
                    actor, context.view, settings.visual.antiFlicker, boneFrame);
            }
            native::GeometryVisibility playerVisibility = actorVisibility;
            if (boneFrameReady &&
                (settings.visual.visibilityColor ||
                 (aimEligible && settings.aim.missMode))) {
                playerVisibility = EvaluateBoneVisibility(
                    context.view.location, boneFrame);
            }
            if (settings.visual.enabled && settings.visual.playerViewRay &&
                threatTeamsValid && !bot && health.health > 0.0f &&
                drawingInRange && onScreen && actorFacingValid) {
                const float forwardLength = Length(actorForward);
                if (std::isfinite(forwardLength) && forwardLength > 0.0001f) {
                    const Vec3 sightStart = boneFrameReady && boneFrame.valid[0]
                        ? boneFrame.world[0]
                        : Vec3{position.x, position.y, position.z + 165.0f};
                    const Vec3 direction{
                        actorForward.x / forwardLength,
                        actorForward.y / forwardLength,
                        actorForward.z / forwardLength,
                    };
                    Vec2 sightStartScreen{};
                    Vec2 sightEndScreen{};
                    bool endProjected = false;
                    for (const float distance : {3000.0f, 1000.0f, 300.0f}) {
                        const Vec3 sightEnd{
                            sightStart.x + direction.x * distance,
                            sightStart.y + direction.y * distance,
                            sightStart.z + direction.z * distance,
                        };
                        if (ProjectToScreen(
                                sightEnd,
                                context.view,
                                options_.screenWidth,
                                options_.screenHeight,
                                sightEndScreen)) {
                            endProjected = true;
                            break;
                        }
                    }
                    if (endProjected &&
                        ProjectToScreen(
                            sightStart,
                            context.view,
                            options_.screenWidth,
                            options_.screenHeight,
                            sightStartScreen) &&
                        IsInsideScreen(
                            sightStartScreen,
                            options_.screenWidth,
                            options_.screenHeight,
                            80.0f)) {
                        const float maximumX =
                            static_cast<float>(options_.screenWidth) * 3.0f;
                        const float maximumY =
                            static_cast<float>(options_.screenHeight) * 3.0f;
                        sightEndScreen.x = std::clamp(
                            sightEndScreen.x, -maximumX, maximumX);
                        sightEndScreen.y = std::clamp(
                            sightEndScreen.y, -maximumY, maximumY);
                        PlayerSignalVisual signal{};
                        signal.start = ImVec2(
                            sightStartScreen.x, sightStartScreen.y);
                        signal.end = ImVec2(
                            sightEndScreen.x, sightEndScreen.y);
                        signal.kind = PlayerSignalKind::ViewDirection;
                        signal.tone = actorTone;
                        frame.playerSignals.push_back(std::move(signal));
                    }
                }
            }
            if (aimEligible && boneFrameReady) {
                const std::uint64_t identity =
                    native::ResolvePlayerIdentity(actor, playerState);
                Vec3 aimWorld{};
                Vec2 aimScreen{};
                int aimBone = -1;
                const int preferredBone = settings.aim.persistentLock &&
                        identity == lockedAimIdentity_
                    ? lockedAimBone_
                    : -1;
                const int aimMode = context.zooming
                    ? aimTuning.adsBone
                    : aimTuning.hipBone;
                if (SelectAimPoint(
                        boneFrame,
                        aimMode,
                        settings.aim,
                        context.view.location,
                        identity,
                        frame.sequence,
                        preferredBone,
                        aimWorld,
                        aimScreen,
                        aimBone)) {
                    const float centerX = static_cast<float>(options_.screenWidth) * 0.5f;
                    const float centerY = static_cast<float>(options_.screenHeight) * 0.5f;
                    const float screenDistance = std::hypot(
                        aimScreen.x - centerX, aimScreen.y - centerY);
                    const bool distanceAllowed = context.zooming ||
                        distanceMeters <= aimTuning.hipDistanceMeters;
                    if (std::isfinite(screenDistance) &&
                        screenDistance <= aimTuning.rangePixels && distanceAllowed) {
                        Vec3 velocity{};
                        const std::uintptr_t movement = ReadPointer(actor + 0x3D8);
                        if (IsValidPointer(movement)) {
                            ReadValue(movement + 0x2B0, velocity);
                            if (!IsFinite(velocity)) velocity = Vec3{};
                        }
                        aimCandidates.push_back(AimCandidate{
                            identity,
                            aimWorld,
                            velocity,
                            aimScreen,
                            screenDistance,
                            distanceMeters,
                            aimBone,
                        });
                    }
                }
            }

            if (!visualEligible || !settings.visual.enabled || !drawingInRange || !onScreen ||
                (combatModeActive && bot) ||
                (bot && settings.visual.filterBots)) {
                continue;
            }

            const float height = bodyBottom.y - bodyTop.y;
            if (!std::isfinite(height) || height < 8.0f ||
                height > static_cast<float>(options_.screenHeight) * 2.0f) {
                continue;
            }
            const float width = height * 0.45f;
            PlayerVisual visual{};
            visual.bounds = ScreenRect{
                bodyBottom.x - width * 0.5f,
                bodyTop.y,
                bodyBottom.x + width * 0.5f,
                bodyBottom.y,
            };
            visual.tone = ToneForVisibility(
                bot, settings.visual.visibilityColor, playerVisibility);
            visual.visible = true;
            visual.drawCornerBox = settings.visual.box;
            visual.drawTracer = settings.visual.snapline;
            visual.drawVitals = settings.visual.health;
            visual.drawPlate = settings.visual.playerName || settings.visual.distance ||
                settings.visual.operatorName || settings.visual.heldWeapon ||
                settings.visual.armorLevel || settings.visual.armorDurability;
            visual.drawSkeleton = settings.visual.skeleton &&
                distanceMeters <= settings.visual.skeletonDistanceMeters;
            visual.tracerOrigin = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                static_cast<float>(options_.screenHeight) - 10.0f);
            visual.vitals.health = health.health;
            visual.vitals.maxHealth = health.maxHealth;
            visual.vitals.armor = health.armor;
            visual.vitals.maxArmor = health.maxArmor;
            visual.vitals.downed = health.downed;

            if (settings.visual.playerName) {
                visual.name = ReadPlayerName(playerState);
                if (visual.name.empty() && bot) visual.name = "AI";
            }
            if (settings.visual.operatorName && !bot) {
                std::int32_t operatorId = 0;
                if (ReadValue(playerState + 0x9E8, operatorId)) {
                    if (const char* operatorName = OperatorName(operatorId)) {
                        if (!visual.name.empty()) visual.name += " | ";
                        visual.name += operatorName;
                    }
                }
            }
            if (settings.visual.distance) {
                AppendDetail(visual.detail, FormatDistance(distanceMeters));
            }
            if (settings.visual.heldWeapon && !bot) {
                const std::uintptr_t weapon = ReadPointer(actor + 0x1790);
                std::uint64_t weaponId = 0;
                if (IsValidPointer(weapon)) ReadValue(weapon + 0x838, weaponId);
                if (const char* weaponName = data::FindHandheldName(weaponId)) {
                    AppendDetail(visual.detail, weaponName);
                }
            }
            if ((settings.visual.armorLevel || settings.visual.armorDurability) && !bot) {
                std::string helmet;
                std::string armor;
                if (settings.visual.armorLevel) {
                    helmet = health.helmetLevel > 0
                        ? "头" + std::to_string(health.helmetLevel)
                        : "无头";
                    armor = health.armorLevel > 0
                        ? "甲" + std::to_string(health.armorLevel)
                        : "无甲";
                }
                if (settings.visual.armorDurability && health.maxHelmet > 0.0f) {
                    if (helmet.empty()) helmet = "头";
                    helmet += " " + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.helmet)))) +
                        "/" + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.maxHelmet))));
                }
                if (settings.visual.armorDurability && health.maxArmor > 0.0f) {
                    if (armor.empty()) armor = "甲";
                    armor += " " + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.armor)))) +
                        "/" + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.maxArmor))));
                }
                AppendDetail(helmet, armor);
                AppendDetail(visual.detail, helmet);
            }
            if (settings.visual.classNameDebug && !className.empty()) {
                AppendDetail(visual.detail, className);
            }

            if (visual.drawSkeleton) {
                if (!boneFrameReady) {
                    boneFrameReady = ReadBoneFrame(
                        actor, context.view, settings.visual.antiFlicker, boneFrame);
                }
                if (boneFrameReady) BuildSkeletonVisual(boneFrame, visual.skeleton);
                if (visual.skeleton.joints.empty()) visual.drawSkeleton = false;
            }
            frame.players.push_back(std::move(visual));
        }

        FinalizeThreatSignals(settings, frame);
        PublishAimFrame(settings.aim, aimTuning, context, aimCandidates, frame);

        PruneThreatState(seenThreats);
        const auto pruneTime = std::chrono::steady_clock::now();
        PruneBoneCache(pruneTime);
        PrunePositionCache(pruneTime);
        if (frame.highValueList.has_value()) {
            auto& entries = frame.highValueList->entries;
            std::stable_sort(entries.begin(), entries.end(),
                [](const HighValueEntry& left, const HighValueEntry& right) {
                    return left.value != right.value
                        ? left.value > right.value
                        : left.distanceMeters < right.distanceMeters;
                });
            if (entries.size() > static_cast<std::size_t>(frame.highValueList->maxRows)) {
                entries.resize(static_cast<std::size_t>(frame.highValueList->maxRows));
            }
        }
        return true;
    }

    void SetAimEnabled(bool enabled) override {
        aimEnabled_.store(enabled, std::memory_order_release);
        aimController_.SetEnabled(enabled);
    }

    void UpdateDisplayGeometry(
        int width, int height, int orientation) override {
        if (width <= 1 || height <= 1) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        options_.screenWidth = width;
        options_.screenHeight = height;
        options_.orientation = ((orientation % 4) + 4) % 4;
    }

    void ReloadCustomItems() override {
        std::lock_guard<std::mutex> lock(mutex_);
        customItems_.Load(customItemPath_);
    }

private:
    struct FrameContext {
        std::uintptr_t world = 0;
        std::uintptr_t level = 0;
        std::uintptr_t actorArray = 0;
        std::int32_t actorCount = 0;
        std::uintptr_t localController = 0;
        std::uintptr_t localPawn = 0;
        std::uintptr_t cameraManager = 0;
        std::uintptr_t namePool = 0;
        Vec3 localPosition{};
        CameraView view{};
        std::int32_t localTeam = -1;
        std::int32_t mapBuildId = 0;
        bool warfare = false;
        bool firing = false;
        bool zooming = false;
        std::uint64_t weaponId = 0;
        std::uintptr_t weaponRoot = 0;
    };

    struct HealthState {
        float health = 0.0f;
        float maxHealth = 100.0f;
        float armor = 0.0f;
        float maxArmor = 100.0f;
        float helmet = 0.0f;
        float maxHelmet = 0.0f;
        int helmetLevel = 0;
        int armorLevel = 0;
        bool downed = false;
    };

    struct BoneFrame {
        std::array<Vec3, kBoneIndices.size()> world{};
        std::array<Vec2, kBoneIndices.size()> screen{};
        std::array<bool, kBoneIndices.size()> valid{};
        std::array<native::GeometryVisibility, kBoneIndices.size()> visibility{};
    };

    struct PositionCacheEntry {
        Vec3 position{};
        std::chrono::steady_clock::time_point updatedAt{};
    };

    struct BoneCacheEntry {
        std::uintptr_t mesh = 0;
        std::uintptr_t boneArray = 0;
        std::array<Vec3, kBoneIndices.size()> world{};
        std::array<bool, kBoneIndices.size()> valid{};
        std::array<
            std::chrono::steady_clock::time_point,
            kBoneIndices.size()> boneUpdatedAt{};
        std::chrono::steady_clock::time_point lastUpdatedAt{};
    };

    struct AimCandidate {
        std::uint64_t identity = 0;
        Vec3 world{};
        Vec3 velocity{};
        Vec2 screen{};
        float screenDistancePixels = 0.0f;
        float worldDistanceMeters = 0.0f;
        int boneIndex = -1;
    };

    struct AimWarningState {
        bool tracking = false;
        bool active = false;
        std::uint64_t lastSeenSequence = 0;
        std::chrono::steady_clock::time_point alignedSince{};
    };

    void UpdateGeometryRuntime(const FeatureSettings& settings) {
        const bool needed =
            (settings.visual.enabled &&
             (settings.visual.modelGeometry ||
              settings.visual.visibilityColor)) ||
            (settings.aim.enabled &&
             aimEnabled_.load(std::memory_order_acquire) &&
             settings.aim.missMode);
        if (!needed) {
            if (geometryRuntime_.IsRunning()) geometryRuntime_.Stop();
            geometrySnapshotReady_ = false;
            geometryRefreshEpoch_ = 0;
            geometryRetryAfter_ = {};
            return;
        }

        if (!geometryRuntime_.IsRunning()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < geometryRetryAfter_) return;
            native::GeometryRuntimeConfig config{};
            for (const std::uintptr_t offset :
                 layout_.geometryInstancePointerOffsets) {
                if (offset == 0 ||
                    offset > kMaximumRemoteAddress - moduleBase_) {
                    continue;
                }
                const std::uintptr_t slot = moduleBase_ + offset;
                if (IsValidReadAddress(slot)) {
                    config.instancePointerSlots.push_back(slot);
                }
            }
            auto readBytes = [this](std::uintptr_t address,
                                    void* destination,
                                    std::size_t size) {
                return memory_ != nullptr &&
                    IsValidReadAddress(address) &&
                    size != 0 &&
                    size <= kMaximumRemoteAddress - address &&
                    memory_->Read(address, destination, size);
            };
            if (config.instancePointerSlots.empty() ||
                !geometryRuntime_.Start(std::move(readBytes), std::move(config))) {
                geometrySnapshotReady_ = false;
                geometryRefreshEpoch_ = 0;
                geometryRetryAfter_ = now + std::chrono::seconds(2);
                return;
            }
            geometryRetryAfter_ = {};
            geometrySnapshotReady_ = false;
            geometryRefreshEpoch_ = geometryRuntime_.RequestRefresh();
        }

        const std::shared_ptr<const native::GeometrySnapshot> current =
            geometryRuntime_.GetSnapshot();
        if (!geometrySnapshotReady_ && current != nullptr &&
            current->available &&
            current->refreshEpoch >= geometryRefreshEpoch_) {
            geometrySnapshotReady_ = true;
        }
    }

    void RequestGeometryRefresh() {
        if (!geometryRuntime_.IsRunning()) return;
        geometrySnapshotReady_ = false;
        geometryRefreshEpoch_ = geometryRuntime_.RequestRefresh();
    }

    native::GeometryVisibility TraceGeometry(const Vec3& origin,
                                             const Vec3& target) const {
        if (!geometrySnapshotReady_) {
            return native::GeometryVisibility::Unavailable;
        }
        return geometryRuntime_.Trace(
            ToGeometryPoint(origin), ToGeometryPoint(target));
    }

    template <typename T>
    bool ReadValue(std::uintptr_t address, T& output) {
        output = T{};
        return IsValidReadAddress(address) &&
               sizeof(T) <= kMaximumRemoteAddress - address && memory_ != nullptr &&
               memory_->Read(address, &output, sizeof(T));
    }

    std::uintptr_t ReadPointer(std::uintptr_t address) {
        std::uintptr_t value = 0;
        return ReadValue(address, value) && IsValidPointer(value) ? value : 0;
    }

    std::vector<std::uintptr_t> CollectActorAddresses(
        const FrameContext& context,
        bool includeStreamingLevels) {
        constexpr std::size_t kMaximumCollectedActors = 50000;
        std::vector<std::uintptr_t> result;
        std::unordered_set<std::uintptr_t> seen;
        result.reserve(static_cast<std::size_t>(context.actorCount));
        seen.reserve(static_cast<std::size_t>(context.actorCount));

        const auto appendArray = [&](const ActorArrayHeader& header) {
            if (!IsValidPointer(header.data) || header.count <= 0 ||
                header.count > kMaximumActorCount || header.capacity < header.count ||
                result.size() >= kMaximumCollectedActors) {
                return;
            }
            std::vector<std::uintptr_t> addresses(
                static_cast<std::size_t>(header.count));
            if (!memory_->Read(
                    header.data,
                    addresses.data(),
                    addresses.size() * sizeof(std::uintptr_t))) {
                for (std::size_t index = 0; index < addresses.size(); ++index) {
                    const std::uintptr_t offset =
                        index * sizeof(std::uintptr_t);
                    if (offset > kMaximumRemoteAddress - header.data) {
                        break;
                    }
                    ReadValue(header.data + offset, addresses[index]);
                }
            }
            for (const std::uintptr_t address : addresses) {
                if (IsValidPointer(address) && seen.insert(address).second) {
                    result.push_back(address);
                    if (result.size() >= kMaximumCollectedActors) break;
                }
            }
        };

        appendArray(ActorArrayHeader{
            context.actorArray,
            context.actorCount,
            context.actorCount,
        });
        if (!includeStreamingLevels || result.size() >= kMaximumCollectedActors) {
            return result;
        }

        ActorArrayHeader persistentObjects{};
        if (ReadValue(context.level + 0x98, persistentObjects)) {
            appendArray(persistentObjects);
        }

        const std::uintptr_t levels = ReadPointer(context.world + 0x158);
        std::int32_t levelCount = 0;
        if (!IsValidPointer(levels) || !ReadValue(context.world + 0x160, levelCount) ||
            levelCount <= 0 || levelCount > 512) {
            return result;
        }
        std::vector<std::uintptr_t> levelAddresses(static_cast<std::size_t>(levelCount));
        if (!memory_->Read(
                levels,
                levelAddresses.data(),
                levelAddresses.size() * sizeof(std::uintptr_t))) {
            return result;
        }
        for (const std::uintptr_t level : levelAddresses) {
            if (!IsValidPointer(level)) continue;
            if (level != context.level) {
                ActorArrayHeader actors{};
                if (ReadValue(level + 0x1F0, actors)) appendArray(actors);
            }
            ActorArrayHeader objects{};
            if (ReadValue(level + 0x98, objects)) appendArray(objects);
            if (result.size() >= kMaximumCollectedActors) break;
        }
        return result;
    }

    std::string ReadUtf16String(std::uintptr_t address, std::size_t maximum = 96) {
        if (!IsValidPointer(address) || maximum == 0 || maximum > 512) return {};
        std::vector<char16_t> buffer(maximum, 0);
        if (!memory_->Read(address, buffer.data(), buffer.size() * sizeof(char16_t))) {
            return {};
        }
        const auto end = std::find(buffer.begin(), buffer.end(), u'\0');
        return Utf16ToUtf8(std::u16string(buffer.begin(), end));
    }

    std::uint64_t ReadStructuredItemId(std::uintptr_t address) {
        std::uint64_t raw = 0;
        if (!ReadValue(address, raw)) return 0;
        const std::uint32_t category = static_cast<std::uint32_t>(raw);
        const std::uint32_t sequence = static_cast<std::uint32_t>(raw >> 32U);
        return category == 0
            ? 0
            : static_cast<std::uint64_t>(category) * 10000ULL + sequence;
    }

    struct ItemInfo {
        std::uint64_t id = 0;
        std::string name;
        int rarity = 0;
        int value = 0;
    };

    std::string ReadDefinitionName(std::uintptr_t definition) {
        if (!IsValidPointer(definition)) return {};
        const std::uintptr_t first = ReadPointer(definition + 0x18);
        const std::uintptr_t second = ReadPointer(first + 0x18);
        const std::uintptr_t third = ReadPointer(second + 0x28);
        return ReadUtf16String(ReadPointer(third + 0x10));
    }

    ItemInfo ReadPickupItem(std::uintptr_t actor) {
        ItemInfo item{};
        const std::uintptr_t definition = ReadPointer(actor + 0x11A8);
        if (IsValidPointer(definition)) {
            ReadValue(definition + 0x68, item.rarity);
            ReadValue(definition + 0xDC, item.value);
            item.name = ReadDefinitionName(definition);
        }
        item.id = ReadStructuredItemId(actor + 0x11F8);
        if (item.name.empty()) {
            if (const char* mapped = data::FindItemName(item.id)) {
                item.name = mapped;
            }
        }
        if (item.id != 0 && item.rarity >= 1 && item.rarity <= 6 &&
            item.value > 0 && item.value < 100000000) {
            itemMetadata_[item.id] = std::pair<int, int>{item.rarity, item.value};
        }
        return item;
    }

    struct ContainerItem {
        std::string name;
        int count = 0;
        int rarity = 0;
        int value = 0;
    };

    std::vector<ContainerItem> ReadContainerItems(
        std::uintptr_t actor,
        int minimumValue,
        int minimumRarity,
        int maximumItems) {
        constexpr std::uintptr_t kItemsArrayOffset = 0x1E28 + 0x108;
        constexpr std::uintptr_t kItemStride = 0x7D8;
        constexpr std::uintptr_t kItemIdOffset = 0x10;
        constexpr std::uintptr_t kItemCountOffset = 0x38;

        const std::uintptr_t arrayAddress = actor + kItemsArrayOffset;
        const std::uintptr_t items = ReadPointer(arrayAddress);
        std::int32_t count = 0;
        if (!ReadValue(arrayAddress + 0x8, count) || !IsValidPointer(items) ||
            count <= 0 || count > 512) {
            return {};
        }

        std::vector<ContainerItem> result;
        result.reserve(static_cast<std::size_t>(std::min(count, maximumItems)));
        for (int index = 0; index < count; ++index) {
            const std::uintptr_t entry = items + kItemStride * index;
            const std::uint64_t itemId = ReadStructuredItemId(entry + kItemIdOffset);
            std::int32_t itemCount = 0;
            ReadValue(entry + kItemCountOffset, itemCount);
            if (itemId == 0 || itemCount <= 0) continue;

            const char* mapped = data::FindItemName(itemId);
            if (mapped == nullptr || *mapped == '\0') continue;
            int rarity = 0;
            int value = 0;
            const auto metadata = itemMetadata_.find(itemId);
            if (metadata != itemMetadata_.end()) {
                rarity = metadata->second.first;
                value = metadata->second.second;
                if (value < minimumValue || rarity < minimumRarity) continue;
            }
            result.push_back(ContainerItem{mapped, itemCount, rarity, value});
        }
        std::stable_sort(result.begin(), result.end(),
            [](const ContainerItem& left, const ContainerItem& right) {
                return left.value != right.value
                    ? left.value > right.value
                    : left.rarity > right.rarity;
            });
        if (maximumItems > 0 && result.size() > static_cast<std::size_t>(maximumItems)) {
            result.resize(static_cast<std::size_t>(maximumItems));
        }
        return result;
    }

    static std::optional<ui::ContainerKind> ContainerKindForClass(
        std::string_view name) {
        using Kind = ui::ContainerKind;
        if (name.find("Computer") != std::string_view::npos) return Kind::Computer;
        if (name.find("Weapon") != std::string_view::npos) return Kind::WeaponCrate;
        if (name.find("Tool") != std::string_view::npos) return Kind::LargeToolbox;
        if (name.find("Ammo") != std::string_view::npos) return Kind::AmmoCrate;
        if (name.find("Locker") != std::string_view::npos) return Kind::Locker;
        if (name.find("Mountain") != std::string_view::npos) return Kind::MountainPack;
        if (name.find("Capsule") != std::string_view::npos) return Kind::ReturnCapsule;
        if (name.find("Storage") != std::string_view::npos) return Kind::StorageBox;
        if (name.find("Briefcase") != std::string_view::npos) return Kind::Briefcase;
        if (name.find("Safe") != std::string_view::npos) return Kind::Safe;
        if (name.find("Lab") != std::string_view::npos) return Kind::LabCoat;
        if (name.find("Mixer") != std::string_view::npos) return Kind::MixerTruck;
        if (name.find("Truck") != std::string_view::npos) return Kind::ContainerTruck;
        if (name.find("Wire") != std::string_view::npos) return Kind::WireFence;
        if (name.find("Password") != std::string_view::npos) return Kind::PasswordDoor;
        if (name.find("Switch") != std::string_view::npos) return Kind::Switch;
        if (name.find("Recon") != std::string_view::npos) return Kind::Recon;
        if (name.find("Exit") != std::string_view::npos ||
            name.find("Extract") != std::string_view::npos) return Kind::ExtractionPoint;
        return std::nullopt;
    }

    static std::optional<ui::ContainerKind> ContainerKindForDisplayName(
        std::string_view name) {
        using Kind = ui::ContainerKind;
        if (name == "查看行动") return Kind::MissionTerminal;
        if (name == "楼梯") return Kind::Stairs;
        if (name == "保险柜") return Kind::Vault;
        if (name == "密码锁") return Kind::CombinationLock;
        return std::nullopt;
    }

    static bool IsContainerKindEnabled(
        const ui::LootSettings& settings,
        const std::optional<ui::ContainerKind>& kind) {
        if (!kind.has_value()) return false;
        const std::size_t index = static_cast<std::size_t>(*kind);
        return index < settings.containerKinds.size() && settings.containerKinds[index];
    }

    static std::string ContainerDetail(
        float distanceMeters,
        const std::vector<ContainerItem>& items) {
        std::string detail = FormatDistance(distanceMeters);
        const std::size_t count = std::min<std::size_t>(items.size(), 3);
        for (std::size_t index = 0; index < count; ++index) {
            if (!detail.empty()) detail += "  ";
            detail += items[index].name;
            if (items[index].count > 1) {
                detail += " x" + std::to_string(items[index].count);
            }
        }
        return detail;
    }

    void ProcessWorldActor(
        std::uintptr_t actor,
        const std::string& className,
        const FrameContext& context,
        const FeatureSettings& settings,
        GameFrame& frame,
        std::unordered_set<std::uintptr_t>& seenThreats) {
        Vec3 position{};
        if (!ReadActorPosition(actor, position, settings.visual.antiFlicker)) return;
        const float distanceMeters = Length(Subtract(position, context.localPosition)) * 0.01f;
        if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) return;

        Vec2 screen{};
        CameraPoint cameraPoint{};
        const bool projected = ProjectToScreen(
            position,
            context.view,
            options_.screenWidth,
            options_.screenHeight,
            screen,
            &cameraPoint);
        const bool suppressLoot = settings.visual.combatMode &&
            (context.firing || context.zooming);

        if (settings.visual.enabled && settings.visual.classNameDebug &&
            !className.empty() && projected &&
            distanceMeters <= static_cast<float>(settings.visual.drawDistanceMeters) &&
            IsInsideScreen(screen, options_.screenWidth, options_.screenHeight, 40.0f) &&
            frame.worldLabels.size() < 2048) {
            WorldLabel label{};
            label.anchor = ImVec2(screen.x, screen.y);
            label.title = className;
            label.detail = FormatDistance(distanceMeters);
            label.kind = WorldLabelKind::Container;
            label.tone = SemanticTone::Muted;
            frame.worldLabels.push_back(std::move(label));
        }

        if (!suppressLoot && settings.loot.password && projected &&
            distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
            IsInsideScreen(screen, options_.screenWidth, options_.screenHeight, 40.0f)) {
            std::int32_t password = 0;
            if (className.find("Computer") != std::string::npos) {
                ReadValue(actor + 0xFEC, password);
            } else if (className.find("Password") != std::string::npos ||
                       className.find("Combination") != std::string::npos) {
                ReadValue(actor + 0x1048, password);
            }
            if (password > 0 && password <= 10000) {
                frame.worldLabels.push_back(WorldLabel{
                    ImVec2(screen.x, screen.y),
                    "密码 " + std::to_string(password),
                    FormatDistance(distanceMeters),
                    WorldLabelKind::Container,
                    SemanticTone::Caution,
                    true,
                });
            }
        }

        const std::optional<data::CustomItemEntry> customItem =
            customItems_.Match(className);
        if (!suppressLoot && (IsPickupClass(className) || customItem.has_value()) &&
            settings.loot.enabled &&
            distanceMeters <= static_cast<float>(settings.loot.itemDistanceMeters)) {
            ItemInfo item = ReadPickupItem(actor);
            if (item.name.empty() && customItem.has_value()) {
                item.name = customItem->displayName;
                item.rarity = std::max(item.rarity, 1);
            }
            if (!item.name.empty() && item.value >= settings.loot.minimumItemValue &&
                item.rarity >= settings.loot.minimumItemRarity && projected &&
                IsInsideScreen(screen, options_.screenWidth, options_.screenHeight, 40.0f)) {
                WorldLabel label{};
                label.anchor = ImVec2(screen.x, screen.y);
                label.title = item.name;
                label.detail = "价值 " + std::to_string(std::max(0, item.value)) +
                    "  " + FormatDistance(distanceMeters);
                label.kind = WorldLabelKind::Item;
                label.tone = ToneForRarity(item.rarity);
                label.emphasized = item.rarity >= 5;
                if (customItem.has_value()) {
                    label.colorOverride = customItem->color;
                    label.titleSizeOverride = customItem->fontSize;
                }
                frame.worldLabels.push_back(std::move(label));
            }
            if (settings.loot.highValueList && !item.name.empty() &&
                item.value >= settings.loot.minimumListValue &&
                item.rarity >= settings.loot.minimumListRarity) {
                if (!frame.highValueList.has_value()) {
                    HighValueList list{};
                    list.origin = ImVec2(
                        std::max(12.0f, static_cast<float>(options_.screenWidth) - 332.0f),
                        76.0f);
                    list.width = 310.0f;
                    list.maxRows = std::clamp(settings.loot.listLimit, 1, 50);
                    list.title = "高价值物资";
                    frame.highValueList = std::move(list);
                }
                frame.highValueList->entries.push_back(HighValueEntry{
                    item.name,
                    item.value,
                    distanceMeters,
                    ToneForRarity(item.rarity),
                });
            }
        }

        if (!suppressLoot && IsDeadBodyClass(className) &&
            (settings.loot.playerBox || settings.loot.botBox) &&
            distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
            projected && IsInsideScreen(screen, options_.screenWidth, options_.screenHeight, 40.0f)) {
            std::uint8_t looted = 0;
            ReadValue(actor + 0x2678, looted);
            if (looted == 0) {
                const std::uintptr_t owner = ReadPointer(actor + 0x2668);
                const bool bot = !IsValidPointer(owner) || !IsValidPointer(ReadPointer(owner + 0x390));
                if ((!bot && settings.loot.playerBox) || (bot && settings.loot.botBox)) {
                    std::vector<ContainerItem> items;
                    if (settings.loot.boxContents) {
                        items = ReadContainerItems(
                            actor,
                            settings.loot.minimumContainerValue,
                            settings.loot.minimumContainerRarity,
                            8);
                    }
                    WorldLabel label{};
                    label.anchor = ImVec2(screen.x, screen.y);
                    label.title = bot ? "人机盒子" : "玩家盒子";
                    label.detail = ContainerDetail(distanceMeters, items);
                    label.kind = WorldLabelKind::Container;
                    label.tone = bot ? SemanticTone::Muted : SemanticTone::Ally;
                    frame.worldLabels.push_back(std::move(label));
                }
            }
        }

        if (!suppressLoot && settings.loot.containers) {
            if (IsExtractionClass(className)) {
                const auto kind = ui::ContainerKind::ExtractionPoint;
                if (IsContainerKindEnabled(settings.loot, kind) &&
                    distanceMeters <=
                        static_cast<float>(settings.loot.containerDistanceMeters) &&
                    projected &&
                    IsInsideScreen(
                        screen,
                        options_.screenWidth,
                        options_.screenHeight,
                        40.0f)) {
                    frame.worldLabels.push_back(WorldLabel{
                        ImVec2(screen.x, screen.y),
                        "撤离点",
                        FormatDistance(distanceMeters),
                        WorldLabelKind::Container,
                        SemanticTone::Ally,
                        true,
                    });
                }
            }

            std::int32_t containerId = 0;
            float detectRatio = 0.0f;
            ReadValue(actor + 0x2378, containerId);
            ReadValue(actor + 0x720, detectRatio);
            const bool legacyCandidate = detectRatio == 200.0f ||
                detectRatio == 360.0f || detectRatio == 20.0f;
            const bool containerClass = className.find("B_neat") != std::string::npos;
            const bool knownContainer = IsKnownContainerId(containerId);
            const bool preciseContainer = knownContainer && containerClass;
            if (legacyCandidate &&
                distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
                projected && IsInsideScreen(screen, options_.screenWidth, options_.screenHeight, 40.0f)) {
                std::uint8_t opened = 0;
                if (preciseContainer) {
                    ReadValue(actor + 0x2384, opened);
                }
                const std::string displayName = knownContainer
                    ? std::string(ContainerName(containerId))
                    : ReadDefinitionName(ReadPointer(actor + 0x11A8));
                auto kind = ContainerKindForId(containerId);
                if (!kind.has_value()) {
                    kind = ContainerKindForDisplayName(displayName);
                }
                if (!kind.has_value()) {
                    kind = ContainerKindForClass(className);
                }
                if (opened == 0 && IsContainerKindEnabled(settings.loot, kind)) {
                    std::vector<ContainerItem> items;
                    if (settings.loot.containerContents) {
                        items = ReadContainerItems(
                            actor,
                            settings.loot.minimumContainerValue,
                            settings.loot.minimumContainerRarity,
                            8);
                    }
                    WorldLabel label{};
                    label.anchor = ImVec2(screen.x, screen.y);
                    label.title = displayName.empty() ? "容器" : displayName;
                    label.detail = ContainerDetail(distanceMeters, items);
                    label.kind = WorldLabelKind::Container;
                    label.tone = SemanticTone::Accent;
                    frame.worldLabels.push_back(std::move(label));
                }
            }
        }

        const data::ThreatObjectInfo* threat = data::FindThreatObject(className);
        if (settings.visual.enabled &&
            (settings.visual.throwableWarning ||
             settings.visual.throwableTrajectory) &&
            threat != nullptr &&
            distanceMeters <= 50.0f) {
            seenThreats.insert(actor);
            const auto now = std::chrono::steady_clock::now();
            const auto [firstSeen, inserted] = threatFirstSeen_.try_emplace(actor, now);
            (void)inserted;
            const bool expired =
                std::chrono::duration<float>(now - firstSeen->second).count() > 10.0f;
            if (!expired && settings.visual.throwableTrajectory) {
                auto& trail = projectileTrails_[actor];
                if (trail.empty() || Length(Subtract(trail.back(), position)) > 5.0f) {
                    trail.push_back(position);
                }
                if (trail.size() > 140) {
                    trail.erase(trail.begin(), trail.begin() + (trail.size() - 140));
                }
            }
            if (!expired && settings.visual.throwableWarning) {
                frame.dangerousObjectNearby = true;
            }
            if (!expired && projected) {
                ProjectileVisual projectile{};
                projectile.center = ImVec2(screen.x, screen.y);
                projectile.label = settings.visual.throwableWarning
                    ? std::string(threat->displayName)
                    : std::string{};
                projectile.distanceMeters = settings.visual.throwableWarning
                    ? distanceMeters
                    : -1.0f;
                projectile.tone = SemanticTone::Danger;
                projectile.colorOverride = IM_COL32(
                    threat->red,
                    threat->green,
                    threat->blue,
                    threat->alpha);
                if (settings.visual.throwableWarning) {
                    const float projectionScale =
                        (static_cast<float>(options_.screenWidth) * 0.5f) /
                        std::tan(context.view.fieldOfView * kPi / 360.0f);
                    projectile.rangeRadius = std::clamp(
                        threat->radiusCentimeters * projectionScale /
                            std::max(cameraPoint.forward, 1.0f),
                        6.0f,
                        static_cast<float>(
                            std::max(options_.screenWidth, options_.screenHeight)));
                }
                if (settings.visual.throwableTrajectory) {
                    const auto found = projectileTrails_.find(actor);
                    if (found != projectileTrails_.end()) {
                        for (const Vec3& point : found->second) {
                            Vec2 projectedPoint{};
                            if (ProjectToScreen(
                                    point,
                                    context.view,
                                    options_.screenWidth,
                                    options_.screenHeight,
                                    projectedPoint)) {
                                projectile.trajectory.emplace_back(
                                    projectedPoint.x, projectedPoint.y);
                            }
                        }
                    }
                }
                frame.projectiles.push_back(std::move(projectile));
            }
        }
    }

    bool BuildFrameContext(FrameContext& context,
                           bool battlefieldMode,
                           native::PositionReadMode positionMode,
                           bool antiFlicker,
                           std::string& diagnostic) {
        if (!IsValidPointer(moduleBase_)) {
            diagnostic = "数据链等待：模块地址无效";
            return false;
        }
        context.namePool = moduleBase_ + layout_.namePoolOffset;
        const std::uintptr_t worldAddress = moduleBase_ + layout_.worldOffset;
        if (!IsValidPointer(context.namePool) || !IsValidPointer(worldAddress)) {
            diagnostic = "数据链等待：世界入口无效";
            return false;
        }

        context.world = ReadPointer(worldAddress);
        if (!IsValidPointer(context.world)) {
            diagnostic = "数据链等待：世界对象暂不可读";
            return false;
        }
        context.level = ReadPointer(context.world + 0xF8);
        if (!IsValidPointer(context.level)) {
            diagnostic = "数据链等待：关卡对象暂不可读";
            return false;
        }

        ActorArrayHeader actors{};
        if (!ReadValue(context.level + 0x1F0, actors) ||
            !IsValidPointer(actors.data) || actors.count <= 0 ||
            actors.count > kMaximumActorCount || actors.capacity < actors.count) {
            diagnostic = "数据链等待：人物数组暂不可读";
            return false;
        }
        context.actorArray = actors.data;
        context.actorCount = actors.count;

        const std::uintptr_t gameInstance = ReadPointer(context.world + 0x190);
        const std::uintptr_t localPlayers = ReadPointer(gameInstance + 0x38);
        const std::uintptr_t localPlayer = ReadPointer(localPlayers);
        context.localController = ReadPointer(localPlayer + 0x30);
        context.localPawn = ReadPointer(context.localController + 0x3F0);
        context.cameraManager = ReadPointer(context.localController + 0x408);
        if (!IsValidPointer(context.localController) || !IsValidPointer(context.localPawn) ||
            !IsValidPointer(context.cameraManager)) {
            diagnostic = "数据链等待：本地角色或相机暂不可读";
            return false;
        }

        const std::uintptr_t mapState = ReadPointer(context.world + 0x140);
        std::int32_t mapBuild = 0;
        if (IsValidPointer(mapState)) ReadValue(mapState + 0x6E8, mapBuild);
        context.mapBuildId = mapBuild;
        context.warfare = mapBuild > 9000;

        const std::uintptr_t localState = ReadPointer(context.localPawn + 0x390);
        std::int32_t primaryTeam = -1;
        std::int32_t secondaryTeam = -1;
        if (IsValidPointer(localState)) {
            if (!ReadValue(localState + 0x658, primaryTeam)) {
                primaryTeam = native::kUnknownPlayerTeam;
            }
            if (!ReadValue(localState + 0x65C, secondaryTeam)) {
                secondaryTeam = native::kUnknownPlayerTeam;
            }
        }
        context.localTeam =
            (context.warfare || battlefieldMode) ? secondaryTeam : primaryTeam;

        const std::uintptr_t blackboard = ReadPointer(context.localPawn + 0x2430);
        std::uint8_t zooming = 0;
        std::uint8_t firing = 0;
        if (IsValidPointer(blackboard)) {
            ReadValue(blackboard + 0x5AC, zooming);
            ReadValue(blackboard + 0x5AD, firing);
        }
        context.zooming = zooming != 0;
        context.firing = firing != 0;
        context.weaponRoot = ReadPointer(context.localPawn + 0x1790);
        if (IsValidPointer(context.weaponRoot)) {
            ReadValue(context.weaponRoot + 0x838, context.weaponId);
        }

        CameraView first{};
        CameraView second{};
        const bool firstValid = ReadValue(context.cameraManager + 0x3590, first) && IsFinite(first);
        const bool secondValid = ReadValue(context.cameraManager + 0x3590, second) && IsFinite(second);
        if (secondValid) context.view = second;
        else if (firstValid) context.view = first;
        else if (lastViewValid_) context.view = lastView_;
        else {
            diagnostic = "数据链等待：相机数据暂不可读";
            return false;
        }
        lastView_ = context.view;
        lastViewValid_ = true;

        std::int32_t localNameIndex = -1;
        ReadValue(context.localPawn + 0x1C, localNameIndex);
        const std::string localClassName =
            DecodeName(localNameIndex, context.namePool);
        if (!ReadCharacterPosition(
                context.localPawn,
                localClassName,
                positionMode,
                antiFlicker,
                context.localPosition)) {
            context.localPosition = context.view.location;
        }
        const std::uintptr_t verifiedWorld = ReadPointer(worldAddress);
        if (verifiedWorld != context.world) {
            diagnostic = "数据链等待：世界对象正在切换";
            return false;
        }
        diagnostic.clear();
        return true;
    }

    bool ReadActorPosition(std::uintptr_t actor, Vec3& position, bool allowCache) {
        constexpr auto kCacheLifetime = std::chrono::milliseconds(300);
        const auto now = std::chrono::steady_clock::now();
        const std::uintptr_t component = ReadPointer(actor + 0x180);
        Vec3 candidate{};
        bool valid = IsValidPointer(component) &&
            ((ReadValue(component + 0x168, candidate) && IsFinite(candidate) && IsNonzero(candidate)) ||
             (ReadValue(component + 0x220, candidate) && IsFinite(candidate) && IsNonzero(candidate)));
        if (valid) {
            if (allowCache) {
                positionCache_[actor] = PositionCacheEntry{candidate, now};
            }
            position = candidate;
            return true;
        }
        if (allowCache) {
            const auto found = positionCache_.find(actor);
            if (found != positionCache_.end() &&
                now - found->second.updatedAt <= kCacheLifetime) {
                position = found->second.position;
                return true;
            }
            if (found != positionCache_.end()) positionCache_.erase(found);
        }
        return false;
    }

    bool ReadCharacterPosition(
        std::uintptr_t actor,
        std::string_view className,
        native::PositionReadMode mode,
        bool antiFlicker,
        Vec3& position) {
        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        native::CharacterPositionResolver::Coordinate coordinate{};
        if (!characterPositions_.Read(
                actor,
                className,
                mode,
                antiFlicker,
                coordinate,
                readBytes)) {
            return false;
        }
        position = Vec3{coordinate[0], coordinate[1], coordinate[2]};
        return IsFinite(position) && IsNonzero(position);
    }

    bool ReadActorFacing(std::uintptr_t actor,
                         Vec3& forward,
                         float& headingRadians) {
        forward = Vec3{};
        headingRadians = 0.0f;
        const std::uintptr_t mesh = ReadPointer(actor + 0x3D0);
        if (!IsValidPointer(mesh)) return false;

        Transform transform{};
        if (!ReadValue(mesh + 0x210, transform)) return false;
        const Quaternion& rotation = transform.rotation;
        const float normSquared = rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        if (!std::isfinite(normSquared) || normSquared < 0.25f || normSquared > 2.25f) {
            return false;
        }

        forward = Vec3{
            1.0f - 2.0f * (rotation.y * rotation.y + rotation.z * rotation.z),
            2.0f * (rotation.x * rotation.y + rotation.w * rotation.z),
            2.0f * (rotation.x * rotation.z - rotation.w * rotation.y),
        };
        const float horizontalLength = HorizontalLength(forward);
        if (!IsFinite(forward) || !std::isfinite(horizontalLength) ||
            horizontalLength <= 0.0001f) {
            forward = Vec3{};
            return false;
        }
        headingRadians = std::atan2(forward.y, forward.x);
        return std::isfinite(headingRadians);
    }

    bool ObserveAimWarning(
        std::uintptr_t actor,
        bool aligned,
        std::uint64_t sequence,
        std::chrono::steady_clock::time_point now) {
        constexpr float kSustainSeconds = 1.0f;
        AimWarningState& state = aimWarningStates_[actor];
        state.lastSeenSequence = sequence;
        if (!aligned) {
            state.tracking = false;
            state.active = false;
            state.alignedSince = std::chrono::steady_clock::time_point{};
            return false;
        }
        if (!state.tracking) {
            state.tracking = true;
            state.active = false;
            state.alignedSince = now;
        }
        state.active = std::chrono::duration<float>(now - state.alignedSince).count() >=
            kSustainSeconds;
        return state.active;
    }

    void FinalizeThreatSignals(const FeatureSettings& settings,
                               GameFrame& frame) {
        for (auto iterator = aimWarningStates_.begin();
             iterator != aimWarningStates_.end();) {
            if (iterator->second.lastSeenSequence != frame.sequence) {
                iterator = aimWarningStates_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        if (!settings.visual.enabled) return;
        float alertY = static_cast<float>(options_.screenHeight) * 0.70f;
        const float alertStep = std::max(44.0f, 48.0f * settings.visual.fontScale);
        if (settings.visual.nearbyEnemy && frame.nearbyEnemyCount > 0) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            label.title = "50米内敌人数量：" + std::to_string(frame.nearbyEnemyCount);
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }

        if (settings.visual.throwableWarning && frame.dangerousObjectNearby) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            label.title = "警告：附近有危险道具！";
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }

        if (!settings.visual.aimWarning) return;
        const std::size_t visibleCount =
            std::min<std::size_t>(frame.aimWarningPlayers.size(), 3);
        for (std::size_t index = 0; index < visibleCount; ++index) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            if (frame.aimWarningPlayers[index].empty()) {
                label.title = "正在瞄准你";
            } else {
                label.title = frame.aimWarningPlayers[index];
                label.detail = "正在瞄准你";
            }
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }
    }

    bool ReadHealth(std::uintptr_t actor,
                    std::uintptr_t playerState,
                    HealthState& state) {
        const std::uintptr_t healthComponent = ReadPointer(actor + 0x10C8);
        const std::uintptr_t healthSet = ReadPointer(healthComponent + 0x280);
        if (!IsValidPointer(healthSet)) return false;
        if (!ReadValue(healthSet + 0x38, state.health) ||
            !ReadValue(healthSet + 0x50, state.maxHealth) ||
            !std::isfinite(state.health) || !std::isfinite(state.maxHealth) ||
            state.maxHealth <= 0.0f || state.maxHealth > 100000.0f) {
            return false;
        }
        state.health = std::clamp(state.health, 0.0f, state.maxHealth);

        std::uint8_t downed = 0;
        if (IsValidPointer(playerState)) {
            ReadValue(playerState + 0x366, downed);
        }
        float downedHealth = 0.0f;
        float maximumDownedHealth = 0.0f;
        ReadValue(healthSet + 0x110, downedHealth);
        ReadValue(healthSet + 0x120, maximumDownedHealth);
        state.downed = downed != 0 ||
            (std::isfinite(downedHealth) && std::isfinite(maximumDownedHealth) &&
             maximumDownedHealth > 0.0f && downedHealth > 0.0f);

        const std::uintptr_t equipmentComponent = ReadPointer(actor + 0x2420);
        const std::uintptr_t equipment = ReadPointer(equipmentComponent + 0x1D8);
        if (IsValidPointer(equipment)) {
            std::int32_t helmetDefinition = 0;
            std::int32_t armorDefinition = 0;
            ReadValue(equipment + 0x30, helmetDefinition);
            ReadValue(equipment + 0xF0, armorDefinition);
            state.helmetLevel = EquipmentLevel(helmetDefinition);
            state.armorLevel = EquipmentLevel(armorDefinition);
            ReadValue(equipment + 0x48, state.helmet);
            ReadValue(equipment + 0x4C, state.maxHelmet);
            ReadValue(equipment + 0x108, state.armor);
            ReadValue(equipment + 0x10C, state.maxArmor);
            if (!std::isfinite(state.helmet) || !std::isfinite(state.maxHelmet) ||
                state.maxHelmet <= 0.0f || state.maxHelmet > 100000.0f) {
                state.helmet = 0.0f;
                state.maxHelmet = 0.0f;
            } else {
                state.helmet = std::clamp(state.helmet, 0.0f, state.maxHelmet);
            }
            if (!std::isfinite(state.armor) || !std::isfinite(state.maxArmor) ||
                state.maxArmor <= 0.0f || state.maxArmor > 100000.0f) {
                state.armor = 0.0f;
                state.maxArmor = 100.0f;
            } else {
                state.armor = std::clamp(state.armor, 0.0f, state.maxArmor);
            }
        }
        return true;
    }

    std::string ReadPlayerName(std::uintptr_t playerState) {
        const std::uintptr_t nameAddress = ReadPointer(playerState + 0x470);
        if (!IsValidPointer(nameAddress)) return {};
        std::array<char16_t, 32> buffer{};
        if (!memory_->Read(nameAddress, buffer.data(), buffer.size() * sizeof(char16_t))) return {};
        std::size_t length = 0;
        while (length < buffer.size() && buffer[length] != 0) ++length;
        return Utf16ToUtf8(std::u16string(buffer.data(), length));
    }

    std::string DecodeName(std::int32_t index, std::uintptr_t namePool) {
        const auto cached = nameCache_.find(index);
        if (cached != nameCache_.end()) return cached->second;
        if (index < 0 || !IsValidPointer(namePool)) return {};

        const std::uint32_t unsignedIndex = static_cast<std::uint32_t>(index);
        const std::uint32_t blockOffset = (unsignedIndex >> 15U) & 0x1FFF8U;
        const std::uint32_t entryOffset = (unsignedIndex * 2U) & 0x7FFFEU;
        const std::uintptr_t chunk = ReadPointer(namePool + blockOffset + 0x38);
        if (!IsValidPointer(chunk) ||
            entryOffset > kMaximumRemoteAddress - chunk) return {};
        const std::uintptr_t entry = chunk + entryOffset;
        std::uint16_t header = 0;
        if (!ReadValue(entry, header)) return {};
        const std::size_t length = header >> 6U;
        if (length == 0 || length > kMaximumNameLength) return {};

        std::string bytes(length, '\0');
        if (!memory_->Read(entry + 2, bytes.data(), bytes.size())) return {};
        if ((header & 1U) != 0U) {
            const std::uint8_t key = AlternatingNameKey(length, entry);
            for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
                bytes[offset] = static_cast<char>(
                    static_cast<std::uint8_t>(bytes[offset]) ^ key);
            }
        } else {
            const std::uint8_t key = SequentialNameKey(length);
            for (char& value : bytes) {
                value = static_cast<char>(static_cast<std::uint8_t>(value) ^ key);
            }
        }

        const std::size_t terminator = bytes.find('\0');
        if (terminator != std::string::npos) bytes.resize(terminator);
        if (bytes.empty() || std::any_of(bytes.begin(), bytes.end(), [](char value) {
                const unsigned char byte = static_cast<unsigned char>(value);
                return byte < 0x20U || byte > 0x7EU;
            })) {
            bytes.clear();
        }
        if (!bytes.empty()) {
            if (nameCache_.size() > 8192) nameCache_.clear();
            nameCache_[index] = bytes;
        }
        return bytes;
    }

    static std::uint8_t AlternatingNameKey(std::size_t length,
                                           std::uintptr_t entry) {
        const std::uint32_t size = static_cast<std::uint32_t>(length);
        switch (size % 9U) {
            case 0: return static_cast<std::uint8_t>(((size & 0x1FU) + size + 128U) | 0x7FU);
            case 1: return static_cast<std::uint8_t>(((size ^ 0xDFU) + size + 128U) | 0x7FU);
            case 2: return static_cast<std::uint8_t>(((size ^ 0xCFU) + size + 128U) | 0x7FU);
            case 3: return static_cast<std::uint8_t>((33U * size + 128U) | 0x7FU);
            case 4: return static_cast<std::uint8_t>(
                (((entry >> 6U) + ((entry >> 8U) & 0xFFU) + 128U) | 0x7FU));
            case 5: return static_cast<std::uint8_t>((3U * size + 133U) | 0x7FU);
            case 6: return static_cast<std::uint8_t>((((4U * size) | 5U) + 128U) | 0x7FU);
            case 7: return static_cast<std::uint8_t>(
                ((((entry >> 10U) | 7U) + (entry >> 6U) + 128U) | 0x7FU));
            case 8: return static_cast<std::uint8_t>(((size ^ 0xCU) + size + 128U) | 0x7FU);
            default: return 0;
        }
    }

    static std::uint8_t SequentialNameKey(std::size_t length) {
        const std::uint32_t size = static_cast<std::uint32_t>(length);
        switch (size % 9U) {
            case 0: return static_cast<std::uint8_t>((((size & 0x1FU) + size) | 0x7FU) ^ 0x80U);
            case 1: return static_cast<std::uint8_t>((((size ^ 0xDFU) + size) | 0x7FU) ^ 0x80U);
            case 2: return static_cast<std::uint8_t>((((size | 0xCFU) + size) | 0x7FU) ^ 0x80U);
            case 3: return static_cast<std::uint8_t>(((33U * size) | 0x7FU) ^ 0x80U);
            case 4: return static_cast<std::uint8_t>(((size + (size >> 2U)) | 0x7FU) ^ 0x80U);
            case 5: return static_cast<std::uint8_t>(((3U * size + 5U) | 0x7FU) ^ 0x80U);
            case 6: return static_cast<std::uint8_t>(((((4U * size) | 5U) + size) | 0x7FU) ^ 0x80U);
            case 7: return static_cast<std::uint8_t>(((((size >> 4U) | 7U) + size) | 0x7FU) ^ 0x80U);
            case 8: return static_cast<std::uint8_t>((((size ^ 0xCU) + size) | 0x7FU) ^ 0x80U);
            default: return 0;
        }
    }

    native::GeometryVisibility EvaluateBoneVisibility(
        const Vec3& origin,
        BoneFrame& frame) const {
        bool anyVisible = false;
        bool anyOccluded = false;
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (!frame.valid[index]) continue;
            const native::GeometryVisibility visibility =
                TraceGeometry(origin, frame.world[index]);
            frame.visibility[index] = visibility;
            anyVisible = anyVisible ||
                visibility == native::GeometryVisibility::Visible;
            anyOccluded = anyOccluded ||
                visibility == native::GeometryVisibility::Occluded;
        }
        if (anyVisible) return native::GeometryVisibility::Visible;
        if (anyOccluded) return native::GeometryVisibility::Occluded;
        return native::GeometryVisibility::Unavailable;
    }

    bool ReadBoneFrame(std::uintptr_t actor,
                       const CameraView& view,
                       bool antiFlicker,
                       BoneFrame& frame) {
        constexpr auto kCacheLifetime = std::chrono::milliseconds(300);
        frame = BoneFrame{};
        const auto now = std::chrono::steady_clock::now();
        auto cached = boneCache_.find(actor);
        bool cacheFresh = antiFlicker && cached != boneCache_.end() &&
            now - cached->second.lastUpdatedAt <= kCacheLifetime;

        std::uintptr_t mesh = ReadPointer(actor + 0x3D0);
        if (!IsValidPointer(mesh) && cacheFresh) mesh = cached->second.mesh;
        std::uintptr_t boneArray = IsValidPointer(mesh)
            ? ReadPointer(mesh + 0x730)
            : 0;
        if (!IsValidPointer(boneArray) && cacheFresh) {
            boneArray = cached->second.boneArray;
        }

        std::array<Vec3, kBoneIndices.size()> currentWorld{};
        std::array<bool, kBoneIndices.size()> currentValid{};
        bool anyCurrent = false;
        Transform componentTransform{};
        if (IsValidPointer(mesh) && IsValidPointer(boneArray) &&
            ReadValue(mesh + 0x210, componentTransform)) {
            const Matrix4 componentMatrix = TransformToMatrix(componentTransform);
            for (std::size_t index = 0; index < kBoneIndices.size(); ++index) {
                Transform boneTransform{};
                const std::uintptr_t address = boneArray +
                    static_cast<std::uintptr_t>(kBoneIndices[index]) * sizeof(Transform);
                if (!ReadValue(address, boneTransform)) continue;
                Vec3 world = MatrixTranslation(
                    Multiply(TransformToMatrix(boneTransform), componentMatrix));
                if (index == 0) world.z += 7.0f;
                if (!IsFinite(world)) continue;
                currentWorld[index] = world;
                currentValid[index] = true;
                anyCurrent = true;
            }
        }

        if (antiFlicker && anyCurrent) {
            BoneCacheEntry& entry = boneCache_[actor];
            if (IsValidPointer(mesh)) entry.mesh = mesh;
            if (IsValidPointer(boneArray)) entry.boneArray = boneArray;
            for (std::size_t index = 0; index < currentValid.size(); ++index) {
                if (!currentValid[index]) continue;
                entry.world[index] = currentWorld[index];
                entry.valid[index] = true;
                entry.boneUpdatedAt[index] = now;
            }
            entry.lastUpdatedAt = now;
            cached = boneCache_.find(actor);
        }

        std::array<Vec3, kBoneIndices.size()> world = currentWorld;
        std::array<bool, kBoneIndices.size()> worldValid = currentValid;
        if (antiFlicker && cached != boneCache_.end()) {
            for (std::size_t index = 0; index < worldValid.size(); ++index) {
                if (worldValid[index] || !cached->second.valid[index] ||
                    now - cached->second.boneUpdatedAt[index] > kCacheLifetime) {
                    continue;
                }
                world[index] = cached->second.world[index];
                worldValid[index] = true;
            }
        }
        bool anyProjected = false;
        for (std::size_t index = 0; index < worldValid.size(); ++index) {
            if (!worldValid[index]) continue;
            Vec2 screen{};
            if (!ProjectToScreen(world[index],
                                 view,
                                 options_.screenWidth,
                                 options_.screenHeight,
                                 screen) ||
                !IsInsideScreen(screen,
                                options_.screenWidth,
                                options_.screenHeight,
                                80.0f)) {
                continue;
            }
            frame.world[index] = world[index];
            frame.screen[index] = screen;
            frame.valid[index] = true;
            anyProjected = true;
        }
        return anyProjected;
    }

    static void BuildSkeletonVisual(const BoneFrame& frame,
                                    SkeletonVisual& skeleton) {
        skeleton.joints.resize(kBoneIndices.size());
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (!frame.valid[index]) continue;
            skeleton.joints[index] = BoneJoint{
                ImVec2(frame.screen[index].x, frame.screen[index].y), true};
        }
        skeleton.links.assign(kBoneLinks.begin(), kBoneLinks.end());
    }

    void BuildModelGeometry(const CameraView& view,
                            GameFrame& frame) const {
        if (!geometrySnapshotReady_) return;

        Vec3 forward{};
        Vec3 right{};
        Vec3 up{};
        CameraAxes(view, forward, right, up);
        (void)right;
        (void)up;
        const Vec3 target{
            view.location.x + forward.x * 100000.0f,
            view.location.y + forward.y * 100000.0f,
            view.location.z + forward.z * 100000.0f,
        };
        const native::GeometryRaycastHit hit = geometryRuntime_.Raycast(
            ToGeometryPoint(view.location), ToGeometryPoint(target));
        if (!hit || hit.mesh->indices.size() < 3 ||
            hit.mesh->vertices.size() < 3) {
            return;
        }

        constexpr std::size_t kMaximumRenderedTriangles = 2500;
        GeometryModelVisual model{};
        model.tone = SemanticTone::Ally;
        model.segments.reserve(kMaximumRenderedTriangles * 3);
        const std::size_t triangleCount = hit.mesh->indices.size() / 3;

        const auto appendTriangle = [&](std::size_t triangleIndex) {
            if (triangleIndex >= triangleCount ||
                model.segments.size() >= kMaximumRenderedTriangles * 3) {
                return;
            }
            const std::size_t base = triangleIndex * 3;
            const std::uint32_t firstIndex = hit.mesh->indices[base];
            const std::uint32_t secondIndex = hit.mesh->indices[base + 1];
            const std::uint32_t thirdIndex = hit.mesh->indices[base + 2];
            if (firstIndex >= hit.mesh->vertices.size() ||
                secondIndex >= hit.mesh->vertices.size() ||
                thirdIndex >= hit.mesh->vertices.size()) {
                return;
            }
            const auto& firstWorld = hit.mesh->vertices[firstIndex];
            const auto& secondWorld = hit.mesh->vertices[secondIndex];
            const auto& thirdWorld = hit.mesh->vertices[thirdIndex];
            Vec2 first{};
            Vec2 second{};
            Vec2 third{};
            if (!ProjectToScreen(
                    Vec3{firstWorld.x, firstWorld.y, firstWorld.z},
                    view,
                    options_.screenWidth,
                    options_.screenHeight,
                    first) ||
                !ProjectToScreen(
                    Vec3{secondWorld.x, secondWorld.y, secondWorld.z},
                    view,
                    options_.screenWidth,
                    options_.screenHeight,
                    second) ||
                !ProjectToScreen(
                    Vec3{thirdWorld.x, thirdWorld.y, thirdWorld.z},
                    view,
                    options_.screenWidth,
                    options_.screenHeight,
                    third)) {
                return;
            }
            constexpr float kProjectionMargin = 160.0f;
            if (!IsInsideScreen(
                    first,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin) &&
                !IsInsideScreen(
                    second,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin) &&
                !IsInsideScreen(
                    third,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin)) {
                return;
            }
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(first.x, first.y), ImVec2(second.x, second.y)});
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(second.x, second.y), ImVec2(third.x, third.y)});
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(third.x, third.y), ImVec2(first.x, first.y)});
        };

        const std::size_t hitTriangle =
            static_cast<std::size_t>(hit.triangleIndex);
        if (hitTriangle < triangleCount) appendTriangle(hitTriangle);
        const std::size_t stride = triangleCount <= kMaximumRenderedTriangles
            ? 1
            : (triangleCount - 1) / kMaximumRenderedTriangles + 1;
        for (std::size_t triangle = 0;
             triangle < triangleCount &&
             model.segments.size() < kMaximumRenderedTriangles * 3;
             triangle += stride) {
            if (triangle == hitTriangle) continue;
            appendTriangle(triangle);
        }
        if (!model.segments.empty()) {
            frame.modelGeometry = std::move(model);
        }
    }

    static std::size_t WeaponProfileIndex(std::uint64_t weaponId) {
        return WeaponProfileFor(weaponId);
    }

    static ui::AimTuning ResolveAimTuning(const ui::AimSettings& settings,
                                           std::uint64_t weaponId) {
        ui::AimTuning tuning = settings.weaponProfilesEnabled
            ? settings.weaponProfiles[WeaponProfileIndex(weaponId)]
            : settings.defaults;
        const auto finiteOr = [](float value, float fallback) {
            return std::isfinite(value) ? value : fallback;
        };
        tuning.rangePixels = std::clamp(finiteOr(tuning.rangePixels, 200.0f), 0.0f, 4000.0f);
        tuning.hipDistanceMeters = std::clamp(
            finiteOr(tuning.hipDistanceMeters, 50.0f), 0.0f, 1000.0f);
        tuning.hipSpeed = std::clamp(finiteOr(tuning.hipSpeed, 30.0f), 1.0f, 100.0f);
        tuning.adsSpeed = std::clamp(finiteOr(tuning.adsSpeed, 30.0f), 1.0f, 100.0f);
        tuning.horizontalSpeed = std::clamp(
            finiteOr(tuning.horizontalSpeed, 70.0f), 0.0f, 400.0f);
        tuning.verticalSpeed = std::clamp(
            finiteOr(tuning.verticalSpeed, 70.0f), 0.0f, 400.0f);
        tuning.prediction = std::clamp(finiteOr(tuning.prediction, 1.0f), 0.0f, 2.0f);
        tuning.recoil = std::clamp(finiteOr(tuning.recoil, 0.1f), 0.0f, 2.0f);
        tuning.smoothing = std::clamp(finiteOr(tuning.smoothing, 20.0f), 0.01f, 200.0f);
        tuning.hipBone = std::clamp(tuning.hipBone, 0, 7);
        tuning.adsBone = std::clamp(tuning.adsBone, 0, 7);
        return tuning;
    }

    static bool AimTriggered(const ui::AimSettings& settings,
                             const FrameContext& context) {
        switch (settings.triggerMode) {
            case 0: return context.firing;
            case 1: return context.zooming;
            case 2: return context.firing || context.zooming;
            default: return false;
        }
    }

    bool SelectAimPoint(const BoneFrame& frame,
                        int mode,
                        const ui::AimSettings& settings,
                        const Vec3& traceOrigin,
                        std::uint64_t identity,
                        std::uint64_t sequence,
                        int preferredBone,
                        Vec3& world,
                        Vec2& screen,
                        int& boneIndex) const {
        const auto selectable = [&](std::size_t index) {
            return index < frame.valid.size() && frame.valid[index] &&
                (!settings.missMode ||
                 frame.visibility[index] ==
                     native::GeometryVisibility::Visible);
        };
        const auto select = [&](int index) {
            if (index < 0 || index >= static_cast<int>(frame.valid.size()) ||
                !selectable(static_cast<std::size_t>(index))) {
                return false;
            }
            const std::size_t selected = static_cast<std::size_t>(index);
            world = frame.world[selected];
            screen = frame.screen[selected];
            boneIndex = index;
            return true;
        };
        const auto selectPair = [&](int first, int second) {
            const bool firstValid = selectable(static_cast<std::size_t>(first));
            const bool secondValid = selectable(static_cast<std::size_t>(second));
            if (firstValid && secondValid) {
                world = Vec3{
                    (frame.world[first].x + frame.world[second].x) * 0.5f,
                    (frame.world[first].y + frame.world[second].y) * 0.5f,
                    (frame.world[first].z + frame.world[second].z) * 0.5f,
                };
                screen = Vec2{
                    (frame.screen[first].x + frame.screen[second].x) * 0.5f,
                    (frame.screen[first].y + frame.screen[second].y) * 0.5f,
                };
                if (settings.missMode &&
                    TraceGeometry(traceOrigin, world) !=
                        native::GeometryVisibility::Visible) {
                    return select(first) || select(second);
                }
                boneIndex = first;
                return true;
            }
            return select(first) || select(second);
        };

        switch (mode) {
            case 0: if (select(0)) return true; break;
            case 1: if (select(1)) return true; break;
            case 2: if (select(2)) return true; break;
            case 3: if (selectPair(9, 10)) return true; break;
            case 4: if (selectPair(11, 12)) return true; break;
            case 5: if (selectPair(13, 14)) return true; break;
            case 6: {
                if (preferredBone >= 0 && select(preferredBone)) return true;
                constexpr std::array<int, 15> regions{
                    0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
                float totalWeight = 0.0f;
                for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                    if (!selectable(index)) continue;
                    const float weight = settings.randomBoneWeights[
                        static_cast<std::size_t>(regions[index])];
                    if (std::isfinite(weight) && weight > 0.0f) totalWeight += weight;
                }
                if (totalWeight > 0.0f) {
                    std::uint64_t hash = identity ^ ((sequence / 120U) + 0x9E3779B97F4A7C15ULL);
                    hash ^= hash >> 30U;
                    hash *= 0xBF58476D1CE4E5B9ULL;
                    hash ^= hash >> 27U;
                    hash *= 0x94D049BB133111EBULL;
                    hash ^= hash >> 31U;
                    float choice = static_cast<float>(hash % 1000000ULL) /
                        1000000.0f * totalWeight;
                    for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                        if (!selectable(index)) continue;
                        const float weight = settings.randomBoneWeights[
                            static_cast<std::size_t>(regions[index])];
                        if (!std::isfinite(weight) || weight <= 0.0f) continue;
                        if (choice <= weight) return select(static_cast<int>(index));
                        choice -= weight;
                    }
                }
                break;
            }
            case 7: {
                if (preferredBone >= 0 && select(preferredBone)) return true;
                const float centerX = static_cast<float>(options_.screenWidth) * 0.5f;
                const float centerY = static_cast<float>(options_.screenHeight) * 0.5f;
                float bestDistance = std::numeric_limits<float>::max();
                int bestIndex = -1;
                for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                    if (!selectable(index)) continue;
                    const float dx = frame.screen[index].x - centerX;
                    const float dy = frame.screen[index].y - centerY;
                    const float distance = dx * dx + dy * dy;
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestIndex = static_cast<int>(index);
                    }
                }
                if (bestIndex >= 0) return select(bestIndex);
                break;
            }
            default: break;
        }
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (select(static_cast<int>(index))) return true;
        }
        return false;
    }

    void PublishAimFrame(const ui::AimSettings& settings,
                         const ui::AimTuning& tuning,
                         const FrameContext& context,
                         const std::vector<AimCandidate>& candidates,
                         GameFrame& frame) {
        const bool enabled = settings.enabled &&
            aimEnabled_.load(std::memory_order_acquire) &&
            WeaponAllowsAim(context.weaponId);
        if (!enabled) {
            aimController_.ClearTarget();
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
            return;
        }
        const bool touchInput =
            options_.inputMode == ui::AimInputMode::WriteTouch ||
            options_.inputMode == ui::AimInputMode::KernelTouch;
        if (settings.showTouchArea && touchInput) {
            frame.touchRegion = TouchRegionVisual{
                ImVec2(settings.touchX, settings.touchY),
                settings.touchRange,
            };
        }

        const bool triggered = AimTriggered(settings, context);
        const AimCandidate* selected = nullptr;
        bool lockedTargetMissing = false;
        if (triggered && settings.persistentLock && lockedAimIdentity_ != 0) {
            const auto found = std::find_if(
                candidates.begin(), candidates.end(), [this](const AimCandidate& candidate) {
                    return candidate.identity == lockedAimIdentity_;
                });
            if (found != candidates.end()) selected = &*found;
            else lockedTargetMissing = true;
        }
        if (selected == nullptr && !lockedTargetMissing && !candidates.empty()) {
            selected = &*std::min_element(
                candidates.begin(), candidates.end(), [&settings](
                    const AimCandidate& left, const AimCandidate& right) {
                    return settings.targetAlgorithm == 0
                        ? left.screenDistancePixels < right.screenDistancePixels
                        : left.worldDistanceMeters < right.worldDistanceMeters;
                });
        }

        AimGuide guide{};
        guide.center = ImVec2(
            static_cast<float>(options_.screenWidth) * 0.5f,
            static_cast<float>(options_.screenHeight) * 0.5f);
        guide.radius = tuning.rangePixels;
        guide.drawCircle = settings.drawRange;
        guide.drawTargetRay = settings.drawTargetRay && selected != nullptr;
        if (selected != nullptr) {
            guide.target = ImVec2(selected->screen.x, selected->screen.y);
            guide.targetValid = true;
        }

        if (!triggered || selected == nullptr) {
            aimController_.ClearTarget();
            if (!triggered || !settings.persistentLock) {
                lockedAimIdentity_ = 0;
                lockedAimBone_ = -1;
            }
            if (guide.drawCircle || guide.drawTargetRay) frame.aimGuide = guide;
            return;
        }

        if (settings.persistentLock) {
            lockedAimIdentity_ = selected->identity;
            lockedAimBone_ = selected->boneIndex;
            guide.locked = true;
        } else {
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
        }

        aim::TargetSnapshot snapshot{};
        snapshot.valid = true;
        snapshot.identity = selected->identity;
        snapshot.world = aim::Vec3{selected->world.x, selected->world.y, selected->world.z};
        snapshot.velocity = aim::Vec3{
            selected->velocity.x, selected->velocity.y, selected->velocity.z};
        snapshot.screenDistancePixels = selected->screenDistancePixels;
        snapshot.worldDistanceMeters = selected->worldDistanceMeters;
        snapshot.projectileSpeedCmPerSecond = ResolveProjectileSpeed(context);
        snapshot.firing = context.firing;
        snapshot.zooming = context.zooming;
        snapshot.curvedMotion = settings.curvedMotion;
        snapshot.triggerMode = settings.triggerMode;
        snapshot.orientation = options_.orientation;
        snapshot.touchRange = settings.touchRange;
        snapshot.touchCenterX = settings.touchX;
        snapshot.touchCenterY = settings.touchY;
        snapshot.tuning = tuning;
        snapshot.view.location = aim::Vec3{
            context.view.location.x, context.view.location.y, context.view.location.z};
        snapshot.view.pitch = context.view.rotation.pitch;
        snapshot.view.yaw = context.view.rotation.yaw;
        snapshot.view.roll = context.view.rotation.roll;
        snapshot.view.fieldOfView = context.view.fieldOfView;
        snapshot.view.halfWidth = static_cast<float>(options_.screenWidth) * 0.5f;
        snapshot.view.halfHeight = static_cast<float>(options_.screenHeight) * 0.5f;
        aimController_.Publish(snapshot);
        if (guide.drawCircle || guide.drawTargetRay) frame.aimGuide = guide;
    }

    void AddRadarBlip(const FrameContext& context,
                      const Vec3& position,
                      bool bot,
                      std::string_view className,
                      float headingRadians,
                      bool headingValid,
                      SemanticTone tone,
                      RadarVisual& radar) {
        const Vec3 delta = Subtract(position, context.localPosition);
        const float yaw = context.view.rotation.yaw * kPi / 180.0f;
        const float forwardX = std::cos(yaw);
        const float forwardY = std::sin(yaw);
        const float rightX = -forwardY;
        const float rightY = forwardX;
        const float rangeCentimeters = std::max(1.0f, radar.maxDistanceMeters) * 100.0f;
        RadarBlip blip{};
        blip.normalizedPosition = ImVec2(
            (delta.x * rightX + delta.y * rightY) / rangeCentimeters,
            -(delta.x * forwardX + delta.y * forwardY) / rangeCentimeters);
        blip.headingRadians = headingRadians;
        blip.headingValid = headingValid;
        blip.kind = bot ? RadarBlipKind::Bot : RadarBlipKind::Player;
        blip.tone = tone;
        if (className.find("Boss") != std::string_view::npos &&
            tone != SemanticTone::Accent &&
            tone != SemanticTone::Muted) {
            blip.tone = SemanticTone::Danger;
        }
        radar.blips.push_back(std::move(blip));
    }

    float ResolveProjectileSpeed(const FrameContext& context) {
        if (!IsValidPointer(context.weaponRoot)) {
            projectileSpeedReader_.Invalidate();
            return ProjectileSpeedFor(context.weaponId);
        }
        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        auto resolveName = [this](std::uint32_t index) {
            return DecodeName(static_cast<std::int32_t>(index),
                              moduleBase_ + layout_.namePoolOffset);
        };
        const std::optional<float> speed = projectileSpeedReader_.Read(
            context.weaponRoot, readBytes, resolveName);
        return speed.value_or(ProjectileSpeedFor(context.weaponId));
    }

    void PruneThreatState(
        const std::unordered_set<std::uintptr_t>& seenThreats) {
        const auto prune = [&seenThreats](auto& states) {
            for (auto iterator = states.begin(); iterator != states.end();) {
                if (seenThreats.find(iterator->first) == seenThreats.end()) {
                    iterator = states.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        };
        prune(projectileTrails_);
        prune(threatFirstSeen_);
    }

    void PruneBoneCache(std::chrono::steady_clock::time_point now) {
        constexpr auto kRetention = std::chrono::seconds(1);
        for (auto iterator = boneCache_.begin(); iterator != boneCache_.end();) {
            if (now - iterator->second.lastUpdatedAt > kRetention) {
                iterator = boneCache_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void PrunePositionCache(std::chrono::steady_clock::time_point now) {
        constexpr auto kRetention = std::chrono::milliseconds(300);
        for (auto iterator = positionCache_.begin();
             iterator != positionCache_.end();) {
            if (now - iterator->second.updatedAt > kRetention) {
                iterator = positionCache_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void ResetWorldState() {
        world_ = 0;
        positionCache_.clear();
        boneCache_.clear();
        nameCache_.clear();
        itemMetadata_.clear();
        projectileTrails_.clear();
        threatFirstSeen_.clear();
        aimWarningStates_.clear();
        hudMapCache_.Clear();
        characterPositions_.Clear();
        positionReadMode_ = native::PositionReadMode::Standard;
        projectileSpeedReader_.Invalidate();
        lockedAimIdentity_ = 0;
        lockedAimBone_ = -1;
        aimController_.ClearTarget();
        lastViewValid_ = false;
        lastView_ = CameraView{};
    }

    void CloseLocked() noexcept {
        opened_ = false;
        aimController_.Stop();
        geometryRuntime_.Stop();
        geometrySnapshotReady_ = false;
        geometryRefreshEpoch_ = 0;
        geometryRetryAfter_ = {};
        if (memory_ != nullptr) memory_->Close();
        memory_.reset();
        processId_ = -1;
        moduleBase_ = 0;
        layout_ = VersionLayout{};
        options_ = RuntimeOptions{};
        customItemPath_.clear();
        customItems_.Clear();
        ResetWorldState();
        aimEnabled_.store(false, std::memory_order_release);
    }

    std::mutex mutex_;
    RuntimeOptions options_{};
    VersionLayout layout_{};
    std::unique_ptr<native::MemoryTransport> memory_;
    pid_t processId_ = -1;
    std::uintptr_t moduleBase_ = 0;
    std::uintptr_t world_ = 0;
    CameraView lastView_{};
    bool lastViewValid_ = false;
    bool opened_ = false;
    std::atomic_bool aimEnabled_{false};
    std::unordered_map<std::uintptr_t, PositionCacheEntry> positionCache_;
    std::unordered_map<std::uintptr_t, BoneCacheEntry> boneCache_;
    std::unordered_map<std::int32_t, std::string> nameCache_;
    std::unordered_map<std::uint64_t, std::pair<int, int>> itemMetadata_;
    std::unordered_map<std::uintptr_t, std::vector<Vec3>> projectileTrails_;
    std::unordered_map<
        std::uintptr_t,
        std::chrono::steady_clock::time_point> threatFirstSeen_;
    std::unordered_map<std::uintptr_t, AimWarningState> aimWarningStates_;
    native::HudMapCache hudMapCache_{};
    native::CharacterPositionResolver characterPositions_{};
    native::PositionReadMode positionReadMode_ = native::PositionReadMode::Standard;
    native::ProjectileSpeedReader projectileSpeedReader_{};
    native::GeometryRuntime geometryRuntime_{};
    bool geometrySnapshotReady_ = false;
    std::uint64_t geometryRefreshEpoch_ = 0;
    std::chrono::steady_clock::time_point geometryRetryAfter_{};
    std::uint64_t lockedAimIdentity_ = 0;
    int lockedAimBone_ = -1;
    aim::AimController aimController_;
    data::CustomItemCatalog customItems_;
    std::string customItemPath_;
};

std::unique_ptr<GameBackend> CreateNativeGameBackend() {
    return std::make_unique<NativeGameBackend>();
}

}  // namespace lengjing::game
