#include "archer_hunt_plugin.h"
#include "hunt_targeting.h"
#include "game.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "CMagic.h"
#include "CItem.h"
#include "config.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr int kReliableAttackRange = 1;
constexpr int kMinMobClumpSize = 2;
constexpr int kArcherSafetyBufferTiles = 1;
constexpr DWORD kArcherRetreatHoldMs = 3000;
constexpr DWORD kFailedRetreatDestAvoidMs = 1000;
constexpr int kMinAttackIntervalMs = 25;
constexpr int kMaxAttackIntervalMs = 5000;
constexpr int kMinTargetSwitchAttackIntervalMs = 0;
constexpr int kMaxTargetSwitchAttackIntervalMs = 5000;
constexpr int kMinItemActionIntervalMs = 100;
constexpr int kMaxItemActionIntervalMs = 5000;
constexpr int kMinScatterIntervalMs = 100;
constexpr int kMinScatterFanAngle = 30;
constexpr int kMaxScatterFanAngle = 180;
constexpr double kPi = 3.14159265358979323846;
// Candidate aim directions put a mob this far inside a fan edge, so rounding
// the aim to a tile doesn't push that mob out of the fan.
constexpr double kScatterEdgeMarginRad = 3.0 * kPi / 180.0;
// Jumping to another tile for Scatter must gain at least this many hits (and at
// least a quarter more than the current tile), otherwise the archer hops around
// for +1 mob instead of shooting.
constexpr int kMinScatterRepositionGain = 2;
// An equipped arrow pack at or below kArrowSwapDurability is replaced; an
// inventory pack at or below kArrowDropDurability isn't worth equipping and is
// dropped. Bows are item types 500xxx.
constexpr int kArrowSwapDurability = 3;
constexpr int kArrowDropDurability = 5;
constexpr OBJID kBowTypeMin = 500000;
constexpr OBJID kBowTypeMax = 501000;

DWORD ClampMs(int value, int minValue, int maxValue)
{
    return static_cast<DWORD>(std::clamp(value, minValue, maxValue));
}

DWORD GetAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.attackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
}

DWORD GetCycloneAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.cycloneAttackIntervalMs, kMinAttackIntervalMs, kMaxAttackIntervalMs);
}

DWORD GetTargetSwitchAttackIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.targetSwitchAttackIntervalMs,
        kMinTargetSwitchAttackIntervalMs, kMaxTargetSwitchAttackIntervalMs);
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

DWORD GetScatterIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.scatterIntervalMs, kMinScatterIntervalMs, kMaxAttackIntervalMs);
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

