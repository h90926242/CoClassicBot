#pragma once
#include "plugin.h"
#include "hunt_settings.h"
#include "hunt_town.h"
#include "hunt_buffs.h"
#include "hunt_loot.h"
#include "revive_utils.h"
#include <memory>
#include <unordered_map>
#include <unordered_set>

class CHero;
class CRole;
class CGameMap;
class TravelPlugin;
class CItem;
class CMagic;
struct CMapItem;

// ── BaseHuntPlugin ───────────────────────────────────────────────────────────
// Abstract base class for the shared hunt loop. Subclasses (melee, archer)
// implement the pure virtual combat interface; everything else (state machine,
// travel, loot, buffs, town runs, safety, zone management) lives here.
//
// Abstract base class — MeleeHuntPlugin and ArcherHuntPlugin inherit from this.
class BaseHuntPlugin : public IPlugin {
public:
    BaseHuntPlugin() { m_enabled = false; }

    // ── IPlugin overrides ────────────────────────────────────────────────
    void Update() override;
    void RenderUI() override;
    bool OnMapClick(const Position& tile) override;
    const char* GetName() const override = 0;  // subclass provides name

    // Renders shared hunting settings (zone, monsters, timings, loot, etc.)
    void RenderSharedUI();

    // Split UI methods — General section only, then everything else
    void RenderGeneralUI();
    void RenderSettingsUI();

    // Switches this hunt mode on or off, the same way its Enabled checkbox does:
    // on turns the other hunt mode off and remembers this one for the Home key,
    // off stops the automation.
    void SetHuntEnabled(bool enabled);

    // Home key: stops whichever hunt mode is running, otherwise starts the one
    // last switched on for this hero (nothing if none was used yet).
    static void ToggleHuntHotkey();

