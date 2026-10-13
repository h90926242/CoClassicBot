#pragma once
#include "CHero.h"
#include "CGameMap.h"
#include "CUIWindow.h"
#include "log.h"
#include "offsets.h"
#include <string>

class CStatTable;
class CEntitySet;
class CEntityInfo;

// Game addresses are resolved at runtime (signature scan + per-build cache)
// — see offsets.h. Never hardcode an RVA here; add a GameAddr entry instead.
namespace GameRva {
    constexpr uintptr_t CNETCLIENT_GET_INSTANCE = 0x0B8f00;
    constexpr uintptr_t CNETCLIENT_SEND_MSG = 0x42E0;
}
namespace GameVtableIndex {
    // v1079: slot 66 (+0x210), Themida-virtualized. Was 59 before the patch.
    constexpr size_t CRole_SetCommand = 66;
}

// =====================================================================
// Game — static accessor for game data
// =====================================================================
class Game
{
public:
    static void Init() {
        g_qwModuleBase = (ULONG64)GetModuleHandleA(nullptr);
        spdlog::info("[game] Base: 0x{:X}", (uintptr_t)g_qwModuleBase);
    }

    static uintptr_t Base() { return (uintptr_t)g_qwModuleBase; }

    // Absolute address of a resolved game entry, as T; nullptr if unresolved.
    template <typename T>
    static T Resolve(GameAddr id) {
        return reinterpret_cast<T>(GameOffsets::Get(id));
    }

        template <typename T>
    static T Resolve(uintptr_t rva) {
        return reinterpret_cast<T>(Base() + rva);
    }

    // v1079: the manager is heap-allocated; the global only holds a pointer to it.
    static CRoleMgr* GetRoleMgr() {
        auto** slot = Resolve<CRoleMgr**>(GameAddr::RoleMgr);
        return slot ? *slot : nullptr;
    }

    static CGameMap* GetMap() {
        return Resolve<CGameMap*>(GameAddr::GameMap);
    }

    static CEntitySet* GetEntitySet() {
        return Resolve<CEntitySet*>(GameAddr::EntitySet);
    }

    static CEntityInfo* GetEntityInfo() {
        auto** slot = Resolve<CEntityInfo**>(GameAddr::EntityInfo);
        return slot ? *slot : nullptr;
    }

    static CHero* GetHero() {
        return CHero::GetSingletonPtr();
    }

    // A client window by id (GameUIWindow::*), or nullptr.
    static CUIWindow* GetUIWindow(int id) {
        auto* mgr = Resolve<CUIWindowMgr*>(GameAddr::UIWindowMgr);
        // The manager is created lazily; an empty map may not have its head node yet.
        if (!mgr || mgr->m_windows.size() == 0)
            return nullptr;
        const auto it = mgr->m_windows.find(id);
        return it != mgr->m_windows.end() ? it->second.get() : nullptr;
    }

    // Closes a client window like its X button. Returns false if it wasn't open.
    static bool CloseUIWindow(int id) {
        CUIWindow* window = GetUIWindow(id);
        if (!window || !window->IsOpen())
            return false;
        window->Close();
        return true;
    }
};

namespace GameCall {
    using CEntity_RenderVisualFn = void(*)(void*);
    using CNetClient_GetInstanceFn = void*(*)();
    using CNetClient_SendMsgFn = uint32_t(*)(int64_t, const uint8_t*, int64_t);
    using CRole_SetCommandFn = void(*)(CRole*, CCommand*);
    using CStatTable_GetValueFn = int(*)(const CStatTable*, int);
    // v1079 replacements. Both only read memory; unlike the game's own HP/MP
    // getters they don't feed its anti-tamper call hash (0x1CD7D0).
    using CHero_GetStatFn = uint32_t(*)(const CHero*, int);
    using StatBlock_DecryptFn = void(*)(const void* key, const void* block, uint32_t out[4]);
    using CHero_GetCurrentManaFn = int(*)(const CHero*);
    using CHero_GetMaxHpFn = int(*)(const CHero*);
    using CHero_GetMaxManaFn = int(*)(const CHero*);
    using CHero_FatalStrikeFn = void(*)(CHero*, OBJID);
    using CHero_WalkFn = void(*)(CHero*, int, int);
    using CHero_JumpFn = void(*)(CHero*, int, int);
    using CHero_StartMiningFn = void(*)(CHero*);
    using MsgUpdate_ProcessFn = uint8_t(*)(void*);
    using TradeWindow_HandleMessageFn = uint64_t(*)(int64_t*, int, int64_t);
    // CGameUI::ShowMsg — adds a chat message to the display.
    // sender/receiver/suffix/message are std::string* (MSVC ABI).
    using CGameUI_ShowMsgFn = uint64_t(*)(
        void* gameUI,             // CGameUI* this
        const std::string* sender,
        const std::string* receiver,
        const std::string* suffix,
        const std::string* message,
        uint32_t color,           // ARGB
        uint16_t style,
        uint16_t channel,         // 0x7D1 = whisper
        uint32_t timestamp,
        uint32_t extra
    );

