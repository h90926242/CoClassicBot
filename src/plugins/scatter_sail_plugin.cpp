#include "scatter_sail_plugin.h"
#include "hunt_targeting.h"
#include "inventory_utils.h"
#include "plugin_mgr.h"
#include "travel_plugin.h"
#include "game.h"
#include "hooks.h"
#include "CHero.h"
#include "CGameMap.h"
#include "CRole.h"
#include "CMagic.h"
#include "CItem.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr int kMinStride = 4;
constexpr int kMaxStride = 16;              // MAX_JUMP_DIST with room for the landing tile to shift
constexpr int kMinLaneWidth = 8;
constexpr int kMaxLaneWidth = 30;
constexpr int kMinRevisitSeconds = 15;
constexpr int kMaxRevisitSeconds = 600;
constexpr int kMinScatterIntervalMs = 100;
constexpr int kMaxScatterIntervalMs = 5000;
constexpr int kMaxChainLeadMs = 200;
constexpr int kMinScatterFanAngle = 30;
constexpr int kMaxScatterFanAngle = 180;
constexpr int kMinItemActionIntervalMs = 100;
constexpr int kMaxItemActionIntervalMs = 5000;
constexpr double kPi = 3.14159265358979323846;

// With Fly a jump takes this long whatever its length (measured live); without
// it the hero covers about kGroundJumpTilesPerSec after a short wind-up.
constexpr DWORD kFlyFlightMs = 257;
constexpr DWORD kGroundJumpWindupMs = 90;
constexpr int kGroundJumpTilesPerSec = 14;
// The client checks a jump from the tile the hero is over when it is issued,
// and flags it as a different move beyond 18 tiles: a queued jump is only
// issued once its landing tile is within this reach of the hero.
constexpr int kQueueReach = 17;
constexpr DWORD kMinJumpIssueGapMs = 50;
constexpr DWORD kTakeoffWaitMs = 400;       // an issued jump that hasn't taken off by then was dropped
constexpr DWORD kFlightLostGraceMs = 500;   // a flight this far overdue was interrupted
constexpr DWORD kIssuedJumpTimeoutMs = 1500;
constexpr DWORD kRefusedLandingAvoidMs = 5000;
// Pauses on the ground between jumps that aren't chained are mostly short with
// a long tail: kMinPauseMs + (longest - kMinPauseMs) * u^3 for a uniform u. A
// "misclick" (a click that hit a rock) costs a longer one.
constexpr int kMinPauseMs = 40;
constexpr int kMaxPauseMaxMs = 3000;
constexpr int kMisclickPauseMinMs = 500;
constexpr int kMisclickPauseMaxMs = 1200;
constexpr int kMaxMisclickChance = 30;
constexpr int kMaxStrideVariation = 60;
// A cast that is ready just before a takeoff waits for it when the landing
// tile promises this many more hits, but no longer than kCastHoldMaxMs.
constexpr int kCastHoldGain = 2;
constexpr DWORD kCastHoldMaxMs = 300;
// While holding a tile with a good fan, look for a better one this often.
constexpr DWORD kHoldReplanMs = 150;
constexpr int kMaxRepositionGain = 10;
constexpr DWORD kSailPauseResetMs = 1000;   // the course starts over after a pause this long
constexpr DWORD kFallbackPathRetryMs = 2000;
constexpr DWORD kFallbackPathMaxMs = 8000;
constexpr DWORD kLootGiveUpMs = 8000;       // loot not picked up by then is skipped for a while
constexpr int kLootWaitRange = 2;           // wait this close to loot a mob stands on

ScatterSailSettings g_scatterSailSettings;

bool SameTile(const Position& a, const Position& b)
{
    return a.x == b.x && a.y == b.y;
}

uint64_t TileKey(const Position& pos)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) << 32) | static_cast<uint32_t>(pos.y);
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return static_cast<DWORD>(std::clamp(settings.itemActionIntervalMs,
        kMinItemActionIntervalMs, kMaxItemActionIntervalMs));
}

DWORD GetScatterIntervalMs(const ScatterSailSettings& sail)
{
    return static_cast<DWORD>(std::clamp(sail.scatterIntervalMs, kMinScatterIntervalMs, kMaxScatterIntervalMs));
}

DWORD EstimateFlightMs(const CHero* hero, const Position& from, const Position& to)
{
    if (hero->IsFlyActive())
        return kFlyFlightMs;
    return kGroundJumpWindupMs + static_cast<DWORD>(from.DistanceTo(to) * 1000.0f / kGroundJumpTilesPerSec);
}

CMagic* FindScatterSkill(const CHero* hero)
{
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

// Zone identity for SailPlanner::SetZone: changes when the zone is edited.
uint64_t ZoneKey(const AutoHuntSettings& settings)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(settings.zoneMapId);
    mix(static_cast<uint64_t>(settings.zoneMode));
    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        mix(TileKey(settings.zoneCenter));
        mix(static_cast<uint64_t>(settings.zoneRadius));
    } else {
        for (const Position& vertex : settings.zonePolygon)
            mix(TileKey(vertex));
    }
    return hash;
}

} // namespace

ScatterSailSettings& GetScatterSailSettings()
{
    return g_scatterSailSettings;
}

// =============================================================================
// Enable / hotkey
// =============================================================================

void ScatterSailPlugin::SetSailEnabled(bool enabled)
{
    SetHuntEnabled(enabled);
    ResetSail();
    if (enabled)
        GetAutoHuntSettings().lastHuntMode = kScatterSailHuntMode;
}