int GridDistance(const Position& a, const Position& b)
{
    return (std::max)(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool IsArcherModeEnabled(const AutoHuntSettings& settings)
{
    return settings.archerMode || settings.combatMode == AutoHuntCombatMode::Archer;
}

bool IsScatterLogicEnabled(const AutoHuntSettings& settings)
{
    return IsArcherModeEnabled(settings) && settings.useScatterLogic;
}

int GetRegularArcherAttackRange(const AutoHuntSettings& settings)
{
    return (std::max)(1, settings.rangedAttackRange);
}

int GetArcherSafetyDistance(const AutoHuntSettings& settings)
{
    if (!IsArcherModeEnabled(settings))
        return 0;
    CHero* hero = Game::GetHero();
    if (hero && hero->IsFlyActive())
        return 0;
    return (std::max)(0, settings.archerSafetyDistance);
}

int GetRequiredArcherThreatDistance(int safetyDist)
{
    return safetyDist > 0 ? (safetyDist + kArcherSafetyBufferTiles) : 0;
}

bool IsBow(const CItem* item)
{
    return item && item->GetTypeID() >= kBowTypeMin && item->GetTypeID() < kBowTypeMax;
}

} // anonymous namespace


// =============================================================================
// Scatter geometry
// =============================================================================

CMagic* ArcherHuntPlugin::FindScatterMagic(const CHero* hero) const
{
    if (!hero)
        return nullptr;

    CMagic* best = nullptr;
    for (const auto& magicRef : hero->m_vecMagic) {
        CMagic* magic = magicRef.get();
        if (!magic || !magic->IsEnabled())
            continue;

        const char* name = magic->GetName();
        if (!name || _strnicmp(name, "Scatter", 7) != 0)
            continue;

        if (!best || magic->GetLevel() > best->GetLevel())
            best = magic;
    }

    return best;
}

ArcherHuntPlugin::ScatterGeometry ArcherHuntPlugin::GetScatterGeometry(
    const CHero* hero, const AutoHuntSettings& settings) const
{
    ScatterGeometry geo;
    const CMagic* scatter = FindScatterMagic(hero);
    if (!scatter)
        return geo;

    // magictype `range` is the fan's hit radius; `distance` only limits how far
    // away the aim tile may be (Scatter lvl 5: range=12, distance=20). Data with
    // range=0 falls back to distance.
    const int skillRange = (int)scatter->GetRange();
    const int skillDistance = (int)scatter->GetDistance();
    geo.radius = settings.scatterRangeOverride > 0
        ? settings.scatterRangeOverride
        : (skillRange > 0 ? skillRange : skillDistance);
    geo.aimDistance = skillDistance > 0 ? skillDistance : geo.radius;

    const int fanAngle = std::clamp(settings.scatterFanAngle, kMinScatterFanAngle, kMaxScatterFanAngle);
    geo.halfAngleRad = fanAngle * kPi / 360.0;
    geo.cosHalfAngle = std::cos(geo.halfAngleRad);
    return geo;
}

bool ArcherHuntPlugin::IsTargetInScatterSector(
    const Position& origin, const Position& aimPos, const Position& targetPos, const ScatterGeometry& geo) const
{
    if (geo.radius <= 0)
        return false;
    if (aimPos.x == origin.x && aimPos.y == origin.y)
        return false;
    if (GridDistance(origin, targetPos) > geo.radius)
        return false;

    const double dirX = (double)(aimPos.x - origin.x);
    const double dirY = (double)(aimPos.y - origin.y);
    const double targetX = (double)(targetPos.x - origin.x);
    const double targetY = (double)(targetPos.y - origin.y);
    if (targetX == 0.0 && targetY == 0.0)
        return false;

    // Inside the fan when the angle between the aim direction and the target is
    // at most half the fan angle: dot(dir, t) >= |dir| * |t| * cos(half).
    const double dot = dirX * targetX + dirY * targetY;
    const double lenProduct = std::sqrt((dirX * dirX + dirY * dirY) * (targetX * targetX + targetY * targetY));
    return dot >= lenProduct * geo.cosHalfAngle - 1e-9;
}

bool ArcherHuntPlugin::FindBestScatterShot(const std::vector<CRole*>& targets, const Position& origin,
    const ScatterGeometry& geo, int minimumHits, Position& outCastPos, CRole*& outPrimaryTarget, int& outHitCount) const
{
    outCastPos = {};
    outPrimaryTarget = nullptr;
    outHitCount = 0;

    if (!geo.IsValid() || targets.empty())
        return false;

    const int requiredHits = (std::max)(1, minimumHits);
    int inRadiusCount = 0;
    for (CRole* target : targets) {
        if (target && GridDistance(origin, target->m_posMap) <= geo.radius)
            ++inRadiusCount;
    }
    if (inRadiusCount < requiredHits)
        return false;

    // Aim at the fan's edge (capped by the aim distance): a far aim tile keeps
    // the tile-rounding error in direction to a few degrees, where aiming at an
    // adjacent mob only allows 45-degree steps. Near the map border the aim is
    // pulled in until it lands on the map.
    const CGameMap* map = Game::GetMap();
    const int aimLength = (std::min)(geo.radius, geo.aimDistance);
    auto makeAimTile = [&](double angle, Position& outAim) {
        for (int len = aimLength; len >= 1; --len) {
            const Position aim = {
                origin.x + (int)std::lround(std::cos(angle) * len),
                origin.y + (int)std::lround(std::sin(angle) * len)
            };
            if (aim.x == origin.x && aim.y == origin.y)
                return false;
            if (map && (aim.x < 0 || aim.y < 0
                    || aim.x >= map->m_sizeMap.iWidth || aim.y >= map->m_sizeMap.iHeight)) {
                continue;
            }
            outAim = aim;
            return true;
        }
        return false;
    };

    // Candidate directions: straight at each mob, and with that mob just inside
    // either fan edge. The best fan always has a mob at (or near) one of its
    // edges, so this finds it up to tile rounding. Each candidate is scored with
    // its rounded aim tile, since that is what the server sees.
    const double edgeOffset = geo.halfAngleRad - (std::min)(kScatterEdgeMarginRad, geo.halfAngleRad * 0.25);
    const double directionOffsets[] = {0.0, edgeOffset, -edgeOffset};

    bool found = false;
    int bestPrimaryDist = (std::numeric_limits<int>::max)();

    for (CRole* directionTarget : targets) {
        if (!directionTarget)
            continue;

        const Position& dirPos = directionTarget->m_posMap;
        if (dirPos.x == origin.x && dirPos.y == origin.y)
            continue;
        if (GridDistance(origin, dirPos) > geo.radius)
            continue;

        const double baseAngle = std::atan2((double)(dirPos.y - origin.y), (double)(dirPos.x - origin.x));
        for (double offset : directionOffsets) {
            Position candidateAimPos = {};
            if (!makeAimTile(baseAngle + offset, candidateAimPos))
                continue;

            int hitCount = 0;
            CRole* primaryTarget = nullptr;
            int primaryDist = (std::numeric_limits<int>::max)();
            for (CRole* target : targets) {
                if (!target || !IsTargetInScatterSector(origin, candidateAimPos, target->m_posMap, geo))
                    continue;

                ++hitCount;
                const int targetDist = GridDistance(origin, target->m_posMap);
                if (!primaryTarget || targetDist < primaryDist) {
                    primaryTarget = target;
                    primaryDist = targetDist;
                }
            }

            if (hitCount < requiredHits || !primaryTarget)
                continue;

            if (!found
                || hitCount > outHitCount
                || (hitCount == outHitCount && primaryDist < bestPrimaryDist)) {
                found = true;
                outCastPos = candidateAimPos;
                outPrimaryTarget = primaryTarget;
                outHitCount = hitCount;
                bestPrimaryDist = primaryDist;
            }

            // Every mob in radius is hit: no direction can do better.
            if (outHitCount == inRadiusCount)
                return true;
        }
    }

    return found;
}

bool ArcherHuntPlugin::FindBestScatterApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& targets, const ScatterGeometry& geo, int minimumHits, Position& outApproachPos,
    Position& outCastPos, CRole*& outPrimaryTarget, int& outHitCount,
    bool preferFarTiles) const
{
    outApproachPos = {};
    outCastPos = {};
    outPrimaryTarget = nullptr;
    outHitCount = 0;

    if (!hero || !map || !geo.IsValid() || !IsArcherModeEnabled(settings))
        return false;

    const int requiredHits = (std::max)(1, minimumHits);
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const int localTargetRadius = geo.radius + CGameMap::MAX_JUMP_DIST + 2;
    std::vector<CRole*> localTargets;
    localTargets.reserve(targets.size());
    for (CRole* target : targets) {
        if (target && GridDistance(effectivePos, target->m_posMap) <= localTargetRadius)
            localTargets.push_back(target);
    }

    if (localTargets.empty())
        return false;

    const Position jumpOrigin = GetEffectiveHeroPosition(hero);
    const int safetyDist = GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    bool found = false;
    int bestMinThreatDist = -1;
    float bestMoveDist = preferFarTiles ? 0.0f : (std::numeric_limits<float>::max)();
    float bestCastDist = (std::numeric_limits<float>::max)();

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const Position candidate = {jumpOrigin.x + dx, jumpOrigin.y + dy};
            if (candidate.x == jumpOrigin.x && candidate.y == jumpOrigin.y)
                continue;
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(jumpOrigin.x, jumpOrigin.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;
            int minThreatDist = (std::numeric_limits<int>::max)();
            if (requiredThreatDist > 0) {
                for (CRole* t : localTargets) {
                    if (!t)
                        continue;
                    const int threatDist = CGameMap::TileDist(candidate.x, candidate.y, t->m_posMap.x, t->m_posMap.y);
                    if (threatDist < minThreatDist)
                        minThreatDist = threatDist;
                }
                if (minThreatDist < requiredThreatDist)
                    continue;
            }

            // Upper bound before the fan search: only mobs within the hit
            // radius of this tile can be hit from it.
            int inRadiusCount = 0;
            for (CRole* t : localTargets) {
                if (t && GridDistance(candidate, t->m_posMap) <= geo.radius)
                    ++inRadiusCount;
            }
            if (inRadiusCount < requiredHits || (found && inRadiusCount < outHitCount))
                continue;

            Position candidateCastPos = {};
            CRole* candidatePrimaryTarget = nullptr;
            int candidateHitCount = 0;
            if (!FindBestScatterShot(localTargets, candidate, geo, requiredHits,
                    candidateCastPos, candidatePrimaryTarget, candidateHitCount)) {
                continue;
            }

            const float moveDist = jumpOrigin.DistanceTo(candidate);
            const float castDist = candidate.DistanceTo(candidateCastPos);
            const bool betterMove = preferFarTiles ? (moveDist > bestMoveDist) : (moveDist < bestMoveDist);
            if (!found
                || candidateHitCount > outHitCount
                || (candidateHitCount == outHitCount && minThreatDist > bestMinThreatDist)
                || (candidateHitCount == outHitCount && minThreatDist == bestMinThreatDist && betterMove)
                || (candidateHitCount == outHitCount && minThreatDist == bestMinThreatDist
                    && moveDist == bestMoveDist && castDist < bestCastDist)) {
                found = true;
                outApproachPos = candidate;
                outCastPos = candidateCastPos;
                outPrimaryTarget = candidatePrimaryTarget;
                outHitCount = candidateHitCount;
                bestMinThreatDist = minThreatDist;
                bestMoveDist = moveDist;
                bestCastDist = castDist;
            }
        }
    }

    return found;
}


