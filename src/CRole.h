#pragma once
#include "CMapObj.h"
#include <unordered_map>
#include <vector>

// =====================================================================
// User status flags
// =====================================================================
const __int64 USERSTATUS_NORMAL       = 0ll;
const __int64 USERSTATUS_CRIME        = 1ll << 0;
const __int64 USERSTATUS_POISON       = 1ll << 1;
const __int64 USERSTATUS_INVISIBLE    = 1ll << 2;
const __int64 USERSTATUS_DIE          = 1ll << 3;
const __int64 USERSTATUS_XPFULL       = 1ll << 4;
const __int64 USERSTATUS_DEAD         = 1ll << 5;
const __int64 USERSTATUS_TEAMLEADER   = 1ll << 6;
const __int64 USERSTATUS_ATKSPEED     = 1ll << 7;
const __int64 USERSTATUS_SHIELD       = 1ll << 8;
const __int64 USERSTATUS_ATKPOWER     = 1ll << 9;
const __int64 USERSTATUS_GHOST        = 1ll << 10;
const __int64 USERSTATUS_DISAPPEARING = 1ll << 11;
const __int64 USERSTATUS_MAGICDEF     = 1ll << 12;
const __int64 USERSTATUS_BOWDEF       = 1ll << 13;
const __int64 USERSTATUS_RED          = 1ll << 14;
const __int64 USERSTATUS_BLACK        = 1ll << 15;
const __int64 USERSTATUS_ATKRANGE     = 1ll << 16;
const __int64 USERSTATUS_REFLECT      = 1ll << 17;
const __int64 USERSTATUS_SUPERMAN     = 1ll << 18;
const __int64 USERSTATUS_BODYSHIELD   = 1ll << 19;
const __int64 USERSTATUS_MAGICDMG     = 1ll << 20;
const __int64 USERSTATUS_ATKSPEEDEX   = 1ll << 21;
const __int64 USERSTATUS_INVISIBILTY  = 1ll << 22;
const __int64 USERSTATUS_CYCLONE      = 1ll << 23;
const __int64 USERSTATUS_SYNCRIME     = 1ll << 24;
const __int64 USERSTATUS_REFLECTMAGIC = 1ll << 25;
const __int64 USERSTATUS_DODGE        = 1ll << 26;
const __int64 USERSTATUS_FLY          = 1ll << 27;
const __int64 USERSTATUS_CHARGEUP     = 1ll << 28;
const __int64 USERSTATUS_FROZEN       = 1ll << 29;
const __int64 USERSTATUS_PRAY         = 1ll << 30;
const __int64 USERSTATUS_FOLLOWPRAY   = 1ll << 31;
const __int64 USERSTATUS_CURSE        = 1ll << 32;
const __int64 USERSTATUS_BLESS        = 1ll << 33;

// =====================================================================
// Command types
// =====================================================================
const int _COMMAND_NULL      = 0;
const int _COMMAND_PICKUP    = 1;
const int _COMMAND_STOP      = 1;
const int _COMMAND_STANDBY   = 2;
const int _COMMAND_WALK      = 3;
const int _COMMAND_RUN       = 4;
const int _COMMAND_EMOTION   = 6;
const int _COMMAND_ACTION    = 7;
const int _COMMAND_FOLLOW    = 9;
const int _COMMAND_SHITHAPPEN = 10;
const int _COMMAND_DIE       = 11;
const int _COMMAND_SPATTACK  = 12;
const int _COMMAND_FAINT     = 13;
const int _COMMAND_WOUND     = 15;
const int _COMMAND_JUMP      = 16;
const int _COMMAND_DEFEND    = 16;
const int _COMMAND_WALKFORWARD = 17;
const int _COMMAND_RUNFORWARD  = 18;
const int _COMMAND_ATTACK    = 20;
const int _COMMAND_SHOOT     = 21;
const int _COMMAND_RUSHATK   = 23;
const int _COMMAND_INTONE    = 23;
const int _COMMAND_LOCKATK   = 24;
const int _COMMAND_MINE      = 25;
const int _COMMAND_DASH      = 25;

// v1079 renumbered movement: CHero_Walk issues 14 or 15, and a live trace of
// the hero walking showed iType 15. JUMP (16) is unchanged. The older
// constants above are kept for reference but are not trustworthy for moves.
const int _COMMAND_MOVE_1079_A = 14;
const int _COMMAND_MOVE_1079_B = 15;
// Mining pushes command 8 (BEGIN); while the pickaxe loop runs the hero then
// sits in _COMMAND_ACTION (7) / ACCOMPLISH — the same state a landed jump
// leaves (both verified live 2026-09-28).
const int _COMMAND_MINE_1079 = 8;