void ScatterSailPlugin::ToggleHuntHotkey()
{
    auto* sail = PluginManager::Get().GetPlugin<ScatterSailPlugin>();
    bool huntRunning = false;
    for (auto& p : PluginManager::Get().GetPlugins()) {
        auto* hunt = dynamic_cast<BaseHuntPlugin*>(p.get());
        if (hunt && hunt->m_enabled)
            huntRunning = true;
    }

    if (sail && !huntRunning && GetAutoHuntSettings().lastHuntMode == kScatterSailHuntMode) {
        spdlog::info("[hunt] Home key: starting {}", sail->GetName());
        sail->SetSailEnabled(true);
        return;
    }
    BaseHuntPlugin::ToggleHuntHotkey();
}

void ScatterSailPlugin::ResetSail()
{
    m_planner.Reset();
    m_mobs.clear();
    m_course.clear();
    m_heading = 0.0f;
    m_hasHeading = false;
    m_plannedHits = 0;
    m_flight = {};
    m_issued = {};
    m_lastJumpIssueTick = 0;
    m_chainNext = true;
    m_pauseMs = 0;
    m_strideFactor = 1.0f;
    m_groundSinceTick = 0;
    m_holding = false;
    m_lastHoldPlanTick = 0;
    m_goal = {};
    m_goalHits = 0;
    m_refusedLandingUntil.clear();
    m_fallbackPathTick = 0;
    m_fallbackPathActive = false;
    m_fallbackPathForLoot = false;
    m_lastSailTick = 0;
    m_lastCastTick = 0;
    m_castHoldTick = 0;
    m_lastCastOrigin = {};
    m_lastCastAim = {};
    m_lastCastHits = 0;
    m_lootId = 0;
    m_lootStartTick = 0;
    m_jumpCount = 0;
    m_queuedJumpCount = 0;
    m_misclickCount = 0;
    m_snapCount = 0;
    m_refusedCount = 0;
    m_castCount = 0;
    m_castHitSum = 0;
}

SailTuning ScatterSailPlugin::GetTuning() const
{
    const ScatterSailSettings& sail = GetScatterSailSettings();
    SailTuning tuning;
    tuning.sweepRadius = std::clamp(sail.laneWidth, kMinLaneWidth, kMaxLaneWidth) / 2;
    tuning.turnRadius = tuning.sweepRadius;
    tuning.revisitMs = static_cast<DWORD>(std::clamp(sail.revisitSeconds, kMinRevisitSeconds, kMaxRevisitSeconds)) * 1000;
    return tuning;
}

// =============================================================================
// Jump chain
// =============================================================================

// Follows the client's jumps. A jump command with status BEGIN hasn't taken
// off yet: on the ground it is winding up, in the air it is queued behind the
// flight in progress. The jump packet goes out at takeoff, so until then the
// server still has the hero where the previous jump ended.
void ScatterSailPlugin::TrackFlight(CHero* hero, DWORD now)
{
    const CCommand& cmd = hero->GetCommand();
    const Position pos = hero->m_posMap;

    if (m_flight.active && (SameTile(pos, m_flight.target)
            || now - m_flight.startTick > m_flight.durationMs + kFlightLostGraceMs)) {
        m_flight.active = false;
        m_groundSinceTick = now;
    }

    auto beginFlight = [&](const Position& target) {
        m_flight.active = true;
        m_flight.from = pos;
        m_flight.target = target;
        m_flight.startTick = now;
        m_flight.durationMs = EstimateFlightMs(hero, pos, target);
        ++m_jumpCount;
        if (m_issued.pending && SameTile(m_issued.target, target)) {
            if (m_issued.queued)
                ++m_queuedJumpCount;
            m_issued.pending = false;
        }
    };

    const bool flying = cmd.iType == _COMMAND_JUMP && cmd.iStatus != _CMDSTATUS_BEGIN
        && !SameTile(cmd.posTarget, pos);
    if (flying && (!m_flight.active || !SameTile(m_flight.target, cmd.posTarget)))
        beginFlight(cmd.posTarget);

    if (!m_issued.pending)
        return;

    if (SameTile(pos, m_issued.target)) {
        // On the landing tile without a flight having been seen: either the
        // client skipped the animation (a "snap"), or another command (an
        // attack reply) covered the jump command while the hero flew.
        if (now < m_issued.earliestLandTick) {
            ++m_snapCount;
            spdlog::debug("[sail] Jump to ({},{}) snapped: there {}ms after it was issued ({}), {}ms after the last cast",
                pos.x, pos.y, now - m_issued.tick, m_issued.queued ? "queued" : "from the ground",
                now - m_lastCastTick);
        }
        ++m_jumpCount;
        m_issued.pending = false;
        m_flight.active = false;
        m_groundSinceTick = now;
    } else if (!m_flight.active && !SameTile(pos, m_issued.from)) {
        // Moving off the takeoff tile under some other command.
        beginFlight(m_issued.target);
    } else if (now - m_issued.tick > kIssuedJumpTimeoutMs) {
        m_issued.pending = false;
    }
}

Position ScatterSailPlugin::GetServerPosition(const CHero* hero) const
{
    return m_flight.active ? m_flight.target : hero->m_posMap;
}