// =============================================================================
// Retreat
// =============================================================================

bool ArcherHuntPlugin::FindSafeArcherRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::vector<CRole*>& threats, CRole* target, Position& outRetreatPos,
    int safetyDistOverride) const
{
    outRetreatPos = {};
    const int safetyDist = safetyDistOverride > 0 ? safetyDistOverride : GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    if (!hero || !map || requiredThreatDist <= 0 || threats.empty())
        return false;

    const Position heroPos = GetEffectiveHeroPosition(hero);
    const int attackRange = GetRegularArcherAttackRange(settings);
    const DWORD now = GetTickCount();
    const bool avoidFailedDest = m_lastFailedRetreatTick != 0
        && (now - m_lastFailedRetreatTick) < kFailedRetreatDestAvoidMs;

    bool found = false;
    bool bestSafe = false;
    bool bestInRange = false;
    int bestMinThreatDist = 0;
    float bestMoveDist = (std::numeric_limits<float>::max)();

    for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
        for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
            const Position candidate = {heroPos.x + dx, heroPos.y + dy};
            if (candidate.x == heroPos.x && candidate.y == heroPos.y)
                continue;
            if (avoidFailedDest
                && candidate.x == m_lastFailedRetreatDest.x
                && candidate.y == m_lastFailedRetreatDest.y) {
                continue;
            }
            if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) > CGameMap::MAX_JUMP_DIST)
                continue;
            if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                continue;
            if (!map->IsWalkable(candidate.x, candidate.y))
                continue;
            if (IsTileOccupied(candidate.x, candidate.y))
                continue;
            if (!map->CanJump(heroPos.x, heroPos.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                continue;

            int minThreatDist = (std::numeric_limits<int>::max)();
            for (CRole* t : threats) {
                if (!t) continue;
                const int d = CGameMap::TileDist(candidate.x, candidate.y, t->m_posMap.x, t->m_posMap.y);
                if (d < minThreatDist)
                    minThreatDist = d;
            }

            const bool isSafe = minThreatDist >= requiredThreatDist;
            const bool inAttackRange = target && attackRange > 0
                && CGameMap::TileDist(candidate.x, candidate.y, target->m_posMap.x, target->m_posMap.y) <= attackRange;
            const float moveDist = heroPos.DistanceTo(candidate);

            const bool better = !found
                || (isSafe && !bestSafe)
                || (isSafe == bestSafe && inAttackRange && !bestInRange)
                || (isSafe == bestSafe && inAttackRange == bestInRange && minThreatDist > bestMinThreatDist)
                || (isSafe == bestSafe && inAttackRange == bestInRange && minThreatDist == bestMinThreatDist && moveDist < bestMoveDist);

            if (better) {
                found = true;
                outRetreatPos = candidate;
                bestSafe = isSafe;
                bestInRange = inAttackRange;
                bestMinThreatDist = minThreatDist;
                bestMoveDist = moveDist;
            }
        }
    }

    return found;
}