inline bool IsMoveCommandType(int iType)
{
    return iType == _COMMAND_MOVE_1079_A || iType == _COMMAND_MOVE_1079_B;
}

// =====================================================================
// Command status
// =====================================================================
const int _CMDSTATUS_BEGIN      = 0;
const int _CMDSTATUS_DEPART     = 1;
const int _CMDSTATUS_PROGRESS   = 2;
const int _CMDSTATUS_CONTINUE   = 3;
const int _CMDSTATUS_WAITING    = 4;
const int _CMDSTATUS_ACCOMPLISH = 6;

// =====================================================================
// CCommand — action command
// =====================================================================
class CCommand {
public:
    CCommand() {}
    int iType;
    int iStatus;

    union {
        char szCmd[256];

        struct {
            OBJID  idTarget;
            Position posTarget;
            int    nDir;
            int    nArg;
            DWORD  dwData;
            int    nUnknown;
            BOOL   bData;
            int    nData;
            BOOL   bHeroSendMsg;
            int    nFrameStart;
            int    nFrameEnd;
            DWORD  dwTimestamp;
            int    nFrameInterval;
            BOOL   bDiagnal;
            BOOL   bAddUp;
            DWORD  dwIndex;
        };
    };
};

// =====================================================================
// CRole — base entity class
//
// Hierarchy: CMapObj -> CRole -> CHero
//
// All offsets verified via Ghidra + Cheat Engine on the 64-bit client.
// =====================================================================
#pragma pack(push, 1)
class CRole : public CMapObj
{
public:
    virtual ~CRole() {}

private:
    BYTE _pad20[0x10];            // +0x20

public:
    __int64 m_nStatusFlag;         // +0x30

private:
    BYTE _pad38[0x30];            // +0x38  (deque<PAniEffect> + unknowns)

public:
    // ── CRoleInfo fields ──
    OBJID m_id;                    // +0x68  entity ID
private:
    BYTE _pad6C[0x28];            // +0x6C
public:
    char m_szName[16];             // +0x94  null-terminated ASCII
private:
    BYTE _padA4[0x34];            // +0xA4
public:
    Position m_posMap;               // +0xD8  map cell position
    Position m_posWorld;             // +0xE0  world position
    Position m_posScr;               // +0xE8  screen position

private:
    BYTE _padF0[0x188 - 0xF0];     // +0xF0
public:
    CCommand m_cmdAction;           // +0x188  current action command
    // v1079 inserted 0x10 bytes somewhere between m_cmdAction and m_nMaxHp;
    // every field below moved by +0x10 (verified live 2026-09-27).
private:
    BYTE _pad290[0x3E0 - 0x290];    // +0x290
public:
    int   m_nMaxHp;                // +0x3E0 cached max HP (was 0x3D0)
private:
    BYTE _pad3E4[0x6F0 - 0x3E4];  // +0x3E4
public:
    int   m_nStamina;              // +0x6F0 current stamina (PP) (was 0x6E0)
    int   m_nMaxStamina;           // +0x6F4 max stamina
private:
    BYTE _pad6F8[0x724 - 0x6F8];  // +0x6F8
public:
    OBJID m_idSyndicate;           // +0x724  syndicate/guild ID (0 = none) (was 0x714)
    int   m_nSyndicateRank;        // +0x728  syndicate rank (100=Leader, 90=Deputy, 50=Member)

    // ── Helpers ──
    OBJID GetID() const { return m_id; }
    const char* GetName() const { return m_szName; }
    BOOL TestState(__int64 nState) const { return (m_nStatusFlag & nState) != 0; }