ScatterSailPlugin::JumpSlot ScatterSailPlugin::GetJumpSlot(const CHero* hero, DWORD now, Position& outOrigin)
{
    const CCommand& cmd = hero->GetCommand();
    const bool takeoffPending = cmd.iType == _COMMAND_JUMP && cmd.iStatus == _CMDSTATUS_BEGIN
        && !SameTile(cmd.posTarget, hero->m_posMap);
    if (now - m_lastJumpIssueTick < kMinJumpIssueGapMs)
        return JumpSlot::None;

    if (m_flight.active) {
        if (takeoffPending)
            return JumpSlot::None;   // the next jump is already queued
        if (!m_chainNext)
            return JumpSlot::None;   // this jump lands and pauses (RollRhythm)
        const DWORD lead = static_cast<DWORD>(std::clamp(GetScatterSailSettings().chainLeadMs, 0, kMaxChainLeadMs));
        if (lead == 0 || now - m_flight.startTick + lead < m_flight.durationMs)
            return JumpSlot::None;
        outOrigin = m_flight.target;
        return JumpSlot::Queue;
    }

    if (takeoffPending && now - m_lastJumpIssueTick < kTakeoffWaitMs)
        return JumpSlot::None;
    if (now - m_groundSinceTick < m_pauseMs)
        return JumpSlot::None;
    // "Wait For Attack Reply Before Jumping" (General): the reply to a cast
    // made on the ground makes the client skip a jump issued just before it.
    if (IsWaitingForActionReply(hero, now))
        return JumpSlot::None;
    outOrigin = hero->m_posMap;
    return JumpSlot::Ground;
}

void ScatterSailPlugin::RollRhythm()
{
    const ScatterSailSettings& sail = GetScatterSailSettings();
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    const double variation = std::clamp(sail.strideVariationPercent, 0, kMaxStrideVariation) / 100.0;
    m_strideFactor = (float)(1.0 + variation * (2.0 * unit(m_rng) - 1.0));

    if (percent(m_rng) < std::clamp(sail.misclickChancePercent, 0, kMaxMisclickChance)) {
        m_chainNext = false;
        m_pauseMs = static_cast<DWORD>(kMisclickPauseMinMs
            + unit(m_rng) * (kMisclickPauseMaxMs - kMisclickPauseMinMs));
        ++m_misclickCount;
        return;
    }

    m_chainNext = percent(m_rng) < std::clamp(sail.chainChancePercent, 0, 100);
    m_pauseMs = 0;
    if (!m_chainNext) {
        const int longest = std::clamp(sail.pauseMaxMs, kMinPauseMs, kMaxPauseMaxMs);
        const double u = unit(m_rng);
        m_pauseMs = static_cast<DWORD>(kMinPauseMs + (longest - kMinPauseMs) * u * u * u);
    }
}

bool ScatterSailPlugin::IsLandingRefused(const Position& pos, DWORD now) const
{
    const auto it = m_refusedLandingUntil.find(TileKey(pos));
    return it != m_refusedLandingUntil.end() && static_cast<int32_t>(it->second - now) > 0;
}

void ScatterSailPlugin::MarkLandingRefused(const Position& pos, DWORD now)
{
    std::erase_if(m_refusedLandingUntil, [now](const auto& entry) {
        return static_cast<int32_t>(entry.second - now) <= 0;
    });
    m_refusedLandingUntil[TileKey(pos)] = now + kRefusedLandingAvoidMs;
}

bool ScatterSailPlugin::IssueJump(CHero* hero, const Position& landing, JumpSlot slot, DWORD now)
{
    // Always the client's own jump (never the packet jump): the chain relies
    // on the client queueing it behind the flight in progress.
    auto jump = GameCall::CHero_Jump();
    if (!jump)
        return false;

    const Position origin = slot == JumpSlot::Queue ? m_flight.target : hero->m_posMap;
    jump(hero, landing.x, landing.y);
    m_lastJumpIssueTick = now;

    // A jump the game accepts shows up as the hero's command at once.
    const CCommand& cmd = hero->GetCommand();
    if (cmd.iType != _COMMAND_JUMP || !SameTile(cmd.posTarget, landing)) {
        ++m_refusedCount;
        MarkLandingRefused(landing, now);
        spdlog::debug("[sail] Jump ({},{}) -> ({},{}) refused, avoiding that tile for {}ms",
            origin.x, origin.y, landing.x, landing.y, kRefusedLandingAvoidMs);
        return false;
    }

    const DWORD flightMs = EstimateFlightMs(hero, origin, landing);
    const DWORD takeoffTick = slot == JumpSlot::Queue
        ? (std::max)(now, m_flight.startTick + m_flight.durationMs)
        : now;
    m_issued.pending = true;
    m_issued.queued = slot == JumpSlot::Queue;
    m_issued.from = origin;
    m_issued.target = landing;
    m_issued.tick = now;
    m_issued.earliestLandTick = takeoffTick + flightMs / 2;
    RollRhythm();
    spdlog::trace("[sail] Jump ({},{}) -> ({},{}) dist={} {} mobs={} plannedHits={} then {} pause={}ms",
        origin.x, origin.y, landing.x, landing.y,
        CGameMap::TileDist(origin.x, origin.y, landing.x, landing.y),
        slot == JumpSlot::Queue ? "queued" : "ground", m_mobs.size(), m_plannedHits,
        m_chainNext ? "chain" : "land", m_pauseMs);
    return true;
}

// =============================================================================
// Zone, fan, landing tiles
// =============================================================================