bool ArcherHuntPlugin::TryArcherDangerRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, CRole* target)
{
    if (!hero || !map)
        return false;

    if (m_buffMgr.IsPreLandingRetreat()) {
        static Log::Throttle s_logPreLanding;
        if (s_logPreLanding.Allow())
            spdlog::trace("[hunt] Retreat skip: preLandingRetreat");
        return false;
    }

    const int safetyDist = GetArcherSafetyDistance(settings);
    const int requiredThreatDist = GetRequiredArcherThreatDistance(safetyDist);
    if (requiredThreatDist <= 0)
        return false;

    const DWORD now = GetTickCount();
    if (hero->IsJumping()) {
        static Log::Throttle s_logJumping;
        if (s_logJumping.Allow())
            spdlog::trace("[hunt] Retreat skip: IsJumping pos=({},{})", hero->m_posMap.x, hero->m_posMap.y);
        return false;
    }
    if (UpdatePendingJumpState(hero, now)) {
        static Log::Throttle s_logPendingJump;
        if (s_logPendingJump.Allow())
            spdlog::trace("[hunt] Retreat skip: pendingJump pos=({},{}) dest=({},{}) age={}ms",
                hero->m_posMap.x, hero->m_posMap.y,
                m_pendingJumpDest.x, m_pendingJumpDest.y, now - m_pendingJumpTick);
        return false;
    }
    if (m_retreatCooldownTick != 0 && now < m_retreatCooldownTick) {
        static Log::Throttle s_logCooldown;
        if (s_logCooldown.Allow())
            spdlog::trace("[hunt] Retreat skip: cooldown {}ms remaining", m_retreatCooldownTick - now);
        return false;
    }
    m_retreatCooldownTick = 0;

    const std::vector<CRole*> nearbyTargets = CollectHuntTargets(settings);
    const Position heroPos = GetEffectiveHeroPosition(hero);
    int closestDist = 9999;
    for (CRole* t : nearbyTargets) {
        if (!t)
            continue;
        const int d = CGameMap::TileDist(heroPos.x, heroPos.y, t->m_posMap.x, t->m_posMap.y);
        if (d < closestDist)
            closestDist = d;
    }
    if (closestDist >= requiredThreatDist)
        return false;

    spdlog::debug("[hunt] THREATENED closestMob={} safetyDist={} pos=({},{}) pendingJump={}",
        closestDist, safetyDist, heroPos.x, heroPos.y, m_pendingJumpTick);

    auto urgentRetreatJump = [&](const Position& dest, const char* reason) {
        if (Pathfinder::Get().IsActive())
            Pathfinder::Get().Stop();
        const DWORD jumpNow = GetTickCount();
        spdlog::debug("[hunt] Retreat JUMP ({},{}) -> ({},{}) reason={}",
            hero->m_posMap.x, hero->m_posMap.y, dest.x, dest.y, reason);
        hero->Jump(dest.x, dest.y);
        m_lastMoveTick = jumpNow;
        ArmPendingJump(hero, dest, jumpNow, true);
        SetState(AutoHuntState::ApproachTarget, reason);
    };

    Position retreatPos = {};
    const bool hasSafeRetreat = FindSafeArcherRetreat(hero, map, settings, nearbyTargets, target, retreatPos);
    if (hasSafeRetreat && IsScatterLogicEnabled(settings)) {
        const ScatterGeometry scatterGeo = GetScatterGeometry(hero, settings);
        const int minHits = (std::max)(1, settings.minimumScatterHits);
        if (scatterGeo.IsValid()) {
            Position scatterPos = {};
            Position scatterCast = {};
            CRole* scatterTarget = nullptr;
            int scatterHits = 0;
            if (FindBestScatterApproach(hero, map, settings, nearbyTargets,
                    scatterGeo, minHits, scatterPos, scatterCast,
                    scatterTarget, scatterHits, /*preferFarTiles=*/true)) {
                int scatterMinThreat = (std::numeric_limits<int>::max)();
                int retreatMinThreat = (std::numeric_limits<int>::max)();
                for (CRole* t : nearbyTargets) {
                    if (!t)
                        continue;
                    const int sd = CGameMap::TileDist(scatterPos.x, scatterPos.y, t->m_posMap.x, t->m_posMap.y);
                    const int rd = CGameMap::TileDist(retreatPos.x, retreatPos.y, t->m_posMap.x, t->m_posMap.y);
                    if (sd < scatterMinThreat)
                        scatterMinThreat = sd;
                    if (rd < retreatMinThreat)
                        retreatMinThreat = rd;
                }
                if (scatterMinThreat >= retreatMinThreat) {
                    spdlog::debug("[hunt] Scatter retreat tile ({},{}) minThreat={} >= safe ({},{}) minThreat={}, hits={}",
                        scatterPos.x, scatterPos.y, scatterMinThreat,
                        retreatPos.x, retreatPos.y, retreatMinThreat, scatterHits);
                    urgentRetreatJump(scatterPos, "Retreating to scatter position");
                    return true;
                }

                spdlog::debug("[hunt] Scatter tile ({},{}) minThreat={} < safe ({},{}) minThreat={}, preferring safety",
                    scatterPos.x, scatterPos.y, scatterMinThreat,
                    retreatPos.x, retreatPos.y, retreatMinThreat);
            }
        }
    }

    if (hasSafeRetreat) {
        urgentRetreatJump(retreatPos, "Retreating to safe distance");
        return true;
    }

    spdlog::debug("[hunt] FindSafeArcherRetreat FAILED targets={}", nearbyTargets.size());
    return false;
}


// =============================================================================
// Target finding
// =============================================================================

