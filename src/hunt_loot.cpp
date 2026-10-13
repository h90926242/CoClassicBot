#include "hunt_loot.h"
#include "hunt_town.h"
#include "hooks.h"
#include "CHero.h"
#include "CGameMap.h"
#include "itemtype.h"
#include "pathfinder.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>

// ── File-local constants ──────────────────────────────────────────────────────
static constexpr int   kLootPickupAttemptLimit   = 2;
static constexpr DWORD kLootTargetSwitchIntervalMs = 100;

static constexpr int kMinLootPickupIgnoreMs = 0;
static constexpr int kMaxLootPickupIgnoreMs = 300000;
static constexpr int kMinItemActionIntervalMs = 100;
static constexpr int kMaxItemActionIntervalMs = 5000;
static constexpr int kMinLootSpawnGraceMs = 0;
static constexpr int kMaxLootSpawnGraceMs = 5000;
static constexpr int kMinLootDecisionDelayMs = 0;
static constexpr int kMaxLootDecisionDelayMs = 10000;
static constexpr DWORD kDropRecordMatchWindowMs = 30000;  // match items against drop records within 30s
// How long an item's first-seen tick outlives the item leaving view.
static constexpr DWORD kLootSeenRetentionMs = 60000;

// ── File-local helpers ────────────────────────────────────────────────────────
namespace {

DWORD ClampMs(int value, int minVal, int maxVal)
{
    return static_cast<DWORD>(std::clamp(value, minVal, maxVal));
}

DWORD GetItemActionIntervalMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.itemActionIntervalMs, kMinItemActionIntervalMs, kMaxItemActionIntervalMs);
}

DWORD GetLootPickupIgnoreMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootPickupIgnoreMs, kMinLootPickupIgnoreMs, kMaxLootPickupIgnoreMs);
}

DWORD GetLootSpawnGraceMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootSpawnGraceMs, kMinLootSpawnGraceMs, kMaxLootSpawnGraceMs);
}

DWORD GetLootDecisionDelayMs(const AutoHuntSettings& settings)
{
    return ClampMs(settings.lootDecisionDelayMs, kMinLootDecisionDelayMs, kMaxLootDecisionDelayMs);
}

// An item announced in chat as our drop. A kill scatters several drops around
// the announced tile, so position alone only counts on the exact tile when the
// message couldn't be parsed; otherwise the item name (and plus, if given) must
// match, which still allows the item to sit one tile off.
bool IsConfirmedDrop(const CMapItem& item, DWORD now)
{
    for (const auto& drop : GetLootDropRecords()) {
        if ((now - drop.tick) > kDropRecordMatchWindowMs)
            continue;
        const int dx = std::abs(item.m_pos.x - drop.pos.x);
        const int dy = std::abs(item.m_pos.y - drop.pos.y);
        if (dx > 1 || dy > 1)
            continue;

        if (drop.itemName.empty()) {
            if (dx == 0 && dy == 0)
                return true;
            continue;
        }
        // itemtype.json names every quality the same ("SilverHelmet"); the drop
        // message adds the quality prefix ("RefinedSilverHelmet"), so accept either.
        const ItemTypeInfo* info = GetItemTypeInfo(item.m_idType);
        if (!info)
            continue;
        if (_stricmp(info->name.c_str(), drop.itemName.c_str()) != 0
            && _stricmp(FormatItemName(item.m_idType, 0).c_str(), drop.itemName.c_str()) != 0)
            continue;
        if (drop.plus >= 0 && item.GetPlus() != drop.plus)
            continue;
        return true;
    }
    return false;
}

bool TickIsFuture(DWORD targetTick, DWORD now)
{
    return static_cast<int32_t>(targetTick - now) > 0;
}

bool IsMovementCommand(const CCommand& cmd)
{
    return IsMoveCommandType(cmd.iType)
        || cmd.iType == _COMMAND_JUMP;
}

bool IsMovementCommandStillAdvancing(const CHero* hero)
{
    if (!hero)
        return false;
    const CCommand& cmd = hero->GetCommand();
    if (!IsMovementCommand(cmd))
        return false;
    return cmd.posTarget.x != hero->m_posMap.x || cmd.posTarget.y != hero->m_posMap.y;
}

} // namespace

// ── HuntLootManager implementation ───────────────────────────────────────────

