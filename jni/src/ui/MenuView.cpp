#include "ui/MenuView.h"

#include "game/ProjectileTrackingFeature.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <string>

namespace lengjing::ui {
namespace {

constexpr ImVec4 kCanvas{0.055f, 0.071f, 0.071f, 0.98f};
constexpr ImVec4 kPanel{0.068f, 0.088f, 0.094f, 1.00f};
constexpr ImVec4 kPanelRaised{0.094f, 0.120f, 0.122f, 1.00f};
constexpr ImVec4 kCard{0.078f, 0.101f, 0.105f, 1.00f};
constexpr ImVec4 kBorder{0.160f, 0.216f, 0.207f, 1.00f};
constexpr ImVec4 kText{0.910f, 0.933f, 0.918f, 1.00f};
constexpr ImVec4 kMuted{0.560f, 0.620f, 0.592f, 1.00f};
constexpr ImVec4 kAccent{0.106f, 0.710f, 0.537f, 1.00f};
constexpr ImVec4 kAccentHover{0.153f, 0.804f, 0.620f, 1.00f};
constexpr ImVec4 kAmber{0.839f, 0.643f, 0.263f, 1.00f};
constexpr ImVec4 kRed{0.867f, 0.325f, 0.325f, 1.00f};
constexpr ImVec4 kGreen{0.235f, 0.745f, 0.420f, 1.00f};
constexpr ImVec4 kButtonText{0.945f, 0.975f, 0.958f, 1.00f};
constexpr ImVec4 kPrimaryButtonText{0.025f, 0.105f, 0.080f, 1.00f};

constexpr std::array<const char*, 6> kPageNames{
    "运行", "视觉", "物资", "雷达", "瞄准", "系统"};

constexpr std::array<Page, 6> kPages{
    Page::Runtime,
    Page::Visual,
    Page::Loot,
    Page::Radar,
    Page::Aim,
    Page::System,
};

constexpr std::array<const char*, 3> kGameVersions{
    "国服", "国际服", "台服"};

constexpr std::array<const char*, 5> kInputModes{
    "只读", "写入触摸（不推荐）", "程序陀螺仪", "内核触摸", "内核陀螺仪"};

constexpr std::array<const char*, 7> kFrameLimits{
    "30 FPS", "60 FPS", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "无限制"};

constexpr std::array<const char*, 3> kRenderBackends{
    "CPU", "Vulkan", "OpenGL"};

constexpr std::array<const char*, kWeaponProfileCount> kWeaponProfiles{
    "步枪", "冲锋枪", "霰弹枪", "轻机枪", "射手步枪",
    "狙击步枪", "手枪", "复合弓", "默认"};

constexpr std::array<const char*, 3> kTriggerModes{
    "开火", "开镜", "开镜开火"};

constexpr std::array<const char*, 2> kTargetAlgorithms{
    "屏幕距离最近", "世界距离最近"};

constexpr std::array<const char*, 8> kAimBones{
    "头部", "胸部", "腰部", "腿部", "膝盖", "脚部", "随机部位", "准星最近"};

constexpr std::array<const char*, 2> kCoverModes{
    "全身可见部位", "仅指定部位"};

constexpr std::array<const char*, kRandomBoneCount> kRandomBoneNames{
    "头部", "胸部", "腰部", "肩部", "手肘", "手腕", "大腿", "膝盖", "脚踝"};

constexpr std::array<const char*, kContainerKindCount> kContainerNames{
    "电脑机箱", "大武器箱", "大工具箱", "一件衣服", "小保险箱", "查看行动",
    "收纳盒", "弹药箱", "登山包", "返回舱", "野外物资箱", "航空储物箱",
    "楼梯", "高级储物箱", "医疗物资堆", "电脑", "高级旅行箱", "保险箱",
    "保险柜", "武器箱", "快递箱", "服务器", "旅行箱", "储物柜", "医疗包",
    "手提箱", "抽屉柜", "工具柜", "旅行袋", "藏匿物", "实验服", "垃圾桶",
    "货柜车", "搅拌车", "密码锁", "铁丝网", "鸟窝", "密码门", "撤离点",
    "拉闸", "侦察"};

static_assert(kContainerNames.size() == kContainerKindCount);

struct MenuStyleScope {
    int colorCount = 0;
    int variableCount = 0;

    MenuStyleScope() {
        PushVar(ImGuiStyleVar_WindowRounding, 18.0f);
        PushVar(ImGuiStyleVar_ChildRounding, 14.0f);
        PushVar(ImGuiStyleVar_FrameRounding, 10.0f);
        PushVar(ImGuiStyleVar_PopupRounding, 12.0f);
        PushVar(ImGuiStyleVar_ScrollbarRounding, 10.0f);
        PushVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 14.0f));
        PushVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 9.0f));
        PushVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 9.0f));
        PushVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

        PushColor(ImGuiCol_Text, kText);
        PushColor(ImGuiCol_TextDisabled, kMuted);
        PushColor(ImGuiCol_WindowBg, kCanvas);
        PushColor(ImGuiCol_ChildBg, kPanel);
        PushColor(ImGuiCol_PopupBg, kPanelRaised);
        PushColor(ImGuiCol_Border, kBorder);
        PushColor(ImGuiCol_FrameBg, kPanelRaised);
        PushColor(ImGuiCol_FrameBgHovered, ImVec4(0.137f, 0.176f, 0.165f, 1.0f));
        PushColor(ImGuiCol_FrameBgActive, ImVec4(0.157f, 0.204f, 0.188f, 1.0f));
        PushColor(ImGuiCol_Button, kPanelRaised);
        PushColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.196f, 0.180f, 1.0f));
        PushColor(ImGuiCol_ButtonActive, ImVec4(0.169f, 0.235f, 0.212f, 1.0f));
        PushColor(ImGuiCol_Header, ImVec4(0.110f, 0.298f, 0.247f, 1.0f));
        PushColor(ImGuiCol_HeaderHovered, ImVec4(0.125f, 0.373f, 0.298f, 1.0f));
        PushColor(ImGuiCol_HeaderActive, ImVec4(0.137f, 0.424f, 0.333f, 1.0f));
        PushColor(ImGuiCol_CheckMark, kAccent);
        PushColor(ImGuiCol_SliderGrab, kAccent);
        PushColor(ImGuiCol_SliderGrabActive, kAccentHover);
        PushColor(ImGuiCol_Separator, kBorder);
        PushColor(ImGuiCol_ScrollbarBg, ImVec4(0.055f, 0.071f, 0.071f, 0.60f));
        PushColor(ImGuiCol_ScrollbarGrab, ImVec4(0.235f, 0.294f, 0.271f, 1.0f));
        PushColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.302f, 0.376f, 0.345f, 1.0f));
    }

    ~MenuStyleScope() {
        ImGui::PopStyleColor(colorCount);
        ImGui::PopStyleVar(variableCount);
    }

    void PushColor(ImGuiCol index, const ImVec4& color) {
        ImGui::PushStyleColor(index, color);
        ++colorCount;
    }

    void PushVar(ImGuiStyleVar index, float value) {
        ImGui::PushStyleVar(index, value);
        ++variableCount;
    }

    void PushVar(ImGuiStyleVar index, const ImVec2& value) {
        ImGui::PushStyleVar(index, value);
        ++variableCount;
    }
};

enum class ActionTone {
    Primary,
    Neutral,
    Danger,
};

const char* PageName(Page page) {
    for (std::size_t i = 0; i < kPages.size(); ++i) {
        if (kPages[i] == page) {
            return kPageNames[i];
        }
    }
    return kPageNames[0];
}

float AnimateToward(float current, float target, float speed) {
    const float deltaTime = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.10f);
    return current + (target - current) * std::min(1.0f, deltaTime * speed);
}

ImVec4 Mix(const ImVec4& from, const ImVec4& to, float amount) {
    amount = std::clamp(amount, 0.0f, 1.0f);
    return ImVec4(
        from.x + (to.x - from.x) * amount,
        from.y + (to.y - from.y) * amount,
        from.z + (to.z - from.z) * amount,
        from.w + (to.w - from.w) * amount);
}