    BOOL IsDead() const { return (m_nStatusFlag & USERSTATUS_DEAD) != 0; }
    BOOL IsPlayer() const { return m_id >= 1000000; }
    BOOL IsMonster() const { return m_id >= 400000 && m_id < 500000 && !IsGuard() && !IsPatrol(); }
    BOOL HasXpSkillReady() const { return TestState(USERSTATUS_XPFULL); }
    BOOL IsCycloneActive() const { return TestState(USERSTATUS_CYCLONE); }
    BOOL IsFlyActive() const { return TestState(USERSTATUS_FLY); }
    BOOL IsStigmaActive() const { return TestState(USERSTATUS_ATKPOWER); }
    BOOL IsSupermanActive() const { return TestState(USERSTATUS_SUPERMAN); }
    BOOL IsMagicShieldActive() const { return TestState(USERSTATUS_SHIELD); }
    int GetStamina() const { return m_nStamina; }
    int GetMaxStamina() const { return m_nMaxStamina; }
    BOOL IsMining() const { return m_cmdAction.iType == _COMMAND_MINE; }
    BOOL IsJumping() const {
        return m_cmdAction.iType == _COMMAND_JUMP
            && (m_cmdAction.posTarget.x != m_posMap.x || m_cmdAction.posTarget.y != m_posMap.y);
    }
    BOOL IsGuard() const {
        return m_id >= 400000 && m_id < 500000
            && strstr(m_szName, "Guard") == m_szName;
    }
    BOOL IsPatrol() const {
        return m_id >= 400000 && m_id < 500000
            && strstr(m_szName, "Patrol") == m_szName;
    }
    BOOL HasSyndicate() const { return m_idSyndicate != 0; }

    void GetPos(Position& pos) const { pos = m_posMap; }
    const CCommand& GetCommand() const { return m_cmdAction; }

    void SetCommand(CCommand* cmd);
};
#pragma pack(pop)

static_assert(offsetof(CRole, m_nStatusFlag) == 0x30, "CRole::m_nStatusFlag");
static_assert(offsetof(CRole, m_id)          == 0x68, "CRole::m_id");
static_assert(offsetof(CRole, m_szName)      == 0x94, "CRole::m_szName");
static_assert(offsetof(CRole, m_posMap)      == 0xD8, "CRole::m_posMap");
static_assert(offsetof(CRole, m_posWorld)    == 0xE0, "CRole::m_posWorld");
static_assert(offsetof(CRole, m_posScr)      == 0xE8, "CRole::m_posScr");
static_assert(offsetof(CRole, m_cmdAction)   == 0x188, "CRole::m_cmdAction");
static_assert(offsetof(CRole, m_nMaxHp)      == 0x3E0, "CRole::m_nMaxHp");
static_assert(offsetof(CRole, m_nStamina)     == 0x6F0, "CRole::m_nStamina");
static_assert(offsetof(CRole, m_nMaxStamina)  == 0x6F4, "CRole::m_nMaxStamina");
static_assert(offsetof(CRole, m_idSyndicate)  == 0x724, "CRole::m_idSyndicate");
static_assert(offsetof(CRole, m_nSyndicateRank) == 0x728, "CRole::m_nSyndicateRank");

using PRole = Ref<CRole>;

class CHero; // forward declaration

// =====================================================================
// RoleList — snapshot of the roles currently known to the client.
//
// Indexable like the std::deque the pre-v1079 client used, so callers keep
// `list.size()`, `list[i]` and range-for. Elements point into the game's own
// container: take the snapshot and use it within the same frame.
// =====================================================================
class RoleList
{
public:
    size_t size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }
    const PRole& operator[](size_t i) const { return *m_items[i]; }

    struct const_iterator {
        std::vector<const PRole*>::const_iterator it;
        const PRole& operator*() const { return **it; }
        const_iterator& operator++() { ++it; return *this; }
        bool operator!=(const const_iterator& o) const { return it != o.it; }
    };
    const_iterator begin() const { return { m_items.begin() }; }
    const_iterator end() const { return { m_items.end() }; }

    void push_back(const PRole* role) { m_items.push_back(role); }

private:
    std::vector<const PRole*> m_items;
};

// =====================================================================
// CRoleMgr — nearby roles (monsters, and presumably players/NPCs)
//
// v1079: heap object reached through the RoleMgr global (see Game::GetRoleMgr).
// The hero is NOT in here — use CHero::GetSingletonPtr().
// Only monsters were verified live so far; players/NPCs are unconfirmed.
// Game uses the same MSVC STL as us, so the unordered_map ABI matches.
// =====================================================================
#pragma pack(push, 1)
class CRoleMgr
{
private:
    BYTE _pad00[0x30];                                // +0x00 vtable + unknowns
public:
    std::unordered_map<OBJID, PRole> m_mapRole;       // +0x30 roles by entity ID

    RoleList Roles() const {
        RoleList list;
        if (m_mapRole.size() >= 10000)                // corrupt / wrong layout guard
            return list;
        for (const auto& entry : m_mapRole)
            list.push_back(&entry.second);
        return list;
    }
};
#pragma pack(pop)

static_assert(offsetof(CRoleMgr, m_mapRole) == 0x30, "CRoleMgr::m_mapRole");
static_assert(sizeof(std::unordered_map<OBJID, PRole>) == 0x40, "unordered_map ABI");
