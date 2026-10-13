// Offline simulation of the Archer Hunt 2 guidance (src/sail_planner.*).
//
//   sail_sim <map dump> <out.json> [key=value ...]
//
// The map dump is what CGameMap::DumpToFile writes (tests/dmap_to_dump.py makes
// one from a client DMap file). The hero hunts a polygon with jumps while
// simulated mobs spawn in clumps, chase the hero, get hit by Scatter and
// respawn; the jumps, the casts and the swept ground go to the JSON file
// (tests/render_sail_sim.py draws it).
//
// Options (defaults in brackets):
//   seconds [180]        simulated time
//   seed [1]
//   polygon              hunt zone as "x:y;x:y;..." (the zonePolygon format of the config file)
//   guide [clump]        clump = clumps lead, scouting when none is in view; scout = scouting only
//   work [9] travel [14] stride near mobs / over empty ground
//   fan [180] radius [12] minhits [1] gain [2]   Scatter fan angle and radius, minimum hits, reposition gain
//   chain [40] pause [700] misclick [3]          rhythm: chain chance %, longest pause ms, misclick chance %
//   density [5]          mobs per 100 zone tiles
//   clump [15]           mobs per spawn clump
//   hp [1]               Scatter hits a mob takes
//   view [18]            mobs farther than this from the hero aren't known to the client
#include "CGameMap.h"
#include "hunt_settings.h"
#include "sail_planner.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

// Stub for base.h global (not used by the planner)
ULONG64 g_qwModuleBase = 0;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr DWORD kFrameMs = 16;
constexpr DWORD kFlightMs = 257;        // jump duration with Fly, any distance
constexpr DWORD kChainLeadMs = 100;     // a chained jump is queued this long before landing
constexpr DWORD kScatterIntervalMs = 800;
constexpr DWORD kHoldReplanMs = 150;    // while holding a tile, look for a better one this often
constexpr DWORD kRespawnMinMs = 15000;
constexpr DWORD kRespawnMaxMs = 25000;
constexpr int kClumpRadius = 6;         // mobs of one clump spawn this close to its center
constexpr int kAggroRange = 9;          // mobs this close walk toward the hero...
constexpr DWORD kMobStepMs = 500;       // ...one tile per this long
constexpr int kMinPauseMs = 40;
constexpr int kMisclickPauseMinMs = 500;
constexpr int kMisclickPauseMaxMs = 1200;

struct SimMob {
    Position spawn;
    Position pos;
    int hp = 1;
    bool alive = true;
    DWORD respawnTick = 0;
    DWORD nextStepTick = 0;
};

struct LoadedMap {
    std::vector<CellInfo> cells;
    CGameMap map;
};

bool LoadDump(const char* path, LoadedMap& out)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    uint32_t w = 0, h = 0, id = 0;
    fread(&w, 4, 1, f);
    fread(&h, 4, 1, f);
    fread(&id, 4, 1, f);
    out.cells.resize((size_t)w * h);
    memset(out.cells.data(), 0, out.cells.size() * sizeof(CellInfo));
    for (size_t i = 0; i < out.cells.size(); ++i) {
        uint16_t mask = 0;
        int16_t alt = 0;
        fread(&mask, 2, 1, f);
        fread(&alt, 2, 1, f);
        out.cells[i].layer.mask = mask;
        out.cells[i].layer.altitude = alt;
    }
    fclose(f);

    memset(&out.map, 0, sizeof(out.map));
    out.map.m_sizeMap.iWidth = (int)w;
    out.map.m_sizeMap.iHeight = (int)h;
    out.map.m_pCellInfo = out.cells.data();
    out.map.m_idMap = id;
    return true;
}

