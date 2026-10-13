#pragma once
#include "base.h"
#include <functional>
#include <vector>

class CGameMap;

// ── Scatter fan ──────────────────────────────────────────────────────────────
// The server resolves a Scatter cast as a fan: the caster is the origin, the
// aim tile sets the direction, and mobs within `radius` and inside the fan
// angle are hit.
struct SailFan {
    int radius = 0;              // hit radius (magictype `range`)
    int aimDistance = 0;         // max distance of the aim tile (magictype `distance`)
    double halfAngleRad = 0.0;   // half the fan angle
    bool IsValid() const { return radius > 0 && aimDistance > 0; }
};

struct SailShot {
    Position aim;
    int hits = 0;
};

// Aim tile of the fan that hits the most mobs from `origin` (hits == 0 if none
// is in reach). The aim is kept on the map.
SailShot FindBestSailShot(const std::vector<Position>& mobs, const Position& origin,
    const SailFan& fan, int mapWidth, int mapHeight);

// Most mobs one fan can hit from `origin`, whatever its direction. Cheaper than
// FindBestSailShot; used to rate tiles the hero isn't standing on yet. The
// overload with `scratch` reuses that buffer across many calls.
int CountBestFanHits(const std::vector<Position>& mobs, const Position& origin, const SailFan& fan);
int CountBestFanHits(const std::vector<Position>& mobs, const Position& origin, const SailFan& fan,
    std::vector<double>& scratch);

// ── Sail planner ─────────────────────────────────────────────────────────────
// Decides where the hero jumps next. Two guides:
//
// - Clumps (PlanClump), while mobs are in view: the hero heads for the tile
//   whose Scatter fan catches the most mobs, the way the archer hunt picks its
//   Scatter position, but looking past one jump and taking the way there in
//   strides.
// - Scouting (PlanScout), with no mob worth a cast in view: a smooth course
//   over the ground swept least recently. The zone is cut into cells that
//   remember when the hero last passed within sweepRadius of them.
struct SailTuning {
    int sweepRadius = 9;       // tiles around the hero that count as swept
    int turnRadius = 9;        // tightest turn of the course; lanes end up about twice this apart
    int lookahead = 32;        // tiles of course scored per decision
    DWORD revisitMs = 60000;   // a swept cell counts as fully stale again after this long
};

struct SailPlan {
    bool valid = false;
    Position landing;                // tile to jump to
    float heading = 0.0f;            // course after this step (radians, tile space)
    std::vector<Position> preview;   // scored course, landing first
};

struct ClumpTuning {
    int minHits = 1;           // a fan with fewer mobs isn't worth a jump
    int repositionGain = 2;    // a tile with a good fan is only left for this many more hits
};

struct ClumpPlan {
    enum class Action {
        Scout,   // no mob worth a cast in view (or no way to one): sweep the zone
        Hold,    // the fan from here is as good as it gets nearby: stay and cast
        Jump,
    };
    Action action = Action::Scout;
    Position landing;          // Jump: tile to jump to
    float heading = 0.0f;
    int landingHits = 0;       // best fan from the landing tile
    Position goal;             // tile this is heading for; the landing itself when within one jump
    int goalHits = 0;
    int hereHits = 0;          // best fan from the origin
};

// Jump length for the next step: `workStride` with mobs around (short hops, so
// Scatter keeps up), `travelStride` over empty ground, blended in between.
int ChooseSailStride(const std::vector<Position>& mobs, const Position& origin,
    const SailFan& fan, int workStride, int travelStride);

class SailPlanner {
public:
    static constexpr int kCellSize = 4;

    using ZoneFn = std::function<bool(const Position&)>;
    // Whether the hero can jump from `from` and stand on `to`.
    using LandFn = std::function<bool(const Position& from, const Position& to)>;

    // Forgets the zone, the coverage and the course.
    void Reset();

    // (Re)builds the cells when the zone changed (zoneKey differs). Bounds are
    // the zone's bounding box in tiles.
    void SetZone(const CGameMap* map, int minX, int minY, int maxX, int maxY,
        const ZoneFn& inZone, uint64_t zoneKey);
    bool HasZone() const { return !m_cells.empty(); }

    // Marks the cells within the sweep radius of `at` as swept.
    void Sweep(const Position& at, DWORD now, const SailTuning& tuning);

    // Next jump for the mobs in view. From a tile with a good fan the hero
    // only moves for a clearly better one (within this jump's `stride`, or
    // farther away and worth the trip); from a tile without one it heads for
    // the best fan it can get.
    ClumpPlan PlanClump(const Position& origin, float heading, bool hasHeading, int stride,
        const std::vector<Position>& mobs, const SailFan& fan, const ClumpTuning& tuning,
        const LandFn& canLand) const;

    // Next step of the scouting course from `origin`: a fan of smooth courses
    // scored by how much stale ground they sweep.
    SailPlan PlanScout(const Position& origin, float heading, bool hasHeading, int stride,
        DWORD now, const SailTuning& tuning, const LandFn& canLand);

    // One step toward a fixed tile (loot, the way back into the zone): the
    // tile itself when it is within `stride`, otherwise the landing tile
    // closest to it.
    SailPlan PlanToward(const Position& origin, const Position& target, int stride,
        const LandFn& canLand) const;

    // ── Read access for the overlay and stats ──
    struct CellView {
        Position center;
        float staleness;   // 0 = just swept, 1 = due
    };
    void ForEachCell(DWORD now, const SailTuning& tuning,
        const std::function<void(const CellView&)>& fn) const;
    float SweptPercent(DWORD now, const SailTuning& tuning) const;
    Position GetGlobalTarget() const { return m_globalTarget; }

private:
    struct Cell {
        bool valid = false;
        Position anchor;             // walkable zone tile nearest the cell's center
        bool swept = false;
        DWORD sweptTick = 0;
        uint32_t stamp = 0;          // scratch: last candidate that counted this cell
    };

    bool IsTileOpen(const Position& tile) const;   // on the map, walkable, in the zone
    int CellIndexAt(int tileX, int tileY) const;
    float Staleness(const Cell& cell, DWORD now, const SailTuning& tuning) const;
    float SweepGain(double x, double y, float weight, DWORD now, const SailTuning& tuning);
    bool SnapLanding(const Position& origin, Position wanted, int maxDist,
        const LandFn& canLand, Position& outTile) const;
    void UpdateGlobalTarget(const Position& origin, DWORD now, const SailTuning& tuning);
    bool GlobalDirection(const Position& origin, double& outX, double& outY) const;

    const CGameMap* m_map = nullptr;
    ZoneFn m_inZone;
    uint64_t m_zoneKey = 0;
    int m_minX = 0;
    int m_minY = 0;
    int m_cols = 0;
    int m_rows = 0;
    std::vector<Cell> m_cells;
    uint32_t m_stamp = 0;

    Position m_globalTarget = {};
    int m_globalCell = -1;
    float m_globalScore = 0.0f;
    DWORD m_globalTick = 0;
    std::vector<uint16_t> m_field;   // steps to m_globalCell through valid cells
};
