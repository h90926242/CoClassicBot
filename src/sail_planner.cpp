#include "sail_planner.h"
#include "CGameMap.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;
// Candidate aim directions put a mob this far inside a fan edge, so rounding
// the aim to a tile doesn't push that mob out of the fan.
constexpr double kFanEdgeMarginRad = 3.0 * kPi / 180.0;
constexpr DWORD kGlobalRefreshMs = 700;
// The far target only changes for a region this much better than the current.
constexpr float kGlobalSwitchGain = 1.3f;
constexpr int kLandingSnapRadius = 3;
constexpr uint16_t kNoField = 0xFFFF;
constexpr int kMaxCells = 400000;

// Score of a candidate course: the share of stale ground it sweeps (about
// 0..1), plus the terms below.
constexpr double kGlobalWeight = 0.6;             // heading for the stalest region...
constexpr double kLocalGainForNoGlobal = 0.5;     // ...fades out as nearby courses reach this gain
constexpr double kAnyTurnCost = 0.04;             // leaving the current line at all
constexpr double kFirstTurnCost = 0.12;           // turning hard on this step
constexpr double kTotalTurnCost = 0.12;           // turning far overall
constexpr double kBlockedCost = 0.15;             // course cut short by the zone edge or terrain

// Clump guidance looks at standing tiles on a grid this coarse, lined up with
// the tile the jump starts from.
constexpr int kClumpGrid = 2;
// Tiles kept as far goals: the best ones, none within kGoalSpacing of a better one.
constexpr int kMaxGoals = 16;
constexpr int kGoalSpacing = 6;
// A goal is worth this much less for every jump of travel it is away, so a
// bigger clump only wins over a nearer one when it is clearly bigger.
constexpr double kJumpDiscount = 0.85;
// Between tiles that lead to equally good goals, the one with more hits now wins.
constexpr double kImmediateHitWeight = 0.05;
constexpr int kMaxLandingTries = 24;
// Share of a landing tile's worth that a full turn-around costs it.
constexpr double kClumpTurnCost = 0.15;

// Later points of a course count less: the course is re-planned every step.
double StepWeight(int step)
{
    return (std::max)(0.35, 1.0 - 0.16 * step);
}