CRole* ArcherHuntPlugin::FindBestArcherTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos, int* outClumpSize, bool* outUseScatter) const
{
    if (outApproachPos)
        *outApproachPos = {};
    if (outAttackPos)
        *outAttackPos = {};
    if (outClumpSize)
        *outClumpSize = 0;
    if (outUseScatter)
        *outUseScatter = false;
    if (!hero || !map)
        return nullptr;

    const bool hasPreferFilter = settings.monsterPreferNames[0] != '\0';
    std::vector<CRole*> targets = hasPreferFilter ? CollectHuntTargets(settings, true) : std::vector<CRole*>{};
    if (targets.empty())
        targets = CollectHuntTargets(settings);
    if (targets.empty())
        return nullptr;

    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const ScatterGeometry scatterGeo = GetScatterGeometry(hero, settings);
    const int minimumScatterHits = (std::max)(1, settings.minimumScatterHits);
    const Position effectivePos = GetEffectiveHeroPosition(hero);
    const DWORD now = GetTickCount();
    const bool retreatHoldActive = TickIsFuture(m_retreatHoldUntilTick, now);
    if (IsScatterLogicEnabled(settings) && scatterGeo.IsValid()) {
        Position scatterCastPos = {};
        CRole* scatterTarget = nullptr;
        int scatterHits = 0;
        const bool hasLocalShot = FindBestScatterShot(targets, effectivePos, scatterGeo, minimumScatterHits,
                scatterCastPos, scatterTarget, scatterHits);

        // Jumping to a better clump only when clumps are prioritized; otherwise
        // Scatter is cast only from where the hero already stands.
        Position scatterApproachPos = {};
        Position scatterApproachCastPos = {};
        CRole* scatterApproachTarget = nullptr;
        int scatterApproachHits = 0;
        bool hasApproachShot = false;
        const bool shouldSearchApproach = settings.prioritizeScatterClumps && !retreatHoldActive
            && (!hasLocalShot || (now - m_lastScatterApproachTick >= 500));
        if (shouldSearchApproach) {
            m_lastScatterApproachTick = now;
            hasApproachShot = FindBestScatterApproach(hero, map, settings, targets, scatterGeo, minimumScatterHits,
                    scatterApproachPos, scatterApproachCastPos, scatterApproachTarget, scatterApproachHits);
        }

        const int repositionGain = (std::max)(kMinScatterRepositionGain, (scatterHits + 3) / 4);
        if (hasApproachShot && scatterApproachHits >= scatterHits + repositionGain) {
            if (outApproachPos)
                *outApproachPos = scatterApproachPos;
            if (outAttackPos)
                *outAttackPos = scatterApproachCastPos;
            if (outClumpSize)
                *outClumpSize = scatterApproachHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterApproachTarget;
        }

        if (hasLocalShot) {
            if (outAttackPos)
                *outAttackPos = scatterCastPos;
            if (outClumpSize)
                *outClumpSize = scatterHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterTarget;
        }

        if (hasApproachShot) {
            if (outApproachPos)
                *outApproachPos = scatterApproachPos;
            if (outAttackPos)
                *outAttackPos = scatterApproachCastPos;
            if (outClumpSize)
                *outClumpSize = scatterApproachHits;
            if (outUseScatter)
                *outUseScatter = true;
            return scatterApproachTarget;
        }
    }

    // No Scatter shot with enough hits (or Scatter logic off): fall back to
    // regular single-target shots.

    // If there's already a target within attack range, hit it
    const int effectiveAttackRange = regularAttackRange > 0 ? regularAttackRange : kReliableAttackRange;
    if (CRole* inRange = FindClosestTarget(targets, effectivePos, effectiveAttackRange))
        return inRange;

    if (retreatHoldActive)
        return nullptr;

    const int scoutStopRange = regularAttackRange > 0
        ? regularAttackRange : (std::max)(1, settings.actionRadius);
    const float scoutClusterRadius = (float)(std::max)(3, scoutStopRange + 2);
    int scoutClusterSize = 0;
    if (CRole* scoutTarget = FindBestClusterTarget(targets, effectivePos, scoutClusterRadius, &scoutClusterSize);
        scoutTarget && scoutClusterSize > 1) {
        Position scoutApproachPos;
        if (FindBestSingleTargetApproach(hero, map, settings, scoutTarget, effectivePos, scoutApproachPos, scoutStopRange) && outApproachPos)
            *outApproachPos = scoutApproachPos;
        if (outClumpSize)
            *outClumpSize = scoutClusterSize;
        return scoutTarget;
    }

    CRole* closest = FindClosestTarget(targets, effectivePos);
    if (!closest)
        return nullptr;

    Position singleTargetApproachPos;
    if (FindBestSingleTargetApproach(hero, map, settings, closest, effectivePos, singleTargetApproachPos, regularAttackRange) && outApproachPos)
        *outApproachPos = singleTargetApproachPos;
    return closest;
}