int GridDistance(const Position& a, const Position& b)
{
    return (std::max)(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

bool SameTile(const Position& a, const Position& b)
{
    return a.x == b.x && a.y == b.y;
}

bool ParsePolygon(const char* text, std::vector<Position>& out)
{
    out.clear();
    for (const char* p = text; *p;) {
        int x = 0, y = 0, used = 0;
        if (sscanf(p, "%d:%d%n", &x, &y, &used) != 2)
            break;
        out.push_back({x, y});
        p += used;
        if (*p == ';')
            ++p;
    }
    return out.size() >= 3;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: sail_sim <map dump> <out.json> [key=value ...]  (see the top of tests/sail_sim.cpp)\n");
        return 2;
    }
    std::map<std::string, std::string> options;
    for (int i = 3; i < argc; ++i) {
        const char* eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "expected key=value, got '%s'\n", argv[i]);
            return 2;
        }
        options[std::string(argv[i], (size_t)(eq - argv[i]))] = eq + 1;
    }
    auto number = [&](const char* key, int fallback) {
        const auto it = options.find(key);
        return it != options.end() ? atoi(it->second.c_str()) : fallback;
    };

    const int seconds = number("seconds", 180);
    const unsigned seed = (unsigned)number("seed", 1);
    const int workStride = number("work", 9);
    const int travelStride = number("travel", 14);
    const int chainChance = number("chain", 40);
    const int pauseMax = (std::max)(kMinPauseMs, number("pause", 700));
    const int misclickChance = number("misclick", 3);
    const int mobHp = (std::max)(1, number("hp", 1));
    const int viewRange = number("view", 18);
    const bool clumpsLead = options.count("guide") == 0 || options["guide"] != "scout";

    static LoadedMap loaded;
    if (!LoadDump(argv[1], loaded)) {
        fprintf(stderr, "failed to open %s\n", argv[1]);
        return 1;
    }
    CGameMap* map = &loaded.map;

    // A hunt polygon on the Desert map unless one is given.
    std::vector<Position> polygon = {
        {496, 478}, {488, 394}, {503, 344}, {556, 247}, {692, 378}, {715, 408},
        {700, 433}, {651, 480}, {628, 500}, {609, 508}, {588, 496},
    };
    if (options.count("polygon") && !ParsePolygon(options["polygon"].c_str(), polygon)) {
        fprintf(stderr, "polygon needs at least 3 points\n");
        return 2;
    }
    int minX = polygon[0].x, maxX = polygon[0].x, minY = polygon[0].y, maxY = polygon[0].y;
    for (const Position& p : polygon) {
        minX = (std::min)(minX, p.x);
        maxX = (std::max)(maxX, p.x);
        minY = (std::min)(minY, p.y);
        maxY = (std::max)(maxY, p.y);
    }
    auto inZone = [&](const Position& p) { return PointInPolygon(p, polygon); };

    SailFan fan;
    fan.radius = number("radius", 12);
    fan.aimDistance = 20;
    fan.halfAngleRad = number("fan", 180) * kPi / 360.0;

    SailTuning tuning;
    ClumpTuning clumpTuning;
    clumpTuning.minHits = (std::max)(1, number("minhits", 1));
    clumpTuning.repositionGain = number("gain", 2);

    SailPlanner planner;
    planner.SetZone(map, minX, minY, maxX, maxY, inZone, 1);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    auto percent = [&]() { return (int)(rng() % 100); };

    std::vector<Position> openTiles;
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            if (map->IsWalkable(x, y) && inZone({x, y}))
                openTiles.push_back({x, y});
        }
    }
    if (openTiles.empty()) {
        fprintf(stderr, "zone has no walkable tile\n");
        return 1;
    }

    // Mobs spawn in clumps around random centers.
    const int mobCount = (std::max)(1, (int)openTiles.size() * number("density", 5) / 100);
    const int clumpSize = (std::max)(1, number("clump", 15));
    std::vector<SimMob> mobs;
    while ((int)mobs.size() < mobCount) {
        const Position center = openTiles[rng() % openTiles.size()];
        for (int i = 0; i < clumpSize && (int)mobs.size() < mobCount; ++i) {
            for (int attempt = 0; attempt < 20; ++attempt) {
                const Position tile = {
                    center.x + (int)(rng() % (2 * kClumpRadius + 1)) - kClumpRadius,
                    center.y + (int)(rng() % (2 * kClumpRadius + 1)) - kClumpRadius,
                };
                if (!map->IsWalkable(tile.x, tile.y) || !inZone(tile))
                    continue;
                SimMob mob;
                mob.spawn = tile;
                mob.pos = tile;
                mob.hp = mobHp;
                mobs.push_back(mob);
                break;
            }
        }
    }

    auto canLand = [&](const Position& from, const Position& to) {
        if (!inZone(to))
            return false;
        for (const SimMob& mob : mobs) {
            if (mob.alive && SameTile(mob.pos, to))
                return false;
        }
        return map->CanJump(from.x, from.y, to.x, to.y);
    };

    FILE* out = fopen(argv[2], "w");
    if (!out) {
        fprintf(stderr, "failed to write %s\n", argv[2]);
        return 1;
    }
    fprintf(out, "{\"polygon\":[");
    for (size_t i = 0; i < polygon.size(); ++i)
        fprintf(out, "%s[%d,%d]", i ? "," : "", polygon[i].x, polygon[i].y);
    fprintf(out, "],\n\"landings\":[");

    // Hero state. `serverPos` is where the server has the hero: the landing
    // tile of the jump in the air.
    Position serverPos = openTiles[openTiles.size() / 2];
    float heading = 0.0f;
    bool hasHeading = false;
    DWORD flightEnd = 0;          // 0 = on the ground
    DWORD groundSince = 0;
    bool hasQueued = false;       // next jump queued behind the one in the air
    Position queued = {};
    float queuedHeading = 0.0f;
    bool queuedByClump = false;
    bool chainNext = true;
    DWORD pauseMs = 0;
    double strideFactor = 1.0;
    DWORD lastCast = 0;
    DWORD lastHoldPlan = 0;
    bool holding = false;

    int jumps = 0, clumpJumps = 0, casts = 0, totalHits = 0, kills = 0, sharpTurns = 0;
    double turnSum = 0.0, strideSum = 0.0;
    DWORD holdMs = 0;
    std::vector<int> castLog;   // ox, oy, ax, ay, hits, t

    std::vector<Position> visible;
    const DWORD start = 1000;
    const DWORD end = start + (DWORD)seconds * 1000;

    // What the plugin does when the client can take a jump: clumps first,
    // scouting when no mob worth a cast is in view. Returns false to stay.
    auto decide = [&](const Position& origin, DWORD now, Position& landing, float& newHeading, bool& byClump) {
        const int stride = std::clamp(
            (int)std::lround(ChooseSailStride(visible, origin, fan, workStride, travelStride) * strideFactor), 4, 16);
        if (clumpsLead) {
            const ClumpPlan clump = planner.PlanClump(origin, heading, hasHeading, stride, visible, fan,
                clumpTuning, canLand);
            if (clump.action == ClumpPlan::Action::Hold)
                return false;
            if (clump.action == ClumpPlan::Action::Jump) {
                landing = clump.landing;
                newHeading = clump.heading;
                byClump = true;
                return true;
            }
        }
        const SailPlan scout = planner.PlanScout(origin, heading, hasHeading, stride, now, tuning, canLand);
        if (!scout.valid)
            return false;
        landing = scout.landing;
        newHeading = scout.heading;
        byClump = false;
        return true;
    };

    auto takeOff = [&](const Position& landing, float newHeading, bool byClump, DWORD now) {
        if (hasHeading) {
            double turn = std::abs((double)newHeading - heading);
            if (turn > kPi)
                turn = 2 * kPi - turn;
            turnSum += turn;
            if (turn > kPi / 2)
                ++sharpTurns;
        }
        strideSum += serverPos.DistanceTo(landing);
        serverPos = landing;
        heading = newHeading;
        hasHeading = true;
        flightEnd = now + kFlightMs;
        fprintf(out, "%s[%d,%d,%u,%d]", jumps ? "," : "", landing.x, landing.y, now - start, byClump ? 1 : 0);
        ++jumps;
        if (byClump)
            ++clumpJumps;

        // Rhythm of the jump after this one (as ScatterSailPlugin::RollRhythm).
        strideFactor = 1.0 + 0.3 * (2.0 * unit(rng) - 1.0);
        if (percent() < misclickChance) {
            chainNext = false;
            pauseMs = (DWORD)(kMisclickPauseMinMs + unit(rng) * (kMisclickPauseMaxMs - kMisclickPauseMinMs));
        } else {
            chainNext = percent() < chainChance;
            const double u = unit(rng);
            pauseMs = chainNext ? 0 : (DWORD)(kMinPauseMs + (pauseMax - kMinPauseMs) * u * u * u);
        }
    };

    for (DWORD now = start; now < end; now += kFrameMs) {
        for (SimMob& mob : mobs) {
            if (!mob.alive) {
                if (now >= mob.respawnTick) {
                    mob.alive = true;
                    mob.pos = mob.spawn;
                    mob.hp = mobHp;
                }
                continue;
            }
            // Mobs near the hero close in on it.
            const int dist = GridDistance(mob.pos, serverPos);
            if (dist > 1 && dist <= kAggroRange && now >= mob.nextStepTick) {
                mob.nextStepTick = now + kMobStepMs;
                const Position step = {
                    mob.pos.x + (serverPos.x > mob.pos.x) - (serverPos.x < mob.pos.x),
                    mob.pos.y + (serverPos.y > mob.pos.y) - (serverPos.y < mob.pos.y),
                };
                if (map->IsWalkable(step.x, step.y))
                    mob.pos = step;
            }
        }

        visible.clear();
        for (const SimMob& mob : mobs) {
            if (mob.alive && GridDistance(mob.pos, serverPos) <= viewRange)
                visible.push_back(mob.pos);
        }

        planner.Sweep(serverPos, now, tuning);

        // Movement
        if (flightEnd != 0 && now >= flightEnd) {
            flightEnd = 0;
            groundSince = now;
        }
        if (flightEnd != 0) {
            if (chainNext && !hasQueued && flightEnd - now <= kChainLeadMs)
                hasQueued = decide(serverPos, now, queued, queuedHeading, queuedByClump);
        } else if (hasQueued) {
            hasQueued = false;
            takeOff(queued, queuedHeading, queuedByClump, now);
        } else if (now - groundSince >= pauseMs && (!holding || now - lastHoldPlan >= kHoldReplanMs)) {
            Position landing;
            float newHeading = 0.0f;
            bool byClump = false;
            lastHoldPlan = now;
            holding = !decide(serverPos, now, landing, newHeading, byClump);
            if (!holding)
                takeOff(landing, newHeading, byClump, now);
        }
        if (flightEnd == 0 && holding)
            holdMs += kFrameMs;

        // Scatter
        if (now - lastCast >= kScatterIntervalMs) {
            const SailShot shot = FindBestSailShot(visible, serverPos, fan,
                map->m_sizeMap.iWidth, map->m_sizeMap.iHeight);
            if (shot.hits >= clumpTuning.minHits) {
                lastCast = now;
                ++casts;
                const double cosHalf = std::cos(fan.halfAngleRad);
                int hits = 0;
                for (SimMob& mob : mobs) {
                    if (!mob.alive || GridDistance(mob.pos, serverPos) > fan.radius)
                        continue;
                    const double dx = shot.aim.x - serverPos.x, dy = shot.aim.y - serverPos.y;
                    const double tx = mob.pos.x - serverPos.x, ty = mob.pos.y - serverPos.y;
                    if (tx == 0 && ty == 0)
                        continue;
                    if (dx * tx + dy * ty < std::sqrt((dx * dx + dy * dy) * (tx * tx + ty * ty)) * cosHalf - 1e-9)
                        continue;
                    ++hits;
                    if (--mob.hp > 0)
                        continue;
                    mob.alive = false;
                    mob.respawnTick = now + kRespawnMinMs + rng() % (kRespawnMaxMs - kRespawnMinMs);
                    ++kills;
                }
                totalHits += hits;
                castLog.insert(castLog.end(), {serverPos.x, serverPos.y, shot.aim.x, shot.aim.y, hits, (int)(now - start)});
            }
        }
    }

    fprintf(out, "],\n\"casts\":[");
    for (size_t i = 0; i + 5 < castLog.size(); i += 6)
        fprintf(out, "%s[%d,%d,%d,%d,%d,%d]", i ? "," : "", castLog[i], castLog[i + 1], castLog[i + 2],
            castLog[i + 3], castLog[i + 4], castLog[i + 5]);
    fprintf(out, "],\n\"cells\":[");
    bool firstCell = true;
    planner.ForEachCell(end, tuning, [&](const SailPlanner::CellView& cell) {
        fprintf(out, "%s[%d,%d,%.2f]", firstCell ? "" : ",", cell.center.x, cell.center.y, cell.staleness);
        firstCell = false;
    });
    fprintf(out, "],\n\"mobs\":[");
    for (size_t i = 0; i < mobs.size(); ++i)
        fprintf(out, "%s[%d,%d,%d]", i ? "," : "", mobs[i].spawn.x, mobs[i].spawn.y, mobs[i].alive ? 1 : 0);
    fprintf(out, "]}\n");
    fclose(out);

    printf("zone tiles %zu, mobs %zu in clumps of %d, %d s, guide=%s\n",
        openTiles.size(), mobs.size(), clumpSize, seconds, clumpsLead ? "clump" : "scout");
    printf("jumps %d (%.2f/s; %d%% led by a clump), mean stride %.1f, mean turn %.0f deg, turns over 90 deg %.0f%%, holding %.0f%% of the time\n",
        jumps, jumps / (double)seconds, jumps ? clumpJumps * 100 / jumps : 0,
        jumps ? strideSum / jumps : 0.0,
        jumps > 1 ? turnSum / (jumps - 1) * 180.0 / kPi : 0.0,
        jumps ? sharpTurns * 100.0 / jumps : 0.0,
        holdMs * 100.0 / (seconds * 1000.0));
    printf("casts %d (%.2f/s), hits/cast %.2f, kills/min %.0f\n",
        casts, casts / (double)seconds, casts ? totalHits / (double)casts : 0.0, kills * 60.0 / seconds);
    return 0;
}
