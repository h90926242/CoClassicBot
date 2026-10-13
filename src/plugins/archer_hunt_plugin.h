#pragma once
#include "base_hunt_plugin.h"

class CMagic;

class ArcherHuntPlugin : public BaseHuntPlugin {
public:
    const char* GetName() const override { return "Archer Hunt"; }
    int GetLastScatterRange() const override { return m_lastScatterRange; }
    Position GetLastScatterAimPos() const { return m_lastScatterAimPos; }

protected:
    // Required virtual overrides
    CRole* FindBestTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) override;
    void HandleCombatApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& approachPos, bool movementCommitted) override;
    void HandleCombatAttack(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target, const Position& attackPos, DWORD now) override;
    bool HandleCombatRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        CRole* target) override;
    bool HandleCombatItems(CHero* hero, const AutoHuntSettings& settings) override;
    bool NeedsTownRunArrows(const CHero* hero, const AutoHuntSettings& settings) const override;
    void RenderCombatUI(AutoHuntSettings& settings) override;
    bool HandleNoTargetIdle(CHero* hero, CGameMap* map, const AutoHuntSettings& settings) override;
    void RefreshCombatState(CHero* hero, const AutoHuntSettings& settings) override;
    AutoHuntCombatMode GetExpectedCombatMode() const override { return AutoHuntCombatMode::Archer; }

private:
    // Scatter geometry. The server resolves a cast as a fan: the hero is the
    // origin, the aim tile sets the direction, and mobs within `radius` and
    // inside the fan angle are hit.
    struct ScatterGeometry {
        int radius = 0;              // hit radius (magictype `range`)
        int aimDistance = 0;         // max distance of the aim tile (magictype `distance`)
        double halfAngleRad = 0.0;   // half the fan angle
        double cosHalfAngle = 0.0;
        bool IsValid() const { return radius > 0 && aimDistance > 0; }
    };

    CMagic* FindScatterMagic(const CHero* hero) const;
    ScatterGeometry GetScatterGeometry(const CHero* hero, const AutoHuntSettings& settings) const;
    bool IsTargetInScatterSector(const Position& origin, const Position& aimPos,
        const Position& targetPos, const ScatterGeometry& geo) const;
    bool FindBestScatterShot(const std::vector<CRole*>& targets, const Position& origin,
        const ScatterGeometry& geo, int minimumHits, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount) const;
    bool FindBestScatterApproach(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& targets, const ScatterGeometry& geo, int minimumHits,
        Position& outApproachPos, Position& outCastPos,
        CRole*& outPrimaryTarget, int& outHitCount,
        bool preferFarTiles = false) const;

    // Retreat
    bool TryArcherDangerRetreat(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, CRole* target);
    bool FindSafeArcherRetreat(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        const std::vector<CRole*>& threats, CRole* target, Position& outRetreatPos,
        int safetyDistOverride = 0) const;

    // Target finding
    CRole* FindBestArcherTarget(CHero* hero, CGameMap* map, const AutoHuntSettings& settings,
        Position* outApproachPos, Position* outAttackPos,
        int* outClumpSize, bool* outUseScatter) const;
    bool FindArcherPatrolPosition(CHero* hero, CGameMap* map,
        const AutoHuntSettings& settings, Position& outPatrolPos) const;

    // Arrow management
    bool TryManageArrows(CHero* hero, const AutoHuntSettings& settings);
    bool NeedsArrows(const CHero* hero, const AutoHuntSettings& settings) const;
    int CountUsableArrowPacks(const CHero* hero) const;

    // Archer-specific state
    int m_lastScatterRange = 0;          // current hit radius
    int m_lastScatterAimDistance = 0;
    Position m_lastScatterAimPos = {};
    int m_lastScatterHitCount = 0;
    mutable DWORD m_lastScatterApproachTick = 0;
    Position m_lastFailedRetreatDest = {};
    DWORD m_lastFailedRetreatTick = 0;
    DWORD m_retreatCooldownTick = 0;
    DWORD m_retreatHoldUntilTick = 0;
    DWORD m_lastArrowTick = 0;
};