int GridDistance(const Position& a, const Position& b)
{
    return (std::max)(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool InFan(const Position& origin, const Position& aim, const Position& target,
    const SailFan& fan, double cosHalfAngle)
{
    if (GridDistance(origin, target) > fan.radius)
        return false;
    const double dirX = (double)(aim.x - origin.x);
    const double dirY = (double)(aim.y - origin.y);
    const double targetX = (double)(target.x - origin.x);
    const double targetY = (double)(target.y - origin.y);
    if (targetX == 0.0 && targetY == 0.0)
        return false;

    // Inside the fan when the angle between the aim direction and the target is
    // at most half the fan angle: dot(dir, t) >= |dir| * |t| * cos(half).
    const double dot = dirX * targetX + dirY * targetY;
    const double lenProduct = std::sqrt((dirX * dirX + dirY * dirY) * (targetX * targetX + targetY * targetY));
    return dot >= lenProduct * cosHalfAngle - 1e-9;
}

double AngleDiff(double a, double b)
{
    double d = a - b;
    while (d > kPi) d -= 2.0 * kPi;
    while (d < -kPi) d += 2.0 * kPi;
    return d;
}

} // namespace

// ── Scatter fan ──────────────────────────────────────────────────────────────

SailShot FindBestSailShot(const std::vector<Position>& mobs, const Position& origin,
    const SailFan& fan, int mapWidth, int mapHeight)
{
    SailShot best;
    if (!fan.IsValid() || mobs.empty())
        return best;

    int inRadius = 0;
    for (const Position& mob : mobs) {
        if ((mob.x != origin.x || mob.y != origin.y) && GridDistance(origin, mob) <= fan.radius)
            ++inRadius;
    }
    if (inRadius == 0)
        return best;

    // Aim at the fan's edge (capped by the aim distance): a far aim tile keeps
    // the tile-rounding error in direction to a few degrees. Near the map
    // border the aim is pulled in until it lands on the map.
    const int aimLength = (std::min)(fan.radius, fan.aimDistance);
    auto makeAimTile = [&](double angle, Position& outAim) {
        for (int len = aimLength; len >= 1; --len) {
            const Position aim = {
                origin.x + (int)std::lround(std::cos(angle) * len),
                origin.y + (int)std::lround(std::sin(angle) * len)
            };
            if (aim.x == origin.x && aim.y == origin.y)
                return false;
            if (aim.x < 0 || aim.y < 0 || aim.x >= mapWidth || aim.y >= mapHeight)
                continue;
            outAim = aim;
            return true;
        }
        return false;
    };

    // Candidate directions: straight at each mob, and with that mob just inside
    // either fan edge. The best fan always has a mob at (or near) one of its
    // edges. Each candidate is scored with its rounded aim tile, since that is
    // what the server sees.
    const double cosHalfAngle = std::cos(fan.halfAngleRad);
    const double edgeOffset = fan.halfAngleRad - (std::min)(kFanEdgeMarginRad, fan.halfAngleRad * 0.25);
    const double directionOffsets[] = {0.0, edgeOffset, -edgeOffset};

    for (const Position& dirMob : mobs) {
        if (dirMob.x == origin.x && dirMob.y == origin.y)
            continue;
        if (GridDistance(origin, dirMob) > fan.radius)
            continue;

        const double baseAngle = std::atan2((double)(dirMob.y - origin.y), (double)(dirMob.x - origin.x));
        for (double offset : directionOffsets) {
            Position aim = {};
            if (!makeAimTile(baseAngle + offset, aim))
                continue;

            int hits = 0;
            for (const Position& mob : mobs) {
                if (InFan(origin, aim, mob, fan, cosHalfAngle))
                    ++hits;
            }
            if (hits > best.hits) {
                best.hits = hits;
                best.aim = aim;
            }
            // Every mob in radius is hit: no direction can do better.
            if (best.hits == inRadius)
                return best;
        }
    }

    return best;
}

int CountBestFanHits(const std::vector<Position>& mobs, const Position& origin, const SailFan& fan)
{
    std::vector<double> angles;
    return CountBestFanHits(mobs, origin, fan, angles);
}

int CountBestFanHits(const std::vector<Position>& mobs, const Position& origin, const SailFan& fan,
    std::vector<double>& angles)
{
    angles.clear();
    if (!fan.IsValid())
        return 0;

    for (const Position& mob : mobs) {
        if (mob.x == origin.x && mob.y == origin.y)
            continue;
        if (GridDistance(origin, mob) > fan.radius)
            continue;
        angles.push_back(std::atan2((double)(mob.y - origin.y), (double)(mob.x - origin.x)));
    }
    const int count = (int)angles.size();
    if (count <= 1 || fan.halfAngleRad >= kPi)
        return count;

    // Widest group of mobs whose directions fit inside one fan.
    std::sort(angles.begin(), angles.end());
    const double width = 2.0 * (fan.halfAngleRad - (std::min)(kFanEdgeMarginRad, fan.halfAngleRad * 0.25));
    int best = 1;
    int end = 0;
    for (int start = 0; start < count; ++start) {
        if (end < start)
            end = start;
        while (end + 1 < start + count) {
            const int next = end + 1;
            const double angle = next < count ? angles[next] : angles[next - count] + 2.0 * kPi;
            if (angle - angles[start] > width)
                break;
            end = next;
        }
        best = (std::max)(best, end - start + 1);
    }
    return best;
}

int ChooseSailStride(const std::vector<Position>& mobs, const Position& origin,
    const SailFan& fan, int workStride, int travelStride)
{
    if (travelStride <= workStride)
        return workStride;

    // Mobs the hero can hit now or after one travel hop.
    constexpr int kMobsForWorkStride = 3;
    const int range = (fan.IsValid() ? fan.radius : 0) + travelStride / 2;
    int nearMobs = 0;
    for (const Position& mob : mobs) {
        if (GridDistance(origin, mob) <= range && ++nearMobs >= kMobsForWorkStride)
            return workStride;
    }
    return travelStride - (travelStride - workStride) * nearMobs / kMobsForWorkStride;
}

// ── Sail planner ─────────────────────────────────────────────────────────────

void SailPlanner::Reset()
{
    m_map = nullptr;
    m_inZone = nullptr;
    m_zoneKey = 0;
    m_minX = 0;
    m_minY = 0;
    m_cols = 0;
    m_rows = 0;
    m_cells.clear();
    m_stamp = 0;
    m_globalTarget = {};
    m_globalCell = -1;
    m_globalScore = 0.0f;
    m_globalTick = 0;
    m_field.clear();
}

void SailPlanner::SetZone(const CGameMap* map, int minX, int minY, int maxX, int maxY,
    const ZoneFn& inZone, uint64_t zoneKey)
{
    if (map == m_map && zoneKey == m_zoneKey && !m_cells.empty()) {
        m_inZone = inZone;
        return;
    }

    Reset();
    if (!map || !inZone || maxX < minX || maxY < minY)
        return;

    minX = (std::max)(minX, 0);
    minY = (std::max)(minY, 0);
    maxX = (std::min)(maxX, map->m_sizeMap.iWidth - 1);
    maxY = (std::min)(maxY, map->m_sizeMap.iHeight - 1);
    if (maxX < minX || maxY < minY)
        return;

    const int cols = (maxX - minX) / kCellSize + 1;
    const int rows = (maxY - minY) / kCellSize + 1;
    if (cols * rows > kMaxCells)
        return;

    m_map = map;
    m_inZone = inZone;
    m_zoneKey = zoneKey;
    m_minX = minX;
    m_minY = minY;
    m_cols = cols;
    m_rows = rows;
    m_cells.resize((size_t)cols * rows);
    m_field.assign(m_cells.size(), kNoField);

    // A cell takes part when at least a quarter of its tiles can be stood on
    // inside the zone.
    constexpr int kMinOpenTiles = kCellSize * kCellSize / 4;
    const double half = (kCellSize - 1) / 2.0;
    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            Cell& cell = m_cells[(size_t)cy * cols + cx];
            int open = 0;
            double bestDist = (std::numeric_limits<double>::max)();
            for (int dy = 0; dy < kCellSize; ++dy) {
                for (int dx = 0; dx < kCellSize; ++dx) {
                    const Position tile = {minX + cx * kCellSize + dx, minY + cy * kCellSize + dy};
                    if (!map->IsWalkable(tile.x, tile.y) || !inZone(tile))
                        continue;
                    ++open;
                    const double dist = (dx - half) * (dx - half) + (dy - half) * (dy - half);
                    if (dist < bestDist) {
                        bestDist = dist;
                        cell.anchor = tile;
                    }
                }
            }
            cell.valid = open >= kMinOpenTiles;
        }
    }
}

