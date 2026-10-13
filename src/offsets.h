#pragma once
#include <cstddef>
#include <cstdint>

// =====================================================================
// Game address resolver
//
// Every game function/global the bot touches is listed here. Addresses
// are NOT hardcoded any more — they are resolved at runtime and stored in
// coclassic_offsets.json (next to coclassic.dll):
//
//   1. If an entry's "build" matches the running ImConquer.exe (or is ""),
//      its "rva" is used directly.
//   2. Otherwise the game was patched: the entry's byte signature ("sig")
//      is scanned for in the unpacked game code to find the new address.
//   3. Whenever an address is confirmed on a new build, a fresh signature
//      is generated and the JSON is rewritten, so the next patch can be
//      followed automatically too.
//
// If a patch changes a function so much that its signature no longer
// matches, that entry is reported as unresolved and the features that
// depend on it are disabled instead of crashing the game. Fix it by
// putting the new RVA in the JSON and setting its "build" to "".
// =====================================================================

enum class GameAddr : size_t {
    // Globals
    HeroPtr,        // slot holding a heap std::shared_ptr<CHero>* (hero = **slot)
    RoleMgr,        // slot holding CRoleMgr* (mgr = *slot)
    EntityInfo,
    EntitySet,
    GameMap,
    FillRectPtr,
    UIWindowMgr,    // the client's window manager (CUIWindowMgr, see CUIWindow.h)
    // Functions
    CEntity_RenderVisual,
    CNetClient_GetInstance,
    CNetClient_SendMsg,
    CHero_GetMaxHp,
    CHero_GetMaxMana,
    CStatTable_GetValue,
    CHero_GetStat,          // v1079: (hero, stat) decodes the obfuscated stat table; stat 1 = current HP
    StatBlock_Decrypt,      // v1079: (key, block, out[16]) decrypts encrypted hero values (current mana)
    CHero_GetCurrentMana,
    CHero_FatalStrike,
    CHero_Walk,
    CHero_Jump,
    CHero_StartMining,
    MsgUpdate_Process,
    TradeWindow_HandleMessage,
    CGameUI_ShowMsg,
    GetTimestamp,

    Count
};

namespace GameOffsets {
    // Resolves every address it can for the running game build. Safe to call
    // repeatedly (already-resolved entries are skipped). Returns true once all
    // required entries (HeroPtr, RoleMgr) are resolved.
    bool Resolve();

    // Absolute address, or 0 if the entry could not be resolved.
    uintptr_t Get(GameAddr id);
    inline bool Has(GameAddr id) { return Get(id) != 0; }
    const char* Name(GameAddr id);

    // Logs a summary of unresolved entries with instructions for fixing them.
    void ReportUnresolved();
}