void ScatterSailPlugin::EnsureZone(CGameMap* map, const AutoHuntSettings& settings)
{
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    if (settings.zoneMode == AutoHuntZoneMode::Circle) {
        minX = settings.zoneCenter.x - settings.zoneRadius;
        maxX = settings.zoneCenter.x + settings.zoneRadius;
        minY = settings.zoneCenter.y - settings.zoneRadius;
        maxY = settings.zoneCenter.y + settings.zoneRadius;
    } else {
        if (settings.zonePolygon.empty())
            return;
        minX = maxX = settings.zonePolygon.front().x;
        minY = maxY = settings.zonePolygon.front().y;
        for (const Position& vertex : settings.zonePolygon) {
            minX = (std::min)(minX, vertex.x);
            maxX = (std::max)(maxX, vertex.x);
            minY = (std::min)(minY, vertex.y);
            maxY = (std::max)(maxY, vertex.y);
        }
    }

    // The planner keeps this callback, so it reads the live settings instead
    // of capturing the reference passed in here.
    m_planner.SetZone(map, minX, minY, maxX, maxY, [](const Position& pos) {
        const AutoHuntSettings& zone = GetAutoHuntSettings();
        return IsPointInHuntZone(zone, zone.zoneMapId, pos);
    }, ZoneKey(settings));
}

SailFan ScatterSailPlugin::GetFan(const CHero* hero, const AutoHuntSettings& settings) const
{
    SailFan fan;
    const CMagic* scatter = FindScatterSkill(hero);
    if (!scatter)
        return fan;

    // magictype `range` is the fan's hit radius; `distance` only limits how far
    // away the aim tile may be. Data with range=0 falls back to distance.
    const int skillRange = (int)scatter->GetRange();
    const int skillDistance = (int)scatter->GetDistance();
    fan.radius = settings.scatterRangeOverride > 0
        ? settings.scatterRangeOverride
        : (skillRange > 0 ? skillRange : skillDistance);
    fan.aimDistance = skillDistance > 0 ? skillDistance : fan.radius;
    fan.halfAngleRad = std::clamp(settings.scatterFanAngle, kMinScatterFanAngle, kMaxScatterFanAngle) * kPi / 360.0;
    return fan;
}

SailPlanner::LandFn ScatterSailPlugin::MakeLandFn(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    bool allowOutsideZone, int safetyDist, DWORD now) const
{
    const int altThreshold = CGameMap::GetHeroAltThreshold();
    return [this, hero, map, &settings, allowOutsideZone, safetyDist, now, altThreshold](
               const Position& from, const Position& to) {
        if (!allowOutsideZone && !IsPointInZone(settings, settings.zoneMapId, to))
            return false;
        // The game refuses to land on a tile a live role stands on.
        if (IsTileOccupied(to.x, to.y) || IsLandingRefused(to, now))
            return false;
        if (!map->CanJump(from.x, from.y, to.x, to.y, altThreshold))
            return false;
        if (safetyDist > 0) {
            for (const Position& mob : m_mobs) {
                if (CGameMap::TileDist(to.x, to.y, mob.x, mob.y) <= safetyDist)
                    return false;
            }
        }
        return true;
    };
}

// Whether a jump queued now would pass the client's checks, which it makes
// from the tile the hero is over at this moment.
static bool IsQueueReachable(const CHero* hero, const CGameMap* map, const Position& landing)
{
    const int dx = landing.x - hero->m_posMap.x;
    const int dy = landing.y - hero->m_posMap.y;
    return dx * dx + dy * dy <= kQueueReach * kQueueReach
        && map->CanJump(hero->m_posMap.x, hero->m_posMap.y, landing.x, landing.y, CGameMap::GetHeroAltThreshold());
}

// =============================================================================
// Sailing
// =============================================================================

void ScatterSailPlugin::Sail(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::shared_ptr<CMapItem>& loot, DWORD now)
{
    // Coming back from a town run, a revive or a buff: start a new course.
    if (now - m_lastSailTick > kSailPauseResetMs) {
        m_hasHeading = false;
        m_course.clear();
    }
    m_lastSailTick = now;

    EnsureZone(map, settings);
    m_fan = GetFan(hero, settings);

    m_mobs.clear();
    for (CRole* target : CollectHuntTargets(settings)) {
        if (target)
            m_mobs.push_back(target->m_posMap);
    }

    m_planner.Sweep(GetServerPosition(hero), now, GetTuning());

    // The pathfinder only runs here as the way out when no jump fits: around
    // terrain to open ground, or to loot. Anything else still running (the
    // walk into the zone) ends here, and so does a path whose goal is gone.
    if (Pathfinder::Get().IsActive()) {
        if (!m_fallbackPathActive || m_fallbackPathForLoot != (loot != nullptr)
            || now - m_fallbackPathTick > kFallbackPathMaxMs) {
            Pathfinder::Get().Stop();
            m_fallbackPathActive = false;
        }
    } else {
        m_fallbackPathActive = false;
    }

    if (!loot)
        m_lootId = 0;
    const bool lootOwnsMove = loot && SailToLoot(hero, map, settings, loot, now);
    if (!lootOwnsMove) {
        m_targetId = 0;
        if (!m_fallbackPathActive)
            SailStep(hero, map, settings, now);

        char status[128];
        if (!m_fan.IsValid())
            snprintf(status, sizeof(status), "Scouting (no Scatter skill)");
        else if (m_fallbackPathActive)
            snprintf(status, sizeof(status), "Finding a way to open ground");
        else if (m_goalHits <= 0)
            snprintf(status, sizeof(status), "Scouting, %d mob(s) in view", (int)m_mobs.size());
        else if (m_holding)
            snprintf(status, sizeof(status), "Holding on a clump of %d", m_goalHits);
        else
            snprintf(status, sizeof(status), "Heading for a clump of %d", m_goalHits);
        SetState(AutoHuntState::AttackTarget, status);
    }

    CastScatter(hero, map, settings, now);
}