bool ArcherHuntPlugin::FindArcherPatrolPosition(CHero* hero, CGameMap* map,
    const AutoHuntSettings& settings, Position& outPatrolPos) const
{
    outPatrolPos = {};
    if (!hero || !map)
        return false;

    const Position anchor = GetZoneAnchor(settings);
    if (IsZeroPos(anchor))
        return false;

    const Position heroPos = GetEffectiveHeroPosition(hero);
    const int patrolRadius = settings.zoneMode == AutoHuntZoneMode::Circle
        ? (std::max)(1, settings.zoneRadius - 1)
        : (std::max)(6, settings.actionRadius * 2);
    if (patrolRadius <= 0)
        return false;

    static const Position kPatrolDirs[] = {
        {1, 0},
        {1, 1},
        {0, 1},
        {-1, 1},
        {-1, 0},
        {-1, -1},
        {0, -1},
        {1, -1},
    };

    int baseDir = 0;
    if (heroPos.x != anchor.x || heroPos.y != anchor.y) {
        long long bestDot = (std::numeric_limits<long long>::min)();
        for (int i = 0; i < (int)std::size(kPatrolDirs); ++i) {
            const long long dot =
                (long long)(heroPos.x - anchor.x) * kPatrolDirs[i].x
                + (long long)(heroPos.y - anchor.y) * kPatrolDirs[i].y;
            if (dot > bestDot) {
                bestDot = dot;
                baseDir = i;
            }
        }
    }

    for (int dirOffset = 1; dirOffset <= (int)std::size(kPatrolDirs); ++dirOffset) {
        const Position dir = kPatrolDirs[(baseDir + dirOffset) % (int)std::size(kPatrolDirs)];
        const Position desired = {
            anchor.x + dir.x * patrolRadius,
            anchor.y + dir.y * patrolRadius
        };
        const float currentDesiredDist = heroPos.DistanceTo(desired);
        if (currentDesiredDist <= 2.0f)
            continue;

        bool found = false;
        Position bestCandidate = {};
        float bestDesiredDist = (std::numeric_limits<float>::max)();
        float bestMoveDist = (std::numeric_limits<float>::max)();
        for (int dx = -CGameMap::MAX_JUMP_DIST; dx <= CGameMap::MAX_JUMP_DIST; ++dx) {
            for (int dy = -CGameMap::MAX_JUMP_DIST; dy <= CGameMap::MAX_JUMP_DIST; ++dy) {
                const Position candidate = {heroPos.x + dx, heroPos.y + dy};
                if (candidate.x == heroPos.x && candidate.y == heroPos.y)
                    continue;
                if (CGameMap::TileDist(heroPos.x, heroPos.y, candidate.x, candidate.y) > CGameMap::MAX_JUMP_DIST)
                    continue;
                if (!IsPointInZone(settings, settings.zoneMapId, candidate))
                    continue;
                if (!map->IsWalkable(candidate.x, candidate.y))
                    continue;
                if (IsTileOccupied(candidate.x, candidate.y))
                    continue;
                if (!map->CanJump(heroPos.x, heroPos.y, candidate.x, candidate.y, CGameMap::GetHeroAltThreshold()))
                    continue;

                const float desiredDist = candidate.DistanceTo(desired);
                const float moveDist = heroPos.DistanceTo(candidate);
                if (!found || desiredDist < bestDesiredDist
                    || (desiredDist == bestDesiredDist && moveDist < bestMoveDist)) {
                    found = true;
                    bestCandidate = candidate;
                    bestDesiredDist = desiredDist;
                    bestMoveDist = moveDist;
                }
            }
        }

        if (found && bestDesiredDist + 0.1f < currentDesiredDist) {
            outPatrolPos = bestCandidate;
            return true;
        }
    }

    return false;
}


// =============================================================================
// FindBestTarget override
// =============================================================================

CRole* ArcherHuntPlugin::FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    Position* outApproachPos, Position* outAttackPos,
    int* outClumpSize, bool* outUseScatter)
{
    return FindBestArcherTarget(hero, map, settings, outApproachPos, outAttackPos, outClumpSize, outUseScatter);
}


// =============================================================================
// HandleCombatRetreat override
// =============================================================================

bool ArcherHuntPlugin::HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target)
{
    if (!hero || !map)
        return false;

    // Handle pending jump landing for retreat tracking
    if (m_pendingJumpTick != 0 && m_pendingJumpIsRetreat) {
        const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
            && hero->m_posMap.y == m_pendingJumpDest.y;
        if (landed) {
            m_retreatHoldUntilTick = GetTickCount() + kArcherRetreatHoldMs;
            m_lastFailedRetreatDest = {};
            m_lastFailedRetreatTick = 0;
        }
    }

    // Handle failed retreat tracking
    if (m_pendingJumpTick != 0 && m_pendingJumpIsRetreat && !hero->IsJumping()) {
        const DWORD now = GetTickCount();
        const bool landed = hero->m_posMap.x == m_pendingJumpDest.x
            && hero->m_posMap.y == m_pendingJumpDest.y;
        if (!landed) {
            const DWORD age = now - m_pendingJumpTick;
            if (age > 2000) {
                m_lastFailedRetreatDest = m_pendingJumpDest;
                m_lastFailedRetreatTick = now;
                m_retreatCooldownTick = 0;
            }
        }
    }

    const DWORD now = GetTickCount();

    // Try danger retreat first
    if (TryArcherDangerRetreat(hero, map, settings, target))
        return true;

    // Hold position after successful retreat
    if (TickIsFuture(m_retreatHoldUntilTick, now)) {
        const Position effectivePos = GetEffectiveHeroPosition(hero);
        const int regularAttackRange = GetRegularArcherAttackRange(settings);
        const int requiredAttackRange = (std::max)(1, m_lastScatterRange > 0 ? m_lastScatterRange : regularAttackRange);
        const int attackDist = target
            ? CGameMap::TileDist(effectivePos.x, effectivePos.y, target->m_posMap.x, target->m_posMap.y)
            : 9999;
        if (attackDist > requiredAttackRange) {
            SetState(AutoHuntState::AttackTarget, "Holding safe position after retreat");
            return true;
        }
    }

    return false;
}


// =============================================================================
// HandleCombatApproach override
// =============================================================================