struct SectionLayoutState {
    bool active = false;
    bool cardOpen = false;
    int columnCount = 1;
    int nextColumn = 0;
    int activeColumn = 0;
    float originX = 0.0f;
    float originY = 0.0f;
    float width = 0.0f;
    float columnWidth = 0.0f;
    float horizontalGap = 0.0f;
    float verticalGap = 0.0f;
    std::array<float, 2> nextY{};
};

SectionLayoutState gSectionLayout;

void CloseSectionCard() {
    if (!gSectionLayout.cardOpen) {
        return;
    }
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
    const float cardBottom =
        ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
    ImGui::EndChild();
    gSectionLayout.nextY[static_cast<std::size_t>(
        gSectionLayout.activeColumn)] =
        cardBottom + gSectionLayout.verticalGap;
    gSectionLayout.cardOpen = false;
}

void BeginSectionLayout(const char* id) {
    gSectionLayout = {};
    gSectionLayout.active = true;
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    gSectionLayout.originX = origin.x;
    gSectionLayout.originY = origin.y;
    gSectionLayout.width = ImGui::GetContentRegionAvail().x;
    gSectionLayout.columnCount =
        gSectionLayout.width >= 760.0f ? 2 : 1;
    gSectionLayout.horizontalGap =
        gSectionLayout.columnCount > 1
            ? ImGui::GetStyle().ItemSpacing.x
            : 0.0f;
    gSectionLayout.verticalGap = 4.0f;
    gSectionLayout.columnWidth =
        (gSectionLayout.width - gSectionLayout.horizontalGap) /
        static_cast<float>(gSectionLayout.columnCount);
    gSectionLayout.nextY.fill(origin.y);
}

void EndSectionLayout() {
    CloseSectionCard();
    float bottom = gSectionLayout.originY;
    for (int column = 0; column < gSectionLayout.columnCount; ++column) {
        bottom = std::max(
            bottom,
            gSectionLayout.nextY[static_cast<std::size_t>(column)] -
                gSectionLayout.verticalGap);
    }
    ImGui::SetCursorScreenPos(ImVec2(gSectionLayout.originX, bottom));
    ImGui::Dummy(ImVec2(gSectionLayout.width, 0.0f));
    ImGui::PopID();
    gSectionLayout = {};
}

