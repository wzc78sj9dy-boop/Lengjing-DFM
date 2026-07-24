#include "test_support.h"

#include "game/native/BoneFrameSource.h"
#include "game/native/PlayerBounds.h"

#include <array>
#include <cmath>

void RunPlayerBoundsTests() {
    using lengjing::game::native::CalculatePlayerAnchorBounds;
    using lengjing::game::native::CalculatePlayerScreenBounds;
    using lengjing::game::native::DoesPlayerScreenBoundsIntersectViewport;
    using lengjing::game::native::IsBoneFrameCacheSourceCompatible;
    using lengjing::game::native::IsReliablePlayerScreenBounds;
    using lengjing::game::native::IsResolvedBoneTransformEnabled;
    using lengjing::game::native::kPlayerBoundsBoneCount;
    using lengjing::game::native::BoneFrameCacheSource;
    using lengjing::game::native::BoneFrameRecordSource;
    using lengjing::game::native::SelectFallbackBoneFrameSource;
    using lengjing::game::native::ShouldReadSecondaryBoneArray;
    using lengjing::game::native::ShouldResetBoneFrameCache;
    using lengjing::game::native::PlayerBoneScreenPoint;
    using lengjing::game::native::PlayerScreenBounds;
    using lengjing::game::native::PreferBoneFrameCandidate;
    using lengjing::game::native::SelectPreferredBoneFrameSource;
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

    REQUIRE(CalculatePlayerAnchorBounds(
        PlayerBoneScreenPoint{1980.0f, 400.0f, true},
        PlayerBoneScreenPoint{1880.0f, 200.0f, true},
        bounds));
    REQUIRE(std::abs(bounds.left - 1830.0f) < 0.001f);
    REQUIRE(std::abs(bounds.right - 2030.0f) < 0.001f);
    REQUIRE(DoesPlayerScreenBoundsIntersectViewport(
        bounds, 1920.0f, 1080.0f));
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));

    REQUIRE(!CalculatePlayerAnchorBounds(
        PlayerBoneScreenPoint{300.0f, 200.0f, true},
        PlayerBoneScreenPoint{300.0f, 400.0f, true},
        bounds));

    REQUIRE(CalculatePlayerAnchorBounds(
        PlayerBoneScreenPoint{960.0f, 3040.0f, true},
        PlayerBoneScreenPoint{960.0f, -1960.0f, true},
        bounds));
    REQUIRE(DoesPlayerScreenBoundsIntersectViewport(
        bounds, 1920.0f, 1080.0f));
    REQUIRE(IsReliablePlayerScreenBounds(bounds, 1920.0f, 1080.0f));

    const PlayerScreenBounds edgeIntersection{
        1910.0f, 100.0f, 3000.0f, 500.0f};
    REQUIRE(DoesPlayerScreenBoundsIntersectViewport(
        edgeIntersection, 1920.0f, 1080.0f));
    REQUIRE(IsReliablePlayerScreenBounds(
        edgeIntersection, 1920.0f, 1080.0f));
    REQUIRE(!DoesPlayerScreenBoundsIntersectViewport(
        PlayerScreenBounds{1921.0f, 100.0f, 2500.0f, 500.0f},
        1920.0f,
        1080.0f));
    REQUIRE(!IsReliablePlayerScreenBounds(
        PlayerScreenBounds{-1000.0f, 100.0f, -1.0f, 500.0f},
        1920.0f,
        1080.0f));
    REQUIRE(!IsReliablePlayerScreenBounds(
        PlayerScreenBounds{0.0f, -8000.0f, 100.0f, 8000.0f},
        1920.0f,
        1080.0f));

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
    REQUIRE(!IsResolvedBoneTransformEnabled(false, false));
    REQUIRE(IsResolvedBoneTransformEnabled(true, false));
    REQUIRE(IsResolvedBoneTransformEnabled(false, true));
    REQUIRE(IsResolvedBoneTransformEnabled(true, true));
    REQUIRE(SelectBoneFrameMesh(ordinaryRecord, 0x3000, false) == 0x3000);
    REQUIRE(SelectBoneFrameMesh(ordinaryRecord, 0, false) == 0x2000);
    const BoneFrameRecordSource encryptedRecord{
        0x4000, 0x5000, true, true};
    REQUIRE(SelectBoneFrameMesh(encryptedRecord, 0x6000, false) == 0x6000);
    REQUIRE(SelectBoneFrameMesh(encryptedRecord, 0x6000, true) == 0x5000);
    REQUIRE(SelectBoneFrameMesh(encryptedRecord, 0, false) == 0);
    const auto firstDecryptPreferred = SelectPreferredBoneFrameSource(
        encryptedRecord,
        0,
        0,
        IsResolvedBoneTransformEnabled(true, false));
    REQUIRE(firstDecryptPreferred.mesh == 0x5000);
    REQUIRE(firstDecryptPreferred.rebuildResolvedTransform);
    const auto secondDecryptPreferred = SelectPreferredBoneFrameSource(
        encryptedRecord,
        0,
        0,
        IsResolvedBoneTransformEnabled(false, true));
    REQUIRE(secondDecryptPreferred.mesh == 0x5000);
    REQUIRE(secondDecryptPreferred.rebuildResolvedTransform);

    const auto ordinaryPreferred = SelectPreferredBoneFrameSource(
        encryptedRecord, 0x7000, 0x6000, false);
    REQUIRE(ordinaryPreferred.root == 0x7000);
    REQUIRE(ordinaryPreferred.mesh == 0x6000);
    REQUIRE(!ordinaryPreferred.rebuildResolvedTransform);
    const auto encryptedPreferred = SelectPreferredBoneFrameSource(
        encryptedRecord, 0x7000, 0x6000, true);
    REQUIRE(encryptedPreferred.root == 0x4000);
    REQUIRE(encryptedPreferred.mesh == 0x5000);
    REQUIRE(encryptedPreferred.rebuildResolvedTransform);
    REQUIRE(!SelectFallbackBoneFrameSource(
        encryptedRecord, 0x7000, 0x6000, true));
    REQUIRE(!SelectFallbackBoneFrameSource(
        encryptedRecord, 0x7000, 0x6000, false));
    REQUIRE(!SelectFallbackBoneFrameSource(
        encryptedRecord, 0, 0, true));
    const BoneFrameRecordSource plainResolver{
        0x8000, 0x9000, false, true};
    const auto plainResolverPreferred = SelectPreferredBoneFrameSource(
        plainResolver, 0xA000, 0xB000, true);
    REQUIRE(plainResolverPreferred.root == 0x8000);
    REQUIRE(plainResolverPreferred.mesh == 0x9000);
    REQUIRE(plainResolverPreferred.rebuildResolvedTransform);
    REQUIRE(!SelectFallbackBoneFrameSource(
        plainResolver, 0xA000, 0xB000, true));
    const auto plainResolverOrdinary = SelectPreferredBoneFrameSource(
        plainResolver, 0xA000, 0xB000, false);
    REQUIRE(plainResolverOrdinary.root == 0xA000);
    REQUIRE(plainResolverOrdinary.mesh == 0xB000);
    REQUIRE(!plainResolverOrdinary.rebuildResolvedTransform);
    const auto plainFallback = SelectFallbackBoneFrameSource(
        plainResolver, 0xA000, 0xB000, false);
    REQUIRE(plainFallback.root == 0x8000);
    REQUIRE(plainFallback.mesh == 0x9000);
    REQUIRE(!plainFallback.rebuildResolvedTransform);

    REQUIRE(PreferBoneFrameCandidate(0, false, 2, true));
    REQUIRE(PreferBoneFrameCandidate(10, false, 2, true));
    REQUIRE(PreferBoneFrameCandidate(8, true, 15, true));
    REQUIRE(!PreferBoneFrameCandidate(15, true, 8, true));
    REQUIRE(!PreferBoneFrameCandidate(8, true, 8, true));
    REQUIRE(ShouldReadSecondaryBoneArray(true, false, 0, 15));
    REQUIRE(!ShouldReadSecondaryBoneArray(true, true, 0, 15));
    REQUIRE(!ShouldReadSecondaryBoneArray(true, true, 14, 15));
    REQUIRE(ShouldReadSecondaryBoneArray(false, true, 14, 15));
    REQUIRE(!ShouldReadSecondaryBoneArray(false, true, 15, 15));

    REQUIRE(!ShouldResetBoneFrameCache(
        ordinaryPreferred,
        0xC000,
        true,
        BoneFrameCacheSource{0x7000, 0x6000, false},
        0xC000,
        true));
    REQUIRE(ShouldResetBoneFrameCache(
        ordinaryPreferred,
        0xC000,
        true,
        BoneFrameCacheSource{0x7000, 0x6000, false},
        0xD000,
        true));
    REQUIRE(ShouldResetBoneFrameCache(
        ordinaryPreferred,
        0xC000,
        true,
        BoneFrameCacheSource{0x7000, 0x6000, false},
        0xC000,
        false));

    REQUIRE(IsBoneFrameCacheSourceCompatible(
        ordinaryRecord,
        0,
        0x3000,
        false,
        BoneFrameCacheSource{0, 0x3000, false}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        ordinaryRecord,
        0,
        0x3000,
        false,
        BoneFrameCacheSource{0, 0x2000, false}));
    REQUIRE(IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        false,
        BoneFrameCacheSource{0x7000, 0x6000, false}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        false,
        BoneFrameCacheSource{0x4000, 0x5000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        true,
        BoneFrameCacheSource{0x7000, 0x6000, false}));
    REQUIRE(IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        true,
        BoneFrameCacheSource{0x4000, 0x5000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        true,
        BoneFrameCacheSource{0x4001, 0x5000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        encryptedRecord,
        0x7000,
        0x6000,
        true,
        BoneFrameCacheSource{0x4000, 0x5000, false}));
    REQUIRE(IsBoneFrameCacheSourceCompatible(
        plainResolver,
        0xA000,
        0xB000,
        true,
        BoneFrameCacheSource{0x8000, 0x9000, true}));
    REQUIRE(!IsBoneFrameCacheSourceCompatible(
        plainResolver,
        0xA000,
        0xB000,
        true,
        BoneFrameCacheSource{0xA000, 0xB000, false}));
}
