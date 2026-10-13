#pragma once
#include "base.h"
#include <cstdint>
#include <vector>

enum class AutoHuntZoneMode {
    Circle = 0,
    Polygon = 1,
};

enum class AutoHuntCombatMode {
    Melee = 0,
    Archer = 1,
};

// ── Skill priority system ────────────────────────────────────────────────────
enum class HuntSkillType : int {
    Superman = 0,
    Cyclone = 1,
    Accuracy = 2,
    XpFly = 3,
    Fly = 4,
    Stigma = 5,
    Count = 6,
};

const char* HuntSkillName(HuntSkillType type);

struct HuntSkillEntry {
    HuntSkillType type;
    bool enabled = false;
};

constexpr int kHuntSkillCount = static_cast<int>(HuntSkillType::Count);

enum class AutoHuntState {
    Idle,
    WaitingForGame,
    Ready,
    TravelToZone,
    AcquireTarget,
    ApproachTarget,
    AttackTarget,
    LootNearby,
    Recover,
    TravelToMarket,
    Repair,
    BuyArrows,
    StoreItems,
    Rest,
    ReturnToZone,
    Failed,
};

// A hunt polygon kept for later reuse, e.g. one per hunting map.
struct SavedHuntPolygon
{
    OBJID mapId = 0;
    std::vector<Position> points;
};

// Longest a hunt jump waits for an attack reply (waitForAttackReply). Replies to
// a Scatter took 250-450 ms; stutters were seen up to ~750 ms after the cast.
// With the Scatter interval at least this long, no reply is in flight at a jump.
constexpr int kAttackReplyWaitMs = 800;

// Range of the Plan Hunting hunt and rest durations.
constexpr int kMinPlanMinutes = 1;
constexpr int kMaxPlanMinutes = 24 * 60;

struct AutoHuntSettings
{
    bool enabled = false;
    bool usePotions = true;
    bool autoRepair = true;
    bool autoStore = true;
    bool autoDepositSilver = false;
    bool autoReviveInTown = true;
    // Skill priority list — order determines cast priority, enabled flag per skill
    HuntSkillEntry skillPriorities[kHuntSkillCount] = {
        { HuntSkillType::Superman, false },
        { HuntSkillType::Cyclone, true },
        { HuntSkillType::Accuracy, false },
        { HuntSkillType::XpFly, false },
        { HuntSkillType::Fly, false },
        { HuntSkillType::Stigma, false },
    };

    // Legacy bools — kept in sync by SyncSkillBoolsFromPriorities()
    bool castSuperman = false;
    bool castCyclone = true;
    bool supermanBeforeCyclone = false;
    bool useAccuracyIfCycloneActive = false;
    bool useStigma = false;
    bool archerMode = false;
    bool castXpFly = false;
    bool castFly = false;
    bool flyOnlyWithCyclone = false;