// One decision of where to go, taken whenever the client can take a jump:
// the mobs in view lead (PlanClump); with none worth a cast the hero scouts
// the zone (PlanScout). Strides, chaining and pauses only shape the way there.
void ScatterSailPlugin::SailStep(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now)
{
    Position origin;
    const JumpSlot slot = GetJumpSlot(hero, now, origin);
    if (slot == JumpSlot::None || !m_planner.HasZone())
        return;
    // Decided to hold (this tile, or the one the hero is about to land on):
    // nothing changes within a frame or two.
    if (m_holding && now - m_lastHoldPlanTick < kHoldReplanMs)
        return;

    const ScatterSailSettings& sail = GetScatterSailSettings();
    const int workStride = std::clamp(sail.workStride, kMinStride, kMaxStride);
    const int travelStride = std::clamp(sail.travelStride, kMinStride, kMaxStride);
    const int stride = std::clamp(
        (int)std::lround(ChooseSailStride(m_mobs, origin, m_fan, workStride, travelStride) * m_strideFactor),
        kMinStride, kMaxStride);

    // Mobs can't reach a flying archer; on foot keep the safety distance when
    // the mobs leave room for it.
    const int safetyDist = hero->IsFlyActive() ? 0 : (std::max)(0, settings.archerSafetyDistance);
    const SailPlanner::LandFn canLand = MakeLandFn(hero, map, settings, false, safetyDist, now);

    ClumpTuning clumpTuning;
    clumpTuning.minHits = (std::max)(1, settings.minimumScatterHits);
    clumpTuning.repositionGain = std::clamp(sail.repositionGain, 1, kMaxRepositionGain);
    const ClumpPlan clump = m_planner.PlanClump(origin, m_heading, m_hasHeading, stride, m_mobs, m_fan,
        clumpTuning, canLand);

    m_holding = clump.action == ClumpPlan::Action::Hold;
    m_goal = clump.goal;
    m_goalHits = clump.action == ClumpPlan::Action::Scout ? 0 : clump.goalHits;
    if (m_holding) {
        m_lastHoldPlanTick = now;
        m_course.clear();
        return;
    }

    if (clump.action == ClumpPlan::Action::Jump) {
        // Too far from where the hero is right now: try again closer to landing.
        if (slot == JumpSlot::Queue && !IsQueueReachable(hero, map, clump.landing))
            return;
        m_plannedHits = clump.landingHits;
        if (IssueJump(hero, clump.landing, slot, now)) {
            m_heading = clump.heading;
            m_hasHeading = true;
            m_course.assign(1, clump.landing);
            if (!SameTile(clump.goal, clump.landing))
                m_course.push_back(clump.goal);
        }
        return;
    }

    const SailTuning tuning = GetTuning();
    SailPlan plan = m_planner.PlanScout(origin, m_heading, m_hasHeading, stride, now, tuning, canLand);
    if (!plan.valid && safetyDist > 0) {
        plan = m_planner.PlanScout(origin, m_heading, m_hasHeading, stride, now, tuning,
            MakeLandFn(hero, map, settings, false, 0, now));
    }
    if (!plan.valid) {
        if (slot == JumpSlot::Ground)
            StartFallbackPath(hero, map, settings, now);
        return;
    }

    if (slot == JumpSlot::Queue && !IsQueueReachable(hero, map, plan.landing))
        return;

    m_plannedHits = 0;
    if (IssueJump(hero, plan.landing, slot, now)) {
        m_heading = plan.heading;
        m_hasHeading = true;
        m_course = std::move(plan.preview);
    }
}

// One jump toward a fixed tile. The course heading follows, so the sail
// carries on from there in the direction it arrived.
void ScatterSailPlugin::MoveToward(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const Position& target, bool allowOutsideZone, DWORD now)
{
    Position origin;
    const JumpSlot slot = GetJumpSlot(hero, now, origin);
    if (slot == JumpSlot::None || SameTile(origin, target))
        return;

    const SailPlan plan = m_planner.PlanToward(origin, target, kMaxStride,
        MakeLandFn(hero, map, settings, allowOutsideZone, 0, now));
    if (!plan.valid) {
        if (slot == JumpSlot::Ground && now - m_fallbackPathTick >= kFallbackPathRetryMs) {
            m_fallbackPathTick = now;
            const auto tilePath = map->FindPath(origin.x, origin.y, target.x, target.y, 200000);
            const auto waypoints = map->SimplifyPath(tilePath);
            if (!waypoints.empty()) {
                Pathfinder::Get().StartPath(waypoints, kMinJumpIssueGapMs);
                m_fallbackPathActive = Pathfinder::Get().IsActive();
                m_fallbackPathForLoot = true;
            }
        }
        return;
    }
    if (slot == JumpSlot::Queue && !IsQueueReachable(hero, map, plan.landing))
        return;

    m_plannedHits = 0;
    if (IssueJump(hero, plan.landing, slot, now)) {
        m_heading = plan.heading;
        m_hasHeading = true;
        m_course = plan.preview;
    }
}