int SailPlanner::CellIndexAt(int tileX, int tileY) const
{
    if (m_cells.empty() || tileX < m_minX || tileY < m_minY)
        return -1;
    const int cx = (tileX - m_minX) / kCellSize;
    const int cy = (tileY - m_minY) / kCellSize;
    if (cx >= m_cols || cy >= m_rows)
        return -1;
    return cy * m_cols + cx;
}

float SailPlanner::Staleness(const Cell& cell, DWORD now, const SailTuning& tuning) const
{
    if (!cell.swept)
        return 1.0f;
    const DWORD age = now - cell.sweptTick;
    if (tuning.revisitMs == 0 || age >= tuning.revisitMs)
        return 1.0f;
    return (float)age / (float)tuning.revisitMs;
}

void SailPlanner::Sweep(const Position& at, DWORD now, const SailTuning& tuning)
{
    if (m_cells.empty())
        return;

    const int radius = (std::max)(1, tuning.sweepRadius);
    const double half = (kCellSize - 1) / 2.0;
    const int cx0 = (std::max)(0, (at.x - radius - m_minX) / kCellSize - 1);
    const int cy0 = (std::max)(0, (at.y - radius - m_minY) / kCellSize - 1);
    const int cx1 = (std::min)(m_cols - 1, (at.x + radius - m_minX) / kCellSize + 1);
    const int cy1 = (std::min)(m_rows - 1, (at.y + radius - m_minY) / kCellSize + 1);
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            Cell& cell = m_cells[(size_t)cy * m_cols + cx];
            if (!cell.valid)
                continue;
            const double dx = m_minX + cx * kCellSize + half - at.x;
            const double dy = m_minY + cy * kCellSize + half - at.y;
            if (dx * dx + dy * dy > (double)radius * radius)
                continue;
            cell.swept = true;
            cell.sweptTick = now;
        }
    }
}