std::shared_ptr<CMapItem> HuntLootManager::FindBestLoot(
    CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
    std::function<bool(OBJID, DWORD)> isLootPickupIgnoredFn,
    std::function<bool(OBJID mapId, const Position&)> isPointInZoneFn) const
{
    if (!hero || !map) return nullptr;
    if (hero->IsBagFull()) {
        spdlog::trace("[hunt-loot] FindBestLoot: bag full, skip");
        return nullptr;
    }

    const DWORD now = GetTickCount();
    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    const DWORD decisionDelayMs = GetLootDecisionDelayMs(settings);
    std::shared_ptr<CMapItem> best;
    float bestDist = (std::numeric_limits<float>::max)();
    int totalItems = 0, skippedFilter = 0, skippedIgnored = 0, skippedZone = 0, inSpawnGrace = 0, inDecisionDelay = 0;
    DWORD shortestDecisionWait = 0;

    for (const auto& itemRef : map->m_vecItems) {
        if (!itemRef) continue;
        ++totalItems;
        // Spawn grace only delays the pickup packet (TryPickupLootItem). Items
        // still in grace are valid targets, so the hero moves onto them and
        // waits there. The decision delay is what keeps the hero fighting: an
        // item is no target at all until it has been on the ground that long.
        const auto seenResult = m_lootSeenTicks.try_emplace(itemRef->m_id, now);
        const DWORD seenAge = now - seenResult.first->second;
        if (seenAge < spawnGraceMs) ++inSpawnGrace;
        if (isLootPickupIgnoredFn && isLootPickupIgnoredFn(itemRef->m_id, now)) { ++skippedIgnored; continue; }

        // Message mode: confirmed drops (our kill via system message) are always
        // looted. Ground mode ignores the messages and reads only the ground
        // item. Other items must pass the item filter (loot list, quality, or plus).
        const bool confirmed = !settings.lootFromGround && IsConfirmedDrop(*itemRef, now);

        // A mob killed at the zone edge can drop just outside it; our own
        // confirmed drops are still looted, anything else must be in the zone.
        if (!confirmed && isPointInZoneFn && !isPointInZoneFn(map->GetId(), itemRef->m_pos)) { ++skippedZone; continue; }
        if (!confirmed && !HuntTownService::ShouldLootMapItem(settings, *itemRef)) {
            ++skippedFilter;
            continue;
        }

        if (seenAge < decisionDelayMs) {
            const DWORD wait = decisionDelayMs - seenAge;
            if (inDecisionDelay == 0 || wait < shortestDecisionWait)
                shortestDecisionWait = wait;
            ++inDecisionDelay;
            continue;
        }

        const float dist = hero->m_posMap.DistanceTo(itemRef->m_pos);
        if (dist < bestDist) {
            bestDist = dist;
            best = itemRef;
        }
    }

    if (best) {
        spdlog::trace("[hunt-loot] FindBestLoot({}): picked id={} type={} plus={} pos=({},{}) dist={:.1f} | ground={} filteredOut={} ignored={} outOfZone={} spawnGrace={} decisionDelay={}",
            settings.lootFromGround ? "ground" : "messages",
            best->m_id, best->m_idType, best->GetPlus(), best->m_pos.x, best->m_pos.y, bestDist,
            totalItems, skippedFilter, skippedIgnored, skippedZone, inSpawnGrace, inDecisionDelay);
    } else if (inDecisionDelay > 0) {
        static Log::Throttle s_logDecisionDelay(std::chrono::milliseconds(250));
        if (s_logDecisionDelay.Allow()) {
            spdlog::trace("[hunt-loot] FindBestLoot: {} item(s) in decision delay, next in {}ms (delay={}ms), hunting on",
                inDecisionDelay, shortestDecisionWait, decisionDelayMs);
        }
    }

    return best;
}

bool HuntLootManager::IsLootPickupIgnored(OBJID itemId, DWORD now) const
{
    const auto it = m_lootPickupAttempts.find(itemId);
    if (it == m_lootPickupAttempts.end())
        return false;
    return TickIsFuture(it->second.ignoreUntilTick, now);
}

void HuntLootManager::RecordLootPickupAttempt(OBJID itemId, DWORD now,
    const AutoHuntSettings& settings)
{
    LootPickupAttemptState& state = m_lootPickupAttempts[itemId];
    if (!TickIsFuture(state.ignoreUntilTick, now)) {
        state.ignoreUntilTick = 0;
        if (state.attempts >= kLootPickupAttemptLimit)
            state.attempts = 0;
    }

    if (state.attempts < UINT8_MAX)
        ++state.attempts;

    if (state.attempts >= kLootPickupAttemptLimit) {
        const DWORD ignoreMs = GetLootPickupIgnoreMs(settings);
        state.attempts = 0;
        state.ignoreUntilTick = now + ignoreMs;
        spdlog::info("[hunt-loot] Item id={} hit pickup attempt limit ({}), ignoring for {}ms",
            itemId, kLootPickupAttemptLimit, ignoreMs);
    } else {
        spdlog::trace("[hunt-loot] Item id={} pickup attempt {}/{}", itemId, state.attempts, kLootPickupAttemptLimit);
    }
}