    // Call after changing skillPriorities to sync legacy bools
    void SyncSkillBoolsFromPriorities();
    bool useScatterLogic = true;
    bool pickupNearbyHpPotionWhenLow = false;
    bool pickupNearbyManaPotionForStigma = false;
    bool prioritizeMobClumps = true;
    bool prioritizeScatterClumps = true;
    int mobSearchRange = 0;
    bool immediateReturnOnPriorityItems = false;
    bool storeTreasureBank = true;
    bool storeComposeBank = true;
    bool packMeteorsIntoScrolls = false;
    // Loot straight from the ground item list by the loot rules alone; drop
    // system messages are ignored. Off = messages also confirm our drops.
    bool lootFromGround = false;
    bool lootRefined = false;
    bool lootUnique = false;
    bool lootElite = false;
    bool lootSuper = false;
    bool storeRefined = false;
    bool storeUnique = false;
    bool storeElite = false;
    bool storeSuper = false;
    bool buyArrows = false;
    int hpPotionPercent = 50;
    int manaPotionPercent = 35;
    int repairPercent = 70;
    int bagStoreThreshold = 36;
    int clumpRadius = 8;
    int minimumMobClump = 5;
    int minimumScatterHits = 3;
    int scatterRangeOverride = 0;
    int scatterFanAngle = 120;      // degrees; width of the server's Scatter hit fan
    int scatterIntervalMs = 350;    // minimum delay between Scatter casts
    // Hold each hunt jump until the server's reply to the last attack/skill has
    // been applied (at most kAttackReplyWaitMs). A reply handled right after a
    // jump starts makes the client skip that jump's animation (a "stutter").
    bool waitForAttackReply = false;
    // Plan Hunting: hunt for planHuntMinutes, then a Market run (the normal
    // repair/store) and planRestMinutes of rest there under Bless, then back.
    // This rest is separate from the player-safety one (safetyRestSec).
    bool planHunting = false;
    int planHuntMinutes = 60;
    int planRestMinutes = 10;
    int actionRadius = 6;
    int rangedAttackRange = 1;
    int archerSafetyDistance = 0;
    int lootRange = 5;
    int movementIntervalMs = 900;
    int attackIntervalMs = 300;
    int cycloneAttackIntervalMs = 100;
    int targetSwitchAttackIntervalMs = 75;
    int itemActionIntervalMs = 700;
    int lootSpawnGraceMs = 1000;
    // Keep hunting this long after a matching item first appears before going
    // for it. Spawn grace counts from the same moment, so it only adds a wait
    // on the tile when it is longer than this delay.
    int lootDecisionDelayMs = 0;
    int selfCastIntervalMs = 1000;
    int npcActionIntervalMs = 400;
    int lootPickupIgnoreMs = 30000;
    int reviveDelayMs = 20000;
    int reviveRetryIntervalMs = 1000;
    int minimumLootPlus = 0;
    int minimumStorePlus = 0;
    int silverKeepAmount = 0;
    uint32_t arrowTypeId = 1050002; // SpeedArrow
    int arrowBuyCount = 3;
    OBJID zoneMapId = 0;
    AutoHuntCombatMode combatMode = AutoHuntCombatMode::Melee;
    // Hunt mode last switched on for this hero (AutoHuntCombatMode value), or
    // -1 if none yet; the Home key starts this one.
    int lastHuntMode = -1;
    AutoHuntZoneMode zoneMode = AutoHuntZoneMode::Circle;
    Position zoneCenter = {0, 0};
    int zoneRadius = 12;
    std::vector<Position> zonePolygon;
    std::vector<SavedHuntPolygon> savedPolygons;
    std::vector<uint32_t> lootItemIds;
    std::vector<uint32_t> warehouseItemIds;
    std::vector<uint32_t> priorityReturnItemIds;
    char monsterNames[256] = "";
    char monsterIgnoreNames[256] = "";
    char monsterPreferNames[256] = "";
    char playerWhitelist[256] = "";
    bool usePacketJump = false;
    bool safetyEnabled = false;
    bool safetyNotifyDiscord = false;
    // Off = a trigger only sends the Discord notification and hunting goes on.
    bool safetyReturnToMarket = true;
    int safetyPlayerRange = 15;
    int safetyDetectionSec = 30;
    int safetyRestSec = 120;

    // Debug map overlays
    bool debugShowActionRadius = false;
    bool debugShowClumpRadius = false;
    bool debugShowMobSearchRange = false;
    bool debugShowLootRange = false;
    bool debugShowSafetyRange = false;
    bool debugShowAttackRange = false;
    bool debugShowArcherSafety = false;
    bool debugShowScatterRange = false;
    bool debugShowBestClump = false;

    // Zone editor state
    int zoneCaptureMode = 0;
    int editDragVertex = -1;
};

AutoHuntSettings& GetAutoHuntSettings();

// Zone geometry free functions
bool IsZeroPos(const Position& pos);
Position PolygonCentroid(const std::vector<Position>& points);
bool PointInPolygon(const Position& point, const std::vector<Position>& polygon);

// Zone helper free functions
bool HasValidHuntZone(const AutoHuntSettings& settings);
bool IsPointInHuntZone(const AutoHuntSettings& settings, OBJID mapId, const Position& pos);
Position GetHuntZoneAnchor(const AutoHuntSettings& settings);