void SectionTitle(const char* title, float height = 0.0f) {
    if (gSectionLayout.active) {
        CloseSectionCard();
        const int column = gSectionLayout.nextColumn;
        gSectionLayout.nextColumn =
            (gSectionLayout.nextColumn + 1) % gSectionLayout.columnCount;
        gSectionLayout.activeColumn = column;
        const ImVec2 cardPosition{
            gSectionLayout.originX +
                static_cast<float>(column) *
                    (gSectionLayout.columnWidth +
                     gSectionLayout.horizontalGap),
            gSectionLayout.nextY[static_cast<std::size_t>(column)]};
        ImGui::SetCursorScreenPos(cardPosition);
        ImGui::SetNextWindowPos(cardPosition, ImGuiCond_Always);
        ImGuiChildFlags childFlags = ImGuiChildFlags_Border;
        if (height <= 0.0f) {
            childFlags |= ImGuiChildFlags_AutoResizeY |
                ImGuiChildFlags_AlwaysAutoResize;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
        ImGui::BeginChild(
            title,
            ImVec2(
                gSectionLayout.columnWidth,
                std::max(0.0f, height)),
            childFlags,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        gSectionLayout.cardOpen = true;
    }

    const ImVec2 marker = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        marker,
        ImVec2(marker.x + 4.0f, marker.y + ImGui::GetTextLineHeight()),
        ImGui::GetColorU32(kAccent),
        2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    ImGui::TextColored(kText, "%s", title);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
}

bool Toggle(const char* label, bool& value) {
    ImGui::PushID(label);

    const float rowHeight = 38.0f;
    const float switchWidth = 42.0f;
    const float switchHeight = 22.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(100.0f, ImGui::GetContentRegionAvail().x);

    ImGui::InvisibleButton("##toggle", ImVec2(width, rowHeight));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool pressed = ImGui::IsItemClicked();
    if (pressed) {
        value = !value;
    }

    const ImGuiID itemId = ImGui::GetItemID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float* switchMotion = storage->GetFloatRef(
        itemId ^ 0x53a91f27U, value ? 1.0f : 0.0f);
    float* hoverMotion = storage->GetFloatRef(
        itemId ^ 0x19d47b63U, 0.0f);
    float* pressMotion = storage->GetFloatRef(
        itemId ^ 0x7c31a4d9U, 0.0f);
    *switchMotion = AnimateToward(*switchMotion, value ? 1.0f : 0.0f, 13.0f);
    *hoverMotion = AnimateToward(*hoverMotion, hovered ? 1.0f : 0.0f, 15.0f);
    *pressMotion = AnimateToward(*pressMotion, held ? 1.0f : 0.0f, 20.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (*hoverMotion > 0.01f) {
        draw->AddRectFilled(
            origin,
            ImVec2(origin.x + width, origin.y + rowHeight),
            ImGui::GetColorU32(ImVec4(
                kAccent.x,
                kAccent.y,
                kAccent.z,
                0.055f * *hoverMotion)),
            8.0f);
    }
    const ImVec2 labelSize = ImGui::CalcTextSize(label);
    draw->AddText(
        ImVec2(origin.x, origin.y + (rowHeight - labelSize.y) * 0.5f),
        ImGui::GetColorU32(Mix(
            ImVec4(0.78f, 0.82f, 0.80f, 1.0f),
            kText,
            *hoverMotion)),
        label);

    const float switchX = origin.x + width - switchWidth;
    const float switchY = origin.y + (rowHeight - switchHeight) * 0.5f;
    const ImVec4 offTrack = Mix(
        ImVec4(0.20f, 0.25f, 0.25f, 1.0f),
        ImVec4(0.29f, 0.36f, 0.34f, 1.0f),
        *hoverMotion);
    const ImVec4 onTrack = Mix(kAccent, kAccentHover, *hoverMotion);
    const ImVec4 track = Mix(offTrack, onTrack, *switchMotion);
    draw->AddRectFilled(
        ImVec2(switchX, switchY),
        ImVec2(switchX + switchWidth, switchY + switchHeight),
        ImGui::GetColorU32(track), switchHeight * 0.5f);
    draw->AddRect(
        ImVec2(switchX, switchY),
        ImVec2(switchX + switchWidth, switchY + switchHeight),
        ImGui::GetColorU32(Mix(kBorder, kAccentHover, *switchMotion)),
        switchHeight * 0.5f);

    const float knobRadius = 8.0f;
    const float knobX =
        switchX + 11.0f + (switchWidth - 22.0f) * *switchMotion;
    draw->AddCircleFilled(
        ImVec2(knobX, switchY + switchHeight * 0.5f),
        knobRadius - *pressMotion * 0.8f,
        ImGui::GetColorU32(ImVec4(0.95f, 0.97f, 0.96f, 1.0f)));

    ImGui::PopID();
    return pressed;
}

bool GridToggle(const char* label, bool& value) {
    ImGui::TableNextColumn();
    return Toggle(label, value);
}

int ToggleGridColumns() {
    const float width = ImGui::GetContentRegionAvail().x;
    if (width >= 720.0f) {
        return 3;
    }
    return width >= 460.0f ? 2 : 1;
}

bool BeginToggleGrid(const char* id) {
    return ImGui::BeginTable(
        id, ToggleGridColumns(),
        ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings);
}

template <std::size_t N>
bool Combo(const char* label, int& selected, const std::array<const char*, N>& options) {
    const int safeIndex = std::clamp(selected, 0, static_cast<int>(N) - 1);
    bool changed = false;
    ImGui::TextColored(kMuted, "%s", label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo(
            "##value", options[static_cast<std::size_t>(safeIndex)])) {
        for (std::size_t i = 0; i < N; ++i) {
            const bool active = safeIndex == static_cast<int>(i);
            if (ImGui::Selectable(options[i], active)) {
                selected = static_cast<int>(i);
                changed = true;
            }
            if (active) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

bool DriverCombo(RuntimeModel& runtime) {
    const bool valid = runtime.driverIndex >= 0 &&
        runtime.driverIndex < static_cast<int>(runtime.driverOptions.size());
    const char* preview = valid
        ? runtime.driverOptions[static_cast<std::size_t>(runtime.driverIndex)].c_str()
        : "未配置";
    bool changed = false;
    ImGui::TextColored(kMuted, "驱动");
    ImGui::PushID("runtime_driver");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##value", preview)) {
        for (std::size_t i = 0; i < runtime.driverOptions.size(); ++i) {
            const bool active = runtime.driverIndex == static_cast<int>(i);
            if (ImGui::Selectable(runtime.driverOptions[i].c_str(), active)) {
                runtime.driverIndex = static_cast<int>(i);
                changed = true;
            }
            if (active) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void SliderCaption(const char* label, const char* value) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float availableWidth =
        std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float lineHeight = ImGui::GetTextLineHeight();
    const ImVec2 valueSize = ImGui::CalcTextSize(value);
    const float valueX = std::max(
        origin.x,
        origin.x + availableWidth - valueSize.x);
    const float labelMaximumX = std::max(origin.x, valueX - 10.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(
        origin,
        ImVec2(labelMaximumX, origin.y + lineHeight),
        true);
    draw->AddText(origin, ImGui::GetColorU32(kMuted), label);
    draw->PopClipRect();
    draw->AddText(
        ImVec2(valueX, origin.y),
        ImGui::GetColorU32(kText),
        value);
    ImGui::Dummy(ImVec2(0.0f, lineHeight));
}

bool SliderIntRow(
    const char* label,
    int* value,
    int minimum,
    int maximum,
    const char* format) {
    char formatted[64]{};
    std::snprintf(formatted, sizeof(formatted), format, *value);
    SliderCaption(label, formatted);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImVec4 transparentText = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    transparentText.w = 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, transparentText);
    const bool changed =
        ImGui::SliderInt("##value", value, minimum, maximum, format);
    ImGui::PopStyleColor();
    ImGui::PopID();
    return changed;
}

bool SliderFloatRow(
    const char* label,
    float* value,
    float minimum,
    float maximum,
    const char* format) {
    char formatted[64]{};
    std::snprintf(formatted, sizeof(formatted), format, *value);
    SliderCaption(label, formatted);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    ImVec4 transparentText = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    transparentText.w = 0.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, transparentText);
    const bool changed =
        ImGui::SliderFloat("##value", value, minimum, maximum, format);
    ImGui::PopStyleColor();
    ImGui::PopID();
    return changed;
}

bool AnimatedButton(
    const char* label,
    const ImVec2& requestedSize,
    const ImVec4& base,
    const ImVec4& hoveredColor,
    const ImVec4& pressedColor,
    const ImVec4& textColor,
    bool selected = false) {
    ImGui::PushID(label);
    const ImVec2 size(
        std::max(1.0f, requestedSize.x),
        std::max(1.0f, requestedSize.y));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##motion_button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    const ImGuiID itemId = ImGui::GetItemID();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    float* hoverMotion = storage->GetFloatRef(
        itemId ^ 0x41ac73d5U, selected ? 1.0f : 0.0f);
    float* pressMotion = storage->GetFloatRef(
        itemId ^ 0x2b968e41U, 0.0f);
    *hoverMotion = AnimateToward(
        *hoverMotion,
        hovered || selected ? 1.0f : 0.0f,
        14.0f);
    *pressMotion = AnimateToward(
        *pressMotion,
        held ? 1.0f : 0.0f,
        20.0f);

    const float inset = *pressMotion * 2.0f;
    const ImVec2 minimum(origin.x + inset, origin.y + inset);
    const ImVec2 maximum(
        origin.x + size.x - inset,
        origin.y + size.y - inset);
    const ImVec4 color = Mix(
        Mix(base, hoveredColor, *hoverMotion),
        pressedColor,
        *pressMotion);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (*hoverMotion > 0.02f) {
        draw->AddRectFilled(
            ImVec2(minimum.x - 2.0f, minimum.y - 2.0f),
            ImVec2(maximum.x + 2.0f, maximum.y + 2.0f),
            ImGui::GetColorU32(ImVec4(
                kAccent.x,
                kAccent.y,
                kAccent.z,
                0.055f * *hoverMotion)),
            11.0f);
    }
    draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(color), 9.0f);
    draw->AddRect(
        minimum,
        maximum,
        ImGui::GetColorU32(Mix(kBorder, kAccent, selected ? 0.72f : 0.12f)),
        9.0f);
    if (selected) {
        draw->AddRectFilled(
            ImVec2(minimum.x + 12.0f, maximum.y - 3.0f),
            ImVec2(maximum.x - 12.0f, maximum.y - 1.0f),
            ImGui::GetColorU32(kAccent),
            1.0f);
    }

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->PushClipRect(
        ImVec2(minimum.x + 4.0f, minimum.y),
        ImVec2(maximum.x - 4.0f, maximum.y),
        true);
    draw->AddText(
        ImVec2(
            origin.x + (size.x - textSize.x) * 0.5f,
            origin.y + (size.y - textSize.y) * 0.5f + *pressMotion),
        ImGui::GetColorU32(textColor),
        label);
    draw->PopClipRect();
    ImGui::PopID();
    return clicked;
}

template <std::size_t N>
bool SegmentedChoice(
    const char* id,
    int& selected,
    const std::array<const char*, N>& options) {
    ImGui::PushID(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float width = ImGui::GetContentRegionAvail().x;
    const bool vertical =
        width < 60.0f * static_cast<float>(N) +
            spacing * static_cast<float>(N - 1);
    const float buttonWidth = vertical
        ? width
        : std::max(
              1.0f,
              (width - spacing * static_cast<float>(N - 1)) /
                  static_cast<float>(N));
    bool changed = false;

    for (std::size_t i = 0; i < N; ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool active = selected == static_cast<int>(i);
        const ImVec4 base = active
            ? ImVec4(0.10f, 0.42f, 0.33f, 1.0f)
            : ImVec4(0.10f, 0.13f, 0.13f, 1.0f);
        if (AnimatedButton(
                options[i],
                ImVec2(buttonWidth, 40.0f),
                base,
                ImVec4(0.12f, 0.50f, 0.39f, 1.0f),
                ImVec4(0.14f, 0.56f, 0.43f, 1.0f),
                active ? kText : kButtonText,
                active) &&
            !active) {
            selected = static_cast<int>(i);
            changed = true;
        }
        ImGui::PopID();
        if (!vertical && i + 1 < N) {
            ImGui::SameLine();
        }
    }

    ImGui::PopID();
    return changed;
}

bool ActionButton(const char* label, ActionTone tone, const ImVec2& size) {
    ImVec4 base = kAccent;
    ImVec4 hovered = kAccentHover;
    ImVec4 active{0.08f, 0.60f, 0.45f, 1.0f};
    ImVec4 textColor = kPrimaryButtonText;
    if (tone == ActionTone::Neutral) {
        base = ImVec4(0.18f, 0.22f, 0.21f, 1.0f);
        hovered = ImVec4(0.23f, 0.28f, 0.26f, 1.0f);
        active = ImVec4(0.27f, 0.33f, 0.30f, 1.0f);
        textColor = kButtonText;
    } else if (tone == ActionTone::Danger) {
        base = ImVec4(0.60f, 0.20f, 0.20f, 1.0f);
        hovered = ImVec4(0.70f, 0.24f, 0.24f, 1.0f);
        active = ImVec4(0.52f, 0.15f, 0.15f, 1.0f);
        textColor = kButtonText;
    }
    return AnimatedButton(
        label, size, base, hovered, active, textColor);
}

bool NavButton(const char* label, bool active, const ImVec2& size) {
    return AnimatedButton(
        label,
        size,
        active ? ImVec4(0.10f, 0.31f, 0.25f, 1.0f)
               : ImVec4(0.06f, 0.08f, 0.08f, 0.0f),
        active ? ImVec4(0.12f, 0.38f, 0.30f, 1.0f)
               : kPanelRaised,
        ImVec4(0.13f, 0.43f, 0.34f, 1.0f),
        active ? kText : kButtonText,
        active);
}

void StatusMetric(const char* label, const std::string& value, const ImVec4& color) {
    ImGui::TableNextColumn();
    ImGui::TextColored(kMuted, "%s", label);
    ImGui::TextColored(color, "%s", value.c_str());
}

void Mark(UiActions& actions, SettingsDomain domain, bool changed) {
    if (changed) {
        actions.SettingsChanged(domain);
    }
}

bool AllVisualEnabled(const VisualSettings& visual) {
    return visual.box && visual.snapline && visual.skeleton && visual.distance &&
        visual.playerName && visual.health && visual.offscreenWarning &&
        visual.operatorName && visual.heldWeapon && visual.armorLevel &&
        visual.armorDurability && visual.downedPlayer && visual.nearbyEnemy &&
        visual.crosshair && visual.throwableWarning && visual.aimWarning &&
        visual.aimWarningRay && visual.playerViewRay;
}

void SetAllVisual(VisualSettings& visual, bool value) {
    visual.box = value;
    visual.snapline = value;
    visual.skeleton = value;
    visual.distance = value;
    visual.playerName = value;
    visual.health = value;
    visual.offscreenWarning = value;
    visual.operatorName = value;
    visual.heldWeapon = value;
    visual.armorLevel = value;
    visual.armorDurability = value;
    visual.downedPlayer = value;
    visual.nearbyEnemy = value;
    visual.crosshair = value;
    visual.throwableWarning = value;
    visual.aimWarning = value;
    visual.aimWarningRay = value;
    visual.playerViewRay = value;
}

bool AllLootEnabled(const LootSettings& loot) {
    return loot.playerBox && loot.botBox && loot.password && loot.containers &&
        loot.boxContents && loot.containerContents && loot.highValueList;
}

void SetAllLoot(LootSettings& loot, bool value) {
    loot.playerBox = value;
    loot.botBox = value;
    loot.password = value;
    loot.containers = value;
    loot.boxContents = value;
    loot.containerContents = value;
    loot.highValueList = value;
}

bool AllContainersEnabled(const LootSettings& loot) {
    return std::all_of(
        loot.containerKinds.begin(), loot.containerKinds.end(),
        [](bool value) { return value; });
}

bool AimQuickOptionsEnabled(const AimSettings& aim) {
    return aim.ignoreBots && aim.persistentLock && aim.drawRange && aim.drawTargetRay;
}

void SetAimQuickOptions(AimSettings& aim, bool value) {
    aim.ignoreBots = value;
    aim.persistentLock = value;
    aim.drawRange = value;
    aim.drawTargetRay = value;
}

AimTuning& CurrentAimTuning(AimSettings& aim) {
    if (!aim.weaponProfilesEnabled) {
        return aim.defaults;
    }
    aim.weaponProfileIndex = std::clamp(
        aim.weaponProfileIndex, 0, static_cast<int>(kWeaponProfileCount) - 1);
    return aim.weaponProfiles[static_cast<std::size_t>(aim.weaponProfileIndex)];
}

void RenderRuntime(UiModel& model, UiActions& actions) {
    RuntimeModel& runtime = model.runtime;
    VisualSettings& visual = model.visual;
    SystemSettings& system = model.system;

    SectionTitle("运行状态", 310.0f);
    const float metricsWidth = ImGui::GetContentRegionAvail().x;
    const int metricColumns =
        metricsWidth >= 420.0f
            ? 3
            : (metricsWidth >= 280.0f ? 2 : 1);
    if (ImGui::BeginTable(
            "##runtime_metrics", metricColumns,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        const char* state = runtime.stopping ? "停止中" : (runtime.active ? "运行中" : "未运行");
        const ImVec4 stateColor = runtime.stopping ? kAmber : (runtime.active ? kGreen : kMuted);
        StatusMetric("状态", state, stateColor);
        char fps[32]{};
        std::snprintf(fps, sizeof(fps), "%.1f", runtime.framesPerSecond);
        StatusMetric("帧率", fps, kAmber);
        const int rendererIndex = std::clamp(
            static_cast<int>(runtime.activeRenderBackend),
            static_cast<int>(RenderBackend::Cpu),
            static_cast<int>(RenderBackend::OpenGl));
        const char* renderer = kRenderBackends[
            static_cast<std::size_t>(rendererIndex)];
        StatusMetric("渲染方式", renderer, kAccent);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (ImGui::BeginTable(
            "##coordinate_decrypt_modes", 2,
            ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableNextColumn();
        Mark(
            actions,
            SettingsDomain::Visual,
            Toggle("坐标解密", visual.coordinateDecrypt));
        ImGui::TableNextColumn();
        Mark(
            actions,
            SettingsDomain::Visual,
            Toggle("算法解密", visual.algorithmDecrypt));
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    int renderBackend = std::clamp(
        static_cast<int>(system.renderBackend),
        static_cast<int>(RenderBackend::Cpu),
        static_cast<int>(RenderBackend::OpenGl));
    if (Combo("渲染方式", renderBackend, kRenderBackends)) {
        system.renderBackend = static_cast<RenderBackend>(renderBackend);
        actions.SettingsChanged(SettingsDomain::System);
    }

    SectionTitle("运行环境");
    if (runtime.active || runtime.busy || runtime.stopping) ImGui::BeginDisabled();
    ImGui::TextColored(kMuted, "游戏版本");
    Mark(
        actions, SettingsDomain::Runtime,
        SegmentedChoice("game_version", runtime.gameVersionIndex, kGameVersions));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    Mark(actions, SettingsDomain::Runtime, DriverCombo(runtime));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    int inputMode = std::clamp(
        static_cast<int>(model.aim.inputMode),
        0,
        static_cast<int>(kInputModes.size()) - 1);
    if (Combo("输入模式", inputMode, kInputModes)) {
        model.aim.inputMode = static_cast<AimInputMode>(inputMode);
        actions.SettingsChanged(SettingsDomain::Aim);
    }
    if (runtime.active || runtime.busy || runtime.stopping) ImGui::EndDisabled();
}

void RenderVisual(UiModel& model, UiActions& actions) {
    VisualSettings& visual = model.visual;

    SectionTitle("视觉总览");
    if (BeginToggleGrid("##visual_overview")) {
        Mark(actions, SettingsDomain::Visual, GridToggle("绘制开关", visual.enabled));
        Mark(actions, SettingsDomain::Visual, GridToggle("人数统计", visual.playerCount));
        Mark(actions, SettingsDomain::Visual, GridToggle("算法防闪", visual.antiFlicker));
        Mark(actions, SettingsDomain::Visual, GridToggle("模型绘制", visual.modelGeometry));
        Mark(actions, SettingsDomain::Visual, GridToggle("掩体变色", visual.visibilityColor));
        bool allEnabled = AllVisualEnabled(visual);
        if (GridToggle("绘制全开", allEnabled)) {
            SetAllVisual(visual, allEnabled);
            actions.SettingsChanged(SettingsDomain::Visual);
        }
        ImGui::EndTable();
    }

    SectionTitle("人物标记");
    if (BeginToggleGrid("##player_visuals")) {
        Mark(actions, SettingsDomain::Visual, GridToggle("人物方框", visual.box));
        Mark(actions, SettingsDomain::Visual, GridToggle("人物射线", visual.snapline));
        Mark(actions, SettingsDomain::Visual, GridToggle("人物骨骼", visual.skeleton));
        Mark(actions, SettingsDomain::Visual, GridToggle("人物距离", visual.distance));
        Mark(actions, SettingsDomain::Visual, GridToggle("人物名字", visual.playerName));
        Mark(actions, SettingsDomain::Visual, GridToggle("人物血量", visual.health));
        Mark(actions, SettingsDomain::Visual, GridToggle("屏外预警", visual.offscreenWarning));
        Mark(actions, SettingsDomain::Visual, GridToggle("干员名称", visual.operatorName));
        Mark(actions, SettingsDomain::Visual, GridToggle("手持武器", visual.heldWeapon));
        Mark(actions, SettingsDomain::Visual, GridToggle("头甲等级", visual.armorLevel));
        Mark(actions, SettingsDomain::Visual, GridToggle("头甲耐久", visual.armorDurability));
        Mark(actions, SettingsDomain::Visual, GridToggle("倒地绘制", visual.downedPlayer));
        ImGui::EndTable();
    }

    SectionTitle("预警与辅助");
    if (BeginToggleGrid("##visual_assists")) {
        Mark(actions, SettingsDomain::Visual, GridToggle("过滤人机", visual.filterBots));
        Mark(actions, SettingsDomain::Visual, GridToggle("附近敌人", visual.nearbyEnemy));
        Mark(actions, SettingsDomain::Visual, GridToggle("战场模式", visual.battlefieldMode));
        Mark(actions, SettingsDomain::Visual, GridToggle("战斗模式", visual.combatMode));
        Mark(actions, SettingsDomain::Visual, GridToggle("准星绘制", visual.crosshair));
        Mark(actions, SettingsDomain::Visual, GridToggle("道具预警", visual.throwableWarning));
        Mark(actions, SettingsDomain::Visual, GridToggle("道具轨迹", visual.throwableTrajectory));
        Mark(actions, SettingsDomain::Visual, GridToggle("被瞄预警", visual.aimWarning));
        Mark(actions, SettingsDomain::Visual, GridToggle("被瞄射线", visual.aimWarningRay));
        Mark(actions, SettingsDomain::Visual, GridToggle("玩家视线", visual.playerViewRay));
        ImGui::EndTable();
    }

    SectionTitle("显示范围");
    Mark(actions, SettingsDomain::Visual, SliderIntRow(
        "绘制距离", &visual.drawDistanceMeters, 0, 1000, "%d 米"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "预警大小", &visual.warningSize, 0.0f, 1000.0f, "%.0f"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "预警距离", &visual.warningDistanceMeters, 0.0f, 1000.0f, "%.0f 米"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "骨骼距离", &visual.skeletonDistanceMeters, 50.0f, 500.0f, "%.0f 米"));

    SectionTitle("绘制样式");
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "准星大小", &visual.crosshairSize, 5.0f, 500.0f, "%.0f"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "准星厚度", &visual.crosshairThickness, 1.0f, 10.0f, "%.1f"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "线条粗细", &visual.lineThickness, 1.0f, 10.0f, "%.1f"));
    Mark(actions, SettingsDomain::Visual, SliderFloatRow(
        "字体大小", &visual.fontScale, 0.3f, 3.0f, "%.1f"));

}

void RenderLoot(UiModel& model, UiActions& actions) {
    LootSettings& loot = model.loot;

    SectionTitle("物资总览");
    if (BeginToggleGrid("##loot_overview")) {
        Mark(actions, SettingsDomain::Loot, GridToggle("绘制物资", loot.enabled));
        bool allEnabled = AllLootEnabled(loot);
        if (GridToggle("物资全开", allEnabled)) {
            SetAllLoot(loot, allEnabled);
            actions.SettingsChanged(SettingsDomain::Loot);
        }
        ImGui::EndTable();
    }

    SectionTitle("物资标记");
    if (BeginToggleGrid("##loot_features")) {
        Mark(actions, SettingsDomain::Loot, GridToggle("绘制盒子", loot.playerBox));
        Mark(actions, SettingsDomain::Loot, GridToggle("人机盒子", loot.botBox));
        Mark(actions, SettingsDomain::Loot, GridToggle("密码显示", loot.password));
        Mark(actions, SettingsDomain::Loot, GridToggle("容器绘制", loot.containers));
        Mark(actions, SettingsDomain::Loot, GridToggle("盒内物资", loot.boxContents));
        Mark(actions, SettingsDomain::Loot, GridToggle("容器物资", loot.containerContents));
        Mark(actions, SettingsDomain::Loot, GridToggle("物品列表", loot.highValueList));
        ImGui::EndTable();
    }

    SectionTitle("筛选设置");
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "物资距离", &loot.itemDistanceMeters, 0, 500, "%d 米"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "物资价值", &loot.minimumItemValue, 0, 500000, "%d"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "物资等级", &loot.minimumItemRarity, 1, 6, "%d 级"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "容器距离", &loot.containerDistanceMeters, 0, 500, "%d 米"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "容器价值", &loot.minimumContainerValue, 0, 500000, "%d"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "容器等级", &loot.minimumContainerRarity, 1, 6, "%d 级"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "列表数量", &loot.listLimit, 1, 20, "%d 个"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "列表价值", &loot.minimumListValue, 0, 500000, "%d"));
    Mark(actions, SettingsDomain::Loot, SliderIntRow(
        "列表等级", &loot.minimumListRarity, 1, 6, "%d 级"));

    SectionTitle("容器类型");
    bool allContainers = AllContainersEnabled(loot);
    if (Toggle("容器全开", allContainers)) {
        loot.containerKinds.fill(allContainers);
        actions.SettingsChanged(SettingsDomain::Loot);
    }
    if (BeginToggleGrid("##container_types")) {
        for (std::size_t i = 0; i < kContainerNames.size(); ++i) {
            Mark(
                actions, SettingsDomain::Loot,
                GridToggle(kContainerNames[i], loot.containerKinds[i]));
        }
        ImGui::EndTable();
    }

    SectionTitle("自定义物资");
    ImGui::TextColored(kMuted, "已加载 %zu 条", loot.customItemCount);
    if (ActionButton(
            "重新加载", ActionTone::Primary,
            ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        actions.ReloadCustomItems();
    }
}

void RenderRadar(UiModel& model, UiActions& actions) {
    RadarSettings& radar = model.radar;

    SectionTitle("雷达开关");
    if (BeginToggleGrid("##radar_toggles")) {
        Mark(actions, SettingsDomain::Radar, GridToggle("独立雷达", radar.overlay));
        Mark(actions, SettingsDomain::Radar, GridToggle("小地图雷达", radar.miniMap));
        Mark(actions, SettingsDomain::Radar, GridToggle("大地图雷达", radar.bigMap));
        Mark(actions, SettingsDomain::Radar, GridToggle("显示自己", radar.showSelf));
        Mark(actions, SettingsDomain::Radar, GridToggle("小地图人机", radar.miniMapBots));
        Mark(actions, SettingsDomain::Radar, GridToggle("大地图人机", radar.bigMapBots));
        ImGui::EndTable();
    }

    SectionTitle("独立雷达");
    const float maxX = static_cast<float>(std::max(1, model.runtime.screenWidth));
    const float maxY = static_cast<float>(std::max(1, model.runtime.screenHeight));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "水平位置", &radar.overlayX, 0.0f, maxX, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "垂直位置", &radar.overlayY, 0.0f, maxY, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "雷达大小", &radar.overlaySize, 50.0f, 600.0f, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "雷达范围", &radar.overlayRangeMeters, 50.0f, 2000.0f, "%.0f 米"));

    SectionTitle("地图雷达");
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "水平偏移", &radar.mapOffsetX, -800.0f, 800.0f, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "垂直偏移", &radar.mapOffsetY, -800.0f, 800.0f, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "字体大小", &radar.mapFontSize, 0.0f, 40.0f, "%.0f"));
    Mark(actions, SettingsDomain::Radar, SliderFloatRow(
        "圆点大小", &radar.mapDotSize, 0.0f, 40.0f, "%.1f"));
}

void RenderAim(UiModel& model, UiActions& actions) {
    AimSettings& aim = model.aim;

    SectionTitle("瞄准设置");
    if (BeginToggleGrid("##aim_overview")) {
        bool enabled = aim.enabled;
        if (GridToggle("瞄准开关", enabled)) {
            aim.enabled = enabled;
            actions.AimEnabledChanged(enabled);
            actions.SettingsChanged(SettingsDomain::Aim);
        }
        bool quickOptions = AimQuickOptionsEnabled(aim);
        if (GridToggle("一键开关", quickOptions)) {
            SetAimQuickOptions(aim, quickOptions);
            actions.SettingsChanged(SettingsDomain::Aim);
        }
        Mark(actions, SettingsDomain::Aim, GridToggle("漏打模式", aim.missMode));
        Mark(actions, SettingsDomain::Aim, GridToggle("忽略人机", aim.ignoreBots));
        Mark(actions, SettingsDomain::Aim, GridToggle("倒地不瞄", aim.ignoreDowned));
        Mark(actions, SettingsDomain::Aim, GridToggle("持续锁定", aim.persistentLock));
        Mark(actions, SettingsDomain::Aim, GridToggle("曲线瞄准", aim.curvedMotion));
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            Mark(actions, SettingsDomain::Aim,
                 GridToggle("子弹追踪", aim.trajectoryTracking));
#endif
        Mark(actions, SettingsDomain::Aim, GridToggle("可见检测", aim.requireVisibility));
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            Mark(actions, SettingsDomain::Aim,
                 GridToggle("目标状态过滤", aim.rejectTargetState));
            Mark(actions, SettingsDomain::Aim,
                 GridToggle("死亡目标过滤", aim.rejectDeadTarget));
#endif
        Mark(actions, SettingsDomain::Aim, GridToggle("范围限制", aim.enforceFov));
        Mark(actions, SettingsDomain::Aim, GridToggle("距离限制", aim.enforceDistance));
        Mark(actions, SettingsDomain::Aim, GridToggle("范围圆圈", aim.drawRange));
        Mark(actions, SettingsDomain::Aim, GridToggle("目标射线", aim.drawTargetRay));
        ImGui::EndTable();
    }
    if (aim.missMode) {
        Mark(actions, SettingsDomain::Aim,
             Combo("漏打范围", aim.coverMode, kCoverModes));
    }

#if LENGJING_ENABLE_PROJECTILE_TRACKING
        SectionTitle("真人");
        if (BeginToggleGrid("##players_tracking")) {
            Mark(actions, SettingsDomain::Aim,
                 GridToggle("死亡箱", aim.playerDeadBox));
            ImGui::EndTable();
        }

        SectionTitle("人机");
        if (BeginToggleGrid("##robots_tracking")) {
            Mark(actions, SettingsDomain::Aim,
                 GridToggle("死亡箱", aim.robotDeadBox));
            ImGui::EndTable();
        }
#endif

    SectionTitle("武器配置");
    Mark(
        actions, SettingsDomain::Aim,
        Toggle("武器独立配置", aim.weaponProfilesEnabled));
    if (aim.weaponProfilesEnabled) {
        Mark(
            actions, SettingsDomain::Aim,
            Combo("武器分组", aim.weaponProfileIndex, kWeaponProfiles));
    }
    AimTuning& tuning = CurrentAimTuning(aim);

    SectionTitle("目标选择");
    Mark(actions, SettingsDomain::Aim, Combo("触发方式", aim.triggerMode, kTriggerModes));
    Mark(actions, SettingsDomain::Aim, Combo("目标算法", aim.targetAlgorithm, kTargetAlgorithms));
    Mark(actions, SettingsDomain::Aim, Combo("腰射锁定部位", tuning.hipBone, kAimBones));
    Mark(actions, SettingsDomain::Aim, Combo("开镜锁定部位", tuning.adsBone, kAimBones));
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        Mark(actions, SettingsDomain::Aim, SliderIntRow(
            "命中率", &aim.hitPercentage, 0, 100, "%d%%"));
#endif

    if (tuning.hipBone == 6 || tuning.adsBone == 6) {
        SectionTitle("随机部位");
        for (std::size_t i = 0; i < kRandomBoneNames.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            Mark(actions, SettingsDomain::Aim, SliderFloatRow(
                kRandomBoneNames[i], &aim.randomBoneWeights[i], 0.0f, 100.0f, "%.0f"));
            ImGui::PopID();
        }
    }

    SectionTitle("距离范围");
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "瞄准范围", &tuning.rangePixels, 0.0f, 1314.0f, "%.0f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "腰射距离", &tuning.hipDistanceMeters, 5.0f, 300.0f, "%.0f 米"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "开镜距离", &tuning.adsDistanceMeters, 5.0f, 300.0f, "%.0f 米"));

    SectionTitle("触摸区域");
    Mark(actions, SettingsDomain::Aim, Toggle("显示触摸范围", aim.showTouchArea));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "触摸范围", &aim.touchRange, 100.0f, 800.0f, "%.0f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "触摸位置 X", &aim.touchX, 0.0f,
        static_cast<float>(std::max(1, model.runtime.screenWidth)), "%.1f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "触摸位置 Y", &aim.touchY, 0.0f,
        static_cast<float>(std::max(1, model.runtime.screenHeight)), "%.1f"));

    SectionTitle("瞄准速度");
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "水平速度", &tuning.horizontalSpeed, 0.0f, 200.0f, "%.0f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "垂直速度", &tuning.verticalSpeed, 0.0f, 200.0f, "%.0f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "腰射速度", &tuning.hipSpeed, 1.0f, 50.0f, "%.0f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "开镜速度", &tuning.adsSpeed, 1.0f, 50.0f, "%.0f"));

    SectionTitle("高级调节");
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "预判强度", &tuning.prediction, 0.0f, 2.0f, "%.2f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "压枪强度", &tuning.recoil, 0.0f, 2.0f, "%.2f"));
    Mark(actions, SettingsDomain::Aim, SliderFloatRow(
        "瞄准平滑", &tuning.smoothing, 0.0f, 200.0f, "%.0f"));
}