// No course fits (terrain or mobs box the hero in): let the pathfinder walk
// to the stalest region, or to the zone anchor.
void ScatterSailPlugin::StartFallbackPath(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now)
{
    if (now - m_fallbackPathTick < kFallbackPathRetryMs)
        return;
    m_fallbackPathTick = now;

    Position target = m_planner.GetGlobalTarget();
    if (IsZeroPos(target))
        target = GetZoneAnchor(settings);
    if (IsZeroPos(target) || SameTile(target, hero->m_posMap))
        return;

    const auto tilePath = map->FindPath(hero->m_posMap.x, hero->m_posMap.y, target.x, target.y, 200000);
    const auto waypoints = map->SimplifyPath(tilePath);
    if (waypoints.empty())
        return;

    spdlog::debug("[sail] No course from ({},{}), pathing to ({},{})",
        hero->m_posMap.x, hero->m_posMap.y, target.x, target.y);
    Pathfinder::Get().StartPath(waypoints, kMinJumpIssueGapMs);
    m_fallbackPathActive = Pathfinder::Get().IsActive();
    m_fallbackPathForLoot = false;
    m_hasHeading = false;
}

// Returns true while loot owns the movement (heading for it, or waiting on its
// tile for the pickup).
bool ScatterSailPlugin::SailToLoot(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    const std::shared_ptr<CMapItem>& loot, DWORD now)
{
    if (m_lootId != loot->m_id) {
        m_lootId = loot->m_id;
        m_lootStartTick = now;
    } else if (now - m_lootStartTick > kLootGiveUpMs) {
        // Can't get it (tile taken, pickup refused): leave it for the ignore
        // time, the same as items that failed their pickup attempts.
        spdlog::debug("[sail] Giving up on loot id={} at ({},{}) after {}ms",
            loot->m_id, loot->m_pos.x, loot->m_pos.y, now - m_lootStartTick);
        constexpr int kMaxAttemptsToIgnore = 4;
        for (int i = 0; i < kMaxAttemptsToIgnore && !m_lootMgr.IsLootPickupIgnored(loot->m_id, now); ++i)
            m_lootMgr.RecordLootPickupAttempt(loot->m_id, now, settings);
        m_lootStartTick = now;
        return false;
    }

    m_targetId = loot->m_id;
    const Position lootPos = loot->m_pos;
    if (SameTile(hero->m_posMap, lootPos) && !m_flight.active) {
        const bool picked = m_lootMgr.TryPickupLootItem(hero, settings, loot, now,
            [this](DWORD) { return m_flight.active; });
        SetState(AutoHuntState::LootNearby, picked ? "Picking up loot" : "Settling on loot");
        return true;
    }

    SetState(AutoHuntState::LootNearby, "Sailing to loot");
    if (m_fallbackPathActive || (m_flight.active && SameTile(m_flight.target, lootPos)))
        return true;

    // A mob on the item's tile: wait next to it; Scatter keeps going.
    const Position serverPos = GetServerPosition(hero);
    if (IsTileOccupied(lootPos.x, lootPos.y)
        && CGameMap::TileDist(serverPos.x, serverPos.y, lootPos.x, lootPos.y) <= kLootWaitRange)
        return true;

    MoveToward(hero, map, settings, lootPos, true, now);
    return true;
}

void ScatterSailPlugin::CastScatter(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now)
{
    if (!m_fan.IsValid())
        return;
    if (now - m_lastCastTick < GetScatterIntervalMs(GetScatterSailSettings()))
        return;
    CMagic* scatter = FindScatterSkill(hero);
    if (!scatter)
        return;

    // The server resolves the fan from where it has the hero.
    const Position origin = GetServerPosition(hero);
    const SailShot shot = FindBestSailShot(m_mobs, origin, m_fan, map->m_sizeMap.iWidth, map->m_sizeMap.iHeight);
    if (shot.hits < (std::max)(1, settings.minimumScatterHits)) {
        m_castHoldTick = 0;
        return;
    }

    // About to take off for a clearly better tile: cast from there instead.
    const CCommand& cmd = hero->GetCommand();
    const bool takeoffPending = cmd.iType == _COMMAND_JUMP && cmd.iStatus == _CMDSTATUS_BEGIN
        && !SameTile(cmd.posTarget, origin);
    if (takeoffPending && m_plannedHits >= shot.hits + kCastHoldGain) {
        if (m_castHoldTick == 0)
            m_castHoldTick = now;
        if (now - m_castHoldTick < kCastHoldMaxMs)
            return;
    }
    m_castHoldTick = 0;

    hero->MagicAttack(scatter->GetMagicType(), shot.aim);
    // A reply that arrives in flight leaves the hero's command alone, so only
    // a cast from the ground is something a jump can wait for.
    if (!m_flight.active)
        NoteActionSent(hero, now);
    m_lastCastTick = now;
    m_lastCastOrigin = origin;
    m_lastCastAim = shot.aim;
    m_lastCastHits = shot.hits;
    ++m_castCount;
    m_castHitSum += shot.hits;
    spdlog::trace("[sail] Scatter from ({},{}) aim ({},{}) hits={} {}",
        origin.x, origin.y, shot.aim.x, shot.aim.y, shot.hits,
        m_flight.active ? "in flight" : "on the ground");
}

// =============================================================================
// Update — the shared hunt housekeeping, then sailing
// =============================================================================