// Stale ground within the sweep radius of a course point. Each cell counts
// once per candidate course (m_stamp).
float SailPlanner::SweepGain(double x, double y, float weight, DWORD now, const SailTuning& tuning)
{
    const int radius = (std::max)(1, tuning.sweepRadius);
    const double half = (kCellSize - 1) / 2.0;
    const int cx0 = (std::max)(0, (int)std::floor((x - radius - m_minX) / kCellSize) - 1);
    const int cy0 = (std::max)(0, (int)std::floor((y - radius - m_minY) / kCellSize) - 1);
    const int cx1 = (std::min)(m_cols - 1, (int)std::floor((x + radius - m_minX) / kCellSize) + 1);
    const int cy1 = (std::min)(m_rows - 1, (int)std::floor((y + radius - m_minY) / kCellSize) + 1);
    float gain = 0.0f;
    for (int cy = cy0; cy <= cy1; ++cy) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            Cell& cell = m_cells[(size_t)cy * m_cols + cx];
            if (!cell.valid || cell.stamp == m_stamp)
                continue;
            const double dx = m_minX + cx * kCellSize + half - x;
            const double dy = m_minY + cy * kCellSize + half - y;
            if (dx * dx + dy * dy > (double)radius * radius)
                continue;
            cell.stamp = m_stamp;
            gain += Staleness(cell, now, tuning) * weight;
        }
    }
    return gain;
}

bool SailPlanner::SnapLanding(const Position& origin, Position wanted, int maxDist,
    const LandFn& canLand, Position& outTile) const
{
    bool found = false;
    int bestDist = (std::numeric_limits<int>::max)();
    for (int dy = -kLandingSnapRadius; dy <= kLandingSnapRadius; ++dy) {
        for (int dx = -kLandingSnapRadius; dx <= kLandingSnapRadius; ++dx) {
            const int dist = dx * dx + dy * dy;
            if (dist >= bestDist)
                continue;
            const Position tile = {wanted.x + dx, wanted.y + dy};
            if (tile.x == origin.x && tile.y == origin.y)
                continue;
            const int ox = tile.x - origin.x;
            const int oy = tile.y - origin.y;
            if (ox * ox + oy * oy > maxDist * maxDist)
                continue;
            if (!canLand(origin, tile))
                continue;
            found = true;
            bestDist = dist;
            outTile = tile;
        }
    }
    return found;
}

