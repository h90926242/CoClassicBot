#pragma once
#include "archer_hunt_plugin.h"
#include "sail_planner.h"
#include <random>
#include <unordered_map>

// AutoHuntSettings::lastHuntMode value for this plugin (the other hunt modes
// store their AutoHuntCombatMode), so the Home key can start it again.
constexpr int kScatterSailHuntMode = 2;

struct ScatterSailSettings
{
    int workStride = 9;             // jump length with mobs in reach
    int travelStride = 14;          // jump length over empty ground
    // A tile with a good Scatter fan is only left for one that promises this
    // many more hits (and at least a quarter more).
    int repositionGain = 2;
    // Scouting, with no mob worth a cast in view:
    int laneWidth = 18;             // tiles between neighbouring passes
    int revisitSeconds = 60;        // a swept area counts as due again after this long
    int scatterIntervalMs = 800;    // minimum delay between Scatter casts
    // The next jump is queued this long before the current one lands, so the
    // hero takes off again the moment it touches down. 0 = jump after landing.
    int chainLeadMs = 100;
    bool showCourse = true;         // draw the course and the swept ground on the map

    // Rhythm. An unbroken chain of equal jumps looks like a machine; the
    // defaults follow a hand-played recording (Ctrl+click under Cyclone + Fly):
    // 39% of jumps chained, the rest after a pause on the ground of typically
    // 100 ms, one in ten over 600 ms, and strides between 3 and 14 tiles.
    int chainChancePercent = 40;    // how often the next jump follows without touching down
    int pauseMaxMs = 700;           // longest ordinary pause on the ground; most are far shorter
    int misclickChancePercent = 3;  // jumps followed by a long pause, like a click that hit a rock
    int strideVariationPercent = 30;   // each stride is this much longer or shorter at random
};

ScatterSailSettings& GetScatterSailSettings();

// ── ScatterSailPlugin ("Archer Hunt 2") ──────────────────────────────────────
// Archer hunt for Cyclone + Fly. Where it goes is led by the mobs, like the
// archer hunt: the hero heads for the tile whose Scatter fan catches the most
// mobs, stays there while that fan is as good as it gets nearby, and scouts
// the zone when no mob worth a cast is in view (SailPlanner). How it gets
// there is its own: short jumps chained in the air (like holding Ctrl and
// clicking ahead) with a hand-played rhythm, and Scatter cast on the way
// whenever the cooldown allows. Loot is jumped to and picked up.
//
// Travel, town runs, buffs, potions, arrows, revive and player safety are the
// shared hunt pieces from BaseHuntPlugin / ArcherHuntPlugin; only the loop
// that drives them (Update) and the movement and combat are this plugin's.
class ScatterSailPlugin : public ArcherHuntPlugin {
public:
    const char* GetName() const override { return "Archer Hunt 2"; }
    void Update() override;
    void RenderUI() override;
    int GetLastScatterRange() const override { return m_fan.radius; }

    // Same as the Enabled checkbox: on turns the other hunt modes off.
    void SetSailEnabled(bool enabled);

    // Home key: like BaseHuntPlugin::ToggleHuntHotkey, but starts this plugin
    // when it was the hunt mode used last.
    static void ToggleHuntHotkey();

    // ── Map overlay ──
    const SailPlanner& GetPlanner() const { return m_planner; }
    SailTuning GetTuning() const;
    const std::vector<Position>& GetCourse() const { return m_course; }
    Position GetLastCastOrigin() const { return m_lastCastOrigin; }
    Position GetLastCastAim() const { return m_lastCastAim; }
    DWORD GetLastCastTick() const { return m_lastCastTick; }

private:
    // A jump the client is flying right now. The server has the hero on
    // `target` from takeoff on.
    struct Flight {
        bool active = false;
        Position from;
        Position target;
        DWORD startTick = 0;
        DWORD durationMs = 0;   // expected
    };