void ArcherHuntPlugin::HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& approachPos, bool movementCommitted)
{
    if (!hero || !map || !target)
        return;

    // A Scatter shot from the current tile needs no approach: FindBestScatterShot
    // already checked the aim tile is within reach from here. Closing to
    // rangedAttackRange would only walk into the pack.
    if (m_lastUseScatter && IsZeroPos(approachPos))
        return;

    const Position effectiveAttackPos = GetEffectiveHeroPosition(hero);
    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const bool useRegularArcherRange = !IsZeroPos(approachPos) || regularAttackRange > 0;
    const bool bypassMobRangeChecks = regularAttackRange <= 0;

    // Compute attack distance based on scatter or regular
    // (scatter attackPos is passed through approachPos for scatter-approach tiles)
    const int moveDist = CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y,
        target->m_posMap.x, target->m_posMap.y);
    const int approachDist = !IsZeroPos(approachPos)
        ? CGameMap::TileDist(effectiveAttackPos.x, effectiveAttackPos.y, approachPos.x, approachPos.y)
        : moveDist;
    const int requiredAttackRange = useRegularArcherRange ? regularAttackRange : kReliableAttackRange;

    if (bypassMobRangeChecks)
        return;  // No approach needed when range checks bypassed

    // If approach is within range, skip
    if (!IsZeroPos(approachPos) && approachDist == 0)
        return;
    if (IsZeroPos(approachPos) && moveDist <= requiredAttackRange)
        return;

    // Pre-landing retreat: don't approach
    if (m_buffMgr.IsPreLandingRetreat())
        return;

    // Don't re-evaluate while movement is in flight
    if (movementCommitted)
        return;

    const int actionRadius = (std::max)(1, settings.actionRadius);
    const int localWalkRadius = regularAttackRange > 0
        ? (std::max)(actionRadius, regularAttackRange)
        : (std::max)(actionRadius, (std::max)(1, m_lastScatterRange));

    if (!IsZeroPos(approachPos)) {
        const bool startedPath = StartPathTo(hero, map, approachPos, 0);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget, "Jumping to scatter clump");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    } else {
        const bool startedPath = StartPathNearTarget(hero, map, target->m_posMap, requiredAttackRange);
        if (startedPath) {
            SetState(AutoHuntState::ApproachTarget, "Closing distance to target");
        } else {
            SetState(AutoHuntState::ApproachTarget, "Unable to reach target");
        }
    }
}


// =============================================================================
// HandleCombatAttack override
// =============================================================================

void ArcherHuntPlugin::HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    CRole* target, const Position& attackPos, DWORD now)
{
    if (!hero || !target)
        return;

    const bool movementCommitted = hero->IsJumping() || (m_pendingJumpTick != 0);
    const int regularAttackRange = GetRegularArcherAttackRange(settings);
    const bool bypassMobRangeChecks = regularAttackRange <= 0;
    const bool useScatter = !IsZeroPos(attackPos) && IsScatterLogicEnabled(settings);
    const int clumpSize = m_lastClumpSize;
    const bool isScatterClump = useScatter && clumpSize >= (std::max)(1, settings.minimumScatterHits);

    const bool targetChanged = m_targetSwitched;
    const bool justFinishedApproach = (m_state == AutoHuntState::ApproachTarget);
    const DWORD attackInterval = hero->IsCycloneActive()
        ? GetCycloneAttackIntervalMs(settings)
        : GetAttackIntervalMs(settings);
    const DWORD nextAttackDelay = (targetChanged || justFinishedApproach)
        ? GetTargetSwitchAttackIntervalMs(settings)
        : attackInterval;

    if (Pathfinder::Get().IsActive())
        Pathfinder::Get().Stop();

    // Scatter attack
    m_lastScatterAimPos = useScatter ? attackPos : Position{};
    if (useScatter) {
        if (CMagic* scatter = FindScatterMagic(hero)) {
            const DWORD scatterDelay = (std::max)(nextAttackDelay, GetScatterIntervalMs(settings));
            if (!IsZeroPos(attackPos) && now - m_lastAttackTick >= scatterDelay) {
                hero->MagicAttack(scatter->GetMagicType(), attackPos);
                m_lastAttackTick = now;
                NoteActionSent(hero, now);
            }
            if (hero->IsJumping()) {
                SetState(AutoHuntState::AttackTarget,
                    isScatterClump ? "Casting Scatter during jump" : "Casting Scatter at target during jump");
            } else if (isScatterClump) {
                SetState(AutoHuntState::AttackTarget, "Casting Scatter at mob clump");
            } else {
                SetState(AutoHuntState::AttackTarget, "Casting Scatter");
            }
            return;
        }
    }

    // Regular ranged attack
    if (now - m_lastAttackTick >= nextAttackDelay) {
        hero->ShootTarget(target->GetID());
        m_lastAttackTick = now;
        NoteActionSent(hero, now);
    }

    const int moveDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y,
        target->m_posMap.x, target->m_posMap.y);
    if (movementCommitted && moveDist > kReliableAttackRange) {
        SetState(AutoHuntState::AttackTarget, "Attacking target during jump");
    } else {
        SetState(AutoHuntState::AttackTarget, "Attacking target");
    }
}


// =============================================================================
// HandleCombatItems override
// =============================================================================

bool ArcherHuntPlugin::HandleCombatItems(CHero* hero, const AutoHuntSettings& settings)
{
    return TryManageArrows(hero, settings);
}


// =============================================================================
// NeedsTownRunArrows override
// =============================================================================

bool ArcherHuntPlugin::NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    return NeedsArrows(hero, settings);
}


// =============================================================================
// HandleNoTargetIdle override
// =============================================================================

bool ArcherHuntPlugin::HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings)
{
    if (!hero || !map)
        return false;

    // Hold position after retreat even with no target
    if (TickIsFuture(m_retreatHoldUntilTick, GetTickCount())) {
        m_targetId = 0;
        SetState(AutoHuntState::AttackTarget, "Holding safe position after retreat");
        return true;
    }

    Position patrolPos = {};
    if (FindArcherPatrolPosition(hero, map, settings, patrolPos) && StartPathTo(hero, map, patrolPos, 0)) {
        m_targetId = 0;
        SetState(AutoHuntState::AcquireTarget, "Scouting hunt zone for mob clumps");
        return true;
    }

    return false;
}


// =============================================================================
// RefreshCombatState override
// =============================================================================

void ArcherHuntPlugin::RefreshCombatState(CHero* hero, const AutoHuntSettings& settings)
{
    const ScatterGeometry geo = GetScatterGeometry(hero, settings);
    m_lastScatterRange = geo.radius;
    m_lastScatterAimDistance = geo.aimDistance;
}