// Picks the stalest region, discounted by distance, as the far target, and
// fills m_field with the cell steps to it through valid cells, so the pull
// toward it follows the zone's shape instead of a straight line.
void SailPlanner::UpdateGlobalTarget(const Position& origin, DWORD now, const SailTuning& tuning)
{
    if (m_globalTick != 0 && now - m_globalTick < kGlobalRefreshMs)
        return;
    m_globalTick = now;

    auto regionScore = [&](int cx, int cy) {
        float sum = 0.0f;
        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (nx < 0 || ny < 0 || nx >= m_cols || ny >= m_rows)
                    continue;
                const Cell& around = m_cells[(size_t)ny * m_cols + nx];
                if (around.valid)
                    sum += Staleness(around, now, tuning);
            }
        }
        const Cell& cell = m_cells[(size_t)cy * m_cols + cx];
        const float dist = origin.DistanceTo(cell.anchor);
        return sum / (1.0f + dist / 48.0f);
    };

    int best = -1;
    float bestScore = 0.0f;
    for (int cy = 0; cy < m_rows; ++cy) {
        for (int cx = 0; cx < m_cols; ++cx) {
            const Cell& cell = m_cells[(size_t)cy * m_cols + cx];
            if (!cell.valid || Staleness(cell, now, tuning) < 0.5f)
                continue;
            const float score = regionScore(cx, cy);
            if (score > bestScore) {
                bestScore = score;
                best = cy * m_cols + cx;
            }
        }
    }

    if (best < 0) {
        m_globalCell = -1;
        m_globalTarget = {};
        m_globalScore = 0.0f;
        return;
    }

    // Keep the current target while it is still stale and nothing is clearly better.
    if (m_globalCell >= 0) {
        const Cell& current = m_cells[m_globalCell];
        if (Staleness(current, now, tuning) >= 0.5f) {
            const float currentScore = regionScore(m_globalCell % m_cols, m_globalCell / m_cols);
            if (currentScore * kGlobalSwitchGain >= bestScore) {
                m_globalScore = currentScore;
                return;
            }
        }
    }

    m_globalCell = best;
    m_globalTarget = m_cells[best].anchor;
    m_globalScore = bestScore;

    std::fill(m_field.begin(), m_field.end(), kNoField);
    std::vector<int> frontier;
    frontier.push_back(best);
    m_field[best] = 0;
    for (size_t head = 0; head < frontier.size(); ++head) {
        const int index = frontier[head];
        const int cx = index % m_cols;
        const int cy = index / m_cols;
        const uint16_t next = (uint16_t)(m_field[index] + 1);
        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (nx < 0 || ny < 0 || nx >= m_cols || ny >= m_rows)
                    continue;
                const int neighbor = ny * m_cols + nx;
                if (!m_cells[neighbor].valid || m_field[neighbor] != kNoField)
                    continue;
                m_field[neighbor] = next;
                frontier.push_back(neighbor);
            }
        }
    }
}

// Unit direction from `origin` toward the far target, a few cells along the
// shortest way through the zone.
bool SailPlanner::GlobalDirection(const Position& origin, double& outX, double& outY) const
{
    if (m_globalCell < 0)
        return false;

    Position aim = m_globalTarget;
    int index = CellIndexAt(origin.x, origin.y);
    if (index >= 0 && m_field[index] != kNoField) {
        constexpr int kStepsAhead = 5;
        for (int step = 0; step < kStepsAhead && m_field[index] > 0; ++step) {
            const int cx = index % m_cols;
            const int cy = index / m_cols;
            int next = index;
            for (int ny = cy - 1; ny <= cy + 1; ++ny) {
                for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                    if (nx < 0 || ny < 0 || nx >= m_cols || ny >= m_rows)
                        continue;
                    const int neighbor = ny * m_cols + nx;
                    if (m_field[neighbor] < m_field[next])
                        next = neighbor;
                }
            }
            if (next == index)
                break;
            index = next;
        }
        aim = m_cells[index].anchor;
    }

    const double dx = aim.x - origin.x;
    const double dy = aim.y - origin.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0)
        return false;
    outX = dx / len;
    outY = dy / len;
    return true;
}

bool SailPlanner::IsTileOpen(const Position& tile) const
{
    return tile.x >= 0 && tile.y >= 0
        && tile.x < m_map->m_sizeMap.iWidth && tile.y < m_map->m_sizeMap.iHeight
        && m_map->IsWalkable(tile.x, tile.y) && m_inZone(tile);
}