void HuntLootManager::PruneLootPickupAttempts(CGameMap* map)
{
    if (m_lootPickupAttempts.empty() && m_lootSeenTicks.empty())
        return;
    if (!map) {
        ResetLootPickupAttempts();
        return;
    }

    std::unordered_set<OBJID> activeItemIds;
    activeItemIds.reserve(map->m_vecItems.size());
    for (const auto& itemRef : map->m_vecItems) {
        if (itemRef)
            activeItemIds.insert(itemRef->m_id);
    }

    for (auto it = m_lootPickupAttempts.begin(); it != m_lootPickupAttempts.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()) {
            it = m_lootPickupAttempts.erase(it);
        } else {
            ++it;
        }
    }

    // The hero hunts on during the decision delay and may leave view range of
    // the item; keep its first-seen tick so coming back doesn't restart the
    // delay (or the spawn grace) from zero.
    const DWORD now = GetTickCount();
    for (auto it = m_lootSeenTicks.begin(); it != m_lootSeenTicks.end();) {
        if (activeItemIds.find(it->first) == activeItemIds.end()
            && now - it->second > kLootSeenRetentionMs) {
            it = m_lootSeenTicks.erase(it);
        } else {
            ++it;
        }
    }
}

void HuntLootManager::ResetLootPickupAttempts()
{
    m_lootPickupAttempts.clear();
    m_lootSeenTicks.clear();
    m_lastLootItemId = 0;
}

bool HuntLootManager::TryPickupLootItem(CHero* hero, const AutoHuntSettings& settings,
    const std::shared_ptr<CMapItem>& item, DWORD now,
    std::function<bool(DWORD)> updatePendingJumpFn)
{
    if (!hero || !item)
        return false;

    const DWORD spawnGraceMs = GetLootSpawnGraceMs(settings);
    const auto seenResult = m_lootSeenTicks.try_emplace(item->m_id, now);
    const DWORD seenAge = now - seenResult.first->second;
    if (seenAge < spawnGraceMs) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=spawn_grace age={}ms grace={}ms",
            item->m_id, item->m_idType, seenAge, spawnGraceMs);
        return false;
    }

    const int actualDist = CGameMap::TileDist(hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
    if (actualDist != 0) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} dist={} reason=not_on_tile",
            item->m_id, item->m_idType, actualDist);
        return false;
    }

    if (hero->IsJumping() || IsMovementCommandStillAdvancing(hero)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=movement_not_settled pos=({},{}) target=({},{})",
            item->m_id, item->m_idType, hero->m_posMap.x, hero->m_posMap.y, item->m_pos.x, item->m_pos.y);
        return false;
    }

    if (updatePendingJumpFn && updatePendingJumpFn(now)) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pending_jump age={}ms",
            item->m_id, item->m_idType, now);
        return false;
    }

    if (Pathfinder::Get().IsActive()) {
        Pathfinder::Get().Stop();
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=stopping_pathfinder",
            item->m_id, item->m_idType);
        return false;
    }

    const DWORD pathfinderJumpAge = now - Pathfinder::Get().GetLastJumpTick();
    if (Pathfinder::Get().GetLastJumpTick() != 0 && pathfinderJumpAge < 500) {
        spdlog::trace("[hunt-loot] Pickup wait id={} type={} reason=pathfinder_settle age={}ms",
            item->m_id, item->m_idType, pathfinderJumpAge);
        return false;
    }

    const DWORD interval = m_lastLootItemId == item->m_id
        ? GetItemActionIntervalMs(settings)
        : kLootTargetSwitchIntervalMs;
    const DWORD elapsed = now - m_lastLootTick;
    if (elapsed < interval) {
        spdlog::trace("[hunt-loot] TryPickup throttled id={} type={} elapsed={}ms interval={}ms",
            item->m_id, item->m_idType, elapsed, interval);
        return false;
    }

    hero->PickupItem(*item);
    m_lastLootTick   = now;
    m_lastLootItemId = item->m_id;
    RecordLootPickupAttempt(item->m_id, now, settings);
    spdlog::trace("[hunt-loot] PickupItem id={} type={} pos=({},{}) sent_on_tile",
        item->m_id, item->m_idType, item->m_pos.x, item->m_pos.y);
    return true;
}