// The steps up to the zone check mirror BaseHuntPlugin::Update (same order,
// same shared helpers); from there this plugin sails instead of running the
// base loot and combat phases.
void ScatterSailPlugin::Update()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    TravelPlugin* travel = PluginManager::Get().GetPlugin<TravelPlugin>();

    CHero* hero = Game::GetHero();
    CGameMap* map = Game::GetMap();
    RefreshRuntimeState(hero, map);

    if (!m_enabled) {
        if (m_state != AutoHuntState::Idle)
            StopAutomation(true);
        return;
    }

    // Archer mode for the shared pieces (XP Fly, arrows, safety distance).
    settings.combatMode = GetExpectedCombatMode();

    if (!hero || !map) {
        SetState(AutoHuntState::WaitingForGame, "Waiting for hero and map");
        return;
    }

    const DWORD now = GetTickCount();
    TrackFlight(hero, now);

    m_lootMgr.PruneLootPickupAttempts(map);
    PruneLootDropRecords();

    if (!HasValidZone(settings)) {
        SetState(AutoHuntState::Failed, "Configure a valid hunt zone first");
        return;
    }

    if (HandleDeath(hero, travel, settings))
        return;

    // Player safety: only detect while hunting in zone, but always handle active rest
    if (m_safetyResting) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else if (settings.safetyEnabled
               && map->GetId() == settings.zoneMapId
               && IsPointInZone(settings, map->GetId(), hero->m_posMap)) {
        if (CheckPlayerSafety(hero, map, travel, settings))
            return;
    } else {
        m_nearbyPlayerTicks.clear();
    }

    if (HandlePlanRest(hero, travel, settings))
        return;

    if (travel && travel->IsTraveling()) {
        if (m_state == AutoHuntState::TravelToMarket) {
            HandleTravelToMarket(travel, hero, settings);
            return;
        }
        if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
            HandleTravelToZone(travel, settings);
            return;
        }

        SetState(m_state, "Waiting for travel plugin");
        return;
    }

    UpdatePendingJumpState(hero, now);

    {
        const HuntBuffCallbacks buffCb = MakeBuffCallbacks(hero, map, settings);
        if (m_buffMgr.TryPreLandingSafety(hero, map, settings, buffCb))
            return;

        // Cast skills in priority order (flyOnlyWithCyclone is handled inside TryCastFly/XpFly)
        for (int i = 0; i < kHuntSkillCount; i++) {
            const auto& entry = settings.skillPriorities[i];
            if (!entry.enabled) continue;
            bool cast = false;
            switch (entry.type) {
                case HuntSkillType::Superman:
                    cast = m_buffMgr.TryCastSuperman(hero, settings, buffCb);
                    break;
                case HuntSkillType::Cyclone:
                    cast = m_buffMgr.TryCastCyclone(hero, settings, buffCb);
                    break;
                case HuntSkillType::Accuracy:
                    cast = m_buffMgr.TryCastAccuracy(hero, settings, buffCb);
                    break;
                case HuntSkillType::XpFly:
                    cast = m_buffMgr.TryCastXpFly(hero, settings, buffCb);
                    break;
                case HuntSkillType::Fly:
                    cast = m_buffMgr.TryCastFly(hero, settings, buffCb);
                    break;
                case HuntSkillType::Stigma:
                    cast = m_buffMgr.TryCastStigma(hero, settings, m_lastMana, buffCb);
                    break;
                default: break;
            }
            if (cast) {
                NoteActionSent(hero, now);
                return;
            }
        }

        if (m_buffMgr.TryUsePotions(hero, settings, m_lastHp, m_lastMaxHp, m_lastMana, m_lastMaxMana, buffCb))
            return;
    }

    // Arrows
    if (HandleCombatItems(hero, settings))
        return;

    if (m_state == AutoHuntState::TravelToMarket) {
        HandleTravelToMarket(travel, hero, settings);
        return;
    }

    if (m_state == AutoHuntState::Repair) {
        m_townService.HandleRepairState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::BuyArrows) {
        m_townService.HandleBuyArrowsState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::StoreItems) {
        m_townService.HandleStoreState(hero, map, settings, MakeTownCallbacks(travel, hero, settings));
        return;
    }

    if (m_state == AutoHuntState::TravelToZone || m_state == AutoHuntState::ReturnToZone) {
        HandleTravelToZone(travel, settings);
        return;
    }

    // Meteor packing
    if (settings.packMeteorsIntoScrolls && now - m_lastPackTick >= GetItemActionIntervalMs(settings)
        && CountInventoryItemsByType(hero, ItemTypeId::METEOR) >= 10) {
        if (CItem* meteor = FindInventoryItemByType(hero, ItemTypeId::METEOR)) {
            hero->UseItem(meteor->GetID());
            m_lastPackTick = now;
            SetState(AutoHuntState::Recover, "Packing Meteors into MeteorScrolls");
            return;
        }
    }

    // Plan Hunting: Market run once the hunt time is up; the rest follows it.
    if (CheckPlanHuntTimer(travel, hero, settings))
        return;

    // Repair / storage — at Market (arrows handled separately below)
    if (m_townService.NeedTownRun(hero, settings, false)) {
        BeginTravelToMarket(travel, hero, settings);
        return;
    }

    // Arrow restocking — buy at local blacksmith or travel to zone city
    if (NeedsTownRunArrows(hero, settings)) {
        if (HuntTownService::HasBlacksmithOnMap(m_lastMapId)) {
            m_townService.ResetBuyArrowsSequence();
            SetState(AutoHuntState::BuyArrows, "Buying arrows");
            return;
        }
        if (HuntTownService::HasBlacksmithOnMap(settings.zoneMapId)) {
            BeginTravelToZone(travel, settings);
            return;
        }
    }

    // The loot target is needed before the zone check: FindBestLoot also returns
    // our confirmed drops just outside the zone.
    const bool onZoneMap = map->GetId() == settings.zoneMapId;
    std::shared_ptr<CMapItem> loot = onZoneMap
        ? m_lootMgr.FindBestLoot(hero, map, settings,
            [this](OBJID id, DWORD tick) { return m_lootMgr.IsLootPickupIgnored(id, tick); },
            [this, &settings](OBJID mapId, const Position& pos) { return IsPointInZone(settings, mapId, pos); })
        : nullptr;
    const bool collectingLootOutsideZone = loot && !IsPointInZone(settings, map->GetId(), loot->m_pos);

    // In flight the hero's tile lags behind; where the jump lands decides.
    const Position serverPos = GetServerPosition(hero);
    if (!onZoneMap
        || (!IsPointInZone(settings, map->GetId(), serverPos) && !collectingLootOutsideZone)) {
        BeginTravelToZone(travel, settings);
        return;
    }

    if (travel && travel->IsTraveling()) {
        SetState(AutoHuntState::Ready, "Waiting for travel plugin");
        return;
    }

    Sail(hero, map, settings, loot, now);
}