    using GetTimestampFn = long long*(*)(long long*);

    // Each accessor returns nullptr when the function could not be resolved
    // for the running game build — callers must check before calling.
    inline CEntity_RenderVisualFn CEntity_RenderVisual() { return Game::Resolve<CEntity_RenderVisualFn>(GameAddr::CEntity_RenderVisual); }
    inline CNetClient_GetInstanceFn CNetClient_GetInstance() {
        static auto fn = Game::Resolve<CNetClient_GetInstanceFn>(GameRva::CNETCLIENT_GET_INSTANCE);
        return fn;
    }
    inline CNetClient_SendMsgFn CNetClient_SendMsg() {
        static auto fn = Game::Resolve<CNetClient_SendMsgFn>(GameRva::CNETCLIENT_SEND_MSG);
        return fn;
    }
    inline CStatTable_GetValueFn CStatTable_GetValue() { return Game::Resolve<CStatTable_GetValueFn>(GameAddr::CStatTable_GetValue); }
    inline CHero_GetStatFn CHero_GetStat() { return Game::Resolve<CHero_GetStatFn>(GameAddr::CHero_GetStat); }
    inline StatBlock_DecryptFn StatBlock_Decrypt() { return Game::Resolve<StatBlock_DecryptFn>(GameAddr::StatBlock_Decrypt); }
    inline CHero_GetMaxHpFn CHero_GetMaxHp() { return Game::Resolve<CHero_GetMaxHpFn>(GameAddr::CHero_GetMaxHp); }
    inline CHero_GetCurrentManaFn CHero_GetCurrentMana() { return Game::Resolve<CHero_GetCurrentManaFn>(GameAddr::CHero_GetCurrentMana); }
    inline CHero_GetMaxManaFn CHero_GetMaxMana() { return Game::Resolve<CHero_GetMaxManaFn>(GameAddr::CHero_GetMaxMana); }
    inline CHero_FatalStrikeFn CHero_FatalStrike() { return Game::Resolve<CHero_FatalStrikeFn>(GameAddr::CHero_FatalStrike); }
    inline CHero_WalkFn CHero_Walk() { return Game::Resolve<CHero_WalkFn>(GameAddr::CHero_Walk); }
    inline CHero_JumpFn CHero_Jump() { return Game::Resolve<CHero_JumpFn>(GameAddr::CHero_Jump); }
    inline CHero_StartMiningFn CHero_StartMining() { return Game::Resolve<CHero_StartMiningFn>(GameAddr::CHero_StartMining); }
    inline MsgUpdate_ProcessFn MsgUpdate_Process() { return Game::Resolve<MsgUpdate_ProcessFn>(GameAddr::MsgUpdate_Process); }
    inline TradeWindow_HandleMessageFn TradeWindow_HandleMessage() { return Game::Resolve<TradeWindow_HandleMessageFn>(GameAddr::TradeWindow_HandleMessage); }
    inline CGameUI_ShowMsgFn CGameUI_ShowMsg() { return Game::Resolve<CGameUI_ShowMsgFn>(GameAddr::CGameUI_ShowMsg); }
    inline GetTimestampFn GetTimestamp() { return Game::Resolve<GetTimestampFn>(GameAddr::GetTimestamp); }
}

// Returns true if a name appears in a comma/semicolon-separated whitelist (case-insensitive).
inline bool IsNameWhitelisted(const char* name, const char* whitelist)
{
    if (!name || !name[0] || !whitelist || !whitelist[0])
        return false;
    const char* p = whitelist;
    while (*p) {
        // skip delimiters/whitespace
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ',' && *p != ';' && *p != '\n' && *p != '\r' && *p != '\t')
            ++p;
        // trim trailing spaces
        const char* end = p;
        while (end > start && *(end - 1) == ' ')
            --end;
        size_t len = end - start;
        if (len > 0 && _strnicmp(start, name, len) == 0 && name[len] == '\0')
            return true;
    }
    return false;
}

// Returns true if any non-whitelisted player (other than heroId) is on the entity list.
inline bool AreOtherPlayersNearby(OBJID heroId, const char* whitelist = nullptr)
{
    CRoleMgr* mgr = Game::GetRoleMgr();
    const RoleList roles = mgr ? mgr->Roles() : RoleList{};
    if (!mgr || roles.empty() || roles.size() >= 10000)
        return false;

    for (size_t i = 0; i < roles.size() && i < 500; ++i) {
        const auto& roleRef = roles[i];
        if (!roleRef)
            continue;
        const CRole* role = roleRef.get();
        if (!role->IsPlayer() || role->GetID() == heroId)
            continue;
        if (whitelist && IsNameWhitelisted(role->GetName(), whitelist))
            continue;
        return true;
    }
    return false;
}

// Convenience wrapper — resolves hero ID automatically.
inline bool ArePlayersNearby(const char* whitelist = nullptr)
{
    const CHero* hero = Game::GetHero();
    return AreOtherPlayersNearby(hero ? hero->GetID() : 0, whitelist);
}