SailPlan SailPlanner::PlanScout(const Position& origin, float heading, bool hasHeading, int stride,
    DWORD now, const SailTuning& tuning, const LandFn& canLand)
{
    SailPlan plan;
    if (m_cells.empty() || !m_map || stride <= 0 || !canLand)
        return plan;

    UpdateGlobalTarget(origin, now, tuning);
    double globalX = 0.0;
    double globalY = 0.0;
    const bool hasGlobal = GlobalDirection(origin, globalX, globalY);

    const int radius = (std::max)(1, tuning.sweepRadius);
    const int altThreshold = CGameMap::GetHeroAltThreshold();

    struct Candidate {
        std::vector<Position> points;
        double gain = 0.0;           // share of the stale ground a full course could sweep
        double globalDot = 0.0;      // how well the course heads for the far target (-1..1)
        double turnCost = 0.0;
        double blocked = 0.0;        // share of the course cut off by the zone edge or terrain
        double firstHeading = 0.0;
    };

    // Scores every candidate course for one step length; `anyDirection` drops
    // the turn limit (first step of a run, or when the limited fan is boxed in).
    auto search = [&](int stepLen, bool anyDirection) {
        const double step = (double)stepLen;
        const double maxTurn = anyDirection
            ? kPi
            : std::clamp(step / (double)(std::max)(1, tuning.turnRadius), 0.30, 1.40);
        const int steps = std::clamp((int)std::ceil(tuning.lookahead / step), 2, 6);
        const bool turnsCost = hasHeading && !anyDirection;

        // Total turn of each candidate: the course turns by at most maxTurn
        // per step until it has turned that far, then runs straight.
        std::vector<double> turns;
        if (anyDirection) {
            constexpr int kDirections = 16;
            for (int i = 0; i < kDirections; ++i)
                turns.push_back(AngleDiff(i * 2.0 * kPi / kDirections, 0.0));
        } else {
            turns.push_back(0.0);
            for (double turn = maxTurn / 2.0; turn < kPi - 1e-6; turn += maxTurn / 2.0) {
                turns.push_back(turn);
                turns.push_back(-turn);
            }
            turns.push_back(kPi);
            turns.push_back(-kPi);
        }

        // Stale cells a full course would sweep at best, to scale the gain:
        // one full disc, then the part of each further disc that the previous
        // one doesn't already cover.
        const double discCells = kPi * radius * radius / (double)(kCellSize * kCellSize);
        const double freshShare = (std::min)(1.0, 2.0 * step / (kPi * radius));
        double fullGain = 0.0;
        for (int i = 0; i < steps; ++i)
            fullGain += discCells * StepWeight(i) * (i == 0 ? 1.0 : freshShare);

        std::vector<Candidate> candidates;
        candidates.reserve(turns.size());
        double bestGain = 0.0;
        for (double totalTurn : turns) {
            ++m_stamp;
            Candidate candidate;
            double theta = hasHeading ? (double)heading : 0.0;
            double remaining = totalTurn;
            double x = origin.x;
            double y = origin.y;
            Position prev = origin;
            double gain = 0.0;

            for (int i = 0; i < steps; ++i) {
                const double turn = std::clamp(remaining, -maxTurn, maxTurn);
                theta += turn;
                remaining -= turn;
                x += std::cos(theta) * step;
                y += std::sin(theta) * step;
                Position tile = {(int)std::lround(x), (int)std::lround(y)};

                if (i == 0) {
                    // The first point is the tile the hero jumps to next; the
                    // rest of the course continues from the tile it snapped to.
                    if (!SnapLanding(origin, tile, (std::min)(stepLen + 2, CGameMap::MAX_JUMP_DIST - 2), canLand, tile))
                        break;
                    x = tile.x;
                    y = tile.y;
                    candidate.firstHeading = std::atan2((double)(tile.y - origin.y), (double)(tile.x - origin.x));
                } else if (!IsTileOpen(tile) || !m_map->CanJump(prev.x, prev.y, tile.x, tile.y, altThreshold)) {
                    break;
                }

                gain += SweepGain(x, y, (float)StepWeight(i), now, tuning);
                candidate.points.push_back(tile);
                prev = tile;
            }
            if (candidate.points.empty())
                continue;

            const std::vector<Position>& points = candidate.points;
            candidate.gain = gain / fullGain;
            candidate.blocked = (double)(steps - (int)points.size()) / steps;

            if (hasGlobal) {
                const double dx = points.back().x - origin.x;
                const double dy = points.back().y - origin.y;
                const double len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.5)
                    candidate.globalDot = (dx * globalX + dy * globalY) / len;
            }

            if (turnsCost) {
                const double firstTurn = (std::min)(1.5,
                    std::abs(AngleDiff(candidate.firstHeading, heading)) / maxTurn);
                candidate.turnCost = (totalTurn != 0.0 ? kAnyTurnCost : 0.0)
                    + kFirstTurnCost * firstTurn * firstTurn
                    + kTotalTurnCost * std::abs(totalTurn) / kPi;
            }

            bestGain = (std::max)(bestGain, candidate.gain);
            candidates.push_back(std::move(candidate));
        }

        // The far target only pulls when there is little stale ground within
        // reach of any course: with fresh ground ahead the course holds its
        // line, which is what lays the lanes next to each other.
        const double globalWeight = kGlobalWeight * std::clamp(1.0 - bestGain / kLocalGainForNoGlobal, 0.0, 1.0);
        double bestScore = -(std::numeric_limits<double>::max)();
        for (Candidate& candidate : candidates) {
            const double score = candidate.gain
                + globalWeight * candidate.globalDot
                - candidate.turnCost
                - kBlockedCost * candidate.blocked;
            if (score > bestScore) {
                bestScore = score;
                plan.valid = true;
                plan.landing = candidate.points[0];
                plan.heading = (float)candidate.firstHeading;
                plan.preview = std::move(candidate.points);
            }
        }
    };

    search(stride, !hasHeading);
    if (!plan.valid && hasHeading)
        search(stride, true);
    if (!plan.valid && stride > 4)
        search((std::max)(3, stride / 2), true);
    return plan;
}