    // The last jump handed to the client, until it is seen taking off.
    struct IssuedJump {
        bool pending = false;
        bool queued = false;    // issued behind a flight in progress
        Position from;
        Position target;
        DWORD tick = 0;
        DWORD earliestLandTick = 0;   // on the landing tile before this = the client skipped the flight
    };

    enum class JumpSlot {
        None,     // the client can't take a jump now
        Ground,   // jump at once from where the hero stands
        Queue,    // queue behind the flight in progress
    };

    void ResetSail();
    void TrackFlight(CHero* hero, DWORD now);
    Position GetServerPosition(const CHero* hero) const;
    JumpSlot GetJumpSlot(const CHero* hero, DWORD now, Position& outOrigin);
    bool IssueJump(CHero* hero, const Position& landing, JumpSlot slot, DWORD now);
    // Decides how the jump after the one just issued follows it (chained in
    // the air, or after a pause on the ground) and how long its stride is.
    void RollRhythm();
    bool IsLandingRefused(const Position& pos, DWORD now) const;
    void MarkLandingRefused(const Position& pos, DWORD now);

    void EnsureZone(CGameMap* map, const AutoHuntSettings& settings);
    SailFan GetFan(const CHero* hero, const AutoHuntSettings& settings) const;
    // Landing test for the planner. safetyDist > 0 also keeps that many tiles
    // between the landing tile and every mob.
    SailPlanner::LandFn MakeLandFn(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        bool allowOutsideZone, int safetyDist, DWORD now) const;

    void Sail(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::shared_ptr<CMapItem>& loot, DWORD now);
    void SailStep(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now);
    bool SailToLoot(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::shared_ptr<CMapItem>& loot, DWORD now);
    void MoveToward(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const Position& target, bool allowOutsideZone, DWORD now);
    void StartFallbackPath(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now);
    void CastScatter(CHero* hero, CGameMap* map, const AutoHuntSettings& settings, DWORD now);

    SailPlanner m_planner;
    SailFan m_fan;
    std::vector<Position> m_mobs;       // hunt targets this tick
    std::vector<Position> m_course;     // planned course, next landing first
    float m_heading = 0.0f;
    bool m_hasHeading = false;
    int m_plannedHits = 0;              // Scatter hits expected from the next landing

    Flight m_flight;
    IssuedJump m_issued;
    DWORD m_lastJumpIssueTick = 0;
    std::mt19937 m_rng{std::random_device{}()};
    bool m_chainNext = true;            // the next jump may be queued behind the one in the air
    DWORD m_pauseMs = 0;                // otherwise it waits this long after landing
    float m_strideFactor = 1.0f;        // random length of the next stride, around 1
    DWORD m_groundSinceTick = 0;        // when the last flight landed
    bool m_holding = false;             // staying on a tile with a good fan
    DWORD m_lastHoldPlanTick = 0;
    Position m_goal = {};               // tile the clump guidance is heading for or holding
    int m_goalHits = 0;                 // its fan; 0 while scouting
    std::unordered_map<uint64_t, DWORD> m_refusedLandingUntil;
    DWORD m_fallbackPathTick = 0;       // when the pathfinder was last asked to take over
    bool m_fallbackPathActive = false;
    bool m_fallbackPathForLoot = false; // that path leads to loot, not to open ground
    DWORD m_lastSailTick = 0;           // last tick the sail ran (the course resets after a pause)

    DWORD m_lastCastTick = 0;
    DWORD m_castHoldTick = 0;           // since when a ready cast waits for the takeoff
    Position m_lastCastOrigin = {};
    Position m_lastCastAim = {};
    int m_lastCastHits = 0;

    OBJID m_lootId = 0;                 // loot being sailed to, and since when
    DWORD m_lootStartTick = 0;

    // Session counters (Runtime section)
    int m_jumpCount = 0;
    int m_queuedJumpCount = 0;
    int m_misclickCount = 0;
    int m_snapCount = 0;                // jumps that arrived without flying
    int m_refusedCount = 0;
    int m_castCount = 0;
    int m_castHitSum = 0;
};