void RenderSystem(UiModel& model, UiActions& actions) {
    SystemSettings& system = model.system;
    RuntimeModel& runtime = model.runtime;

    SectionTitle("性能");
    Mark(actions, SettingsDomain::System, Combo("帧率上限", system.frameLimitIndex, kFrameLimits));

    SectionTitle("运行日志");
    const float logHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, 160.0f, 360.0f);
    if (ImGui::BeginChild(
            "##runtime_log", ImVec2(0.0f, logHeight), true,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
        ImGui::TextUnformatted(runtime.logText.c_str());
        if (system.autoScrollLogs && atBottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    if (BeginToggleGrid("##log_options")) {
        Mark(actions, SettingsDomain::System, GridToggle("自动滚动", system.autoScrollLogs));
        Mark(actions, SettingsDomain::System, GridToggle("气泡通知", system.toastNotifications));
        ImGui::EndTable();
    }
    if (ActionButton(
            "清空日志", ActionTone::Neutral,
            ImVec2(ImGui::GetContentRegionAvail().x, 42.0f))) {
        actions.ClearLogs();
    }

    SectionTitle("本地配置");
    if (ActionButton(
            "重置配置", ActionTone::Danger,
            ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        actions.ResetLocalSettings();
    }

    SectionTitle("设备信息");
    if (ImGui::BeginTable(
            "##device_info", 2,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        StatusMetric(
            "版本", runtime.buildVersion.empty() ? "未标记" : runtime.buildVersion,
            runtime.buildVersion.empty() ? kMuted : kText);
        StatusMetric(
            "分辨率",
            runtime.screenWidth > 0 && runtime.screenHeight > 0
                ? std::to_string(runtime.screenWidth) + " x " + std::to_string(runtime.screenHeight)
                : "未获取",
            runtime.screenWidth > 0 && runtime.screenHeight > 0 ? kText : kMuted);
        ImGui::EndTable();
    }
}

void RenderNavigation(UiModel& model, ImTextureID logoTexture) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 headerOrigin = ImGui::GetCursorScreenPos();
    const float headerWidth = ImGui::GetContentRegionAvail().x;
    constexpr float identityHeight = 52.0f;
    draw->AddRectFilledMultiColor(
        headerOrigin,
        ImVec2(headerOrigin.x + headerWidth, headerOrigin.y + identityHeight),
        ImGui::GetColorU32(ImVec4(0.08f, 0.20f, 0.18f, 0.52f)),
        ImGui::GetColorU32(ImVec4(0.06f, 0.10f, 0.11f, 0.06f)),
        ImGui::GetColorU32(ImVec4(0.06f, 0.10f, 0.11f, 0.02f)),
        ImGui::GetColorU32(ImVec4(0.08f, 0.20f, 0.18f, 0.24f)));

    const ImVec2 titlePos(headerOrigin.x + 10.0f, headerOrigin.y + 6.0f);
    if (logoTexture != nullptr) {
        constexpr float logoSize = 40.0f;
        draw->AddImage(
            logoTexture, titlePos,
            ImVec2(titlePos.x + logoSize, titlePos.y + logoSize));
        draw->AddCircleFilled(
            ImVec2(titlePos.x + 35.0f, titlePos.y + 35.0f), 4.0f,
            ImGui::GetColorU32(model.runtime.active ? kGreen : kAccent));
    } else {
        draw->AddCircleFilled(
            ImVec2(titlePos.x + 20.0f, titlePos.y + 20.0f), 10.0f,
            ImGui::GetColorU32(ImVec4(0.08f, 0.22f, 0.19f, 1.0f)));
        draw->AddCircleFilled(
            ImVec2(titlePos.x + 20.0f, titlePos.y + 20.0f), 4.0f,
            ImGui::GetColorU32(model.runtime.active ? kGreen : kAccent));
    }

    const ImVec2 brandSize = ImGui::CalcTextSize("棱镜");
    draw->AddText(
        ImVec2(
            titlePos.x + 52.0f,
            titlePos.y + (40.0f - brandSize.y) * 0.5f),
        ImGui::GetColorU32(kText),
        "棱镜");

    const char* status = model.runtime.stopping
        ? "停止中"
        : (model.runtime.busy
            ? "处理中"
            : (model.runtime.active ? "运行中" : "待机"));
    const ImVec4 statusColor =
        model.runtime.stopping || model.runtime.busy
        ? kAmber
        : (model.runtime.active ? kGreen : kMuted);
    const ImVec2 statusSize = ImGui::CalcTextSize(status);
    const float chipWidth = statusSize.x + 28.0f;
    const ImVec2 chipMinimum(
        headerOrigin.x + headerWidth - chipWidth - 10.0f,
        headerOrigin.y + 10.0f);
    const ImVec2 chipMaximum(
        chipMinimum.x + chipWidth,
        chipMinimum.y + 32.0f);
    draw->AddRectFilled(
        chipMinimum,
        chipMaximum,
        ImGui::GetColorU32(ImVec4(
            statusColor.x,
            statusColor.y,
            statusColor.z,
            0.11f)),
        16.0f);
    draw->AddRect(
        chipMinimum,
        chipMaximum,
        ImGui::GetColorU32(ImVec4(
            statusColor.x,
            statusColor.y,
            statusColor.z,
            0.48f)),
        16.0f);
    draw->AddCircleFilled(
        ImVec2(chipMinimum.x + 12.0f, chipMinimum.y + 16.0f),
        3.5f,
        ImGui::GetColorU32(statusColor));
    draw->AddText(
        ImVec2(
            chipMinimum.x + 20.0f,
            chipMinimum.y + (32.0f - statusSize.y) * 0.5f),
        ImGui::GetColorU32(statusColor),
        status);

    ImGui::Dummy(ImVec2(0.0f, identityHeight + 4.0f));
    const float width = ImGui::GetContentRegionAvail().x;
    const int columns = width >= 760.0f ? 6 : (width >= 480.0f ? 3 : 2);
    if (ImGui::BeginTable(
            "##workspace_navigation",
            columns,
            ImGuiTableFlags_SizingStretchSame |
                ImGuiTableFlags_NoSavedSettings)) {
        for (std::size_t i = 0; i < kPages.size(); ++i) {
            ImGui::TableNextColumn();
            const bool clicked = NavButton(
                kPageNames[i],
                model.page == kPages[i],
                ImVec2(ImGui::GetContentRegionAvail().x, 40.0f));
            if (clicked) {
                model.page = kPages[i];
            }
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

void RenderContent(UiModel& model, UiActions& actions) {
    const ImVec2 titleOrigin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(titleOrigin.x + 5.0f, titleOrigin.y + ImGui::GetTextLineHeight() * 0.5f),
        4.0f,
        ImGui::GetColorU32(kAccent));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16.0f);
    ImGui::TextColored(kText, "%s", PageName(model.page));
    ImGui::Dummy(ImVec2(0.0f, 3.0f));

    BeginSectionLayout("##workspace_cards");

    switch (model.page) {
    case Page::Runtime:
        RenderRuntime(model, actions);
        break;
    case Page::Visual:
        RenderVisual(model, actions);
        break;
    case Page::Loot:
        RenderLoot(model, actions);
        break;
    case Page::Radar:
        RenderRadar(model, actions);
        break;
    case Page::Aim:
        RenderAim(model, actions);
        break;
    case Page::System:
        RenderSystem(model, actions);
        break;
    }

    EndSectionLayout();
}

float ActionDockHeight(float width) {
    return width >= 560.0f ? 100.0f : 150.0f;
}

void RenderRuntimeAction(
    UiModel& model,
    UiActions& actions,
    const ImVec2& size) {
    RuntimeModel& runtime = model.runtime;
    if (runtime.busy || runtime.stopping) {
        ImGui::BeginDisabled();
        ActionButton(
            runtime.stopping ? "停止中" : "处理中",
            ActionTone::Neutral,
            size);
        ImGui::EndDisabled();
    } else if (runtime.active) {
        if (ActionButton("停止", ActionTone::Danger, size)) {
            actions.StopRuntime();
        }
    } else {
        const bool clicked = ActionButton("启动", ActionTone::Primary, size);
        if (clicked) {
            actions.StartRuntime();
        }
    }
}

void RenderActionDock(UiModel& model, UiActions& actions, float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.073f, 0.077f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.28f, 0.25f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 9.0f));
    ImGui::BeginChild(
        "##action_dock",
        ImVec2(0.0f, height),
        ImGuiChildFlags_Border,
        ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    const ImVec2 labelOrigin = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(
        labelOrigin,
        ImVec2(
            labelOrigin.x + 4.0f,
            labelOrigin.y + ImGui::GetTextLineHeight()),
        ImGui::GetColorU32(kAccent),
        2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    ImGui::TextColored(kText, "运行控制");

    const float width = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    if (width >= 540.0f) {
        const float primaryWidth = width * 0.48f;
        const float secondaryWidth =
            (width - primaryWidth - spacing * 2.0f) * 0.5f;
        RenderRuntimeAction(
            model,
            actions,
            ImVec2(primaryWidth, 44.0f));
        ImGui::SameLine();
        const bool hideClicked = ActionButton(
            "隐藏菜单",
            ActionTone::Neutral,
            ImVec2(secondaryWidth, 44.0f));
        if (hideClicked) {
            actions.HideMenu();
        }
        ImGui::SameLine();
        if (ActionButton(
                "退出程序",
                ActionTone::Danger,
                ImVec2(secondaryWidth, 44.0f))) {
            actions.ExitApplication();
        }
    } else {
        RenderRuntimeAction(model, actions, ImVec2(width, 40.0f));
        const float halfWidth = (width - spacing) * 0.5f;
        const bool hideClicked = ActionButton(
            "隐藏菜单",
            ActionTone::Neutral,
            ImVec2(halfWidth, 40.0f));
        if (hideClicked) {
            actions.HideMenu();
        }
        ImGui::SameLine();
        if (ActionButton(
                "退出程序",
                ActionTone::Danger,
                ImVec2(halfWidth, 40.0f))) {
            actions.ExitApplication();
        }
    }
    ImGui::EndChild();
}

}  // namespace

void MenuView::SetLogoTexture(void* texture) noexcept {
    logoTexture_ = texture;
}

void MenuView::RequestRecenter() noexcept {
    positionInitialized_ = false;
    dragActive_ = false;
    animationAnchorValid_ = false;
}

void MenuView::ClearTopOverlayBounds() noexcept {
    topOverlayValid_ = false;
}

void MenuView::SetTopOverlayBounds(
    float minimumX,
    float minimumY,
    float maximumX,
    float maximumY,
    float layoutMaximumY) noexcept {
    topOverlayValid_ =
        maximumX > minimumX && maximumY > minimumY;
    if (!topOverlayValid_) {
        return;
    }
    topOverlayMinimumX_ = minimumX;
    topOverlayMinimumY_ = minimumY;
    topOverlayMaximumX_ = maximumX;
    topOverlayMaximumY_ = maximumY;
    topOverlayLayoutMaximumY_ = std::max(maximumY, layoutMaximumY);
}

void MenuView::Render(UiModel& model, UiActions& actions) {
    if (!model.visible) {
        dragActive_ = false;
        wasVisible_ = false;
        windowAnimation_ = 0.0f;
        return;
    }

    MenuStyleScope style;
    ImGuiIO& io = ImGui::GetIO();
    if (!wasVisible_) {
        windowAnimation_ = 0.0f;
        pageAnimation_ = 1.0f;
        animationAnchorValid_ = topOverlayValid_;
        if (animationAnchorValid_) {
            animationAnchorMinimumX_ = topOverlayMinimumX_;
            animationAnchorMinimumY_ = topOverlayMinimumY_;
            animationAnchorMaximumX_ = topOverlayMaximumX_;
            animationAnchorMaximumY_ = topOverlayMaximumY_;
        }
        wasVisible_ = true;
    }
    if (!pageStateInitialized_) {
        animatedPage_ = model.page;
        pageStateInitialized_ = true;
    }
    windowAnimation_ = AnimateToward(windowAnimation_, 1.0f, 9.0f);
    const float windowEase =
        1.0f - (1.0f - windowAnimation_) * (1.0f - windowAnimation_);

    constexpr float viewportMargin = 12.0f;
    constexpr float topOverlayGap = 12.0f;
    constexpr float dragRegionHeight = 52.0f;
    const float displayWidth = std::max(1.0f, io.DisplaySize.x);
    const float displayHeight = std::max(1.0f, io.DisplaySize.y);
    const float topInset = topOverlayValid_
        ? std::max(
              viewportMargin,
              topOverlayLayoutMaximumY_ + topOverlayGap)
        : viewportMargin;
    const float availableWidth = std::max(1.0f, displayWidth - viewportMargin * 2.0f);
    const float availableHeight =
        std::max(1.0f, displayHeight - topInset - viewportMargin);
    const float windowWidth = std::min(1120.0f, availableWidth);
    const float windowHeight = std::min(820.0f, availableHeight);
    const float minWidth = std::min(360.0f, availableWidth);
    const float minHeight = std::min(440.0f, availableHeight);

    const float minimumX = viewportMargin;
    const float minimumY = topInset;
    const float maximumX =
        std::max(minimumX, displayWidth - windowWidth - viewportMargin);
    const float maximumY =
        std::max(minimumY, displayHeight - windowHeight - viewportMargin);
    const bool displayChanged =
        displayWidth != lastDisplayWidth_ || displayHeight != lastDisplayHeight_;
    if (!positionInitialized_) {
        windowX_ = (displayWidth - windowWidth) * 0.5f;
        windowY_ = std::clamp(
            (displayHeight - windowHeight) * 0.5f,
            minimumY,
            maximumY);
        positionInitialized_ = true;
    } else if (displayChanged) {
        windowX_ = (displayWidth - windowWidth) * 0.5f;
        windowY_ = std::clamp(
            (displayHeight - windowHeight) * 0.5f,
            minimumY,
            maximumY);
        dragActive_ = false;
    }
    windowX_ = std::clamp(windowX_, minimumX, maximumX);
    windowY_ = std::clamp(windowY_, minimumY, maximumY);
    if (dragActive_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            windowX_ = std::clamp(
                io.MousePos.x - dragOffsetX_, minimumX, maximumX);
            windowY_ = std::clamp(
                io.MousePos.y - dragOffsetY_, minimumY, maximumY);
        } else {
            dragActive_ = false;
        }
    }
    lastDisplayWidth_ = displayWidth;
    lastDisplayHeight_ = displayHeight;

    if (animationAnchorValid_ && windowEase < 0.999f) {
        const float anchorWidth =
            animationAnchorMaximumX_ - animationAnchorMinimumX_;
        const float anchorHeight =
            animationAnchorMaximumY_ - animationAnchorMinimumY_;
        const float anchorCenterX =
            (animationAnchorMinimumX_ + animationAnchorMaximumX_) * 0.5f;
        const float anchorCenterY =
            (animationAnchorMinimumY_ + animationAnchorMaximumY_) * 0.5f;
        const float targetCenterX = windowX_ + windowWidth * 0.5f;
        const float targetCenterY = windowY_ + windowHeight * 0.5f;
        const float drawWidth =
            anchorWidth + (windowWidth - anchorWidth) * windowEase;
        const float drawHeight =
            anchorHeight + (windowHeight - anchorHeight) * windowEase;
        const float drawCenterX =
            anchorCenterX + (targetCenterX - anchorCenterX) * windowEase;
        const float drawCenterY =
            anchorCenterY + (targetCenterY - anchorCenterY) * windowEase;
        ImGui::SetNextWindowPos(
            ImVec2(
                drawCenterX - drawWidth * 0.5f,
                drawCenterY - drawHeight * 0.5f),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(
                std::max(1.0f, drawWidth),
                std::max(1.0f, drawHeight)),
            ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(std::max(0.18f, windowEase));
        ImGui::Begin(
            "棱镜##menu_transition",
            nullptr,
            ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::End();
        if (windowEase < 0.48f) {
            return;
        }
    }

    const float contentEase = animationAnchorValid_
        ? std::clamp(
              (windowEase - 0.48f) / 0.52f,
              0.0f,
              1.0f)
        : windowEase;
    style.PushVar(
        ImGuiStyleVar_Alpha,
        std::max(0.03f, contentEase));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(minWidth, minHeight), ImVec2(availableWidth, availableHeight));
    ImGui::SetNextWindowPos(
        ImVec2(
            windowX_,
            windowY_ +
                (animationAnchorValid_
                    ? 0.0f
                    : (1.0f - windowEase) * 16.0f)),
        ImGuiCond_Always);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("棱镜##main_menu", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const ImVec2 windowPosition = ImGui::GetWindowPos();
    const ImVec2 dragCursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(
        "##window_drag_region",
        ImVec2(
            std::max(1.0f, ImGui::GetContentRegionAvail().x),
            dragRegionHeight),
        ImGuiButtonFlags_MouseButtonLeft);
    const bool dragRegionActivated = ImGui::IsItemActivated();
    if (dragRegionActivated) {
        dragActive_ = true;
        dragOffsetX_ = io.MousePos.x - windowPosition.x;
        dragOffsetY_ = io.MousePos.y - windowPosition.y;
    }
    if (dragActive_ && !ImGui::IsItemActive() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragActive_ = false;
    }
    ImGui::SetCursorScreenPos(dragCursor);

    RenderNavigation(model, static_cast<ImTextureID>(logoTexture_));
    if (model.page != animatedPage_) {
        animatedPage_ = model.page;
        pageAnimation_ = 0.0f;
    }
    pageAnimation_ = AnimateToward(pageAnimation_, 1.0f, 10.0f);
    const float pageEase =
        1.0f - (1.0f - pageAnimation_) * (1.0f - pageAnimation_);

    const float dockHeight = ActionDockHeight(ImGui::GetContentRegionAvail().x);
    const float bodyHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y);
    const float dockSpacing = ImGui::GetStyle().ItemSpacing.y;
    const bool dockInsideScroll =
        bodyHeight < dockHeight + 80.0f + dockSpacing;
    const float contentHeight = dockInsideScroll
        ? bodyHeight
        : std::max(1.0f, bodyHeight - dockHeight - dockSpacing);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.045f, 0.061f, 0.064f, 0.72f));
    if (ImGui::BeginChild(
            "##workspace_content",
            ImVec2(0.0f, contentHeight),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoSavedSettings)) {
        const float initialX = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(initialX + (1.0f - pageEase) * 28.0f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_Alpha,
            ImGui::GetStyle().Alpha * std::max(0.06f, pageEase));
        RenderContent(model, actions);
        ImGui::PopStyleVar();
        if (dockInsideScroll) {
            ImGui::Dummy(ImVec2(0.0f, dockSpacing));
            RenderActionDock(model, actions, dockHeight);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (!dockInsideScroll) {
        RenderActionDock(model, actions, dockHeight);
    }
    ImGui::End();
}

}  // namespace lengjing::ui