ClumpPlan SailPlanner::PlanClump(const Position& origin, float heading, bool hasHeading, int stride,
    const std::vector<Position>& mobs, const SailFan& fan, const ClumpTuning& tuning,
    const LandFn& canLand) const
{
    ClumpPlan plan;
    if (!m_map || !m_inZone || mobs.empty() || !fan.IsValid() || stride <= 0 || !canLand)
        return plan;

    std::vector<double> scratch;
    const int minHits = (std::max)(1, tuning.minHits);
    plan.hereHits = CountBestFanHits(mobs, origin, fan, scratch);

    // Standing tiles worth a look: within the fan radius of a mob (farther
    // ones hit nothing) or within this jump's stride of the origin.
    int minX = origin.x - stride, maxX = origin.x + stride;
    int minY = origin.y - stride, maxY = origin.y + stride;
    for (const Position& mob : mobs) {
        minX = (std::min)(minX, mob.x - fan.radius);
        maxX = (std::max)(maxX, mob.x + fan.radius);
        minY = (std::min)(minY, mob.y - fan.radius);
        maxY = (std::max)(maxY, mob.y + fan.radius);
    }
    minX = (std::max)(minX, 0);
    minY = (std::max)(minY, 0);
    maxX = (std::min)(maxX, m_map->m_sizeMap.iWidth - 1);
    maxY = (std::min)(maxY, m_map->m_sizeMap.iHeight - 1);

    struct Spot {
        Position tile;
        int hits = 0;   // best fan from this tile
    };
    std::vector<Spot> spots;
    const int startX = origin.x - ((origin.x - minX) / kClumpGrid) * kClumpGrid;
    const int startY = origin.y - ((origin.y - minY) / kClumpGrid) * kClumpGrid;
    for (int y = startY; y <= maxY; y += kClumpGrid) {
        for (int x = startX; x <= maxX; x += kClumpGrid) {
            const Position tile = {x, y};
            if (IsTileOpen(tile))
                spots.push_back({tile, CountBestFanHits(mobs, tile, fan, scratch)});
        }
    }

    // Goals: the tiles with the fullest fans.
    std::vector<const Spot*> ranked;
    for (const Spot& spot : spots) {
        if (spot.hits >= minHits)
            ranked.push_back(&spot);
    }
    std::sort(ranked.begin(), ranked.end(), [&origin](const Spot* a, const Spot* b) {
        if (a->hits != b->hits)
            return a->hits > b->hits;
        return origin.DistanceTo(a->tile) < origin.DistanceTo(b->tile);
    });
    std::vector<const Spot*> goals;
    for (const Spot* spot : ranked) {
        bool nearBetter = false;
        for (const Spot* goal : goals) {
            if (GridDistance(goal->tile, spot->tile) < kGoalSpacing) {
                nearBetter = true;
                break;
            }
        }
        if (nearBetter)
            continue;
        goals.push_back(spot);
        if ((int)goals.size() >= kMaxGoals)
            break;
    }
    if (goals.empty())
        return plan;

    // What standing on a tile is worth: its own fan, or a fuller one farther
    // on, discounted for every jump it is still away (at least one: a tile
    // next to a goal is not the goal).
    struct Landing {
        const Spot* spot;
        const Spot* goal;
        double value;
        double score;
    };
    std::vector<Landing> landings;
    for (const Spot& spot : spots) {
        const int dx = spot.tile.x - origin.x;
        const int dy = spot.tile.y - origin.y;
        if ((dx == 0 && dy == 0) || dx * dx + dy * dy > stride * stride)
            continue;

        Landing landing = {&spot, &spot, spot.hits >= minHits ? (double)spot.hits : 0.0, 0.0};
        for (const Spot* goal : goals) {
            if (goal == &spot)
                continue;
            const double jumps = (std::max)(1.0, (double)spot.tile.DistanceTo(goal->tile) / stride);
            const double value = goal->hits * std::pow(kJumpDiscount, jumps);
            if (value > landing.value) {
                landing.value = value;
                landing.goal = goal;
            }
        }
        landing.score = landing.value + kImmediateHitWeight * spot.hits;
        if (hasHeading) {
            // Among tiles that are about as good, carry on rather than double back.
            const double turn = std::abs(AngleDiff(std::atan2((double)dy, (double)dx), heading));
            landing.score -= kClumpTurnCost * landing.value * turn / kPi;
        }
        landings.push_back(landing);
    }
    std::sort(landings.begin(), landings.end(), [](const Landing& a, const Landing& b) {
        return a.score > b.score;
    });

    // A tile with a good fan is only left for a clearly better one (the same
    // rule as the reposition gain of the archer hunt); from a tile with no
    // fan any goal will do.
    const bool goodHere = plan.hereHits >= minHits;
    const double needed = goodHere
        ? plan.hereHits + (std::max)(tuning.repositionGain, (plan.hereHits + 3) / 4)
        : 0.0;
    int tries = 0;
    for (const Landing& landing : landings) {
        if (landing.value <= 0.0 || landing.value < needed)
            continue;
        if (++tries > kMaxLandingTries)
            break;
        if (!canLand(origin, landing.spot->tile))
            continue;

        plan.action = ClumpPlan::Action::Jump;
        plan.landing = landing.spot->tile;
        plan.heading = (float)std::atan2((double)(plan.landing.y - origin.y), (double)(plan.landing.x - origin.x));
        plan.landingHits = landing.spot->hits;
        plan.goal = landing.goal->tile;
        plan.goalHits = landing.goal->hits;
        return plan;
    }

    if (goodHere) {
        plan.action = ClumpPlan::Action::Hold;
        plan.goal = origin;
        plan.goalHits = plan.hereHits;
    }
    return plan;
}