    // ── Debug state accessors (virtual, base provides defaults) ──────────
    virtual int GetLastScatterRange() const { return 0; }
    Position GetLastTargetPos() const { return m_lastTargetPos; }
    Position GetDebugBestClumpCenter() const { return m_debugBestClumpCenter; }
    int GetDebugBestClumpSize() const { return m_debugBestClumpSize; }
    int GetEditDragVertex() const { return m_editDragVertex; }
    void SetEditDragVertex(int idx) { m_editDragVertex = idx; }

protected:
    // ── Virtual combat interface (subclasses implement) ──────────────────
    virtual CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) = 0;

    virtual void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) = 0;

    virtual void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) = 0;

    virtual bool HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target) { return false; }

    virtual void RenderCombatUI(AutoHuntSettings& settings) = 0;

    // Whether this combat mode needs a town run for arrows.
    virtual bool NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const { return false; }

    // Called during Update after buffs to manage combat-specific items (arrows etc.).
    // Return true to short-circuit the rest of Update.
    virtual bool HandleCombatItems(CHero* hero, const AutoHuntSettings& settings) { return false; }

    // Called when no target is found and no loot — subclass can do patrol or idle behavior.
    // Return true to short-circuit the rest of Update.
    virtual bool HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings) { return false; }

    // Called at the top of Update to let subclass refresh combat-mode state.
    virtual void RefreshCombatState(CHero* hero, const AutoHuntSettings& settings) {}

    // Which combat mode this plugin represents — synced to settings each frame.
    virtual AutoHuntCombatMode GetExpectedCombatMode() const = 0;

    // ── Zone capture mode enum ───────────────────────────────────────────
    enum class ZoneCaptureMode {
        None,
        CircleCenter,
        CircleRadius,
        PolygonVertex,
    };

    // ── Shared protected methods ─────────────────────────────────────────
    void SetState(AutoHuntState state, const char* statusText);
    const char* GetStateName() const;
    void RefreshRuntimeState(CHero* hero, CGameMap* map);
    void StopAutomation(bool cancelTravel);

    bool HandleDeath(CHero* hero, TravelPlugin* travel, const AutoHuntSettings& settings);

    bool HasValidZone(const AutoHuntSettings& settings) const;
    bool IsPointInZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos) const;
    Position GetZoneAnchor(const AutoHuntSettings& settings) const;

    Position GetEffectiveHeroPosition(const CHero* hero) const;
    void ClearPendingJumpState();
    void ArmPendingJump(CHero* hero, const Position& destination, DWORD now, bool isRetreat);
    bool UpdatePendingJumpState(CHero* hero, DWORD now);

    // "Wait For Attack Reply Before Jumping": call after sending an attack or
    // skill. Hunt jumps then wait until the hero's command changes (the reply
    // was applied) or kAttackReplyWaitMs passed since the first unanswered send.
    void NoteActionSent(const CHero* hero, DWORD now);
    bool IsWaitingForActionReply(const CHero* hero, DWORD now);

    bool StartPathTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartWalkTo(CHero* hero, CGameMap* map, const Position& destination, int stopRange);
    bool StartPathNearTarget(CHero* hero, CGameMap* map, const Position& targetPos, int desiredRange);

    void BeginTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings);
    void BeginTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);
    void HandleTravelToZone(TravelPlugin* travel, const AutoHuntSettings& settings);
    void HandleTravelToMarket(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);

    HuntTownCallbacks MakeTownCallbacks(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);
    HuntBuffCallbacks MakeBuffCallbacks(CHero* hero, CGameMap* map, const AutoHuntSettings& settings);

    // "Plan Hunting": hunt for a set time, then a Market run and a rest there
    // under Bless. The hunt time runs from the first tick inside the zone.
    // CheckPlanHuntTimer starts the Market run once it is up (true = started),
    // BeginTravelToZone turns into BeginPlanRest when that run is done, and
    // HandlePlanRest drives the rest (true = resting this tick).
    bool IsPlanHuntTimeUp(const AutoHuntSettings& settings, DWORD now) const;
    bool CheckPlanHuntTimer(TravelPlugin* travel, CHero* hero, const AutoHuntSettings& settings);
    void BeginPlanRest(const AutoHuntSettings& settings);
    bool HandlePlanRest(CHero* hero, TravelPlugin* travel, const AutoHuntSettings& settings);

    bool IsPlayerWhitelisted(const AutoHuntSettings& settings, const char* name) const;
    bool CheckPlayerSafety(CHero* hero, CGameMap* map, TravelPlugin* travel, const AutoHuntSettings& settings);

    bool FindClosestZoneTile(CGameMap* map, const AutoHuntSettings& settings,
        const Position& from, Position& outZonePos) const;

    // ── UI helpers (will be moved to hunt_ui in Task 10) ─────────────────
    void RenderItemSelector(AutoHuntSettings& settings);
    void RenderSelectedItemList(const char* title, const char* clearButtonLabel,
        const char* tableId, std::vector<uint32_t>& itemIds);

    // ── Shared state members ─────────────────────────────────────────────
    AutoHuntState m_state = AutoHuntState::Idle;
    char m_statusText[128] = "Disabled";
    Position m_lastHeroPos = {0, 0};
    OBJID m_lastMapId = 0;
    int m_lastHp = 0;
    int m_lastMaxHp = 0;
    int m_lastMana = 0;
    int m_lastMaxMana = 0;
    size_t m_lastBagCount = 0;
    // Shared by all hunt plugins: the zone they edit is shared, the Hunt Zone UI
    // is drawn by whichever one is enabled, and map clicks reach all of them, so
    // a per-plugin mode left capture running in a plugin the UI no longer showed.
    static inline ZoneCaptureMode m_zoneCaptureMode = ZoneCaptureMode::None;
    int m_editDragVertex = -1;
    char m_itemSearch[64] = "";

    DWORD m_lastAttackTick = 0;
    DWORD m_lastPackTick = 0;
    DWORD m_lastMoveTick = 0;

    Position m_pendingJumpDest = {};
    DWORD m_pendingJumpTick = 0;
    Position m_pendingJumpLastPos = {};
    DWORD m_pendingJumpLastProgressTick = 0;
    bool m_pendingJumpIsRetreat = false;

    // Hero command at the first unanswered attack/skill send (see NoteActionSent).
    DWORD m_actionSentTick = 0;
    int m_actionSentCmdType = 0;
    int m_actionSentCmdStatus = 0;
    Position m_actionSentCmdTarget = {};
    DWORD m_actionSentCmdTimestamp = 0;

    DWORD m_approachStartTick = 0;
    OBJID m_unreachableTargetId = 0;
    DWORD m_unreachableExpireTick = 0;

    ReviveState m_reviveState;
    HuntBuffManager m_buffMgr;
    HuntLootManager m_lootMgr;
    HuntTownService m_townService;

    OBJID m_targetId = 0;
    bool m_targetSwitched = false;   // this tick's target differs from last tick's; set before combat dispatch
    int m_lastClumpSize = 0;
    bool m_lastUseScatter = false;   // this tick's target is a Scatter shot (local or approach)
    Position m_lastTargetPos = {};
    Position m_debugBestClumpCenter = {};
    int m_debugBestClumpSize = 0;

    std::unordered_map<OBJID, DWORD> m_nearbyPlayerTicks;
    // Players already reported while safety only notifies; one report per stay in range.
    std::unordered_set<OBJID> m_safetyNotifiedPlayers;
    bool m_safetyResting = false;
    DWORD m_safetyRestStartTick = 0;

    // Plan Hunting
    DWORD m_planHuntStartTick = 0;   // first tick inside the zone; 0 = hunt time not running
    bool m_planResting = false;
    DWORD m_planRestStartTick = 0;
    DWORD m_planRestActionTick = 0;  // last Sit / Bless cast of this rest; 0 = none yet
    int m_planRestStamina = 0;       // stamina at that action, to see whether sitting helps
};