// =============================================================================
// UI
// =============================================================================

void ScatterSailPlugin::RenderUI()
{
    AutoHuntSettings& settings = GetAutoHuntSettings();
    ScatterSailSettings& sail = GetScatterSailSettings();

    bool enabled = m_enabled;
    if (ImGui::Checkbox("Enabled", &enabled))
        SetSailEnabled(enabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(Home key starts/stops the last used hunt mode)");
    ImGui::Separator();

    ImGui::TextDisabled("Led by the mob clumps like Archer Hunt: heads for the tile with the fullest Scatter fan,");
    ImGui::TextDisabled("stays while it is the best around, scouts the zone when no clump is in view.");
    ImGui::TextDisabled("Gets there in short chained jumps and casts Scatter on the way. Made for Cyclone + Fly.");

    ImGui::SliderInt("Stride Near Mobs", &sail.workStride, kMinStride, kMaxStride);
    ImGui::SliderInt("Stride When Clear", &sail.travelStride, kMinStride, kMaxStride);
    ImGui::TextDisabled("Longest jump in tiles. A jump takes the same time at any length with Fly, so stride is speed.");
    ImGui::SliderInt("Reposition Gain", &sail.repositionGain, 1, kMaxRepositionGain);
    ImGui::TextDisabled("A tile with a good fan is only left for one that hits this many more mobs");
    ImGui::TextDisabled("(and at least a quarter more). Lower = moves more, higher = stays longer.");

    ImGui::Separator();
    ImGui::Text("Scouting (no clump in view)");
    ImGui::SliderInt("Lane Width", &sail.laneWidth, kMinLaneWidth, kMaxLaneWidth);
    ImGui::TextDisabled("Distance between neighbouring passes; also how tight the course turns.");
    ImGui::SliderInt("Revisit Time (s)", &sail.revisitSeconds, kMinRevisitSeconds, kMaxRevisitSeconds);
    ImGui::TextDisabled("After this long a swept area counts as due again (about the respawn time).");

    ImGui::Separator();
    ImGui::Text("Rhythm");
    ImGui::SliderInt("Chain Chance", &sail.chainChancePercent, 0, 100, "%d%%");
    ImGui::TextDisabled("How often the next jump follows without touching down. 100%% = one unbroken chain.");
    ImGui::SliderInt("Longest Pause (ms)", &sail.pauseMaxMs, kMinPauseMs, 2000);
    ImGui::TextDisabled("Other jumps wait on the ground first: mostly a short moment, at most this long.");
    ImGui::SliderInt("Misclick Chance", &sail.misclickChancePercent, 0, kMaxMisclickChance, "%d%%");
    ImGui::TextDisabled("Jumps followed by a long pause (%d-%d ms), like a click that landed on a rock.",
        kMisclickPauseMinMs, kMisclickPauseMaxMs);
    ImGui::SliderInt("Stride Variation", &sail.strideVariationPercent, 0, kMaxStrideVariation, "%d%%");
    ImGui::TextDisabled("Each stride is up to this much longer or shorter, so jumps aren't all alike.");

    ImGui::Separator();
    ImGui::SliderInt("Scatter Interval (ms)", &sail.scatterIntervalMs, kMinScatterIntervalMs, 2000);
    ImGui::SliderInt("Minimum Scatter Hits", &settings.minimumScatterHits, 1, 15);
    ImGui::SliderInt("Scatter Range Override", &settings.scatterRangeOverride, 0, CGameMap::MAX_JUMP_DIST);
    if (settings.scatterRangeOverride == 0)
        ImGui::TextDisabled("0 = hit radius from the skill's range (current: %d)", m_fan.radius);
    ImGui::SliderInt("Scatter Fan Angle", &settings.scatterFanAngle,
        kMinScatterFanAngle, kMaxScatterFanAngle, "%d deg");

    ImGui::Separator();
    ImGui::SliderInt("Chain Lead (ms)", &sail.chainLeadMs, 0, kMaxChainLeadMs);
    ImGui::TextDisabled("A chained jump is queued this long before landing, so the hero doesn't touch down.");
    ImGui::TextDisabled("0 = jump only after landing.");
    ImGui::TextDisabled("\"Wait For Attack Reply Before Jumping\" (General) also applies here, to casts made on the ground.");
    ImGui::Checkbox("Show Course On Map", &sail.showCourse);

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

    if (m_enabled) {
        ImGui::Separator();
        ImGui::Text("Jumps: %d (%d chained in the air, %d misclicks, %d snapped, %d refused)",
            m_jumpCount, m_queuedJumpCount, m_misclickCount, m_snapCount, m_refusedCount);
        ImGui::Text("Scatter casts: %d, %.1f mobs per cast",
            m_castCount, m_castCount > 0 ? (float)m_castHitSum / (float)m_castCount : 0.0f);
        ImGui::Text("Zone swept: %.0f%%", m_planner.SweptPercent(GetTickCount(), GetTuning()));
    }
}
