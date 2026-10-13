#include "npc_utils.h"
#include "CRole.h"
#include "game.h"
#include <cstring>

CRole* FindNpcByName(const char* name, const Position& expectedPos, int radius)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    const RoleList roles = mgr ? mgr->Roles() : RoleList{};
    if (!mgr || roles.empty() || roles.size() >= 10000)
        return nullptr;

    CRole* best = nullptr;
    float bestDist = (float)(radius + 1);
    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        const auto& roleRef = roles[i];
        if (!roleRef)
            continue;

        CRole* role = roleRef.get();
        if (role->IsPlayer() || role->IsMonster())
            continue;
        if (name && name[0] && _stricmp(role->GetName(), name) != 0)
            continue;

        const float dist = expectedPos.DistanceTo(role->m_posMap);
        if (dist < bestDist) {
            bestDist = dist;
            best = role;
        }
    }

    return best;
}