// =============================================================================
// Arrow management
// =============================================================================

bool ArcherHuntPlugin::TryManageArrows(CHero* hero, const AutoHuntSettings& settings)
{
    if (!hero || !IsArcherModeEnabled(settings))
        return false;

    const DWORD now = GetTickCount();
    if (now - m_lastArrowTick < GetItemActionIntervalMs(settings))
        return false;

    // Equip the fullest pack when the equipped one is nearly used up, or when
    // the slot is empty because the last pack ran out and vanished.
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    const bool needsPack = equipped
        ? (equipped->IsArrow() && equipped->GetDurability() <= kArrowSwapDurability)
        : IsBow(hero->GetEquip(EquipSlot::RWEAPON));
    if (needsPack) {
        CItem* bestPack = nullptr;
        for (const auto& itemRef : hero->m_deqItem) {
            if (!itemRef || !itemRef->IsArrow() || itemRef->GetDurability() <= kArrowDropDurability)
                continue;
            if (!bestPack || itemRef->GetDurability() > bestPack->GetDurability())
                bestPack = itemRef.get();
        }
        if (bestPack) {
            hero->EquipItem(bestPack->GetID(), EquipSlot::LWEAPON);
            m_lastArrowTick = now;
            SetState(AutoHuntState::Recover, "Equipping arrows");
            return true;
        }
    }

    // Drop inventory packs too empty to be worth equipping
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef || !itemRef->IsArrow())
            continue;
        if (itemRef->GetDurability() > kArrowDropDurability)
            continue;

        hero->DropItem(itemRef->GetID(), hero->m_posMap);
        m_lastArrowTick = now;
        SetState(AutoHuntState::Recover, "Dropping depleted arrows");
        return true;
    }

    return false;
}

bool ArcherHuntPlugin::NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const
{
    if (!IsArcherModeEnabled(settings) || !settings.buyArrows)
        return false;
    return CountUsableArrowPacks(hero) < settings.arrowBuyCount;
}

int ArcherHuntPlugin::CountUsableArrowPacks(const CHero* hero) const
{
    if (!hero)
        return 0;

    int count = 0;
    // Count equipped arrows
    CItem* equipped = hero->GetEquip(EquipSlot::LWEAPON);
    if (equipped && equipped->IsArrow() && equipped->GetDurability() > kArrowSwapDurability)
        ++count;

    // Count inventory packs that would be equipped rather than dropped
    for (const auto& itemRef : hero->m_deqItem) {
        if (!itemRef || !itemRef->IsArrow())
            continue;
        if (itemRef->GetDurability() > kArrowDropDurability)
            ++count;
    }

    return count;
}


// =============================================================================
// RenderCombatUI override
// =============================================================================

void ArcherHuntPlugin::RenderCombatUI(AutoHuntSettings& settings)
{
    ImGui::Checkbox("Use Scatter Logic", &settings.useScatterLogic);
    ImGui::SliderInt("Ranged Attack Range", &settings.rangedAttackRange, 1, CGameMap::MAX_JUMP_DIST);
    ImGui::SliderInt("Safety Distance", &settings.archerSafetyDistance, 0, CGameMap::MAX_JUMP_DIST);
    if (settings.useScatterLogic) {
        ImGui::Checkbox("Prioritize Scatter Clumps", &settings.prioritizeScatterClumps);
        ImGui::TextDisabled("When off, Scatter is only cast from the current tile (no jumping to bigger clumps).");
        ImGui::SliderInt("Minimum Scatter Hits", &settings.minimumScatterHits, 1, 12);
        ImGui::SliderInt("Scatter Range Override", &settings.scatterRangeOverride, 0, CGameMap::MAX_JUMP_DIST);
        if (settings.scatterRangeOverride == 0)
            ImGui::TextDisabled("0 = hit radius from the skill's range (current: %d, aim distance: %d)",
                m_lastScatterRange, m_lastScatterAimDistance);
        ImGui::SliderInt("Scatter Fan Angle", &settings.scatterFanAngle,
            kMinScatterFanAngle, kMaxScatterFanAngle, "%d deg");
        ImGui::SliderInt("Scatter Interval (ms)", &settings.scatterIntervalMs, kMinScatterIntervalMs, 2000);
        ImGui::TextDisabled("Archer mode picks the Scatter direction that hits the most mobs inside the fan,");
        ImGui::TextDisabled("from the current or next jump position. Match the fan angle to your server.");
        ImGui::TextDisabled("If no valid Scatter clump is found, auto hunt falls back to regular single-target attacks.");
    } else {
        ImGui::TextDisabled("Archer mode uses regular attacks only when Scatter logic is off.");
    }
    ImGui::TextDisabled("0 disables the regular archer attack range check and attacks immediately.");
    ImGui::TextDisabled("When set above 0, auto hunt stops and attacks from that many tiles away.");
    ImGui::TextDisabled("Safety Distance prevents jumping closer than N tiles to any mob (0 = disabled).");

    ImGui::Checkbox("Buy Arrows", &settings.buyArrows);
    if (settings.buyArrows) {
        int arrowTypeIdInt = (int)settings.arrowTypeId;
        if (ImGui::InputInt("Arrow Type ID", &arrowTypeIdInt, 1, 100))
            settings.arrowTypeId = arrowTypeIdInt > 0 ? (uint32_t)arrowTypeIdInt : 0;
        if (settings.arrowTypeId != 0) {
            if (const ItemTypeInfo* info = GetItemTypeInfo(settings.arrowTypeId))
                ImGui::TextDisabled("  -> %s", info->name.c_str());
            else
                ImGui::TextDisabled("  -> (unknown type)");
        }
        ImGui::SliderInt("Arrow Packs to Maintain", &settings.arrowBuyCount, 1, 10);
    }

}