SailPlan SailPlanner::PlanToward(const Position& origin, const Position& target, int stride,
    const LandFn& canLand) const
{
    SailPlan plan;
    if (stride <= 0 || !canLand)
        return plan;

    const double dx = target.x - origin.x;
    const double dy = target.y - origin.y;
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist < 0.5)
        return plan;

    Position wanted = target;
    if (dist > stride) {
        wanted.x = origin.x + (int)std::lround(dx / dist * stride);
        wanted.y = origin.y + (int)std::lround(dy / dist * stride);
    }

    Position landing = {};
    const int maxDist = (std::min)(stride + 2, CGameMap::MAX_JUMP_DIST - 2);
    if (dist <= stride && canLand(origin, target))
        landing = target;
    else if (!SnapLanding(origin, wanted, maxDist, canLand, landing))
        return plan;

    plan.valid = true;
    plan.landing = landing;
    plan.heading = (float)std::atan2((double)(landing.y - origin.y), (double)(landing.x - origin.x));
    plan.preview.push_back(landing);
    return plan;
}

void SailPlanner::ForEachCell(DWORD now, const SailTuning& tuning,
    const std::function<void(const CellView&)>& fn) const
{
    for (const Cell& cell : m_cells) {
        if (cell.valid)
            fn({cell.anchor, Staleness(cell, now, tuning)});
    }
}

float SailPlanner::SweptPercent(DWORD now, const SailTuning& tuning) const
{
    int valid = 0;
    float fresh = 0.0f;
    for (const Cell& cell : m_cells) {
        if (!cell.valid)
            continue;
        ++valid;
        fresh += 1.0f - Staleness(cell, now, tuning);
    }
    return valid > 0 ? fresh * 100.0f / valid : 0.0f;
}
