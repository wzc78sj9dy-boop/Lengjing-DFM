#include "test_support.h"

#include "game/native/BoneFrameSource.h"
#include "game/native/PlayerBounds.h"

#include <array>
#include <cmath>

void RunPlayerBoundsTests() {
    using lengjing::game::native::CalculatePlayerAnchorBounds;
    using lengjing::game::native::CalculatePlayerScreenBounds;
    using lengjing::game::native::IsBoneFrameCacheSourceCompatible;
    using lengjing::game::native::IsReliablePlayerScreenBounds;
    using lengjing::game::native::kPlayerBoundsBoneCount;
    using lengjing::game::native::BoneFrameCacheSource;
    using lengjing::game::native::BoneFrameRecordSource;
    using lengjing::game::native::PlayerBoneScreenPoint;
    using lengjing::game::native::PlayerScreenBounds;
    using lengjing::game::native::SelectBoneFrameMesh;
    using lengjing::game::native::SelectPlayerScreenBounds;

    std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount> standing{};
    standing[0] = PlayerBoneScreenPoint{100.0f, 100.0f, true};
    standing[1] = PlayerBoneScreenPoint{100.0f, 115.0f, true};
    standing[2] = PlayerBoneScreenPoint{100.0f, 160.0f, true};
    standing[7] = PlayerBoneScreenPoint{75.0f, 150.0f, true};
    standing[8] = PlayerBoneScreenPoint{125.0f, 150.0f, true};
    standing[13] = PlayerBoneScreenPoint{92.0f, 220.0f, true};
    standing[14] = PlayerBoneScreenPoint{108.0f, 224.0f, true};

    PlayerScreenBounds bounds{};
    REQUIRE(CalculatePlayerAnchorBounds(
        PlayerBoneScreenPoint{300.0f, 400.0f, true},
        PlayerBoneScreenPoint{300.0f, 200.0f, true},
        bounds));
    REQUIRE(std::abs(bounds.left - 250.0f) < 0.001f);
    REQUIRE(std::abs(bounds.top - 200.0f) < 0.001f);
    REQUIRE(std::abs(bounds.right - 350.0f) < 0.001f);
    REQUIRE(std::abs(bounds.bottom - 400.0f) < 0.001f);
    REQUIRE(!CalculatePlayerAnchorBounds(
        PlayerBoneScreenPoint{300.0f, 200.0f, true},
        PlayerBoneScreenPoint{300.0f, 400.0f, true},
        bounds));

    REQUIRE(CalculatePlayerScreenBounds(standing, bounds));
    REQUIRE(bounds.top < 100.0f);
    REQUIRE(bounds.bottom > 224.0f);
    REQUIRE(bounds.left < 75.0f);
    REQUIRE(bounds.right > 125.0f);
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));
    const PlayerScreenBounds standingBounds = bounds;

    auto unevenFeet = standing;
    unevenFeet[13].y = 230.0f;
    unevenFeet[14].y = 218.0f;
    REQUIRE(CalculatePlayerScreenBounds(unevenFeet, bounds));
    REQUIRE(bounds.bottom > 230.0f);

    std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount> collapsed{};
    for (PlayerBoneScreenPoint& point : collapsed) {
        point = PlayerBoneScreenPoint{100.0f, 200.0f, true};
    }
    REQUIRE(!CalculatePlayerScreenBounds(collapsed, bounds));

    auto reversedPose = standing;
    reversedPose[13].y = 90.0f;
    reversedPose[14].y = 92.0f;
    REQUIRE(CalculatePlayerScreenBounds(reversedPose, bounds));

    auto missingFeet = standing;
    missingFeet[13].valid = false;
    missingFeet[14].valid = false;
    REQUIRE(!CalculatePlayerScreenBounds(missingFeet, bounds));

    std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount> horizontalProne{};
    horizontalProne[0] = PlayerBoneScreenPoint{200.0f, 300.0f, true};
    horizontalProne[1] = PlayerBoneScreenPoint{220.0f, 300.0f, true};
    horizontalProne[2] = PlayerBoneScreenPoint{300.0f, 300.0f, true};
    horizontalProne[7] = PlayerBoneScreenPoint{260.0f, 296.0f, true};
    horizontalProne[9] = PlayerBoneScreenPoint{330.0f, 300.0f, true};
    horizontalProne[11] = PlayerBoneScreenPoint{370.0f, 302.0f, true};
    horizontalProne[13] = PlayerBoneScreenPoint{420.0f, 300.0f, true};
    REQUIRE(CalculatePlayerScreenBounds(horizontalProne, bounds));
    REQUIRE(bounds.right - bounds.left > bounds.bottom - bounds.top);
    REQUIRE(bounds.left < 200.0f);
    REQUIRE(bounds.right > 420.0f);
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));

    std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount> diagonalProne{};
    diagonalProne[0] = PlayerBoneScreenPoint{220.0f, 220.0f, true};
    diagonalProne[1] = PlayerBoneScreenPoint{240.0f, 240.0f, true};
    diagonalProne[2] = PlayerBoneScreenPoint{300.0f, 300.0f, true};
    diagonalProne[7] = PlayerBoneScreenPoint{260.0f, 250.0f, true};
    diagonalProne[9] = PlayerBoneScreenPoint{330.0f, 330.0f, true};
    diagonalProne[11] = PlayerBoneScreenPoint{370.0f, 370.0f, true};
    diagonalProne[13] = PlayerBoneScreenPoint{410.0f, 410.0f, true};
    REQUIRE(CalculatePlayerScreenBounds(diagonalProne, bounds));
    REQUIRE(bounds.left < 220.0f);
    REQUIRE(bounds.top < 220.0f);
    REQUIRE(bounds.right > 410.0f);
    REQUIRE(bounds.bottom > 410.0f);
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));

    auto withOutlier = standing;
    withOutlier[3] = PlayerBoneScreenPoint{5000.0f, -5000.0f, true};
    REQUIRE(CalculatePlayerScreenBounds(withOutlier, bounds));
    REQUIRE(std::abs(bounds.left - standingBounds.left) < 0.001f);
    REQUIRE(std::abs(bounds.top - standingBounds.top) < 0.001f);
    REQUIRE(std::abs(bounds.right - standingBounds.right) < 0.001f);
    REQUIRE(std::abs(bounds.bottom - standingBounds.bottom) < 0.001f);

    auto partiallyOffscreen = horizontalProne;
    for (PlayerBoneScreenPoint& point : partiallyOffscreen) {
        if (point.valid) point.x -= 280.0f;
    }
    REQUIRE(CalculatePlayerScreenBounds(partiallyOffscreen, bounds));
    REQUIRE(bounds.left < 0.0f);
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));

    std::array<PlayerBoneScreenPoint, kPlayerBoundsBoneCount> fewerThanFive{};
    fewerThanFive[0] = PlayerBoneScreenPoint{100.0f, 100.0f, true};
    fewerThanFive[1] = PlayerBoneScreenPoint{100.0f, 120.0f, true};
    fewerThanFive[2] = PlayerBoneScreenPoint{100.0f, 160.0f, true};
    fewerThanFive[13] = PlayerBoneScreenPoint{100.0f, 220.0f, true};
    REQUIRE(!CalculatePlayerScreenBounds(fewerThanFive, bounds));

    auto fiveWithOutlier = fewerThanFive;
    fiveWithOutlier[3] = PlayerBoneScreenPoint{5000.0f, -5000.0f, true};
    REQUIRE(!CalculatePlayerScreenBounds(fiveWithOutlier, bounds));

    const PlayerScreenBounds boneBounds{80.0f, 90.0f, 120.0f, 230.0f};
    const PlayerScreenBounds anchorBounds{70.0f, 70.0f, 130.0f, 250.0f};
    PlayerScreenBounds selected{};
    REQUIRE(SelectPlayerScreenBounds(
        true, boneBounds, true, anchorBounds, 1920.0f, 1080.0f, selected));
    REQUIRE(std::abs(selected.top - boneBounds.top) < 0.001f);
    REQUIRE(std::abs(selected.bottom - boneBounds.bottom) < 0.001f);

    const PlayerScreenBounds invalidBone{
        99.0f, 100.0f, 101.0f, 900.0f};
    REQUIRE(!IsReliablePlayerScreenBounds(
        invalidBone, 1920.0f, 1080.0f));
    REQUIRE(!IsReliablePlayerScreenBounds(
        PlayerScreenBounds{0.0f, 0.0f, 400.0f, 10.0f},
        1920.0f, 1080.0f));
    REQUIRE(SelectPlayerScreenBounds(
        true, invalidBone, true, anchorBounds,
        1920.0f, 1080.0f, selected));
    REQUIRE(std::abs(selected.top - anchorBounds.top) < 0.001f);
    REQUIRE(!SelectPlayerScreenBounds(
        true, invalidBone, false, PlayerScreenBounds{},
        1920.0f, 1080.0f, selected));

    const BoneFrameRecordSource ordinaryRecord{0x1000, 0x2000, false};
    REQUIRE(SelectBoneFrameMesh(ordinaryRecord, 0x3000) == 0x3000);
    REQUIRE(SelectBoneFrameMesh(ordinaryRecord, 0) == 0x2000);
    const BoneFrameRecordSource encryptedRecord{0x4000, 0x5000, true};
    REQUIRE(SelectBoneFrameMesh(encryptedRecord, 0x6000) == 0x5000);

    REQUIRE(IsBoneFrameCacheSourceCompatible(
        ordinaryRecord,
        0x3000,
        BoneFrameCacheSource{0, 0x3000, false}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        ordinaryRecord,
        0x3000,
        BoneFrameCacheSource{0, 0x2000, false}));
    REQUIRE(IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x5000,
        BoneFrameCacheSource{0x4000, 0x5000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x5000,
        BoneFrameCacheSource{0x4001, 0x5000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x5000,
        BoneFrameCacheSource{0x4000, 0x5000, false}));
}
